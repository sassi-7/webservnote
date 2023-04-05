#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <cassert>
#include <sys/epoll.h>

#include "locker.h"
#include "threadpool.h"
#include "lst_timer.h"
#include "http_conn.h"
#include "log.h"
#include "sql_connection_pool.h"

#define MAX_FD 65536      //最大文件描述符数
#define MAX_EVENT_NUMBER 10000   //最大事件数
#define TIMESLOT 5      //最小超时单位

#define SYNLOG     //同步写日志
//#define ASYNLOG    //异步写日志

#define listenfdLT      //水平触发阻塞
//#define listenfdET    //边缘触发阻塞

//此三个函数在http_conn.cpp中有定义，gcc在编译的时候会自动链接
extern int addfd(int epollfd, int fd, bool one_shot);
extern int remove(int epollfd, int fd);
extern int setnonblocking(int fd);

//设置定时器相关参数
static int pipefd[2];           //管道，用于进程间通信
static sort_timer_lst timer_lst;
static int epollfd = 0;         //epoll例程(指向被监视文件描述符的保存空间)

//信号处理函数，这里只是用于通知主线程，并不处理，缩短异步处理时间，减少对主程序的影响
void sig_handler(int sig){
	//为保证函数的可重入性，保留原来的errno（这种系统定义的全局变量可能会在中断的时候改变）
	//可重入性表示中断后再次进入该函数，环境变量与之前相同，不会丢失数据
	int save_errno = errno;
	int msg = sig;
	
	//将信号值从管道写端写入，传输字符类型，而非整型
	send(pipefd[1], (char*)&msg, 1, 0);
	
	//将原来的errno设为当前errno
	errno = save_errno;
}

//设置信号函数
void addsig(int sig, void(handler)(int), bool restart = true){
	
	//创建sigaction结构体变量
	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	
	//信号处理器中仅通知主线程，不作处理
	sa.sa_hanlder = handler;
	if(restart)
		sa.sa_flags |= SA_RESTART;
	
	//将所有信号添加到信号集中
	sigfillset(&sa.sa_mask);
	
	//注册信号类型对应的信号处理器(信号处理函数)
	assert(sigaction(sig, &sa, NULL) != -1);
}


//定时器回调函数
void cb_func(client_data *user_data){
	//删除非活动连接在epollfd上的注册事件
	epoll_ctl(epollfd, EPOLL_CTL_DEL, user_data->sockfd, 0);
	assert(user_data);
	//关闭文件描述符
	close(user_data->sockfd);
	
	//减少连接数
	http_conn::m_user_count--;
}

int main(int argc, char *argv[]){

#ifdef SYNLOG
	Log::get_instance()->init("ServerLog", 2000, 800000, 0);  //同步日志模型
#endif

#ifdef ASYNLOG
	Log::get_instance()->init("ServerLog", 2000, 800000, 8);  //异步日志模型
#endif

	if(argc != 2){
		printf("usage：%s port_number\n", basename(argv[0]));    //basename函数输出路径中最后一个斜杠后的字符串
		return 1;
	}
	
	init port = atoi(argv[1]);
	
	addsig(SIGPIPE, SIG_IGN);
	
	//创建数据库连接池
	connection_pool* connPool = connection_pool::GetInstance();
	connPool->init("localhost", "root", "dr57", "sassidb", 3306, 8);
	
	//创建线程池
	threadpool<http_conn> *pool = NULL;
	try{
		pool = new threadpool<http_conn>(connPool);
	}catch(...){         //三个点表示任意类型参数
		return 1;
	}
	
	//创建MAX_FD个http类对象
	http_conn* users = new http_conn[MAX_FD];
	assert(users);
	
	//初始化数据库读取表
	users->initmysql_result(connPool);
	
	int listenfd = socket(PF_INET, SOCK_STREAM, 0);
	assert(listenfd >= 0);
	
	int ret=0;
	
	struct sockaddr_in address;
	bzero(&address, sizeof(address));
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_ANY);
	address.sin_port = htons(port);
	
	int flag=1;
	setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag));
	assert(bind(listenfd, (struct sockaddr*)&address, sizeof(address)) != -1);
	assert(listen(listenfd, 5) != -1);
	
	//创建内核事件表
	epoll_event events[MAX_EVENT_NUMBER];
	//创建epoll例程（保存所监视事件的文件描述符的空间）
	epollfd = epoll_create(5);     //5是大小，实际上只作为操作系统的参考
	assert(epollfd != -1);
	
	//将listenfd放在epoll例程中
	addfd(epollfd, listenfd, false);
	//将epollfd赋值给http类的static变量m_epollfd
	http_conn::m_epollfd = epollfd;
	
	//创建管道套接字
	assert(socketpair(PF_UNIX, SOCK_STREAM, 0, pipefd) != -1);
	
	//设置管道写端为非阻塞，原因是避免信号处理函数阻塞，增加时间开销
	setnonblocking(pipefd[1]);
	
	//统一事件源，将管道读端注册为epoll读事件
	addfd(epollfd, pipefd[0], false);
	
	//注册信号源对应的信号处理函数
	addsig(SIGALRM, sig_handler, false);
	addsig(SIGTERM, sig_handler, false);
	
	//循环条件
	bool stop_server = false;
	
	//连接资源，所有客户端的相关数据
	client_data *users_timer = new client_data[MAX_FD];
	
	//超时标志
	bool timeout = false;
	
	//隔TIMESLOT时间触发一次SIGALRM信号
	alarm(TIMESLOT);
	
	while(!stop_server){
		//等待所监视的文件描述符的事件发生
		int number = epoll_wait(epollfd, events, MAX_EVENT_NUMBER, -1);
		if(number < 0 && errno != EINTR){
			LOG_ERROR("%s", "epoll failure");
			break;
		}
		
		//轮询所有就绪事件并处理
		for(int i=0; i < number; i++){
		  int sockfd = events[i].data.fd;
		  
		  //3.处理信号
		  //管道读端对应文件描述符发生读事件
		  else if((sockfd == pipefd[0]) && (events[i].events & EPOLLIN)){
			//从管道读端读出信号值，成功则返回字节数，失败则返回-1
			//正常情况下，这里的ret返回值总是1，只有14和15两个ascii码对应的字符
			ret = recv(pipefd[0], signals, sizeof(signals), 0);
			if(ret == -1){
				continue;
			}
			else if(ret == 0){
				continue;
			}
			else {
				//信号对应的处理逻辑
				for(int i=0; i < ret; i++){
				  //这里是字符
				  switch(signals[i]){
				  	case SIGALRM:
				  	{
				  	  timeout = true;
				  	  break;
				  	}
				  	case SIGTERM:
				  	{
				  	  stop_server = true;
				  	}
				  }
				}
			}
		  }
		  
		  
		}
	}
}
