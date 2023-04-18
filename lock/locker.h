#ifndef LOCKER_H
#define LOCKER_H

#include <exception>
#include <pthread.h>
#include <semaphore.h>

class sem{                        //信号量类
	private:
		sem_t m_sem;          //sem_t是长整数类型
		
	public:
		sem(){                     //无参构造函数
			if(sem_init(&m_sem, 0, 0) != 0){  //sem_init函数用于初始化未命名的信号量，???第二个参数的作用，
			//函数原型：int sem_init(sem_t *sem, int pshared, unsigned int value);如果 pshared 的值为 0，那么信号量将被进程内的线程共享，如果 pshared 是非零值，那么信号量将在进程之间共享
			//???为啥信号量前要&，该函数需要传入地址
				throw std::exception();        //初始化不成功则抛出异常
			}
		}
		sem(int num){          //有参构造函数
			if(sem_init(&m_sem, 0, num) != 0){
				throw std::exception();
			}
		}
		~sem(){                     //析构函数
			sem_destroy(&m_sem);    //该函数用于销毁信号量
		}
		bool wait(){                //信号量的p操作，等待信号量
			return sem_wait(&m_sem) == 0;   //该函数以原子操作将信号量减一，若信号量为0时，该函数阻塞
		}
		bool post(){                //信号量的v操作，释放信号量
			return sem_post(&m_sem) == 0;     //该函数以原子操作将信号量加一，信号量大于0时，唤醒调用该函数的线程
			//sem_wait与sem_post成功则返回0，失败返回errno(整型)
		}
};


class locker{                    //互斥锁类,用于独占访问或者互斥访问
	private:
		pthread_mutex_t m_mutex;
		
	public:
		locker(){
			if(pthread_mutex_init(&m_mutex, NULL) != 0){   //该函数用于初始化互斥锁
				throw std::exception();
			}
		}
		~locker(){
			pthread_mutex_destroy(&m_mutex);    //用于销毁互斥锁
		}
		bool lock(){
			return pthread_mutex_lock(&m_mutex) == 0;     //以原子操作给互斥锁上锁
		}
		bool unlock(){
			return pthread_mutex_unlock(&m_mutex) == 0;  //以原子操作给互斥锁解锁
		}
		pthread_mutex_t* get(){        //获取互斥锁
			return &m_mutex;
		}
};

//条件变量提供一种线程间的通知机制，当某个共享数据达到某个值时，唤醒等待该共享数据的线程
class cond{             //条件变量类
	private:
		pthread_cond_t m_cond;
		
	public:
		cond(){
			if(pthread_cond_init(&m_cond, NULL) != 0){     //初始化条件变量
				throw std::exception();
			}
		}
		~cond(){
			pthread_cond_destroy(&m_cond);        //销毁条件变量
		}
		bool wait(pthread_mutex_t* m_mutex){
			return pthread_cond_wait(&m_cond, m_mutex) == 0;  //函数用户等待目标条件变量，该函数调用时需要传入mutex参数(加锁的互斥锁)，函数执行时，先把调用线程放入条件变量的请求队列，然后将互斥锁mutex解锁，当函数成功返回0时，互斥锁会再次被锁上，函数内部会有一次解锁和上锁操作
		}
		bool timewait(pthread_mutex_t* m_mutex, struct timespec t){
			return pthread_cond_timedwait(&m_cond, m_mutex, &t) == 0;  //带时间的等待条件变量
		}
		bool signal(){
			return pthread_cond_signal(&m_cond) == 0;    //释放条件变量
		}
		bool broadcast(){
			return pthread_cond_broadcast(&m_cond) == 0;   //以广播的形式通知所有等待目标条件变量的线程
		}

};


#endif