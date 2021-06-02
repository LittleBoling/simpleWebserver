#ifndef THREADPOOL_H
#define THREADPOOL_H

#include<list>
#include<cstdio>
#include<exception>
#include<pthread.h>

#include"locker.h"

template<typename T>
class threadpool
{
public:
    threadpool(int thread_number=8, int max_requests=10000);
    ~threadpool();

    bool append(T* request);
private:
    static void* worker(void* arg);
    void run();

private:
    int m_thread_number;
    int m_max_requests;
    pthread_t* m_threads;
    std::list<T*> m_workqueue;
    locker m_queuelocker;
    sem m_queuestat;
    bool m_stop;
};

//默认创建8个线程，然后分离线程
template<typename T>
threadpool<T>::threadpool(int thread_number, int max_requests):
    m_thread_number(thread_number), m_max_requests(max_requests),m_stop(false), m_threads(NULL)
{
    if((thread_number<=0)||(max_requests<=0))
    {
        throw std::exception();
    }

    m_threads = new pthread_t[m_thread_number];
    if(!m_threads)
    {
        throw std::exception();
    }

    for(int i=0; i<thread_number; ++i)
    {
        printf("Create the %dth thread\n", i);
        if(pthread_create(m_threads+i, NULL, worker, this)!=0)
        {
            delete [] m_threads;
            throw std::exception();
        }

        if(pthread_detach(m_threads[i]))
        {
            delete [] m_threads;
            throw std::exception();
        }
    }
}

template<typename T>
threadpool<T>::~threadpool()
{
    delete [] m_threads;
    m_stop = true;
}

template<typename T>
bool threadpool<T>::append(T* request)
{
    //添加互斥锁，保证同一时间只有一个线程往工作队列中添加数据
    m_queuelocker.lock();
    //工作队列是一个双向链表
    if(m_workqueue.size()>m_max_requests)
    {
        m_queuelocker.unlock();
        return false;
    }

    m_workqueue.push_back(request);
    m_queuelocker.unlock();
    m_queuestat.post();
    return true;
}

//pthread_create函数的第三个参数要求一个静态函数
//静态成员函数存在于对象之外，对象中不包含任何跟静态数据有关的成员
//静态成员函数不能访问动态成员变量，因此要通过静态函数调用动态函数的方式来使用
template<typename T>
void* threadpool<T>::worker(void* arg)
{
    threadpool* pool = (threadpool*)arg;
    pool->run();
    return pool;
}
//获得一个工作线程
template<typename T>
void threadpool<T>::run()
{
    while(!m_stop)
    {
        m_queuestat.wait();
        m_queuelocker.lock();
        if(m_workqueue.empty())
        {
            m_queuelocker.unlock();
            continue;
        }
        T* request = m_workqueue.front();
        m_workqueue.pop_front();
        m_queuelocker.unlock();
        if(!request)
        {
            continue;
        }
        request->process();
    }
}

#endif

