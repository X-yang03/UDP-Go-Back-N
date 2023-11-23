#pragma once

#define _CRT_SECURE_NO_WARNINGS

#include <iostream>
#include <fstream>
#include <iomanip>
#include <sys/types.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include<WinSock2.h>
#include<Windows.h>
#include<WS2tcpip.h>
#include<time.h>
#include <chrono>
#include<list>
#include <conio.h>
#include <thread>
#include <filesystem>
#include <sys/types.h>
#include<queue>
#include <mutex>
#include <shared_mutex>
#include<string>

#pragma comment(lib,"ws2_32.lib")

const std::string serverIP = "127.0.0.1";
const std::string clientIP = "127.0.0.1";

#define intro "Type 'send' to send files, 'recv' to recieve files\n"

#define serverPort 8888
#define clientPort 8000
#define routerPort 8088

#define Fin 0b0001   // Finish  用于断开连接
#define Syn 0b0010
#define Ack 0b0100
#define Fds 0b1000    // File Discriptor 描述文件大小与名称

#define MSS 8178 //首部有14字节的各种信息,因此传输数据最大4082字节

#define wait_time 500  //超时等待00ms
#define MSL 2000

struct FileHead {
	std::string filename;
	int filelen;
};

struct fakeHead {   //伪首部
	u_long srcIP;
	u_long desIP;
	u_short protocol = 17; //协议号,UDP为17
	u_short length = 8192; //sizeof(Message)
};

struct Message {
	u_short srcPort;
	u_short desPort;
	u_short len;  //数据长度
	u_short flag = 0; //16位的flag
	u_short seq = 0;
	u_short ack = 0;
	u_short check;   //校验和
	char message[MSS] = { 0 };

	void set_srcPort(u_short src) {
		srcPort = src;
	}

	void set_desPort(u_short des) {
		desPort = des;
	}

	void set_len(u_short length) {
		len = length;
	}

	void set_seq(u_short s) {
		seq = s;
	}

	void set_ack(u_short a) {
		ack = a;
	}

	void set_SYN() { flag |= Syn; }

	void set_FIN() { flag |= Fin; }

	void set_ACK() { flag |= Ack; }

	void set_FDS() { flag |= Fds; }

	bool if_SYN() { return flag & Syn; }

	bool if_FIN() { return flag & Fin; }

	bool if_ACK() { return flag & Ack; }

	bool if_FDS() { return flag & Fds; }

	void set_check(struct fakeHead* head) {
		check = 0;
		u_int sum = 0;
		for (int i = 0; i < sizeof(struct fakeHead) / 2; i++) { //16位的累加
			sum += ((u_short*)head)[i];
		}

		for (int i = 0; i < sizeof(struct Message) / 2; i++) { //16位的累加
			sum += ((u_short*)this)[i];
		}

		while (sum >> 16) {  //加上进位
			sum = (sum & 0xffff) + (sum >> 16);
		}

		check = ~sum;
		//printf("seq = %d, ack = %d , set check: %d\n", seq,ack,check);
	}

	bool checkValid(struct fakeHead* head) {
		u_int sum = 0;

		for (int i = 0; i < sizeof(struct fakeHead) / 2; i++) { //16位的累加
			sum += ((u_short*)head)[i];
		}

		for (int i = 0; i < sizeof(struct Message) / 2; i++) { //16位的累加
			sum += ((u_short*)this)[i];
		}

		while (sum >> 16) {  //加上进位
			sum = (sum & 0xffff) + (sum >> 16);
		}
		//printf("seq = %d, ack = %d , check valid: %d\n", seq, ack, sum);

		return sum == 0x0ffff;
	}

	void set_data(char* src) {
		memset(message, 0, MSS);
		memcpy(message, src, len);
	}

};

typedef struct Message msg;

class _Client {
private:

public:
	_Client();
	int start_client();
	int cnt_setup();
	void sendFiles();
	int packupFile(std::filesystem::directory_entry);
	void packupFiles();
	void recvAcks();
	int resendWnd();
};

class _Server {

public:
	_Server();
	int server_init();
	int recvfile();
	int cnt_accept(int);
	int dic_accept(int);
	int file_accept(int);
};

#pragma once
