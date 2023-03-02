#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <list>
#include <cstdio>
#include <exception>
#include <pthread.h>
#include "../lock/locker.h"
#include "../sqlpool/sql_connection_pool.h"
#include "../timer/lst_timer.h"
#include "../log/log.h"

template <typename T>
class threadpool
{
public:
    /*thread_number是线程池中线程的数量，max_requests是请求队列中最多允许的、等待处理的请求的数量*/
    threadpool(int actor_model, connection_pool *connPool, int thread_number = 8, int max_request = 10000);
    ~threadpool();
    bool append(T *request, int state);
    bool append_p(T *request);

private:
    /*工作线程运行的函数，它不断从工作队列中取出任务并执行之*/
    static void *worker(void *arg);
    void run();

private:
    int m_thread_number;        //线程池中的线程数
    int m_max_requests;         //请求队列中允许的最大请求数
    pthread_t *m_threads;       //描述线程池的数组，其大小为m_thread_number
    std::list<T *> m_workqueue; //请求队列
    locker m_queuelocker;       //保护请求队列的互斥锁
    //这是一个封装过后的对象,这里和数据库连接池不一样，数据库连接池的信号量要初始化为数据库连接的个数，因为开始时一个连接对象都没使用
    //这里需要把信号量的值初始化为0，因为这个时候任务队列里一个任务也没有，也就是说信号量遇到post就加1,遇到wait就减1
    sem m_queuestat;            //是否有任务需要处理
    connection_pool *m_connPool;  //数据库
    int m_actor_model;          //模型切换
};
template <typename T>
threadpool<T>::threadpool( int actor_model, connection_pool *connPool, int thread_number, int max_requests) : m_actor_model(actor_model),m_thread_number(thread_number), m_max_requests(max_requests), m_threads(NULL),m_connPool(connPool)
{
    if (thread_number <= 0 || max_requests <= 0)
        throw std::exception();
    m_threads = new pthread_t[m_thread_number];
    if (!m_threads)
        throw std::exception();
    for (int i = 0; i < thread_number; ++i)
    {
        if (pthread_create(m_threads + i, NULL, worker, this) != 0)
        {
            delete[] m_threads;
            throw std::exception();
        }
        if (pthread_detach(m_threads[i]))
        {
            delete[] m_threads;
            throw std::exception();
        }
    }
}
template <typename T>
threadpool<T>::~threadpool()
{
    delete[] m_threads;
}
//state参数是标志是读消息还是写消息
//对于reactor模式,需要区分是读消息还是写消息
template <typename T>
bool threadpool<T>::append(T *request, int state)
{
    m_queuelocker.lock();
    if (m_workqueue.size() >= m_max_requests)
    {
        m_queuelocker.unlock();
        return false;
    }
    request->m_state = state;
    m_workqueue.push_back(request);
    m_queuelocker.unlock();
    m_queuestat.post();
    return true;
}
template <typename T>
bool threadpool<T>::append_p(T *request)
{
    m_queuelocker.lock();
    if (m_workqueue.size() >= m_max_requests)
    {
        m_queuelocker.unlock();
        return false;
    }
    m_workqueue.push_back(request);
    m_queuelocker.unlock();
    m_queuestat.post();
    return true;
}
//一个worker就是一个线程
//worker函数之所以这么实现，具体见课本301页
template <typename T>
void *threadpool<T>::worker(void *arg)
{
    threadpool *pool = (threadpool *)arg;
    pool->run();
    return pool;
}
template <typename T>
void threadpool<T>::run()
{
    while (true)
    {
        //获取任务
        m_queuestat.wait();
        m_queuelocker.lock();
        if (m_workqueue.empty())
        {
            m_queuelocker.unlock();
            continue;
        }
        T *request = m_workqueue.front();
        m_workqueue.pop_front();
        m_queuelocker.unlock();
        if (!request)
            continue;
        //处理任务
        //reactor模式
        if (1 == m_actor_model)
        {
            util_timer *timer = request->m_server->users_timer[request->m_sockfd].timer;
            //如果是读
            if (0 == request->m_state)
            {
                if (request->read_once())
                {
                    connectionRAII mysqlcon(&request->mysql, m_connPool);
                    request->process();
                }
                else
                {
                    //下面这句不用，因为deal_timer里会关闭
//                    request->close_conn(true);
//                    LOG_ERROR1("server: fd %d %s", request->m_sockfd, "read data failure");

                    request->m_server->deal_timer(timer, request->m_sockfd);
                }
            }
            //如果是写数据
            else
            {
                if (!request->write()){
                    //下面这句不用，因为deal_timer里会关闭
//                    request->close_conn(true);
//                    LOG_ERROR1("server: fd %d %s", request->m_sockfd, "send data failure");
                    request->m_server->deal_timer(timer, request->m_sockfd);
                }
            }
        }
        //proactor模式，也就是默认模式
        else
        {
            connectionRAII mysqlcon(&request->mysql, m_connPool);
            request->process();
        }
    }
}
#endif
