/*
 * ---------------------------------------------------------------
 * websocketd.cpp
 *     g++ websocketd.cpp -lssl -o websocketd
 *
 * http://tools.ietf.org/html/rfc6455
 * ---------------------------------------------------------------
 */
#include <iostream>
#include <iomanip>
#include <string>

#include <time.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h> 
#include <sys/time.h> /* selectシステムコール */
#include <sys/wait.h>
#include <unistd.h>

#include <netdb.h>
#include <sys/param.h>
#include <sys/uio.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <openssl/sha.h>
#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/buffer.h>

using namespace std;

static const int field_max=60;
static const int buf_len=256; /* バッファのサイズ */
static const int mid_len=512; /* バッファのサイズ */
static const int max_len=2048;
static const int unused=-1;

static const int port_no=8890;

//handyshake
/**
 ** HTTP/1.1 101 Switching Protocols
 ** Upgrade: websocket
 ** Connection: Upgrade
 ** Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=
 ** Sec-WebSocket-Protocol: chat
 **/
static const char *response_handyshake=
					"HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n";
static const char *response_notfound=
					"HTTP/1.1 404 Not Found\r\n";

static long transaction_number=0;
static time_t _date;

//ソケット
static int curr_sock; //接続を待ち受けるソケット
static int fork_sock; //クライアント毎にforkされるソケット

/*
 * ------------------------------------------------------------
 * プロトタイプ
 * ------------------------------------------------------------
 */
//helper
int split(char*,char*,char*);
void decode(char*,long);
string getParam(const string&, const string&);
string getProtocol(const string&);
string getSubstr(const string&, const string&, const string&);
string replaceStr(string*, const string&, const string&);
string trim(const string&);
string sha1(const string&);
string encode(const string&);
string decode(const string&);
int decodeReceiveData(const char*, int*, char*);
int encodeSendData(unsigned char*, const int, const char*, const int);

//ソケット
void init();
void initSocket();
void initSignal();
void closeSocket();
//プロセス
void startChild();
void killChild(int);
//通信用
void mainLoop();

/*
 * ------------------------------------------------------------
 * 実装
 * ------------------------------------------------------------
 */
int main(int argc, char *argv[])
{
	//初期化
	init();
	initSocket();
	initSignal();
	//メインループ
	mainLoop();
}

/*
 * ------------------------------------------------------
 * メインループ
 * ------------------------------------------------------
 */	
void mainLoop()
{
	struct sockaddr_in caddr;//クライアントのアドレス情報
	socklen_t len;
	int sresp;
	int width,pid;

	fd_set readfds;
	struct timeval timeout;

	//メインループ開始
	cout << ">> Begining \"Main Loop\" <<" << endl;

	//接続を受け付ける
	if((listen(curr_sock,SOMAXCONN))==-1){
		perror("listen");
		cout << ">> Exit by error : Cannot listen. <<" << endl;
		exit(-1);
	}
	cout << "curr_sock : listen" << endl;

	while(1){
		//fdsの初期化
		FD_ZERO(&readfds);
		//FD_SET(0,&readfds);//標準入力を監視
		FD_SET(curr_sock,&readfds);//ソケットを監視
		width=curr_sock+1;

		//タイムアウト時間(5秒間監視する)
		timeout.tv_sec=5;
		timeout.tv_usec=0;

		//メッセージの到着を調べる
		sresp=select(width,&readfds,NULL,NULL,&timeout);
		if(sresp && sresp!=-1){

			//curr_sockのビットが立っていればメッセージが到着している
			if(FD_ISSET(curr_sock,&readfds)){
				cout << "curr_sock : reading" << endl;

				//子プロセス用のソケットに接続
				len=sizeof(caddr);
				fork_sock=accept(curr_sock,(struct sockaddr *)&caddr,&len);
				if(fork_sock<0){
					if(errno==EINTR){ continue; }
					perror("accept");
					continue;
				}

				//子プロセスに分岐
				if((pid=fork())==0){
					//pid=0の場合は子プロセス(コピープロセス)：接続用ソケットは使わないので閉じてメインループ終了
					close(curr_sock);
					curr_sock=unused;
					//処理番号(グローバル変数)
					transaction_number++;
					startChild();

					cout << ">> Ending \"Main Loop\" <<" << endl;
					exit(0);
				}
				cout << "pid : " << pid << endl;

				//親プロセス内(オリジナル)：子プロセスID取得し出力、子プロセス用のソケットは使わないので閉じてループ継続
				setpgid(pid,getpid());
				close(fork_sock);
				fork_sock=unused;
			} //<--if(FD_ISSET())
		} //<--if(select())
	} //<--while(1)
}

/*
 * ------------------------------------------------------
 * ソケットを初期化
 * ------------------------------------------------------
 */
void init()
{
	curr_sock=unused;
	fork_sock=unused;
}

void initSocket()
{
	char hostname[buf_len];
	struct sockaddr_in saddr;
	int opt;

	cout << ">> Begining \"Initialize Socket\" <<" << endl;

	/*
	 * -------------------------------------------
	 * サービスポートを用意
	 * -------------------------------------------
	 */
	//自ホスト名を得る
	if(gethostname(hostname,buf_len)==-1){
		perror("gethostname");
		cout << ">> Exit by error : Don't get a hostname. <<" << endl;
		exit(-1);
	}
	cout << "hostname : " << hostname << endl;

	//待ちうけ用ソケットを作る
	if((curr_sock=socket(AF_INET,SOCK_STREAM,0))==-1){
		perror("socket");
		cout << ">> Exit by error : It's fail to make a socket for new connection. <<" << endl;
		exit(-1);
	}
	cout << "curr_sock : socket" << endl;

	//ソケットオプション
	opt=1;
	if(setsockopt(curr_sock,SOL_SOCKET,SO_REUSEADDR,&opt,sizeof(int))!=0){
		perror("setsockopt");
		cout << ">> Exit by error : Cannot set a socket options. <<" << endl;
		exit(-1);
	}
	cout << "curr_sock : setsockopt" << endl;

	//ゼロクリア(bind()でのエラー防止)
	memset((char*)&saddr,0,sizeof(saddr));

	//ソケット名
	saddr.sin_family=AF_INET;
	saddr.sin_addr.s_addr=INADDR_ANY;
	saddr.sin_port=htons(port_no);

	//bind
	if((bind(curr_sock,(struct sockaddr *)&saddr,sizeof(saddr)))==-1){
		perror("bind");
		cout << ">> Exit by error : Don't bind a socket. <<" << endl;
		exit(-1);
	}
	cout << "curr_sock : bind" << endl;
	cout << ">> Ending \"Initialize Socket\" <<" << endl;
}

/*
 * ------------------------------------------------------
 * ソケットを閉じる
 * ------------------------------------------------------
 */
void closeSocket()
{
	cout << ">> Begining \"Close Socket\" <<" << endl;

	if(curr_sock!=unused){
		close(curr_sock);
		cout << "curr_sock : close" << endl;
		curr_sock=unused;
	}
	if(fork_sock!=unused){
		close(fork_sock);
		cout << "fork_sock : close" << endl;
		fork_sock=unused;
	}

	cout << ">> Ending \"Close Socket\" <<" << endl;
	exit(0);
}

/*
 * ------------------------------------------------------
 * 子プロセスを殺す
 * ------------------------------------------------------
 */
void killChild(int sig)
{
	while(waitpid(-1,NULL,WNOHANG)>0);
	signal(SIGCHLD,killChild);
}

/*
 * ------------------------------------------------------
 * 割り込みを受け取るための準備
 * ------------------------------------------------------
 */
void initSignal()
{
	cout << ">> Begining \"Innitialize Signal\" <<" << endl;

	/*signal監視*/
	signal(SIGCHLD,killChild);

	cout << ">> Ending \"Innitialize Signal\" <<" << endl;
}

/*
 * --------------------------------------------------------------
 * 子プロセス内処理
 * --------------------------------------------------------------
 */	
void startChild()
{
	//for handshake
	int msglen;
	string request(max_len, 0); //クライアントからの受信文字列
	string response(max_len, 0); //サーバからの送信文字列
	string seckey;
	string protocol;
	
	//for payload
	char data[max_len];
	unsigned char senddata[max_len];
	char payload[max_len];
	int opcode;
	int senddata_len;
	
	string sec_websocket_protocol;
	string sec_websocket_accept;
	
	int isHandshake = 0;
	int i = 0;
	
	while(1){
		msglen = 0;
		memset(data,0,sizeof(data));
		msglen = read(fork_sock, data, max_len-1);
		if(msglen < 1){
			sleep(100);
		}else{
			request = data;
			if(!isHandshake){
				//ハンドシェイク
				cout << "<REQUEST>" << endl;
				cout << request << endl;

				//送信されたプロトコル名を取得
				protocol.assign(getProtocol(request));
			
				//送信されたキーを取得
				// Sec-WebSocket-Key: wh9PtKqHD/xPmaOlIAS7nQ==
				seckey.assign(getParam(request, "Sec-WebSocket-Key"));
				seckey.append("258EAFA5-E914-47DA-95CA-C5AB0DC85B11");
				seckey.assign(encode(sha1(seckey)));
				//キーの返却値を作る
				sec_websocket_accept.assign("Sec-WebSocket-Accept: ");
				sec_websocket_accept.append(seckey);
				sec_websocket_accept.append("\r\n");
				//cout << "<SECKEY>" << endl;
				//cout << sec_websocket_accept << endl;
				
				//プロトコルの返却値を作る
				sec_websocket_protocol.assign("Sec-WebSocket-Protocol: ");
				sec_websocket_protocol.append(protocol);
				sec_websocket_protocol.append("\r\n");
				//cout << "<PROTOCOL>" << endl;
				//cout << sec_websocket_protocol << endl;

				//レスポンスヘッダ
				response.assign(response_handyshake);
				response.append(sec_websocket_accept);
				//response.append(sec_websocket_protocol);
				response.append("\r\n");
				
				//debug print
				cout << "<RESPONSE>" << endl;
				cout << response << endl;
				
				isHandshake = write(fork_sock,&response[0],response.length());
				
				cout << "<SIZE>" << endl;
				cout << response.length() << endl;
				cout << "<WRITE>" << endl;
				cout << isHandshake << endl;

				isHandshake = 1;
			}
			else if(protocol.compare("chat") == 0) 
			{
				//受信データをデコード
				decodeReceiveData(data, &opcode, payload);

				cout << "00001:" << endl;
				memset(senddata,0,sizeof(senddata));
				cout << "00002:" << endl;
				switch(opcode){
					case 1: //text
						//受信したテキストにしるしをつけて送り返す
						cout << "00003:" << endl;
						i = strlen(payload);
						payload[i] = ':';
						payload[i+1] = 'O';
						payload[i+2] = 'K';
						payload[i+3] = 0;
						//クライアント向けにはマスクしない
						senddata_len = encodeSendData(senddata, opcode, payload, 0/*mask*/);
						break;
					default:
						break;
				}
				cout << "00004:" << endl;
				cout << "senddata_len: " << senddata_len << endl;
				
				for(i=0;i<senddata_len;i++){
					printf("%02x ", senddata[i]);
				}
				cout << "::00005:" << endl;

				write(fork_sock,senddata,senddata_len);
				//write(fork_sock,&response[0],response.length());
				
			} else {
				response.assign(response_notfound);
				write(fork_sock,&response[0],response.length());
			}
		}
	}
}

/*
 * ------------------------------------------------------
 * ":"区切りのパラメータを取得
 * ------------------------------------------------------
 */	
string getParam(const string& base, const string& key)
{
	string result;

	string::size_type searchStart;
	string::size_type searchEnd;
	
	//キーに":"をつける
	string keystr;
	keystr.append(key);
	keystr.append(":");

	return getSubstr(base, keystr, "\n");
}

/*
 * ------------------------------------------------------
 * プロトコルを取得
 * ------------------------------------------------------
 */
string getProtocol(const string& base)
{
	// GET /chat HTTP/1.1
	return getSubstr(base, "/", " ");
}

 /*
 * ------------------------------------------------------
 * 指定文字列で挟まれた文字列を切り出す
 * ------------------------------------------------------
 */	
string getSubstr(const string& base, const string& from_str, const string& to_str)
{
	string result;

	string::size_type searchStart;
	string::size_type searchEnd;
	
	//キーを検索
	searchStart = base.find(from_str);
	if(searchStart != string::npos){
		//値の先頭位置
		searchStart += from_str.length();
	}else{
		return 0;
	}

	//値の最後尾（改行まで）
	searchEnd = base.find(to_str, searchStart+from_str.length());
	if(searchEnd != string::npos){
		//値の先頭から最後尾までを切り取る
		result.assign(base, searchStart, searchEnd-searchStart);
	}else{
		return 0;
	}
	
	//余計な空白等を除去
	result.assign(trim(result));
	return result;
}

/*
 * ------------------------------------------------------
 * 前後の空白を取り除く
 * ------------------------------------------------------
 */
string trim(const string& str)
{
	const char* trimCharacterList = " \t\v\r\n";
	string result;
	 
	// 左側からトリムする文字以外が見つかる位置を検索
	std::string::size_type left = str.find_first_not_of(trimCharacterList);
	 
	if (left != std::string::npos)
	{
		// 左側からトリムする文字以外が見つかった場合は、同じように右側からも検索
		std::string::size_type right = str.find_last_not_of(trimCharacterList);
		// 戻り値を決定
		result = str.substr(left, right - left + 1);
	}
	return result;
}

/*
 * ------------------------------------------------------
 * SHA1(OpenSSL依存)
 * ------------------------------------------------------
 */
std::string sha1(const std::string &data)
{
  //用意
  SHA_CTX encoder;
  unsigned char result[SHA_DIGEST_LENGTH];

  //計算
  SHA1_Init(&encoder);
  SHA1_Update(&encoder, data.c_str(), data.length());
  SHA1_Final(result, &encoder);
  result[SHA_DIGEST_LENGTH] = 0x00;

  return static_cast<std::string>((char*)result);  
}

/*
 * ------------------------------------------------------
 * base64エンコード
 * ------------------------------------------------------
 */
std::string encode(const std::string& data)
{
  //フィルタ作成
  BIO *encoder = BIO_new(BIO_f_base64());
  BIO *bmem = BIO_new(BIO_s_mem());
  encoder = BIO_push(encoder,bmem);
  BIO_write(encoder,data.c_str(),data.length());
  BIO_flush(encoder);

  //結果をここに
  BUF_MEM *bptr;
  BIO_get_mem_ptr(encoder,&bptr);
  
  //charに移行処理
  char *buff = (char *)malloc(bptr->length);
  memcpy(buff, bptr->data, bptr->length-1);
  buff[bptr->length-1] = 0;
  
  //クリア
  BIO_free_all(encoder);

  return static_cast<std::string>(buff);
}

/*
 * ------------------------------------------------------
 * base64デコード
 * ------------------------------------------------------
 */
std::string decode(const std::string& data)
{
  //バッファ領域の確保。デコードデータはエンコードデータよりも小さい。
  const int length = data.length();
  char *buff = (char*)malloc(length);
  memset(buff,0x00,length);

  //フィルタ作成
  BIO *decoder = BIO_new(BIO_f_base64());
  BIO *bmem = BIO_new_mem_buf((char*)data.c_str(),length);
  bmem = BIO_push(decoder,bmem);
  BIO_read(bmem,buff,length);

  BIO_free_all(bmem);

  return static_cast<std::string>(buff);
}

/*
 * ------------------------------------------------------
 * 受信データをデコード
 * ------------------------------------------------------
 */	
int decodeReceiveData(const char *data,  int *opcode, char *payload)
{
	/*
	 * 受信データ
	 *
	 	  0                   1                   2                   3
	 	  0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
		 +-+-+-+-+-------+-+-------------+-------------------------------+
		 |F|R|R|R| opcode|M| Payload len |    Extended payload length    |
		 |I|S|S|S|  (4)  |A|     (7)     |             (16/64)           |
		 |N|V|V|V|       |S|             |   (if payload len==126/127)   |
		 | |1|2|3|       |K|             |                               |
		 +-+-+-+-+-------+-+-------------+ - - - - - - - - - - - - - - - +
		 |     Extended payload length continued, if payload len == 127  |
		 + - - - - - - - - - - - - - - - +-------------------------------+
		 |                               |Masking-key, if MASK set to 1  |
		 +-------------------------------+-------------------------------+
		 | Masking-key (continued)       |          Payload Data         |
		 +-------------------------------- - - - - - - - - - - - - - - - +
		 :                     Payload Data continued ...                :
		 + - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - +
		 |                     Payload Data continued ...                |
		 +---------------------------------------------------------------+
	 *
	 */
	int mask;
	char mask_key[4];
	int payload_len;
	int payload_data_len;
	int payload_offset;
	
	int i;
	
	//opcodeを取得
	*opcode = data[0]&0x0f;
	//mask
	mask = (data[1]&0x80)>>7;
	//payload length
	payload_len = data[1]&0x7f;
	
	if (payload_len == 126)
	{
		//maskingキー
		for (i=0;i<4;i++){
			mask_key[i] = data[i+4];
		}
		//payloadの開始位置
		payload_offset = 8;
		//データ長
		payload_data_len = data[2]<<8 | data[3];
	}
	else if (payload_len == 127)
	{
		//maskingキー
		for (i=0;i<4;i++){
			mask_key[i] = data[i+10];
		}
		//payloadの開始位置
		payload_offset = 14;
		//データ長
		payload_data_len = 0;
		//for(i=0;i<8;i++){ //上位32bitは今回は考えない（64bit変数じゃないと入らない！）
		for(i=0;i<4;i++){
			payload_data_len |= data[10-i]<<(i*8);
		}
	}
	else
	{
		//maskingキー
		for (i=0;i<4;i++){
			mask_key[i] = data[i+2];
		}
		//payloadの開始位置
		payload_offset = 6;
		//データ長
		payload_data_len = payload_len;
	}

	//payloadデータをセット
	for(i=0;i<payload_data_len;i++){
		payload[i] = data[i+payload_offset];
		if(mask==1){
			//mask=1の場合はマスクキーで復号
			payload[i] = payload[i] ^ mask_key[i%4];
		}
	}
	payload[i] = 0;
	
	cout << "<DECODE>" << endl;
	cout << "opcode:" << *opcode << endl;
	cout << "mask:" << mask << endl;
	cout << "payload_len:" << payload_len << endl;
	cout << "payload_data_len:" << payload_data_len << endl;
	cout << "payload_offset:" << payload_offset << endl;
	cout << "payload:" << payload << endl;
	
	return i;
}

/*
 * ------------------------------------------------------
 * 送信データをエンコード
 * ------------------------------------------------------
 */	
int encodeSendData(unsigned char *frame, const int opcode, const char *payload, const int mask)
{
	unsigned char *frameHeader = NULL;
	int frameHeaderSize;
	char mask_key[4];
	int payload_data_len;
	int i,j;
	
	cout << "00100:" << endl;
	payload_data_len = strlen(payload);
	
	if(payload_data_len > 65535){
		//フレームヘッダのサイズ
		/*
		 * [0]     fin|rsv1|rsv2|rsv3|opcode(4)
		 * [1]     mask|payload_len(7)
		 * [2-9]   extended payload length(8*8)
		 * [10-13] mask key(4*8)
		 */
		frameHeaderSize = 10;
	}else if(payload_data_len > 125){
		//フレームヘッダのサイズ
		/*
		 * [0]     fin|rsv1|rsv2|rsv3|opcode(4)
		 * [1]     mask|payload_len(7)
		 * [2-3]   extended payload length(8*2)
		 * [4-7]   mask key(4*8)
		 */
		frameHeaderSize = 4;
	}else{
		//フレームヘッダのサイズ
		/*
		 * [0]     fin|rsv1|rsv2|rsv3|opcode(4)
		 * [1]     mask|payload_len(7)
		 * [2-5]   mask key(4*8)
		 */
		frameHeaderSize = 2;
	}
	if(mask==1){ frameHeaderSize += 4; }
	
	//メモリ確保
	frameHeader = (unsigned char*)realloc(frameHeader, sizeof(unsigned char)*frameHeaderSize);
	
	//opcode
	switch(opcode){
		case 1:
			/*
			 * フレーム[0]
			 * 1000 0001(fin=1,opcode=1)
			 */
			frameHeader[0] = 0x81;
			break;
		default:
			frameHeader[0] = 0x80;
			break;
	}
	
	cout << "00101:" << endl;
	if(payload_data_len > 65535) //データ長が65535(16bit)以上の場合は64bitで表現
	{
		cout << "00102:" << endl;
		/*
		 * フレーム[1]
		 * 1111 1111(mask=1,payload_len=127)
		 * 0111 1111(mask=0,payload_len=127)
		 */
		frameHeader[1] = (mask<<7) + 127; 
		
		/*
		 * フレーム[2-9]
		 * %x0000000000000000-7FFFFFFFFFFFFFFF
		 */
		for(i=0;i<8;i++){ 
			if(i>3){frameHeader[9-i] = 0x00;} //32bit以上は今回は無視(int型だから) 
			frameHeader[9-i] = (payload_data_len&(0xff<<(i*8)))>>(i*8);
		}
	}
	else if(payload_data_len > 125) //データ長が125を超える場合は16bitで表現
	{
		cout << "00103:" << endl;
		/*
		 * フレーム[1]
		 * 1111 1110(mask=1,payload_len=126)
		 * 0111 1110(mask=0,payload_len=126)
		 */
		frameHeader[1] = (mask<<7) + 126; 

		/*
		 * フレーム[2-3]
		 * %x0000-FFFF
		 */
		for(i=0;i<2;i++){ 
			frameHeader[3-i] = (payload_data_len&(0xff<<(i*8)))>>(i*8);
		}
	}
	else //データ長が125以下の場合は拡張領域使わない
	{
		cout << "00104:" << endl;
		frameHeader[1] = (mask<<7) + payload_data_len;
	}

	cout << "00105:" << endl;
	//mask_key
	if(mask==1){
		for(i=0;i<4;i++){
			mask_key[i] = rand()%255; //0-255
			frameHeader[frameHeaderSize-4+i] = mask_key[i];
		}
	}
	
	
	cout << "00106:" << endl;
	//戻り値
	for(i=0;i<frameHeaderSize;i++){
		frame[i] = frameHeader[i];
	}
	for(i=0;i<payload_data_len;i++){
		j = i + frameHeaderSize;
		frame[j] = payload[i]&0xff; //1byte分だけコピー
		if(mask==1){ //mask=1の場合はマスキング
			frame[j] = frame[j]^mask_key[i%4]; 
		}
	}
	free(frameHeader);
	

	cout << "00107:" << endl;
	frame[frameHeaderSize+payload_data_len] = 0;
	
	cout << "<ENCODE>" << endl;
	cout << "opcode:" << opcode << endl;
	cout << "mask:" << mask << endl;
	cout << "payload_data_len:" << payload_data_len << endl;
	cout << "payload_offset:" << frameHeaderSize << endl;
	cout << "payload:" << payload << endl;
	
	return frameHeaderSize+payload_data_len;
}
/*
 * ------------------------------------------------------
 * 置換
 * ------------------------------------------------------
 */
string replaceStr(string *str, const string& from_str, const string& to_str) 
{
	string::size_type pos = str->find(from_str);

	while(pos != string::npos){
		str->replace(pos, from_str.size(), to_str);
		pos = str->find(from_str, pos + to_str.size());
	}
}

/*
 * ------------------------------------------------------
 * データを切り分け
 * ------------------------------------------------------
 */	
int split (char origin[], char *name[], char *value[])
{
	int i;
	int count;
	
	if (origin[0] == '\0') return -1;
	name[0] = origin;
	
	i = 1;
	count = 0;
	while(origin[i] != '\0') {
		if (origin[i] == '=') {
			origin[i] = '\0';
			value[count++] = &origin[++i];
		} else if (origin[i] == '&') {
			origin[i] = '\0';
			name[count] = &origin[++i];
		} else i++;
	}
	
	return count;
}

/*
 * ------------------------------------------------------
 * HTTPデコード
 * ------------------------------------------------------
 */	
void decode(char* s,long len)
{
	int i,j;
	char buf,*s1;

	s1=new char[len];

	for(i=0,j=0;i<len;i++,j++) {
		if(s[i]=='+'){s1[j]=' ';continue;}
		if(s[i]!='%') {s1[j]=s[i];continue;}
		buf=((s[++i]>='A') ? s[i]-'A'+10 : s[i]-'0');
		buf*=16;
		buf+=((s[++i]>='A') ? s[i]-'A'+10 : s[i]-'0');
		s1[j]=buf;
	}
	for(i=0;i<j;i++) s[i]=s1[i];
	s[i]='\0';

	delete[] s1;
}
