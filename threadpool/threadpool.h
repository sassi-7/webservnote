#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <list>
#include <cstdio>
#include <exception>
#include <pthread.h>
#include "locker.h"
#include "sql_connection_pool.h"

template <typename T>
class threadpool{                     //线程池类
	private:
		int m_thread_number;   //线程池中的线程数
		int m_max_requests;     //请求队列中允许的最大请求数
		pthread_t* m_threads;  //描述线程池的数组，大小为m_thread_number
		std::list<T *> m_workqueue;  //请求队列
		locker m_queuelocker;   //保护请求队列的互斥锁
		sem m_queuestat;          //是否有任务需要处理
		bool m_stop;                  //是否结束线程
		connection_pool* m_connPool;      //数据库连接池

	private:
		//工作线程运行的函数，不断从工作队列中取出任务并执行
		static void* worker(void* arg);    //线程处理函数
		void run();

	public:
		//connPool是数据库连接池指针，thread_number是线程池中线程的数量，max_requests是请求队列中最多允许的、等待的数量
		threadpool(connection_pool* connPool, int thread_number = 8, int max_requests = 10000);
		~threadpool();
		bool append(T* request);     //往请求队列添加任务
};

//线程池的创建与回收
template <typename T>
threadpool<T>::threadpool(connection_pool* connPool, \
			int thread_number, int max_requests \
			) : m_thread_number(thread_number), \
			m_max_requests(max_requests), m_stop(false), \
			m_threads(NULL), m_connPool(connPool){   //初始化列表，冒号后面的相当于赋值，例如其中一个m_stop=false
			
			if(thread_number <= 0 || max_requests <= 0)
				throw std::exception();
				
			//线程id初始化
			m_threads = new pthread_t [m_thread_number];
			if(!m_threads)
				throw std::exception();
				
			for(int i = 0; i < thread_number; i++){
				//循环创建线程，并将工作线程按要求进行运行
				if(pthread_create(m_threads + i, NULL, worker, this) != 0){
					delete[] m_threads;
					throw std::exception();
				}
				
				//将线程进行分离后，不用单独对工作线程进行回收
				if(pthread_detach(m_threads[i])){
					delete[] m_threads;
					throw std::exception();
				}
			}

}

//线程池类的析构函数
template <typename T>
threadpool<T>::~threadpool(){
	delete[] m_threads;        //删除线程的数组
	m_stop = true;               //结束线程
}

//向请求队列中添加任务，通过互斥锁保证线程安全，添加完成后通过信号量唤醒工作进程
template <typename T>
bool threadpool<T>::append(T* request){
	m_queuelocker.lock();     //上锁
	
	//根据服务器硬件，预先设置请求队列最大值
	if(m_workqueue.size() >= m_max_requests){
		m_queuelocker.unlock();
		return false;
	}
	
	//添加任务
	m_workqueue.push_back(request);
	m_queuelocker.unlock();
	
	//通过信号量提醒有任务要处理
	m_queuestat.post();
	return true;
}

//内部访问私有成员函数run。个人认为worker函数与run函数的配合是为了满足pthread_create函数的调用，最终是为了运行run函数
template <typename T>
void* threadpool<T>::worker(void* arg){
	threadpool* pool = (threadpool*)arg;
	pool->run();
	return pool;
}

//工作线程从请求队列中取出任务进行处理，注意线程同步
template <typename T>
void threadpool<T>::run(){
	while(!m_stop){
		//等待信号量
		m_queuestat.wait();
		
		//被唤醒后先加上互斥锁
		m_queuelocker.lock();
		if(m_workqueue.empty()){
			m_queuelocker.unlock();
			continue;
		}
		
		//从请求队列中取出第一个任务，然后将任务从请求队列中删除
		T* request = m_workqueue.front();
		m_workqueue.pop_front();
		m_queuelocker.unlock();
		if(!request)continue;
		
		//从连接池中取出一个数据库连接
		//request->mysql = m_connPool->GetConnection();
		//将数据库连接放回连接池
		//m_connPool->ReleaseConnection(request->mysql);

		//以上两个关于数据库连接的获取与释放，由RAII机制处理
		//负责自动获取和释放数据库连接
		connectionRAII mysqlcon(&request->mysql, m_connPool);
		
		//由http_conn类的process方法进行处理
		request->process();
		
		
	}
}



#endif