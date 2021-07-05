#include "http_conn.h"
#include <sys/uio.h>
#include <mysql/mysql.h>
#include <map>

//定义HTTP响应的一些状态
//
const char * ok_200_title = "OK";
const char * error_400_title = "Bad Request";
const char * error_400_form = "Your request has bad syntax or is inherently impossible to statisfy.\n";
const char * error_403_title = "Forbidden";
const char * error_403_form = "You do not have permission to get file from this server.\n";
const char * error_404_title = "Not Found";
const char * error_404_form = "The requested file was not found on this server.\n";
const char * error_500_title = "Internal Error";
const char * error_500_form = "There was an unusual problem serving the request file.\n";
//网站的根目录
const char * doc_root = "../testfile";
map<string, string> sqlUsers;
locker m_sqllock;
connection_pool* sqlPool;
void http_conn::initmysql_result(connection_pool *connPool)
{
    sqlPool = connPool;
    //先从连接池中取一个连接
    MYSQL *mysql = NULL;
    connectionRAII mysqlcon(&mysql, connPool);

    //在user表中检索username，passwd数据，浏览器端输入
    if (mysql_query(mysql, "SELECT user_name,passward FROM userInfo"))
    {
        LOG_ERROR("SELECT error:%s\n", mysql_error(mysql));
        printf("Select error:%s\n", mysql_error(mysql));
    }

    //从表中检索完整的结果集
    MYSQL_RES *result = mysql_store_result(mysql);

    //返回结果集中的列数
    int num_fields = mysql_num_fields(result);

    //返回所有字段结构的数组
    MYSQL_FIELD *fields = mysql_fetch_fields(result);

    //从结果集中获取下一行，将对应的用户名和密码，存入map中
    while (MYSQL_ROW row = mysql_fetch_row(result))
    {
        string temp1(row[0]);
        string temp2(row[1]);
        sqlUsers[temp1] = temp2;
    }
}

//设置socket文件描述符为非阻塞
//这是因为ET模式要求其文件描述符应该为非阻塞的，否则容易产生丢失连接的问题
int setnonblocking(int fd)
{
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

//设置EPOLL的工作模式，可选填是否设置oneshot
void addfd(int epollfd, int fd, bool one_shot)
{
    epoll_event event;
    event.data.fd = fd;
    event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
    if(one_shot)
    {
        event.events |= EPOLLONESHOT;
    }
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    setnonblocking(fd);
}

//@Function: 将监听的文件描述符从epoll的文件描述符集合中移除
void removefd(int epollfd, int fd)
{
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
    close(fd);
}

//该函数是为了重置某个socket上的oneshot时间，以便于其他线程也可以为该线程服务
void modfd(int epollfd, int fd, int ev)
{
    epoll_event event;
    event.data.fd = fd;
    event.events = ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}

int http_conn::m_user_count = 0;
int http_conn::m_epollfd = -1;

//通过删除epoll中文件描述符集合的方式关闭连接
//.h文件的声明中设置了默认值为true
void http_conn::close_conn(bool real_close)
{
    if (real_close && (m_sockfd != -1)) 
    {
       removefd(m_epollfd, m_sockfd);
       m_sockfd = -1;
       //user_count是static类型，因此会一直计数
       m_user_count --;   //关闭一个连接时, 将客户总量减一
    }
}

//该函数由主函数的监听函数调用
void http_conn::init(int sockfd, const sockaddr_in &addr)
{
    m_sockfd = sockfd;
    m_address = addr;
    //如下两行是为了避免TIME_WAIT状态， 仅用于调试，实际使用时应该去掉
    // int reuse = 1;
    // setsockopt(m_sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    addfd(m_epollfd, sockfd, true);
    m_user_count ++;
   
    //此处是根据特征标的不同重载的函数
    //特征标一词出现在C++ primer plus中文版函数重载这一章节
    init();
}

//初始化需要使用的全局变量
void http_conn::init()
{
    mysql = NULL;
    m_check_state = CHECK_STATE_REQUESTLINE;//标记当前行是HTTP请求中（请求头/请求行/请求内容）的哪一部分
    m_linger = false;

    m_method = GET;//提取GET或POST方法
    m_url    = 0; //该变量严格来说应该是存储的端口号后面的具体的方法或文件路径
    m_version = 0;
    m_content = 0;//POST方法的内容
    m_content_length = 0;//内容的长度
    m_host = 0;
    m_start_line = 0;
    m_checked_idx = 0;
    m_read_idx = 0;
    m_write_idx = 0;
    memset(m_read_buf, '\0', READ_BUFFER_SIZE);
    memset(m_write_buf, '\0', WRITE_BUFFER_SIZE);
    memset(m_real_file, '\0', FILENAME_LEN);
}


//从状态机解析的是当前被解析行的状态，分成未读完，读完没毛病和读完有毛病三部分
http_conn::LINE_STATUS http_conn::parse_line()
{
    char temp;
    for(; m_checked_idx < m_read_idx; ++m_checked_idx) 
    {
        temp = m_read_buf[m_checked_idx];
        if (temp == '\r') 
        {
            //由于m_read_idx标记的是当前缓冲区最后一个数据的下一个位置
            //因此若+1相等，则表明当前行没有读完
            if ((m_checked_idx + 1) == m_read_idx)
            {
                return LINE_OPEN;
            }
            //由于http请求是以\n\r分割的，因此若下一个字符为\n则表明当前请求类别已经读完了
            else if (m_read_buf[m_checked_idx + 1] == '\n') 
            {
                //该代码中大量使用了给某个字符串添'\0'的操作，这是为了分割字符串
                m_read_buf[m_checked_idx ++] = '\0';
                m_read_buf[m_checked_idx ++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
        else if (temp == '\n') 
        {
            if ((m_checked_idx > 1) && (m_read_buf[m_checked_idx -1] == '\r'))
            {
                m_read_buf[m_checked_idx - 1] = '\0';
                m_read_buf[m_checked_idx ++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
    }
    return LINE_OPEN;
}


//循环读取客户数据， 直到无数据可读或者对方关闭连接
bool http_conn::read()
{
    if (m_read_idx >= READ_BUFFER_SIZE) 
    {
        return false;
    }

    int bytes_read = 0;
    while(true)
    {
        bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
        if (bytes_read == -1) 
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK) 
            {
                break;
            }
            return false;
        }
        else if (bytes_read == 0) 
        {
            return false;
        }
        m_read_idx += bytes_read;
    }
    return true;
}


//解析HTTP请求行， 获得请求方法、目标URL， 以及HTTP版本号
http_conn::HTTP_CODE http_conn::parse_request_line(char *text)
{
    //
    m_url = strpbrk(text, " \t");
    if (!m_url) 
    {
        return BAD_REQUEST;
    }
    *m_url++ = '\0';

    char *method = text;
    if (strcasecmp(method, "GET") == 0 )
    {
        m_method = GET;
    }
    else if(strcasecmp(method, "POST") == 0)
    {
        m_method = POST;
    }
    else
    {
        return BAD_REQUEST;
    }
    m_url += strspn(m_url, " \t");
    m_version = strpbrk(m_url, " \t");
    if (!m_version) 
    {
        return BAD_REQUEST;
    }

    *m_version++ = '\0';
    m_version += strspn(m_version, " \t");
    if (strcasecmp(m_version, "HTTP/1.1") != 0) 
    {
        return BAD_REQUEST;
    }

    if (strncasecmp(m_url, "http://", 7) == 0) 
    {
        m_url += 7;
        m_url = strchr(m_url, '/');
    }

    if (!m_url || m_url[0] != '/') 
    {
        return BAD_REQUEST;
    }
    //fprintf(stdout, "The m_url is:%s\n", m_url);
    m_check_state = CHECK_STATE_HEADER;
    return NO_REQUEST;
}


//解析HTTP请求的一个头部信息
http_conn::HTTP_CODE http_conn::parse_headers(char *text)
{
    //遇到空行， 表示头部字段解析完毕
    if (text[0] == '\0') 
    {
        //如果HTTP请求有消息体， 则还需要读取m_content_length字节的消息体，
        //状态机转移到CHECK_STATE_CONTENT状态
        if (m_content_length != 0) 
        {
            m_check_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }

        //否则说明我们已经得到了一个完整的HTTP请求
        return GET_REQUEST;
    }

    //处理Connection头部字段
    else if (strncasecmp(text, "Connection:", 11)  == 0)
    {
        text += 11;
        text += strspn(text, " \t");
        if (strcasecmp(text, "keep-alive") == 0) 
        {
            m_linger = true;
        }
    }

    //处理Content-Length头部字节
    else if (strncasecmp(text, "Content-Length:", 15) == 0)
    {
        text += 15;
        text += strspn(text, " \t");
        m_content_length = atol(text);
    }
    //处理Host头部字段
    else if (strncasecmp(text, "Host:", 5) == 0) 
    {
        text += 5;
        text += strspn(text, " \t");
        m_host = text;
    }
    //其他头部字段
    else
    {
        LOG_INFO("Unknow header %s\n", text);
    }
    
    return NO_REQUEST;
}


/*我们没有真正解析HTTP请求的消息体，只是判断它是否被完整地读入了*/
http_conn::HTTP_CODE http_conn::parse_content(char *text)
{
    if (m_read_idx >= (m_content_length + m_checked_idx))
    {
        text[m_content_length] = '\0';
        m_content = text;
        return GET_REQUEST;
    }

    return NO_REQUEST;
}

//主状态机，用于处理
http_conn::HTTP_CODE http_conn::process_read()
{
    LINE_STATUS line_status = LINE_OK;
    HTTP_CODE ret = NO_REQUEST;
    char * text = 0;
    //m_check_state标识主状态机当前所处状态
    //line_status标识被解析的当前行的状态
    while (((m_check_state == CHECK_STATE_CONTENT) && (line_status == LINE_OK))
        || ((line_status = parse_line()) == LINE_OK))
        {
            //getline函数是m_readbuf+start_line的形式，因为在parse_line函数中已经将第一个\r\n处改成了
            //‘\0’因此此时的text就是到第一个\r\n的字符串，m_checked_idx则在\n的下一个字符的位置
            text = get_line();
            m_start_line = m_checked_idx;
            //此时的m_checked_idx就是当前行的长度
            //fprintf(stdout, "got 1 http line: %d\n", m_checked_idx);

            switch (m_check_state)
            {
                case CHECK_STATE_REQUESTLINE:
                {
                    //成功后返回NO_REQUEST
                    //该函数的作用，是提取请求行的关键信息并将m_checked_state转为HEADER的形式
                    ret = parse_request_line(text);
                    if (ret == BAD_REQUEST) 
                    {
                        return BAD_REQUEST;
                    }
                    break;
                }
                case CHECK_STATE_HEADER:
                {
                    ret = parse_headers(text);
                    if (ret == BAD_REQUEST) 
                    {
                        return BAD_REQUEST;;
                    }
                    else if (ret == GET_REQUEST) 
                    {
                        return do_request();
                    }
                    break;
                }
                case CHECK_STATE_CONTENT:
                {
                    ret = parse_content(text);
                    if (ret == GET_REQUEST) 
                    {
                        return do_request();
                    }
                    line_status = LINE_OPEN;
                    break;
                }
                default:
                {
                    return INTERNAL_ERROR;
                }
            }
        }
    return NO_REQUEST;
}

/*当得到一个完整、正确的HTTP请求时，我们就分析目标文件的属性。如果目标文件存在，
 * 对所有用户可读，且不是目录，则使用mmap将其映射到内存地址m_file_address处，并告诉调用者获取文件成功*/
http_conn::HTTP_CODE http_conn::do_request()
{
    strcpy(m_real_file, doc_root);
    int len = strlen(doc_root);
    //Insert my post code
    if(strncasecmp(m_url, "/", 5)==0)
        strcpy(m_url, "/log.html");
    //Get POST request and process m_content
    else if(strncasecmp(m_url, "/login", 6)==0)
    {
        strcpy(m_url, "/caton5.png");
    }
    else if(strncasecmp(m_url, "/register", 9)==0)
    {
        strcpy(m_url, "/register.html");
    }

    if(m_content!=NULL)
    {
        char name[100], passward[100];
        memset(name, '\0', 100);
        memset(passward, '\0', 100);
        int index = 5;
        int i=0;
        //m_content="name=test&passward=test"
        while(m_content[index]!='&')
        {
            name[i++] = m_content[index++];
        }
        index += 10;
        i=0;
        while(m_content[index]!='\0')
        {
            passward[i++] = m_content[index++];
        }

        //MYSQL *mysql = NULL;
        char* sqlSelect = (char*)malloc(sizeof(char)*200);
        strcat(sqlSelect, "Select user_name,passward from userInfo where user_name='");
        strcat(sqlSelect, name);
        strcat(sqlSelect, "'and passward='");
        strcat(sqlSelect, passward);
        strcat(sqlSelect, "'");
        connectionRAII mysqlcon(&mysql, sqlPool);
        int res = mysql_query(mysql, sqlSelect);
        if(res)
        {
            strcpy(m_url, "/error.jpeg");
        }
        
        MYSQL_RES* queryResult = mysql_store_result(mysql);
        //int num_fields = mysql_num_fields(result);
        MYSQL_ROW row=mysql_fetch_row(queryResult);

        if(row==NULL)
        {
            strcpy(m_url, "/caton2.jpg");
        }
        mysql_free_result(queryResult);
    }

    strncpy(m_real_file + len, m_url, FILENAME_LEN - len - 1);
    //stat获取文件信息并保存的m_file_stat结构体中
    if (stat(m_real_file, &m_file_stat) < 0)
    {
        return NO_RESOURCE;
    }
    //st_mode是文件类型,S_IROTH是内核权限值00004
    if (!(m_file_stat.st_mode & S_IROTH))
    {
        return FORBIDDEN_REQUEST;
    }
    //是目录
    if (S_ISDIR(m_file_stat.st_mode))
    {
        return BAD_REQUEST;
    }

    int fd = open(m_real_file, O_RDONLY);

    m_file_address = (char *)mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);

    return FILE_REQUEST;
}

/*对内存映射区执行munmap操作*/
void http_conn::unmap()
{
    if (m_file_address) 
    {
        munmap(m_file_address, m_file_stat.st_size);
        m_file_address = 0;
    }
}

bool http_conn::write()
{
    int temp = 0;
    int bytes_have_send = 0;
    int bytes_to_send = m_write_idx;
    if (bytes_to_send == 0) 
    {
        modfd(m_epollfd, m_sockfd, EPOLLIN);
        init();
        return true;
    }

    while(1)
    {
        //4.21F
        temp = writev(m_sockfd, m_iv, m_iv_count);
        if (temp <= -1) 
        {
            /*如果TCP写缓冲没有空间，
             * 则等待一下轮EPOLLOUT事件。虽然在此期间，服务器无法立即接收到同一个客户的下一个请求，但这可以保证连接的完整性*/
            if (errno == EAGAIN) 
            {
                modfd(m_epollfd, m_sockfd, EPOLLOUT);
                return true;
            }
            unmap();
            return false;
        }


        bytes_to_send -= temp;
        bytes_have_send += temp;
        if (bytes_to_send <= bytes_have_send) 
        {
            /*发送HTTP响应成功，
             * 根据HTTP请求中的Connection字段决定是否立即关闭连接*/
            unmap();
            if (m_linger) 
            {
                init();
                modfd(m_epollfd, m_sockfd, EPOLLIN);
                return true;
            }
            else
            {
                modfd(m_epollfd, m_sockfd, EPOLLIN);
                return false;
            }
        }

    }
}

//往写缓冲中写入代发送数据
bool http_conn::add_response(const char * format, ...)
{
    if (m_write_idx >= WRITE_BUFFER_SIZE) 
    {
        return false;
    }
    //va这个系列是来遍历参数列表中的参数的，因为本函数参数是个列表
    va_list arg_list;
    va_start(arg_list, format);
    int len = vsnprintf(m_write_buf + m_write_idx, WRITE_BUFFER_SIZE - 1 - m_write_idx,
            format, arg_list);
    if (len >= (WRITE_BUFFER_SIZE - 1 - m_write_idx))
    {
        return false;
    }
    m_write_idx += len;
    va_end(arg_list);
    return true;
}

bool http_conn::add_status_line(int status, const char * title)
{
    return add_response("%s %d %s\r\n", "HTTP/1.1", status, title);
}

bool http_conn::add_headers(int content_len)
{
    add_content_length(content_len);
    add_linger();
    add_blank_line();
    return true;
}


bool http_conn::add_content_length(int content_len)
{
    return add_response("Content-Length: %d\r\n", content_len);
}

bool http_conn::add_linger()
{
    return add_response("Connection: %s\r\n", (m_linger == true)?"keep-alive" : "close");
}

bool http_conn::add_blank_line()
{
    return add_response("%s", "\r\n");
}

bool http_conn::add_content(const char * content)
{
    return add_response("%s", content);
}

/*根据服务器处理HTTP请求的结果，决定返回给客户端的内容*/
bool http_conn::process_write(HTTP_CODE ret)
{
    switch (ret)
    {
        case INTERNAL_ERROR:
            {
                add_status_line(500, error_500_title);
                add_headers(strlen(error_500_form));
                if (!add_content(error_500_form)) 
                {
                    return false;
                }
                break;
            }
        case BAD_REQUEST:
            {
                add_status_line(400, error_400_title);
                add_headers(strlen(error_400_form));
            if (!add_content(error_400_form)) 
            {
                return false;
            }
            break;
        }
    case NO_RESOURCE:
        {
            add_status_line(404, error_404_title);
            add_headers(strlen(error_404_form));
            if (!add_content(error_404_form))
            {
                return false;
            }
            break;
        }
    case FORBIDDEN_REQUEST:
        {
            add_status_line(403, error_403_title);
            add_headers(strlen(error_403_form));
            if (!add_content(error_403_form))
            {
                return false;
            }
            break;
        }
    case FILE_REQUEST:
        {
            add_status_line(200, ok_200_title);
            if (m_file_stat.st_size != 0) 
            {
                add_headers(m_file_stat.st_size);
                //第0个元素是输出上面add_XX函数的信息
                //第1个元素才是要输出的文件
                m_iv[0].iov_base = m_write_buf;
                m_iv[0].iov_len  = m_write_idx;
                m_iv[1].iov_base = m_file_address;
                m_iv[1].iov_len = m_file_stat.st_size;
                m_iv_count = 2;
                return true;
            }
            else
            {
                const char * ok_string = "<html><body></body></html>";
                add_headers(strlen(ok_string));
                if (!add_content(ok_string)) 
                {
                    return false;
                }
            }
        }
    default:
        {
            return false;
        }

    }
    
    m_iv[0].iov_base = m_write_buf;
    m_iv[0].iov_len = m_write_idx;
    m_iv_count = 1;
    return true;

}


//threadpool's work process call this function.
//this is interface or HTTP request
void http_conn::process()
{
    //HTTP_CODE返回请求类型
    HTTP_CODE read_ret = process_read();
    //NO_REQUEST表示没有请求或者当前的请求行已经处理完毕
    if (read_ret == NO_REQUEST) 
    {
        //确认请求结束后，向epoll中重新注册oneshot事件
        modfd(m_epollfd, m_sockfd, EPOLLIN);
        return;
    }

    bool write_ret = process_write(read_ret);
    if (!write_ret) 
    {
        close_conn();
    }
    //在一个socket的请求被处理完毕之后，都应该重置epolloneshot时间，以便于其他线程使用
    modfd(m_epollfd, m_sockfd, EPOLLOUT);
}
