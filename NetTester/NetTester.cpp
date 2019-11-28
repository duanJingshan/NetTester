// NetTester.cpp : 此文件包含 "main" 函数。程序执行将在此处开始并结束。
//

#include <iostream>
#include <conio.h>
#include "winsock.h"
#include "stdio.h"
#include "CfgFileParms.h"
#pragma comment (lib,"wsock32.lib")

#define MAX_BUFFER_SIZE 40000 //缓冲的最大大小

typedef char U8; //定义单字节无符号整数
//start------不那么华丽的分割线，到end前的变量都是不那么重要，都是一些统计用的全局变量------------
int myCount = 0; //打印控制
int spin = 0;  //打印动态信息控制
int iWorkMode = 0; //本层实体工作模式
int iSndTotal = 0;  //发送数据总量
int iSndTotalCount = 0; //发送数据总次数
int iSndErrorCount = 0;  //发送错误次数
int iRcvForward = 0;     //转发数据总量
int iRcvForwardCount = 0; //转发数据总次数
int iRcvToUpper = 0;      //从低层递交高层数据总量
int iRcvToUpperCount = 0;  //从低层递交高层数据总次数
int iRcvUnknownCount = 0;  //收到不明来源数据总次数
//start------不那么华丽的分割线---------------------------------------------------------
int SendtoUpper(U8* buf, int len);
int SendtoLower(U8* buf, int len, int ifNo);
void print_data_bit(U8* A, int length, int iMode);
void code(unsigned long x, U8 A[], int length);
unsigned long decode(U8 A[], int length);
int ByteArrayToBitArray(U8* bitA, int iBitLen, U8* byteA, int iByteLen);
int BitArrayToByteArray(U8* bitA, int iBitLen, U8* byteA, int iByteLen);


//============重点来了================================================================================================
//------------重要的控制参数------------------------------------------------------------
//以下从配置文件ne.txt中读取的重要参数
int lowerMode[10]; //如果低层是物理层模拟软件，这个数组放置的是每个接口对应的数据格式——0为比特数组，1为字节数组
int lowerNumber;   //低层实体数量，比如网络层之下可能有多个链路层

//以下为重要的变量
U8* sendbuf;        //用来组织发送数据的缓存，大小为MAX_BUFFER_SIZE

//以下是一些重要的控制参数，但是也可以不用管
SOCKET sock;
struct sockaddr_in local_addr;      //本层实体地址
struct sockaddr_in upper_addr;      //上层实体地址，一般情况下，上层实体只有1个
struct sockaddr_in lower_addr[10];  //最多10个下层对象，数组下标就是下层实体的编号
SOCKET iCmdSock = 0;                //带外命令接口，用来接收一键启动和一键停止的命令
sockaddr_in local_cmd_addr;         //带外命令地址

//------------华丽的分割线，以下是定时器--------------------------------------------
//基于select的定时器，目的是把数据的收发和定时都统一到一个事件驱动框架下
//可以有多个定时器，本设计实现了一个基准定时器，为周期性10ms定时，也可以当作是一种心跳计时器
//其余的定时器可以在这个基础上完成，可行的方案存在多种
//看懂设计思路后，自行扩展以满足需要
//基准定时器一开启就会立即触发一次
struct threadTimer_t {
	int iType;  //为0表示周期性定时器，定时达到后，会自动启动下一次定时
	ULONG ulInterval;
	LARGE_INTEGER llStopTime;
}sBasicTimer;  //全局唯一的计时器，默认是每10毫秒触发一次，设计者可以在这个计时器的基础上实现自己的各种定时。
               //比如计数100次以后，就是1秒。针对不同的事件设置两个变量，一个用来表示是否开始计时，一个用来表示计数到多大
//*************************************************
//名称：StartTimerOnce
//功能：把全局计时器改为1次性,不建议这么做
//输入：计时间隔时间，单位微秒，计时的起始就是本函数调用时。
//输出：直接改变全局计时器变量 sBasicTimer内容
void StartTimerOnce(ULONG ulInterval)
{
	LARGE_INTEGER llFreq;

	sBasicTimer.iType = 1;
	sBasicTimer.ulInterval = ulInterval;
	QueryPerformanceFrequency(&llFreq);
	QueryPerformanceCounter(&sBasicTimer.llStopTime);
	sBasicTimer.llStopTime.QuadPart += llFreq.QuadPart * sBasicTimer.ulInterval / 1000000;
}
//*************************************************
//名称：StartTimerPeriodically
//功能：重设全局计时器的周期性触发间隔时间
//输入：计时间隔时间，单位微秒，计时的起始就是本函数调用时。
//输出：直接改变全局计时器变量 sBasicTimer内容
void StartTimerPeriodically(ULONG ulInterval)
{
	LARGE_INTEGER llFreq;

	sBasicTimer.iType = 0;
	sBasicTimer.ulInterval = ulInterval;
	QueryPerformanceFrequency(&llFreq);
	QueryPerformanceCounter(&sBasicTimer.llStopTime);
	sBasicTimer.llStopTime.QuadPart += llFreq.QuadPart * sBasicTimer.ulInterval / 1000000;
}
//***************重要函数提醒******************************
//名称：TimeOut
//功能：本函数被调用时，意味着sBasicTimer中设置的超时时间到了，函数内容可以全部替换为设计者自己的
//      函数内容全部可以替换
//输入：
//输出：
void TimeOut()
{
	int port;
	char buf[1000];
	int iSndRetval;
	int len;

	myCount++;

	switch (iWorkMode / 10) {
		//发送|打印：发送（0，等待键盘输入；1，自动）|打印（0，定期打印统计；1，每次收发打印细节）
	case 0:
		if (_kbhit()) {
			cout << endl << "从哪个接口发送？";
			cin >> port;
			cout << "输入字符串：";
			cin >> buf;
			len = (int)strlen(buf) + 1; //字符串最后有个结束符
			if (port >= lowerNumber) {
				cout << "没有这个接口" << endl;
				return;
			}
			if (lowerMode[port] == 0) {
				//upper 字节数组转下层位数组
				iSndRetval = ByteArrayToBitArray(sendbuf, MAX_BUFFER_SIZE, buf, len);
				iSndRetval = SendtoLower(sendbuf, iSndRetval, port);
				//iSndRetval = sendto(sock, sendbuf, iSndRetval, 0, (sockaddr*) & (lower_addr[port]), sizeof(sockaddr_in));
			}
			else {
				//upper 字节数组直接发送
				iSndRetval = SendtoLower(buf, len, port);
				//iSndRetval = sendto(sock, buf, retval, 0, (sockaddr*) & (lower_addr[port]), sizeof(sockaddr_in));
				iSndRetval = iSndRetval * 8; //换算成位
			}
			//发送
			if (iSndRetval > 0) {
				iSndTotalCount++;
				iSndTotal += iSndRetval;
			}
		}
		break;
	case 1:
		//超时，没事可做
		break;
	}
	switch (iWorkMode % 10) {
	case 0:
		//只定期打印统计数据
		if (myCount % 50 == 0) {
			spin++;
			switch (spin) {
			case 1:
				printf("\r-");
				break;
			case 2:
				printf("\r\\");
				break;
			case 3:
				printf("\r|");
				break;
			case 4:
				printf("\r/");
				spin = 0;
				break;
			}
			printf("共转发 %d 位，%d 次;递交 %d 位，%d 次；发送 %d 位, %d 次 发送错误 %d 次,不明来源 %d 次__________________________", iRcvForward, iRcvForwardCount, iRcvToUpper, iRcvToUpperCount, iSndTotal, iSndTotalCount, iSndErrorCount, iRcvUnknownCount);
		}
		break;
	case 1:

		break;
	default:
		break;
	}
}
//------------华丽的分割线，以下是数据的收发,--------------------------------------------

//***************重要函数提醒******************************
//名称：RecvfromUpper
//功能：本函数被调用时，意味着收到一份高层下发的数据
//输入：U8 * buf,高层传进来的数据， int len，数据长度，单位字节
//      函数内容全部可以替换
//输出：
void RecvfromUpper(U8 * buf, int len)
{
	int iSndRetval;

	//是高层数据，从接口0发出去
	if (lowerMode[0] == 0) {
		//接口0的模式为bit数组，先转换成bit数组
		iSndRetval = ByteArrayToBitArray(sendbuf, MAX_BUFFER_SIZE, buf, len);
		//发送
		iSndRetval = SendtoLower(sendbuf, iSndRetval,0);
		//sendto(sock, sendbuf, iSndRetval, 0, (sockaddr*) & (lower_addr[0]), sizeof(sockaddr_in));
	}
	else {
		//下层是字节数组接口，可直接发送
		iSndRetval = SendtoLower(buf, len, 0);
		//iSndRetval = sendto(sock, buf, retval, 0, (sockaddr*) & (lower_addr[0]), sizeof(sockaddr_in));
		iSndRetval = iSndRetval * 8;//换算成位
	}
	if (iSndRetval <= 0) {
		iSndErrorCount++;
	}
	else {
		iSndTotal += iSndRetval;
		iSndTotalCount++;
	}
	//printf("\n收到上层数据 %d 位，发送到接口0\n", retval * 8);
	switch (iWorkMode % 10) {
	case 1:
		//打印收发数据
		printf("\n共发送: %d 位, %d 次,转发 %d 位，%d 次;递交 %d 位，%d 次，发送错误 %d 次__________________________\n", iSndTotal, iSndTotalCount, iRcvForward, iRcvForwardCount, iRcvToUpper, iRcvToUpperCount, iSndErrorCount);
		print_data_bit(buf, len, 1);
		break;
	case 0:
		break;
	}
}
//***************重要函数提醒******************************
//名称：RecvfromLower
//功能：本函数被调用时，意味着得到一份从低层实体递交上来的数据
//输入：U8 * buf,低层递交上来的数据， int len，数据长度，单位字节，int ifNo ，低层实体号码，用来区分是哪个低层
//      函数内容全部可以替换
//输出：
void RecvfromLower(U8* buf, int len,int ifNo)
{
	int iSndRetval;
	if (ifNo == 0) {
		//从接口0收到的数据，直接转发到接口1 —— 仅仅用于测试
		if (lowerNumber > 1) {
			if (lowerMode[0] == lowerMode[1]) {
				//接口0和1的数据格式相同，直接转发
				iSndRetval = SendtoLower(buf, len, 1);
				if (lowerMode[0] == 1) {
					iSndRetval = iSndRetval * 8;//换算成位
				}
			}
			else {
				if (lowerMode[0] == 1) {
					//byte to bit
					iSndRetval = ByteArrayToBitArray(sendbuf, MAX_BUFFER_SIZE, buf, len);
					iSndRetval = SendtoLower(sendbuf, iSndRetval, 1);
				}
				else {
					//bit to byte
					iSndRetval = BitArrayToByteArray(buf, len, sendbuf, MAX_BUFFER_SIZE);
					iSndRetval = SendtoLower(sendbuf, iSndRetval, 1);
					//iSndRetval = sendto(sock, sendbuf, iSndRetval, 0, (sockaddr*) & (lower_addr[1]), sizeof(sockaddr_in));
					iSndRetval = iSndRetval * 8;//换算成位
				}
			}
			if (iSndRetval <= 0) {
				iSndErrorCount++;
			}
			else {
				iRcvForward += iSndRetval;
				iRcvForwardCount++;
			}
			goto printSnd;
		}
	}
	//非接口0的数据，或者只有接口0的数据都向上递交
	if (lowerMode[ifNo] == 0) {
		//如果接口0是比特数组格式，先转换成字节数组，再向上递交
		iSndRetval = BitArrayToByteArray(buf, len, sendbuf, MAX_BUFFER_SIZE);
		iSndRetval = SendtoUpper(sendbuf, iSndRetval);
		//iSndRetval = sendto(sock, sendbuf, iSndRetval, 0, (sockaddr*) & (upper_addr), sizeof(sockaddr_in));
		iSndRetval = iSndRetval * 8;//换算成位
	}
	else {
		//低层是字节数组接口，可直接递交
		iSndRetval = SendtoUpper(sendbuf, len);
		//iSndRetval = sendto(sock, buf, retval, 0, (sockaddr*) & (upper_addr), sizeof(sockaddr_in));
		iSndRetval = iSndRetval * 8;//换算成位
	}
	if (iSndRetval <= 0) {
		iSndErrorCount++;
	}
	else {
		iRcvToUpper += iSndRetval;
		iRcvToUpperCount++;
	}
	
printSnd:
	//打印
	switch (iWorkMode % 10) {
	case 1:
		//打印收发数据
		printf("\n共发送: %d 位, %d 次,转发 %d 位，%d 次;递交 %d 位，%d 次，发送错误 %d 次__________________________\n", iSndTotal, iSndTotalCount, iRcvForward, iRcvForwardCount, iRcvToUpper, iRcvToUpperCount, iSndErrorCount);
		print_data_bit(buf, len, lowerMode[ifNo]);
		break;
	case 0:
		break;
	}
}
//***************重要函数提醒******************************
//名称：SendtoUpper
//功能：向高层实体递交数据时，使用这个函数
//输入：U8 * buf,准备递交的数据， int len，数据长度，单位字节，int ifNo
//输出：函数返回值是发送的数据量
int SendtoUpper(U8* buf, int len)
{
	int sendlen;
	sendlen = sendto(sock, buf, len, 0, (sockaddr*) & (upper_addr), sizeof(sockaddr_in));
	return sendlen;
}
//***************重要函数提醒******************************
//名称：SendtoLower
//功能：向低层实体下发数据时，使用这个函数
//输入：U8 * buf,准备下发的数据， int len，数据长度，单位字节,int ifNo
//输出：函数返回值是发送的数据量
int SendtoLower(U8* buf, int len,int ifNo)
{
	int sendlen;
	sendlen = sendto(sock, buf, len, 0, (sockaddr*) & (lower_addr[ifNo]), sizeof(sockaddr_in));
	return sendlen;
}

//------------华丽的分割线，以下是一些数据处理的工具函数---------------------------------
//*************************************************
//名称：ByteArrayToBitArray
//功能：将字节数组流放大为比特数组流
//输入： int iBitLen——位流长度, U8* byteA——被放大字节数组, int iByteLen——字节数组长度
//输出：函数返回值是转出来有多少位；
//      U8* bitA,比特数组，注意比特数组的空间（声明）大小至少应是字节数组的8倍
int ByteArrayToBitArray(U8* bitA, int iBitLen, U8* byteA, int iByteLen)
{
	int i;
	int len;

	len = min(iByteLen, iBitLen / 8);
	for (i = 0; i < len; i++) {
		//每次编码8位
		code(byteA[i], &(bitA[i * 8]), 8);
	}
	return len * 8;
}
//*************************************************
//名称：BitArrayToByteArray
//功能：将字节数组流放大为比特数组流
//输入：U8* bitA,比特数组，int iBitLen——位流长度,  int iByteLen——字节数组长度
//      注意比特数组的空间（声明）大小至少应是字节数组的8倍
//输出：返回值是转出来有多少个字节，如果位流长度不是8位整数倍，则最后1字节不满；
//      U8* byteA——缩小后的字节数组,，
int BitArrayToByteArray(U8* bitA, int iBitLen, U8* byteA, int iByteLen)
{
	int i;
	int len;
	int retLen;

	len = min(iByteLen * 8, iBitLen);
	if (iBitLen > iByteLen * 8) {
		//截断转换
		retLen = iByteLen;
	}
	else {
		if (iBitLen % 8 != 0)
			retLen = iBitLen / 8 + 1;
		else
			retLen = iBitLen / 8;
	}

	for (i = 0; i < len; i += 8) {
		byteA[i / 8] = (U8)decode(bitA + i, 8);
	}
	return retLen;
}
//*************************************************
//名称：print_data_bit
//功能：打印比特数组内容
//输入：U8* A——比特数组, int length——位数, int iMode——打印格式，0和1两种
//输出：直接屏幕打印
void print_data_bit(U8* A, int length, int iMode)
{
	int i, j;
	U8 B[8];
	int lineCount = 0;
	if (iMode == 0) {
		for (i = 0; i < length; i++) {
			lineCount++;
			if (A[i] == 0) {
				printf("0 ");
			}
			else {
				printf("1 ");
			}
			if (lineCount % 8 == 0) {
				printf(" ");
			}
			if (lineCount >= 40) {
				printf("\n");
				lineCount = 0;
			}
		}
	}
	else {
		for (i = 0; i < length; i++) {
			lineCount++;
			code(A[i], B, 8);
			for (j = 0; j < 8; j++) {
				if (B[j] == 0) {
					printf("0 ");
				}
				else {
					printf("1 ");
				}
				lineCount++;
			}
			printf(" ");
			if (lineCount >= 40) {
				printf("\n");
				lineCount = 0;
			}
		}
	}
}
//end=========重要的就这些=====================================================================

//------------华丽的分割线，以下到main以前，都不用管了----------------------------
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
//名称：code
//功能：长整数x中的指定位数，放大到A[]这个比特数组中，建议按8的倍数做
//输入：x，被放大的整数，里面包含length长度的位数
//输出：A[],放大后的比特数组
void code(unsigned long x, U8 A[], int length)
{
	unsigned long test;
	int i;
	//高位在前
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
//名称：decode
//功能：把比特数组A[]里的各位（元素），缩小放回到一个整数中，长度是length位，建议按8的倍数做
//输入：比特数组A[],需要变化的位长
//输出：缩小后，还原的整数
unsigned long decode(U8 A[], int length)
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

//------------华丽的分割线，main来了-----------------
int main(int argc, char* argv[])
{
	U8 * buf;          //存放从高层、低层、各方面来的数据的缓存，大小为MAX_BUFFER_SIZE
	int len;           //buf里有效数据的大小，单位是字节
	int iRecvIntfNo;
	struct sockaddr_in remote_addr;
	WSAData wsa;
	int retval;
	fd_set readfds;
	timeval timeout;
	unsigned long arg;
	string s1, s2, s3;
	char* cpIpAddr;
	int port;
	int i;

	buf = (char*)malloc(MAX_BUFFER_SIZE);
	sendbuf = (char*)malloc(MAX_BUFFER_SIZE);
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
		s2 = "LNK";
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
	local_addr.sin_addr.S_un.S_addr = htonl(INADDR_ANY);
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
	retval = cfgParms.getValueInt(&iWorkMode, CCfgFileParms::BASIC, (char*)"workMode", 0);
	if (retval == -1) {
		iWorkMode = 0;
	}


	//读上层实体参数
	upper_addr.sin_family = AF_INET;
	cpIpAddr = cfgParms.getValueStr(CCfgFileParms::UPPER, (char*)"upperIPAddr", 0);
	if (cpIpAddr == NULL) {
		printf("参数错误\n");
		return 0;
	}
	upper_addr.sin_addr.S_un.S_addr = inet_addr(cpIpAddr);

	retval = cfgParms.getValueInt(&port, CCfgFileParms::UPPER, (char*)"upperPort", 0);
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
		//低层接口是Byte或者是bit,默认是bit
		retval = cfgParms.getValueInt(&(lowerMode[i]), CCfgFileParms::LOWER, (char*)"lowerMode", i);
		if (0 > retval) {
			lowerMode[i] = 0;
		}
	}
	retval = cfgParms.getValueInt(&port, CCfgFileParms::BASIC, (char*)"myCmdPort", 0);
	if (retval == -1) {
		//默认参数，不接受命令
		iCmdSock = 0;
	}
	else {
		iCmdSock = socket(AF_INET, SOCK_DGRAM, 0);
		if (iCmdSock == SOCKET_ERROR) {
			iCmdSock = 0;
		}
		else {
			local_cmd_addr.sin_family = AF_INET;
			local_cmd_addr.sin_addr.S_un.S_addr = htonl(INADDR_ANY);
			local_cmd_addr.sin_port = htons(port);
			if (bind(iCmdSock, (sockaddr*)& local_cmd_addr, sizeof(sockaddr_in)) == SOCKET_ERROR) {
				closesocket(iCmdSock);
				iCmdSock = 0;
			}
		}
	}

	//设置套接字为非阻塞态
	arg = 1;
	ioctlsocket(sock, FIONBIO, &arg);
	if (iCmdSock > 0) {
		arg = 1;
		ioctlsocket(iCmdSock, FIONBIO, &arg);
	}

	initTimer();
	while (1) {
		FD_ZERO(&readfds);
		//采用了基于select机制，不断发送测试数据，和接收测试数据，也可以采用多线程，一线专发送，一线专接收的方案
		//设定超时时间
		if (sock > 0) {
			FD_SET(sock, &readfds);
		}
		if (iCmdSock > 0) {
			FD_SET(iCmdSock, &readfds);
		}
		setSelectTimeOut(&timeout, &sBasicTimer);
		retval = select(0, &readfds, NULL, NULL, &timeout);
		if (true == isTimeOut(&sBasicTimer)) {

			TimeOut();

			continue;
		}
		if (FD_ISSET(sock, &readfds)) {
			for (i = 0; i < 8; i++) {
				buf[i] = 0; //正常情况没有必要，这里只是为了便于调试检查是否有正确的数据接收
			}
			len = sizeof(sockaddr_in);
			retval = recvfrom(sock, buf, MAX_BUFFER_SIZE, 0,(sockaddr*)&remote_addr,&len); //超过这个大小就不能愉快地玩耍了，因为缓冲不够大
			if (retval == 0) {
				closesocket(sock);
				sock = 0;
				printf("close a socket\n");
				continue;
			}
			else if (retval == -1) {
				retval = WSAGetLastError();
				if (retval == WSAEWOULDBLOCK || retval == WSAECONNRESET)
					continue;
				closesocket(sock);
				sock = 0;
				printf("close a socket\n");
				continue;
			}
			//收到数据后,通过源头判断是上层还是下层数据
			if (remote_addr.sin_port == upper_addr.sin_port) {
				//IP地址也应该比对的，偷个懒
				RecvfromUpper(buf, retval);
			}
			else {
				for (iRecvIntfNo = 0; i < lowerNumber; iRecvIntfNo++) {
					//下层收到的数据,检查是哪个接口的
					if (remote_addr.sin_port == lower_addr[iRecvIntfNo].sin_port) {
						RecvfromLower(buf, retval, iRecvIntfNo);
						break;
					}
				}
			}

		}
		if (iCmdSock == 0)
			continue;
		if (FD_ISSET(iCmdSock, &readfds)) {
			retval = recv(iCmdSock, buf, MAX_BUFFER_SIZE, 0);
			if (retval <= 0) {
				continue;
			}
			if (strncmp(buf, "exit", 5) == 0) { 
				//收到退出命令
				break;
			}
		}
	}
	free(sendbuf);
	free(buf);
	if (sock > 0)
		closesocket(sock);
	if (iCmdSock)
		closesocket(iCmdSock);
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
