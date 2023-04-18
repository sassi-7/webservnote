#==========自定义变量==========
#指定c++编译器
CXX = g++
#导入头文件
LIB = -I cgimysql/ -I http/ -I lock/ -I log/ -I threadpool/ -I timer/
#编译器属性指定
CXXFLAGS = $(LIB) -lpthread -lmysqlclient 

#==========c++编译============
# server : main.cpp ./http/http_conn.cpp ./log/log.cpp \
# 			./cgimysql/sql_connection_pool.cpp ./log/log.h \
# 			./cgimysql/sql_connection_pool.h ./log/block_queue.h \
# 			./http/http_conn.h ./lock/locker.h     \
# 			./threadpool/threadpool.h
# 	$(CXX) -o $@ $^  $(CXXFLAGS)

#   $(CXX) $(CXXFLAGS) $^ -o $@   这样会报错显示没有链接成功，把-lpthread这些放后面就不报错了

server : main.cpp ./http/http_conn.cpp ./log/log.cpp \
			./cgimysql/sql_connection_pool.cpp
	$(CXX) -o $@ $^  $(CXXFLAGS)


#==========make伪命令==========
.PHONY : bulid clean

build : server

clean :
	rm -f server
