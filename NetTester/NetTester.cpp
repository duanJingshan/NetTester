// NetTester.cpp : 此文件包含 "main" 函数。程序执行将在此处开始并结束。
//

#include <iostream>
#include <conio.h>
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
void print_data_bit(char* A,int length,int iMode)
{
	int i,j;
	char B[8];
	int lineCount = 0;
	if(iMode == 0){
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
//返回值是转出来有多少位
int ByteArrayToBitArray(char* bitA, int iBitLen, char* byteA, int iByteLen)
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
//返回值是转出来有多少个字节，如果位流长度不是8位整数倍，则最后1字节不满
int BitArrayToByteArray(char* bitA, int iBitLen, char* byteA, int iByteLen)
{
	int i;
	int len;
	int retLen;

	len = min(iByteLen * 8, iBitLen );
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
		byteA[i / 8] = (char)decode(bitA + i, 8);
	}
	return retLen;
}

int main(int argc, char* argv[])
{
	SOCKET sock;
	struct sockaddr_in local_addr;
	struct sockaddr_in upper_addr; 
	struct sockaddr_in lower_addr[10];  //最多10个下层对象，数组下标是下层实体在编号
	int lowerMode[10];
	struct sockaddr_in remote_addr;
	int lowerNumber;
	int len;
	char* buf;
	char* sendbuf;
	WSAData wsa;
	int retval,iSndRetval;
	fd_set readfds;
	timeval timeout;
	int i;
	unsigned long arg;
	int linecount = 0;
	int port;
	string s1, s2, s3;
	int count = 0;
	int spin = 0;
	char* cpIpAddr;
	int iRecvIntfNo;
	int iWorkMode = 0;
	int autoSendTime = 10;
	int autoSendSize = 800;
	int iSndTotal = 0;
	int iSndTotalCount = 0;
	int iSndErrorCount = 0;
	int iRcvForward = 0;
	int iRcvForwardCount = 0;
	int iRcvToUpper = 0;
	int iRcvToUpperCount = 0;
	int iRcvUnknownCount = 0;
	//带外命令接口
	SOCKET iCmdSock = 0;
	sockaddr_in local_cmd_addr;

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
	if (iWorkMode / 10 == 1) {
		retval = cfgParms.getValueInt(&autoSendTime, CCfgFileParms::BASIC, (char*)"autoSendTime", 0);
		if (retval == -1) {
			autoSendTime = 10;
		}
		retval = cfgParms.getValueInt(&autoSendSize, CCfgFileParms::BASIC, (char*)"autoSendSize", 0);
		if (retval == -1) {
			autoSendSize = 800;
		}
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
		//低层接口是Byte或者是bit
		retval = cfgParms.getValueInt(&(lowerMode[i]), CCfgFileParms::LOWER, (char*)"lowerMode", i);
		if (0 > retval) {
			printf("参数错误\n");
			return 0;
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
			count++;
			switch (iWorkMode / 10) {
				//发送|打印：发送（0，等待键盘输入；1，自动）|打印（0，定期打印统计；1，每次收发打印细节）
			case 0:
				if (_kbhit()) {
					cout << endl << "从哪个接口发送？";
					cin >> port;
					cout << "输入字符串：";
					cin >> buf;
					retval = (int)strlen(buf) + 1;
					if(lowerMode[port] == 0){
						//upper 字节数组转下层位数组
						iSndRetval = ByteArrayToBitArray(sendbuf, MAX_BUFFER_SIZE, buf, retval);
						iSndRetval = sendto(sock, sendbuf, iSndRetval, 0, (sockaddr*)&(lower_addr[port]), sizeof(sockaddr_in));
					}else {
						//upper 字节数组直接发送
						iSndRetval = sendto(sock, buf, retval , 0, (sockaddr*) & (lower_addr[port]), sizeof(sockaddr_in));
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
			switch (iWorkMode % 10){
			case 0:
				//只定期打印统计数据
				if (count % 50 == 0) {
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
					printf("共转发 %d 位，%d 次;递交 %d 位，%d 次；发送 %d 位, %d 次 发送错误 %d 次,不明来源 %d 次__________________________", iRcvForward, iRcvForwardCount, iRcvToUpper, iRcvToUpperCount, iSndTotal, iSndTotalCount, iSndErrorCount,iRcvUnknownCount);
				}
				break;
			case 1:

				break;
			default:
				break; 
			}
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
				//是高层数据，从接口0发出去
				if(lowerMode[0] == 0){
					//先转换成bit数组
					iSndRetval = ByteArrayToBitArray(sendbuf, MAX_BUFFER_SIZE,buf, retval );
					//发送
					iSndRetval = sendto(sock,sendbuf, iSndRetval,0,(sockaddr*)&(lower_addr[0]),sizeof(sockaddr_in));
				}
				else {
					//下层是字节接口，直接发送
					iSndRetval = sendto(sock, buf, retval, 0, (sockaddr*) & (lower_addr[0]), sizeof(sockaddr_in));
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
					print_data_bit(buf, retval,1);
					break;
				case 0:
					break;
				}
			}
			else {
				//下层收到的数据
				if (remote_addr.sin_port == lower_addr[0].sin_port) {
					//接口0的数据，转发到接口1
					iRecvIntfNo = 0;
					if (lowerNumber > 1) {
						if (lowerMode[0] == lowerMode[1]) {
							iSndRetval = sendto(sock, buf, retval, 0, (sockaddr*) & (lower_addr[1]), sizeof(sockaddr_in));
							if (lowerMode[0] == 1) {
								iSndRetval = iSndRetval * 8;//换算成位
							}
						}
						else {
							if (lowerMode[0] == 1) {
								//byte to bit
								iSndRetval = ByteArrayToBitArray(sendbuf, MAX_BUFFER_SIZE, buf, retval);
								iSndRetval = sendto(sock, sendbuf, iSndRetval, 0, (sockaddr*) & (lower_addr[1]), sizeof(sockaddr_in));
							}
							else {
								//bit to byte
								iSndRetval = BitArrayToByteArray(buf, retval, sendbuf, MAX_BUFFER_SIZE);
								iSndRetval = sendto(sock, sendbuf, iSndRetval, 0, (sockaddr*) & (lower_addr[1]), sizeof(sockaddr_in));
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
					}
					else {
						//向上递交
						if (lowerMode[0] == 0) {
							//先转换成字节数组，再向上递交
							iSndRetval = BitArrayToByteArray(buf, retval, sendbuf, MAX_BUFFER_SIZE);
							iSndRetval = sendto(sock, sendbuf, iSndRetval, 0, (sockaddr*) & (upper_addr), sizeof(sockaddr_in));
							iSndRetval = iSndRetval * 8;//换算成位
						}
						else {
							iSndRetval = sendto(sock, buf, retval, 0, (sockaddr*) & (upper_addr), sizeof(sockaddr_in));
							iSndRetval = iSndRetval * 8;//换算成位
						}
						if (iSndRetval <= 0) {
							iSndErrorCount++;
						}
						else {
							iRcvToUpper += iSndRetval;
							iRcvToUpperCount++;
						}
					}

				}
				else if(remote_addr.sin_port == lower_addr[1].sin_port){
					//接口1的数据，向上递交
					iRecvIntfNo = 1;
					if (lowerMode[1] == 0) {
						//先转换成字节数组，再向上递交
						iSndRetval = BitArrayToByteArray(buf, retval, sendbuf, MAX_BUFFER_SIZE);
						iSndRetval = sendto(sock, sendbuf, iSndRetval, 0, (sockaddr*) & (upper_addr), sizeof(sockaddr_in));
						iSndRetval = iSndRetval * 8;//换算成位
					}
					else {
						iSndRetval = sendto(sock, buf, retval, 0, (sockaddr*) & (upper_addr), sizeof(sockaddr_in));
						iSndRetval = iSndRetval * 8;//换算成位
					}
					if (iSndRetval <= 0) {
						iSndErrorCount++;
					}
					else {
						iRcvToUpper += iSndRetval;
						iRcvToUpperCount++;
					}
					//printf("\n收到接口1数据 %d 字节，递交给上层\n", retval / 8);
				}
				else {
					//不明来源，打印提示
					iRcvUnknownCount++;
					switch (iWorkMode % 10) {
					case 1:
						//打印收发数据
						printf("\n不明来源数据 %d 次__________________________\n",iRcvUnknownCount);
						print_data_bit(buf, retval, 1);
						break;
					case 0:
						break;
					}
					continue;
				}
				//打印
				switch (iWorkMode % 10) {
				case 1:
					//打印收发数据
					printf("\n共发送: %d 位, %d 次,转发 %d 位，%d 次;递交 %d 位，%d 次，发送错误 %d 次__________________________\n", iSndTotal, iSndTotalCount, iRcvForward, iRcvForwardCount, iRcvToUpper, iRcvToUpperCount, iSndErrorCount);
					print_data_bit(buf, retval,lowerMode[iRecvIntfNo]);
					break;
				case 0:
					break;
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
