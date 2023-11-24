
//client.cpp实现发送端部分
#include "UDP programming.h"
#include <WinSock.h>
#include <IPHlpApi.h>

static std::fstream Client_log; //日志输出，保存为client_log.txt

std::deque<msg> Cache;		//数据缓冲区
std::deque<msg> CurrWnd;    //当前窗口
static int wndSize = 0;
SOCKET Client;
static struct fakeHead sendHead, recvHead;  //伪首部
static sockaddr_in server_addr;
static sockaddr_in client_addr;
static SYSTEMTIME sysTime = { 0 };
static int addrlen;
static u_short baseSeq = 0;   //窗口的起始seq,当前未被确认的最小seq
static u_short nextSeq = 0;		//窗口的末尾seq,指当前未被发送的最小seq
double total_time = 0;
double total_size = 0;

bool ifDone = 0;			//用于主线程与其他线程的同步
bool lossSet = 0;			//是否设置丢包率
int lossrate = 0;			//丢包率， 每lossrate个包发生一次丢包		
int delay = 0;

static std::string currentPath = "./test/";

std::shared_mutex cache_mutex;    //通过mutex类,防止线程之间冲突
std::shared_mutex wnd_mutex;
std::shared_mutex log_mutex;

int CachePush(msg message) {    //将对Cache队列的操作封装为线程安全的
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


//写入日志
static void logger(std::string str) {  
	printf("%s\n", str.c_str());
	GetSystemTime(&sysTime);
	std::string s = std::to_string(sysTime.wMinute) + " : " + std::to_string(sysTime.wSecond)+" : "+ std::to_string(sysTime.wMilliseconds) + "\n" + str+"\n";
	Client_log << s;
	
}

static void sendLog(msg m) {
	char info1[150];
	sprintf(info1, "[Log] Send Package %d, checkSum = %d,\t len = %d, baseSeq = %d, nextSeq = %d, wndSize = %d\n", m.seq, m.check,m.len, baseSeq, nextSeq, CurrWnd.size());
	std::string str = info1;
	logger(str);
}

static void recvLog(msg m) {
	char info[100];
	sprintf(info, "[Log] Recieve Ack %d,\t checkSum = %d ,\t wndSize = %d\n", m.ack, m.check,CurrWnd.size());
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
	server_addr.sin_port = htons(serverPort);
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
	
	//设置阻塞超时(超时重传)
	int timeout = wait_time;
	setsockopt(Client, SOL_SOCKET, SO_RCVTIMEO, (char *)&timeout, sizeof(timeout));  

	printf("set the loss rate(1-99): ");
	std::cin >> lossrate;
	if (lossrate <= 0 || lossrate > 100) {
		lossSet = false;
		std::string str("\nLoss rate out of range! No Loss Rate!\n");
		logger(str);
	}
	else
	{
		lossSet = true;
		lossrate = 100 / lossrate;
		std::string str = "\nMiss occurs every " + std::to_string(lossrate) + " packages\n";
		logger(str);
	}


	printf("set the delay(ms): ");
	std::cin >> delay;
	if (delay < 0) delay = 0;
	s = "\nEach package has a delay of " + std::to_string(delay) + " ms\n";
	logger(s);

	cnt_setup();  //建立握手

	//修改缓冲区大小(其实发送端缓冲区不重要, 接收端缓冲区大小才是关键)
	int buffer_size = wndSize * MSS;
	setsockopt(Client, SOL_SOCKET, SO_SNDBUF, (const char*)&buffer_size, sizeof(buffer_size));

	int nRecvBuf = wndSize * MSS;//设置接收缓冲区大小  否则会出现丢包

	setsockopt(Client, SOL_SOCKET, SO_RCVBUF, (const char*)&nRecvBuf, sizeof(int));

	auto begin = std::chrono::steady_clock::now();
	packupFiles(); //包装数据线程,将二进制数据打包放入缓冲区

	sendFiles();  //当窗口有空位时,滑动窗口,将数据从缓冲区移入窗口并发送
	recvAcks();		//接收线程
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

// 重新发送窗口内的数据包
int _Client::resendWnd() {  
	std::unique_lock lock(wnd_mutex);
	for (int i = 0; i < CurrWnd.size(); i++) {
		msg message = CurrWnd[i];
		sendto(Client, (char*)&message, sizeof(msg), 0, (struct sockaddr*)&server_addr, addrlen);
		std::string s = "[Rsd] Resend Package " + std::to_string(message.seq)+"\n";
		logger(s);
	}
	return 0;

}

//接收线程, 包括超时重传
void _Client::recvAcks() {
	std::thread recvacks([&]() {
		while (true) {
			msg recvBuff;
			int recvLen = recvfrom(Client, (char*)&recvBuff, sizeof(msg), 0, (struct sockaddr*)&server_addr, &addrlen);
			// if receive buffer is valid
			if (recvLen > 0) {
				if (recvBuff.checkValid(&recvHead) && recvBuff.ack>baseSeq) { //校验和正确且ack和seq能对应,此外的情况不做处理,进行忽略
					
					for (int i = baseSeq; i < recvBuff.ack; i++) {    
						//将已确认的分组弹出窗口(窗口向后滑动)
						wndPop();
					}
					recvLog(recvBuff);
					if (recvBuff.if_FIN() && recvBuff.if_ACK()) {
						//挥手信息
						std::string s("[FIN] Destroy the connection!");
						logger(s);
						ifDone = 1;
						return;
					}
					
					
					//更新baseSeq
					baseSeq = recvBuff.ack > baseSeq ? recvBuff.ack : baseSeq;
					
				}
			}
			else {
				//超时，将窗口内的分组进行重传
				SetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE), FOREGROUND_INTENSITY| FOREGROUND_RED);
				resendWnd();
				SetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE), FOREGROUND_INTENSITY| FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);
			}
		}
	});
	recvacks.detach();
}

//发送线程
void _Client::sendFiles() {   
	std::thread sendfiles([&]() {
		msg message;
		while (true) {
			if(CurrWnd.size() < wndSize) {  //当出现空位(窗口滑动)
				while (Cache.empty()) {

				}
				message = CachePop();    //取出缓冲区的数据
				message.set_seq(nextSeq);
				message.set_check(&sendHead);
				wndPush(message);		//加入窗口,并发送

				if (nextSeq %(lossrate+1)|| !lossSet) {  //没有发生丢包，或没有设置丢包率
					Sleep(delay);	//延迟用Sleep实现
					sendto(Client, (char*)&message, sizeof(msg), 0, (struct sockaddr*)&server_addr, addrlen);
					sendLog(message);
				}
				else {	//发生丢包
					std::string s = "[Miss] Miss package with seq " + std::to_string(message.seq) +"\n";
					logger(s);
				}
				nextSeq++;    //更新nextSeq
				if (message.if_FIN()) {
					return;
				}
			}
		}
	});
	sendfiles.detach();
}

void _Client::packupFiles() {    //打包线程
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
		CachePush(wave);     //最后还要打包一条FIN消息

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

	msg *fileDescriptor = new msg;   //文件头,描述长度和名称

	fileDescriptor->set_srcPort(clientPort);
	fileDescriptor->set_desPort(serverPort);
	fileDescriptor->set_len(sizeof(struct FileHead));
	fileDescriptor->set_ACK();
	fileDescriptor->set_FDS(); //FileDescriptor,表示这是一条描述性消息
	fileDescriptor->set_seq(baseSeq);
	fileDescriptor->set_data((char*)&descriptor);
	fileDescriptor->set_check(&sendHead);

	CachePush(*fileDescriptor);   //加入缓冲区
	delete fileDescriptor;

	std::string path = currentPath + filename;

	std::ifstream input(path, std::ios::binary);

	int segments = ceil((float)filelen / MSS);  //分为多个节发送
	msg *file_msg;
	for (int i = 0; i < segments; i++) {
		char buffer[MSS];
		int send_len = (i == segments - 1) ? filelen % MSS : MSS;
		input.read(buffer, send_len);
		
		file_msg = new msg;
		file_msg->set_srcPort(clientPort);
		file_msg->set_desPort(serverPort);
		file_msg->set_len(send_len);
		file_msg->set_ACK();
		file_msg->set_seq(baseSeq);
		file_msg->set_data(buffer);
		file_msg->set_check(&sendHead);
		CachePush(*file_msg);    //加入缓冲区
		delete file_msg;
		}

	return 0;
}

int _Client::cnt_setup() {
	msg *first_shake = new msg;
	first_shake->set_srcPort(clientPort);
	first_shake->set_desPort(serverPort);
	first_shake->set_len(0);
	first_shake->set_seq(baseSeq);
	first_shake->set_SYN();   //第一次握手,仅发出一条SYN消息
	first_shake->set_check(&sendHead);

	char info[50];
	sprintf(info, "[SYN] Seq=%d  len=%d", first_shake->seq, first_shake->len);
	std::string s = info;
	logger(s);

	baseSeq += 1;  //更新Seq
	nextSeq += 1;
	sendto(Client, (char*)first_shake, sizeof(msg), 0, (struct sockaddr*)&server_addr, addrlen);

	msg* recv_msg = new msg;
	while (true) {
		
		int result = recvfrom(Client, (char*)recv_msg, sizeof(msg), 0, (struct sockaddr*)&server_addr, &addrlen);
		if (result < 0) {         //超时
			std::string str("[Error] Time out , try to resend!");
			logger(str);

			sendto(Client, (char*)&first_shake, sizeof(msg), 0, (struct sockaddr*)&server_addr, addrlen);
		}
		else { //成功接收到回复
			recvLog(*recv_msg);
			if (recv_msg->checkValid(&recvHead) && recv_msg->ack == baseSeq) {  //校验和正确,且Seq与ack对应

				if (recv_msg->if_SYN()) {   //握手消息
					int size = recv_msg->message[0];   //获取窗口大小
					wndSize = size;
					sprintf(info,"Set wndSize: %d", size);
					s = info;
					logger(s);

				}
				delete recv_msg;
				delete first_shake;
				break;

			}
			else {
				sendto(Client, (char*)first_shake, sizeof(msg), 0, (struct sockaddr*)&server_addr, addrlen);  //超时重传
			}
		}

	}

	sprintf(info, "[Log] Connection Set Up！");
	s = info;
	logger(s);
	return 0;
}
