// NetTester.cpp : 此文件包含 "main" 函数。程序执行将在此处开始并结束。
//

#include <iostream>
#include "winsock.h"
#include "stdio.h"
#include "CfgFileParms.h"
#pragma comment (lib,"wsock32.lib")

#define MAX_BUFFER_SIZE 40000 //缓冲的最大大小

//基于select的定时器结构，目的是把数据的收发和定时都统一到一个事件驱动框架下
//可以有多个定时器，本设计实现了一个基准定时器，为周期性10ms定时，也可以当作是一种心跳计时器
//其余的定时器可以在这个基础上完成，可行的方案存在多种
//看懂设计思路后，自行扩展以满足需要
//基准定时器一开启就会立即触发一次
struct threadTimer_t {
	int iType; //为0表示周期性定时器，定时达到后，会自动启动下一次定时
	ULONG ulInterval;
	LARGE_INTEGER llStopTime;
}sBasicTimer;

struct socket_list {
	int num;
	SOCKET sock_array[64];
};

void init_list(socket_list* list)
{
	int i;
	list->num = 0;
	for (i = 0; i < 64; i++) {
		list->sock_array[i] = 0;
	}
}
void insert_list(SOCKET s, socket_list* list)
{
	int i;
	for (i = 0; i < 64; i++) {
		if (list->sock_array[i] == 0) {
			list->sock_array[i] = s;
			list->num += 1;
			break;
		}
	}
}
void delete_list(SOCKET s, socket_list* list)
{
	int i;
	for (i = 0; i < 64; i++) {
		if (list->sock_array[i] == s) {
			list->sock_array[i] = 0;
			list->num -= 1;
			break;
		}
	}
}
void make_fdlist(socket_list* list, fd_set* fd_list)
{
	int i;
	for (i = 0; i < 64; i++) {
		if (list->sock_array[i] > 0) {
			FD_SET(list->sock_array[i], fd_list);
		}
	}
}
void initTimer()
{
	sBasicTimer.iType = 0;
	sBasicTimer.ulInterval = 10 * 1000;//10ms,单位是微秒，10ms相对误差较小
	QueryPerformanceCounter(&sBasicTimer.llStopTime);
}
//根据系统当前时间设置select函数要用的超时时间——to，每次在select前使用
void setSelectTimeOut(timeval* to, struct threadTimer_t* sT)
{
	LARGE_INTEGER llCurrentTime;
	LARGE_INTEGER llFreq;
	LONGLONG next;
	//取系统当前时间
	QueryPerformanceFrequency(&llFreq);
	QueryPerformanceCounter(&llCurrentTime);
	if (llCurrentTime.QuadPart >= sT->llStopTime.QuadPart) {
		to->tv_sec = 0;
		to->tv_usec = 0;
		//		sT->llStopTime.QuadPart += llFreq.QuadPart * sT->ulInterval / 1000000;
	}
	else {
		next = sT->llStopTime.QuadPart - llCurrentTime.QuadPart;
		next = next * 1000000 / llFreq.QuadPart;
		to->tv_sec = (long)(next / 1000000);
		to->tv_usec = long(next % 1000000);
	}

}
//根据系统当前时间判断定时器sT是否超时，可每次在select后使用，返回值true表示超时，false表示没有超时
bool isTimeOut(struct threadTimer_t* sT)
{
	LARGE_INTEGER llCurrentTime;
	LARGE_INTEGER llFreq;
	//取系统当前时间
	QueryPerformanceFrequency(&llFreq);
	QueryPerformanceCounter(&llCurrentTime);

	if (llCurrentTime.QuadPart >= sT->llStopTime.QuadPart) {
		if (sT->iType == 0) {
			//定时器是周期性的，重置定时器
			sT->llStopTime.QuadPart += llFreq.QuadPart * sT->ulInterval / 1000000;
		}
		return true;
	}
	else {
		return false;
	}
}
void code(unsigned long x, char A[], int length)
{
	unsigned long test;
	int i;

	test = 1;
	test = test << (length - 1);
	for (i = 0; i < length; i++) {
		if (test & x) {
			A[i] = 1;
		}
		else {
			A[i] = 0;
		}
		test = test >> 1; //本算法利用了移位操作和"与"计算，逐位测出x的每一位是0还是1.
	}
}
unsigned long decode(char A[], int length)
{
	unsigned long x;
	int i;

	x = 0;
	for (i = 0; i < length; i++) {
		if (A[i] == 0) {
			x = x << 1;;
		}
		else {
			x = x << 1;
			x = x | 1;
		}
	}
	return x;
}
int main(int argc, char* argv[])
{
	SOCKET sock;
	struct sockaddr_in local_addr;
	struct sockaddr_in upper_addr; 
	struct sockaddr_in lower_addr[10];  //最多10个下层对象，数组下标是下层实体在编号
	struct sockaddr_in remote_addr;
	int lowerNumber;
	int len;
	char* buf;
	char* sendbuf;
	WSAData wsa;
	int retval;
	struct socket_list sock_list;
	fd_set readfds, writefds, exceptfds;
	timeval timeout;
	int i;
	unsigned long arg;
	int linecount = 0;
	int port;
	string s1, s2, s3;
	int count = 0;
	char* cpIpAddr;

	buf = (char*)malloc(MAX_BUFFER_SIZE);
	sendbuf = (char*)malloc(MAX_BUFFER_SIZE);
	buf = (char*)malloc(MAX_BUFFER_SIZE);
	if (sendbuf == NULL || buf == NULL) {
		if (sendbuf != NULL) {
			free(sendbuf);
		}
		if (buf != NULL) {
			free(buf);
		}
		cout << "内存不够" << endl;
		return 0;
	}

	CCfgFileParms cfgParms;

	if (argc == 4) {
		s1 = argv[1];
		s2 = argv[2];
		s3 = argv[3];
	}
	else if (argc == 3) {
		s1 = argv[1];
		s2 = "NET";
		s3 = argv[2];
	}
	else {
		//从命令行读取
		cout << "请输入设备号：";
		cin >> s1;
		//cout << "请输入层次名（大写）：";
		//cin >> s2;
		s2 = "NET";
		cout << "请输入实体号：";
		cin >> s3;
	}
	WSAStartup(0x101, &wsa);
	sock = socket(AF_INET, SOCK_DGRAM, 0);
	if (sock == SOCKET_ERROR)
		return 0;

	cfgParms.setDeviceID(s1);
	cfgParms.setLayer(s2);
	cfgParms.setEntityID(s3);
	cfgParms.read();
	cfgParms.print(); //打印出来看看是不是读出来了


	if (!cfgParms.isConfigExist) {
		//从键盘输入，需要连接的API端口号
		//偷个懒，要求必须填好配置文件
		return 0;
	}
	
	//取本层实体参数，并设置
	local_addr.sin_family = AF_INET;
	local_addr.sin_addr.S_un.S_addr = htonl(INADDR_LOOPBACK);
	retval = cfgParms.getValueInt(&port, CCfgFileParms::BASIC, (char*)"myPort", 0);
	if (0 > retval) {
		printf("参数错误\n");
		return 0;
	}
	local_addr.sin_port = htons(port);
	if (bind(sock, (sockaddr*)& local_addr, sizeof(sockaddr_in)) != 0) {
		printf("参数错误\n");
		return 0;

	}

	//读上层实体参数
	upper_addr.sin_family = AF_INET;
	cpIpAddr = cfgParms.getValueStr(CCfgFileParms::BASIC, (char*)"upperIPAddr", 0);
	if (cpIpAddr == NULL) {
		printf("参数错误\n");
		return 0;
	}
	upper_addr.sin_addr.S_un.S_addr = inet_addr(cpIpAddr);

	retval = cfgParms.getValueInt(&port, CCfgFileParms::BASIC, (char*)"upperPort", 0);
	if (0 > retval) {
		printf("参数错误\n");
		return 0;
	}
	upper_addr.sin_port = htons(port);


	//取下层实体参数，并设置
	//先取数量
	lowerNumber = cfgParms.getNumber(CCfgFileParms::LOWER);
	if (0 > lowerNumber) {
		printf("参数错误\n");
		return 0;
	}
	//逐个读取
	for (i = 0; i < lowerNumber; i++) {
		lower_addr[i].sin_family = AF_INET;
		cpIpAddr = cfgParms.getValueStr(CCfgFileParms::LOWER, (char*)"lowerIPAddr", i);
		if (cpIpAddr == NULL) {
			printf("参数错误\n");
			return 0;
		}
		lower_addr[i].sin_addr.S_un.S_addr = inet_addr(cpIpAddr);

		retval = cfgParms.getValueInt(&port, CCfgFileParms::LOWER, (char*)"lowerPort", i);
		if (0 > retval) {
			printf("参数错误\n");
			return 0;
		}
		lower_addr[i].sin_port = htons(port);
	}
	//	listen(s,5);
	init_list(&sock_list);
	//设置套接字为非阻塞态
	arg = 1;
	ioctlsocket(sock, FIONBIO, &arg);
	insert_list(sock, &sock_list);

	initTimer();
	while (1) {
		FD_ZERO(&readfds);
		FD_ZERO(&writefds);
		FD_ZERO(&exceptfds);
		make_fdlist(&sock_list, &readfds);
		//采用了基于select机制，不断发送测试数据，和接收测试数据，也可以采用多线程，一线专发送，一线专接收的方案
		//设定超时时间
		setSelectTimeOut(&timeout, &sBasicTimer);
		retval = select(0, &readfds, &writefds, &exceptfds, &timeout);
		if (true == isTimeOut(&sBasicTimer)) {
			//超时，没啥事可做
			continue;
			
		}
		for (i = 0; i < 64; i++) {
			if (sock_list.sock_array[i] == 0)
				continue;
			sock = sock_list.sock_array[i];
			if (FD_ISSET(sock, &readfds)) {
				for (i = 0; i < 8; i++) {
					buf[i] = 0; //正常情况没有必要，这里只是为了便于检查是否有正确的数据接收
				}
				len = sizeof(sockaddr_in);
				retval = recvfrom(sock, buf, MAX_BUFFER_SIZE, 0,(sockaddr*)&remote_addr,&len); //超过这个大小就不能愉快地玩耍了，因为缓冲不够大
				if (retval == 0) {
					closesocket(sock);
					printf("close a socket\n");
					delete_list(sock, &sock_list);
					continue;
				}
				else if (retval == -1) {
					retval = WSAGetLastError();
					if (retval == WSAEWOULDBLOCK || retval == WSAECONNRESET)
						continue;
					closesocket(sock);
					printf("close a socket\n");
					delete_list(sock, &sock_list);
					continue;
				}
				//收到数据后,通过源头判断是上层还是下层数据
				if (remote_addr.sin_port == upper_addr.sin_port) {
					//IP地址也应该比对的，偷个懒
					//是高层数据，从接口0发出去
					//先转换成bit数组
					for (i = 0; i < retval; i++) {
						//每次编码8位
						code(buf[i], sendbuf + i * 8, 8);
					}					//发送
					sendto(sock,sendbuf,retval*8,0,(sockaddr*)&(lower_addr[0]),sizeof(sockaddr_in));
					printf("\n收到上层数据 %d 位，发送到接口0\n", retval * 8);
				}
				else {
					if (remote_addr.sin_port == lower_addr[0].sin_port) {
						//接口0的数据，转发到接口1
						sendto(sock, buf, retval, 0, (sockaddr*)& (lower_addr[1]), sizeof(sockaddr_in));
						printf("\n收到接口0数据 %d 位，发送到接口1\n", retval);
					}else if(remote_addr.sin_port == lower_addr[1].sin_port){
						//接口1的数据，向上递交
						//先转换成字节数组，再向上递交
						for (i = 0; i < retval; i += 8) {
							sendbuf[i / 8] = (char)decode(buf + i, 8);
						}
						sendto(sock, sendbuf, retval/8, 0, (sockaddr*)& (upper_addr), sizeof(sockaddr_in));
						printf("\n收到接口1数据 %d 字节，递交给上层\n", retval / 8);
					}
				}
				//打印？
				printf("\n______________________________\n");
				for (i = 0; i < retval; i++) {
					linecount++;
					if (buf[i] == 0) {
						printf("0 ");
					}
					else {
						printf("1 ");
					}
					if (linecount > 40) {
						printf("\n");
						linecount = 0;
					}
				}
			}

		}
	}
	free(sendbuf);
	free(buf);
	closesocket(sock);
	WSACleanup();
	return 0;
}


// 运行程序: Ctrl + F5 或调试 >“开始执行(不调试)”菜单
// 调试程序: F5 或调试 >“开始调试”菜单

// 入门使用技巧: 
//   1. 使用解决方案资源管理器窗口添加/管理文件
//   2. 使用团队资源管理器窗口连接到源代码管理
//   3. 使用输出窗口查看生成输出和其他消息
//   4. 使用错误列表窗口查看错误
//   5. 转到“项目”>“添加新项”以创建新的代码文件，或转到“项目”>“添加现有项”以将现有代码文件添加到项目
//   6. 将来，若要再次打开此项目，请转到“文件”>“打开”>“项目”并选择 .sln 文件
