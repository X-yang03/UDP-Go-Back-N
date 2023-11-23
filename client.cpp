
//client.cpp实现发送端部分
#include "UDP programming.h"
#include <WinSock.h>
#include <IPHlpApi.h>

static std::fstream Client_log; //日志输出，保存为client_log.txt

std::deque<msg> Cache;
std::deque<msg> CurrWnd;    //当前窗口
static int wndSize = 0;
SOCKET Client;
static struct fakeHead sendHead, recvHead;
static sockaddr_in server_addr;
static sockaddr_in client_addr;
static SYSTEMTIME sysTime = { 0 };
static int addrlen;
static u_short baseSeq = 0;   //消息序列号
static u_short nextSeq = 0;
double total_time = 0;
double total_size = 0;
bool ifDone = 0;

static std::string currentPath = "./test/";

std::shared_mutex cache_mutex;
std::shared_mutex wnd_mutex;
std::shared_mutex log_mutex;

int CachePush(msg message) {
	std::unique_lock lock(cache_mutex);
	Cache.push_back(message);
	return 0;
}

msg CachePop() {
	std::unique_lock lock(cache_mutex);
	msg message = Cache.front();
	Cache.pop_front();
	return message;
}


int wndPush(msg message) {
	std::unique_lock lock(wnd_mutex);
	CurrWnd.push_back(message);
	return 0;
}

int wndPop() {
	std::unique_lock lock(wnd_mutex);
	CurrWnd.pop_front();
	return 0;
}

static void logger(std::string str) {
	printf("%s\n", str.c_str());
	GetSystemTime(&sysTime);
	std::string s = std::to_string(sysTime.wMinute) + " : " + std::to_string(sysTime.wSecond)+" : "+ std::to_string(sysTime.wMilliseconds) + "\n" + str+"\n";
	Client_log << s;
	
}

static void sendLog(msg m) {
	char info1[100];
	sprintf(info1, "[Log] Send Package %d, checkSum = %d,len = %d, baseSeq = %d, nextSeq = %d, wndSize = %d", m.seq, m.check,m.len, baseSeq, nextSeq, CurrWnd.size());
	std::string str = info1;
	logger(str);
}

static void recvLog(msg m) {
	char info[100];
	sprintf(info, "[Log] Recieve Ack %d, checkSum = %d , wndSize = %d", m.ack, m.check,CurrWnd.size());
	std::string str = info;
	logger(str);
}

_Client::_Client() {
	
}

int _Client::start_client() {
	Client_log.open("client_log.txt", std::ios::out);
	WSADATA wsaData;

	if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
		printf("[Error] Socket Error: %s (errno: %d)\n", strerror(errno), errno);
		return 1;
	}

	if ((Client = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) == INVALID_SOCKET) {
		printf("[Error] Socket Error: %s (errno: %d)\n", strerror(errno), errno);
		return 1;
	}


	GetSystemTime(&sysTime);
	std::string s("[Log] Client Socket Start Up!"); 
	logger(s);
	// 初始化地址和伪首部
	client_addr.sin_family = AF_INET;       //IPV4
	client_addr.sin_port = htons(clientPort);     //PORT:8888,htons将主机小端字节转换为网络的大端字节
	inet_pton(AF_INET, clientIP.c_str(), &client_addr.sin_addr.S_un.S_addr);

	server_addr.sin_family = AF_INET;       //IPV4
	server_addr.sin_port = htons(routerPort);
	inet_pton(AF_INET, serverIP.c_str(), &server_addr.sin_addr.S_un.S_addr);

	sendHead.srcIP = client_addr.sin_addr.S_un.S_addr;
	sendHead.desIP = server_addr.sin_addr.S_un.S_addr; //伪首部初始化

	recvHead.srcIP = server_addr.sin_addr.S_un.S_addr;
	recvHead.desIP = client_addr.sin_addr.S_un.S_addr;

	addrlen = sizeof(server_addr);

	if (bind(Client, (LPSOCKADDR)&client_addr, sizeof(client_addr)) == SOCKET_ERROR) { //将Client与client_addr绑定
		printf("bind Error: %s (errno: %d)\n", strerror(errno), errno);
		return 1;
	}

	s = "[Log] Connecting to server..........";
	logger(s);
	
	int timeout = wait_time;
	setsockopt(Client, SOL_SOCKET, SO_RCVTIMEO, (char *)&timeout, sizeof(timeout));

	cnt_setup();  //建立握手

	auto begin = std::chrono::steady_clock::now();
	packupFiles();

	sendFiles();
	recvAcks();
	while (!ifDone) {

	}
	auto after = std::chrono::steady_clock::now();
	total_time = std::chrono::duration<double>(after - begin).count();

	s = "Successfully sent all the files, Time used: "+std::to_string(total_time)+" s, Throughput :" + std::to_string(total_size / total_time)+" Bytes / s";
	logger(s);
	Client_log.close();
	closesocket(Client);
	WSACleanup();


}

int _Client::resendWnd() {
	std::unique_lock lock(wnd_mutex);
	for (int i = 0; i < CurrWnd.size(); i++) {
		msg message = CurrWnd[i];
		sendto(Client, (char*)&message, sizeof(msg), 0, (struct sockaddr*)&server_addr, addrlen);
		std::string s = "[Rsd] Resend Package " + std::to_string(message.seq);
		logger(s);
	}
	return 0;

}

void _Client::recvAcks() {
	std::thread recvacks([&]() {
		while (true) {
			msg recvBuff;
			int recvLen = recvfrom(Client, (char*)&recvBuff, sizeof(msg), 0, (struct sockaddr*)&server_addr, &addrlen);
			// if receive buffer is valid
			if (recvLen > 0) {
				if (recvBuff.checkValid(&recvHead) && recvBuff.ack>baseSeq) { //检验校验和
					recvLog(recvBuff);
					if (recvBuff.if_FIN() && recvBuff.if_ACK()) {
						//挥手信息
						std::string s("[FIN] Destroy the connection!");
						logger(s);
					}
					else {
						/*std::string s = "[Log] Package " + std::to_string(recvBuff.ack-1) + " sent successfully!";
						logger(s);*/
						// 应答确认
					}
					
					for (int i = baseSeq; i < recvBuff.ack; i++) {    
						//将已确认的分组弹出窗口(窗口向后滑动)
						wndPop();
					}
					if (recvBuff.if_FIN()) {
						ifDone = 1;
						return;
					}
					
					baseSeq = recvBuff.ack > baseSeq ? recvBuff.ack : baseSeq;
					
				}
				else {
					
				}
			}
			else {
				//超时，将窗口内的分组进行重传
				
				resendWnd();
			}
		}
	});
	recvacks.detach();
}

void _Client::sendFiles() {
	std::thread sendfiles([&]() {
		while (true) {
			if(CurrWnd.size() < wndSize) {
				while (Cache.empty()) {

				}
				msg message = CachePop();
				message.set_seq(nextSeq);
				message.set_check(&sendHead);
				wndPush(message);
				sendto(Client, (char*)&message, sizeof(msg), 0, (struct sockaddr*)&server_addr, addrlen);
				sendLog(message);
				nextSeq++;
				if (message.if_FIN()) {
					return;
				}
			}
		}
	});
	sendfiles.detach();
}

void _Client::packupFiles() {
	std::thread packupfiles([&]() {
		int seq = 0;
		for (const auto& entry : std::filesystem::directory_iterator(currentPath)) {  //获取所有文件entry
			packupFile(entry);
		}
		msg wave;
		wave.set_srcPort(clientPort);
		wave.set_desPort(serverPort);
		wave.set_len(0);
		wave.set_seq(baseSeq);
		wave.set_FIN();  //设置Fin位
		wave.set_check(&sendHead);
		CachePush(wave);

		Client_log << "[Log] Packed all files!" << std::endl;
		return;
	}
	);
	packupfiles.detach();
}

int _Client::packupFile(std::filesystem::directory_entry entry) {
	std::string filename = entry.path().filename().string();

	int filelen = entry.file_size(); //文件大小
	total_size += filelen;
	struct FileHead descriptor {
		filename,
			filelen
	};

	msg fileDescriptor;

	fileDescriptor.set_srcPort(clientPort);
	fileDescriptor.set_desPort(serverPort);
	fileDescriptor.set_len(sizeof(struct FileHead));
	fileDescriptor.set_ACK();
	fileDescriptor.set_FDS(); //FileDescriptor,表示这是一条描述性消息
	fileDescriptor.set_seq(baseSeq);
	fileDescriptor.set_data((char*)&descriptor);
	fileDescriptor.set_check(&sendHead);

	CachePush(fileDescriptor);

	std::string path = currentPath + filename;

	std::ifstream input(path, std::ios::binary);

	int segments = ceil((float)filelen / MSS);  //分为多个节发送

	for (int i = 0; i < segments; i++) {
		char buffer[MSS];
		int send_len = (i == segments - 1) ? filelen % MSS : MSS;
		input.read(buffer, send_len);
		msg file_msg;

		file_msg.set_srcPort(clientPort);
		file_msg.set_desPort(serverPort);
		file_msg.set_len(send_len);
		file_msg.set_ACK();
		file_msg.set_seq(baseSeq);
		file_msg.set_data(buffer);
		file_msg.set_check(&sendHead);
		CachePush(file_msg);
		}

	return 0;
}

int _Client::cnt_setup() {
	msg first_shake;
	first_shake.set_srcPort(clientPort);
	first_shake.set_desPort(serverPort);
	first_shake.set_len(0);
	first_shake.set_seq(baseSeq);
	first_shake.set_SYN();   //第一次握手,仅发出一条SYN消息
	first_shake.set_check(&sendHead);

	char info[50];
	sprintf(info, "[SYN] Seq=%d  len=%d", first_shake.seq, first_shake.len);
	std::string s = info;
	logger(s);

	baseSeq += 1;  //更新Seq
	nextSeq += 1;
	sendto(Client, (char*)&first_shake, sizeof(msg), 0, (struct sockaddr*)&server_addr, addrlen);

	while (true) {
		msg recv_msg;
		int result = recvfrom(Client, (char*)&recv_msg, sizeof(msg), 0, (struct sockaddr*)&server_addr, &addrlen);
		if (result < 0) {         //超时
			std::string str("[Error] Time out , try to resend!");
			logger(str);

			sendto(Client, (char*)&first_shake, sizeof(msg), 0, (struct sockaddr*)&server_addr, addrlen);
		}
		else { //成功接收到回复
			recvLog(recv_msg);
			if (recv_msg.checkValid(&recvHead) && recv_msg.ack == baseSeq) {  //校验和正确,且Seq与ack对应

				if (recv_msg.if_SYN()) {
					int size = recv_msg.message[0];
					wndSize = size;
				}
				break;

			}
			else {
				sendto(Client, (char*)&first_shake, sizeof(msg), 0, (struct sockaddr*)&server_addr, addrlen);
			}
		}

	}

	sprintf(info, "[Log] Connection Set Up！");
	s = info;
	logger(s);
	return 0;
}
