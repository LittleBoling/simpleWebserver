#include<sys/socket.h>
#include<netinet/in.h>
#include<arpa/inet.h>
#include<stdio.h>
#include<unistd.h>
#include<errno.h>
#include<string.h>
#include<fcntl.h>
#include<stdlib.h>
#include<cassert>
#include<sys/epoll.h>

#include "locker.h"
#include "threadpool.h"
#include "http_conn.h"
#include "timer_heap.h"

#define MAX_FD 65536
#define MAX_EVENT_NUMBER 10000
#define TIMESLOT 5

static int pipefd[2];
static time_heap timer_heap(65530);
static int epollfd=0;

extern int addfd(int epollfd, int fd, bool one_shot);
extern int removefd(int epollfd, int fd);
extern int setnonblocking(int fd);

void addsig(int sig, void(handler)(int), bool restart=true)
{
    struct sigaction sa;
    memset(&sa, '\0', sizeof(sa));
    sa.sa_handler = handler;
    if(restart)
    {
        sa.sa_flags |= SA_RESTART;
    }
    sigfillset(&sa.sa_mask);
    assert(sigaction(sig, &sa, NULL)!=-1);
}

//信号处理函数，将信号值传到管道的另一端
void sig_handler( int sig )
{
  int save_errno = errno;
  int msg = sig;
  send( pipefd[1], ( char* )&msg, 1, 0 );
  errno = save_errno;
}

void time_handler()
{
    timer_heap.tick();
    alarm(TIMESLOT);
}

void cb_func(client_data* user_data)
{
    epoll_ctl(epollfd, EPOLL_CTL_DEL, user_data->sockfd, 0);
    assert(user_data);
    close(user_data->sockfd);
    printf("close fd: %d\n", user_data->sockfd);
}

void show_error(int connfd, const char* info)
{
    printf("%s", info);
    send(connfd, info, strlen(info), 0);
    close(connfd);
}

void writeLog()
{
    if(1)
    {
        Log::get_instance()->init("./WebServerLog", 1, 2000, 800000,800);
    }
    else
        Log::get_instance()->init("./WebServerLog", 1, 2000, 800000, 0);
}

void sqlConnPool(http_conn* users)
{
    //初始化数据库连接池
    connection_pool* m_connPool = connection_pool::GetInstance();
    m_connPool->init("localhost", "littleboling", "abc123", "users_db", 3306, 8);

    //初始化数据库读取表
    users->initmysql_result(m_connPool);
}

int main(int argc, char* argv[])
{
    const char* ip;
    int port;
    if(argc==1)
    {
        ip = "127.0.0.1";
        port = atoi("12345");
    }
    else if(argc>1&&argc<=2)
    {
        printf("Usage: %s ip_address port_number or %s to excute\n", basename(argv[0]), basename(argv[0]));
        return 1;
    }
    else
    { 
        ip = argv[1];
        port = atoi(argv[2]);
    }

    addsig(SIGPIPE, SIG_IGN);

    //Create threadpool
    threadpool<http_conn>* pool = NULL;
    try
    {
        pool = new threadpool<http_conn>;
    }
    catch(...)
    {
        return 1;
    }

    //这里MAX_FD为65535是因为文件描述符上限被设置为65535，而文件描述符本质上又是一个int值
    //因此本项目使用一个大小为65535的数组直接访问文件描述符对应的位置
    http_conn* users = new http_conn[MAX_FD];
    assert(users);
    int user_count = 0;

    int listenfd = socket(PF_INET, SOCK_STREAM, 0);
    assert(listenfd>=0);
    struct linger tmp = {1, 0};
    setsockopt(listenfd, SOL_SOCKET, SO_LINGER, &tmp, sizeof(tmp));

    int ret=0;
    struct sockaddr_in address;
    bzero(&address, sizeof(address));
    address.sin_family = AF_INET;
    inet_pton(AF_INET, ip, &address.sin_addr);
    address.sin_port = htons(port);

    ret = bind(listenfd, (struct sockaddr*)&address, sizeof(address));
    assert(ret>=0);

    ret = listen(listenfd, 5);
    assert(ret>=0);

    epoll_event events[MAX_EVENT_NUMBER];
    int epollfd = epoll_create(5);
    assert(epollfd != -1);
    addfd(epollfd, listenfd, false);
    //这里传参是为了设置使httpconn.cpp中的removefd和modfd可以设置epoll的函数
    http_conn::m_epollfd = epollfd;

    //创建全双工管道
    ret = socketpair(PF_UNIX, SOCK_STREAM, 0, pipefd);
    assert(ret!=-1);

    setnonblocking(pipefd[1]);
    addfd(epollfd, pipefd[0], false);

    addsig(SIGALRM, sig_handler);
    addsig(SIGTERM, sig_handler);
    
    client_data* timerUsers = new client_data[MAX_FD];
    bool timeout = true;
    alarm(TIMESLOT);
    
    writeLog();
    sqlConnPool(users);

    while(true)
    {
        int number = epoll_wait(epollfd, events, MAX_EVENT_NUMBER, -1);
        if((number<0)&&(errno!=EINTR))
        {
            printf("epoll failure\n");
            break;
        }

        for(int i=0; i<number; i++)
        {
            int sockfd = events[i].data.fd;
            if(sockfd == listenfd)
            {
                struct sockaddr_in client_address;
                socklen_t client_addrlength = sizeof(client_address);
                int connfd = accept(listenfd, (struct sockaddr*)&client_address, &client_addrlength);
                if(connfd<0)
                {
                    printf("errno is: %d\n", errno);
                    continue;
                }
                if(http_conn::m_user_count >= MAX_FD)
                {
                    show_error(connfd, "Internal server busy");
                    continue;
                }
                //初始化网络连接结构体
                users[connfd].init(connfd, client_address);

                //定时器相关
                timerUsers[connfd].address = client_address;
                timerUsers[connfd].sockfd = connfd;
                heap_timer* timer = new heap_timer(0);
                timer->user_data = &timerUsers[connfd];
                timer->cb_func = cb_func;
                time_t cur = time(NULL);
                timer->expire = cur + 3*TIMESLOT;
                timerUsers[connfd].timer = timer;
                timer_heap.add_timer(timer);
            }
            else if((sockfd==pipefd[0])&&(events[i].events & EPOLLIN))
            {   
               int sig;
               char signals[1024];
               ret = recv( pipefd[0], signals, sizeof( signals ), 0 );
               if( ret == -1 )
               {
                   // handle the error
                   continue;
               }
               else if( ret == 0 )
               {
                   continue;
               }
               else
               {
                   for( int i = 0; i < ret; ++i )
                   {
                       switch( signals[i] )
                       {
                           case SIGALRM:
                           {
                               timeout = true;
                               break;
                           }
                           case SIGTERM:
                           {
                               ;
                           }
                       }
                   }
               }              
            }
            else if(events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR))
            {
                users[sockfd].close_conn();
            }
            else if(events[i].events & EPOLLIN)
            {
                heap_timer* timer = timerUsers[sockfd].timer;
                if(users[sockfd].read())
                {
                    //users是数组的首地址，sockfd是int型的数字
                    //append函数的参数是指针，所以这里只传地址
                    pool->append(users+sockfd);
                    if(timer)
                    {
                        time_t cur = time(NULL);
                        timer->expire = cur + 3*TIMESLOT;
                        heap_timer* tmp = timer;
                        timer_heap.del_timer(timer);
                        timer_heap.add_timer(tmp);
                    }
                }
                else 
                {
                    users[sockfd].close_conn();
                    cb_func(&timerUsers[sockfd]);
                    if(timer)
                    {
                        timer_heap.del_timer(timer);
                    }
                }
            }
            else if(events[i].events & EPOLLOUT)
            {
                if(!users[sockfd].write())
                {
                    users[sockfd].close_conn();
                }
            }
            else
            {}
        }
        if(timeout)
        {
            time_handler();
            timeout = false;
        }
    }
    close(epollfd);
    close(listenfd);
    close(pipefd[0]);
    close(pipefd[1]);
    delete [] users;
    delete pool;
    return 0;
}
