#ifndef LOCKER_H
#define LOCKER_H

#include<exception>
#include<pthread.h>
#include<semaphore.h>

/*
 * @Funcion: Posix信号，post将sem加一，sem value>0时将随即唤醒一个正在执行sem_wait的线程
*/
class sem
{
public:
    //Create semaphore
    sem()
    {
        if(sem_init(&m_sem, 0, 0)!=0)
        {
            throw std::exception();
        }
    }

    ~sem()
    {
        sem_destroy(&m_sem);
    }
    // Wait semaphore
    bool wait()
    {
        return sem_wait( &m_sem )==0;
    }

    // Add semaphore
    bool post()
    {
        return sem_post( &m_sem )==0;
    }
private:
    sem_t m_sem;
};


/*
 *@Function: 互斥锁
 */
class locker
{
public:
    locker()
    {
        if(pthread_mutex_init(&m_mutex, NULL)!=0)
        {
            throw std::exception();
        }
    }
    ~locker()
    {
        pthread_mutex_destroy(&m_mutex);
    }

    bool lock()
    {
        return pthread_mutex_lock(&m_mutex)==0;
    }

    bool unlock()
    {
        return pthread_mutex_unlock(&m_mutex)==0;
    }
private:
    pthread_mutex_t m_mutex;
};

/*
 *@Function:条件变量，逻辑上是当线程达到某个值时被触发
 *@Note:构造函数中初始化了一个mutex，是因为cond_wait函数需要接受一个mutex参数
 */
class cond
{
public:
    cond()
    {
        if(pthread_mutex_init(&m_mutex, NULL))
        {
            throw std::exception();
        }
        if(pthread_cond_init(&m_cond, NULL)!=0)
        {
            pthread_mutex_destroy(&m_mutex);
            throw std::exception();
        }
    }
    ~cond()
    {
        pthread_mutex_destroy(&m_mutex);
        pthread_cond_destroy(&m_cond);
    }

    bool wait()
    {
        int ret=0;
        pthread_mutex_lock(&m_mutex);
        ret = pthread_cond_wait(&m_cond, &m_mutex);
        pthread_mutex_unlock(&m_mutex);
        return ret==0;
    }
    
    // 唤醒等待条件变量的线程
    bool signal()
    {
        return pthread_cond_signal(&m_cond)==0;
    }
private:
    pthread_mutex_t m_mutex;
    pthread_cond_t m_cond;
};

#endif
