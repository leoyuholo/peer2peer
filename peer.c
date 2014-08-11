#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>	// "struct sockaddr_in"
#include <arpa/inet.h>	// "in_addr_t"
#include <pthread.h>

#include <sys/stat.h>
#include <sys/times.h>
#include <fcntl.h>

#define GETINFO_PERIOD 30
#define CHUNK_SIZE 262144	//256 * 1024
#define MAX_DL_PTHREAD 30

//#define DEBUG
#define MESSAGE
//#define PROGRESS
#define IMMEPEERLIST
#define TIMER
static clock_t c_start, end;
static struct tms buf1,buf2;
#ifdef TIMER
	
#endif

char* peer_ip;
char* file_path;
unsigned short listen_portno = 9000;	//possible buggy if stop and resume come quickly, portno changed but listen_pthread not yet exit
//external
struct sockaddr_in* peerlist = NULL;	//need peerlist_and_bitmap_peer_mutex protect
char** bitmap_peer = NULL;				//need peerlist_and_bitmap_peer_mutex protect
unsigned int numOfPeer = 0;				//need peerlist_and_bitmap_peer_mutex protect
pthread_mutex_t peerlist_and_bitmap_peer_mutex = PTHREAD_MUTEX_INITIALIZER;
//internal
char* bitmap = NULL;					//need bitmap_mutex protect
pthread_mutex_t bitmap_mutex = PTHREAD_MUTEX_INITIALIZER;
unsigned int* internal_chunkmap = NULL;	//need internal_chunkmap_mutex protect, 0 = not yet, 1 = done, 2 = downloading
pthread_mutex_t internal_chunkmap_mutex = PTHREAD_MUTEX_INITIALIZER;

typedef struct download_t{
	struct sockaddr_in peer_addr;
	unsigned int chunkNum;
} download_struct;

typedef struct accept_t{
	struct sockaddr_in accept_addr;
	int accept_sockfd;
} accept_struct;

typedef struct downloadPeerLinkedList_t{
	download_struct download_conn;
	struct downloadPeerLinkedList_t* next;
} downloadPeerLinkedList;

int numOfDownloadPeer = 0;				//need dlPeerLL_mutex protect
downloadPeerLinkedList dlPeerLL;		//need dlPeerLL_mutex protect
pthread_mutex_t dlPeerLL_mutex = PTHREAD_MUTEX_INITIALIZER;

pthread_mutex_t file_mutex = PTHREAD_MUTEX_INITIALIZER;	//protect file read/write

int file_fd = 0;

unsigned int numOfChunk = 0;			//only can be modified in the beginning of do_add or do_seed, no need to be protected
unsigned int bitmap_size = 0;			//only can be modified in the beginning of do_add or do_seed, no need to be protected

unsigned int torrent_id;				//not yet htonl
struct in_addr tracker_ip;
unsigned short tracker_port;
unsigned int fname_length;
char* fname;
unsigned int fsize;

int stop = 0;
int upload = 1;
int download_finished = 0;
int listen_started = 0;
int done_test_by_tracker = 0;
int first_getInfo = 0;
int peerlist_and_bitmap_peer_updated = 0;
int dl_pthread_started = 0;

double print_progress(int mode){
//print progress of download, progress = (number of 1 in bitmap / number of chunk)

#ifdef DEBUG
	//printf("enter print_progress\n");
#endif
	
	double progress;
	
	char* bitmap_tmp;
	
	bitmap_tmp = (char*) malloc(sizeof(char) * bitmap_size);
	
	pthread_mutex_lock(&bitmap_mutex);
	
	{
		memcpy(bitmap_tmp, bitmap, bitmap_size);
	}
	
	pthread_mutex_unlock(&bitmap_mutex);
	
	int i, j;
	int cnt = 0;
	for(i = 0;i < bitmap_size;i++){
		//printf("[%X]", bitmap_tmp[i]);
		for(j = 7;j >= 0; j--){
			if(((bitmap_tmp[i] >> j) & 0x01) == 0x01){
				cnt++;
			}
		}
		//printf("cnt[%d]\n", cnt);
	}
	
	progress = ((double) cnt / numOfChunk) * 100;
	if(mode != 0){
		printf("progress: %.2f%\n", progress);
		
		if(stop){
			printf("condition: stopped\n");
		}else{
			printf("condition: in progress\n");
		}
	}
	return progress;
}

void print_peerlist(){
//print out the peerlist
	
	int i;
	
	printf("torrent id[%X]\n", ntohl(torrent_id));
#ifdef IMMEPEERLIST	
	struct sockaddr_in* peerlist_tmp;
	int numOfPeer_tmp;
	
	numOfPeer_tmp = do_getpeerlist(&peerlist_tmp);
	
	for(i = 0;i < numOfPeer_tmp;i++){
			printf("ip = %s\t:%i\n", inet_ntoa(peerlist_tmp[i].sin_addr), ntohs(peerlist_tmp[i].sin_port));
		}
		
	free(peerlist_tmp);
#else
	pthread_mutex_lock(&peerlist_and_bitmap_peer_mutex);
	
	{
		if(numOfPeer == 0){
			printf("Empty\n");
		}
		for(i = 0;i < numOfPeer;i++){
			printf("ip = %s\t:%i\n", inet_ntoa(peerlist[i].sin_addr), ntohs(peerlist[i].sin_port));
		}
	}
	
	pthread_mutex_unlock(&peerlist_and_bitmap_peer_mutex);
#endif
	return;
}

void do_accept(accept_struct* accept_conn){
//upload pthread

	pthread_detach(pthread_self());
	
	int accept_sockfd = accept_conn->accept_sockfd;
	
	unsigned int arg_len;
	char* cmd_pos;
	char command[100];
	unsigned int cmd_cnt;
	char receive[100];
	int recv_cnt;

	recv_cnt = read(accept_sockfd, &receive, 2);
#ifdef DEBUG
	printf("do_listen recv_cnt[%d]cmd[%X]\n", recv_cnt, receive[0]);
#endif
	
	if(receive[0] == 0x02){
		//peer test response from tracker
#ifdef DEBUG
		printf("do_listen peer test from tracker response\n");
#endif
		
		command[0] = 0x12;
		command[1] = 0x00;
		write(accept_sockfd, command, 2);
		
#ifdef DEBUG
		printf("do_listen sent[%X][%X]\n", command[0], command[1]);
#endif
		
		close(accept_sockfd);
		
		done_test_by_tracker = 1;
		
	}
	
	if(receive[0] == 0x05){
		//bitmap request from peer response
#ifdef DEBUG
		printf("do_listen bitmap request from peer response\n");
#endif
		if(stop){
			
			command[0] = 0x25;
			command[1] = 0x00;
			write(accept_sockfd, command, 2);
			
#ifdef DEBUG
			printf("do_listen sent[%X][%X]", command[0], command[1]);
#endif
			
			close(accept_sockfd);
		}else{
			recv_cnt = read(accept_sockfd, receive, 4);
			recv_cnt = read(accept_sockfd, receive, 4);

			if(stop || !upload){
				command[0] = 0x25;
				command[1] = 0x00;
				write(accept_sockfd, command, 2);
				
#ifdef DEBUG
				printf("do_listen sent[%X][%X]\n", command[0], command[1]);
#endif
				
				close(accept_sockfd);
			}else{
				char* cmd = (char*) malloc(sizeof(char) * (100 + bitmap_size));
				cmd[0] = 0x15;
				cmd[1] = 0x01;
				cmd_pos = cmd + 2;
				arg_len = htonl(bitmap_size);
				memcpy(cmd_pos, &arg_len, 4); cmd_pos += 4;
				
				pthread_mutex_lock(&bitmap_mutex);
				
				{
					memcpy(cmd_pos, bitmap, bitmap_size);
				}
				
				pthread_mutex_unlock(&bitmap_mutex);
				
				cmd_pos += bitmap_size;
				
				cmd_cnt = cmd_pos - cmd;
				write(accept_sockfd, cmd, cmd_cnt);
				
#ifdef DEBUG
//debug purpose
				printf("do_listen sent bitmap[%X][%X][%d]", cmd[0], cmd[1], bitmap_size);
				int i;
				for(i = 0;i < bitmap_size;i++){
					printf("[%X]", cmd[6+i]);
				}
				printf("\n");
//end debug purpose
#endif

				close(accept_sockfd);
			}
		}
	}
	
	if(receive[0] == 0x06){
		//chunk request from peer response
		//presistent?non-presistent?
#ifdef DEBUG
		printf("do_listen chunk request from peer response\n");
#endif
		
		if(stop || !upload){
				
				command[0] = 0x26;
				command[1] = 0x00;
				write(accept_sockfd, command, 2);
				
#ifdef DEBUG
				printf("do_listen sent[%X][%X]", command[0], command[1]);
#endif
				
				close(accept_sockfd);
		}else{
			recv_cnt = read(accept_sockfd, receive, 4);
			recv_cnt = read(accept_sockfd, receive, 4);//torrent id
			
#ifdef DEBUG
			printf("accepted torrent id[%X}\n", ntohl(*(unsigned int*)receive));
#endif

			recv_cnt = read(accept_sockfd, receive, 4);
			recv_cnt = read(accept_sockfd, receive, 4);//offset
			long offset = ntohl(*(unsigned int*)receive);
			
			int have_chunk = 0;
			
			pthread_mutex_lock(&bitmap_mutex);
			
			{
				if(((bitmap[(offset/CHUNK_SIZE)/8] >> ((offset/CHUNK_SIZE)%8)) & 0x01) == 0x01){
					have_chunk = 1;
				}else{
					perror("no chunk");
				}
			}
			
			pthread_mutex_unlock(&bitmap_mutex);
			
			unsigned int read_size;
			char buf[CHUNK_SIZE];
			
			if((fsize - offset) < (CHUNK_SIZE)){
				read_size = fsize - offset;
			}else{
				read_size = CHUNK_SIZE;
			}
			
			if(read_size > 0 && have_chunk == 1){
				//possible buggy zone: separated write
				command[0] = 0x16;
				command[1] = 0x01;
				cmd_pos = command + 2;
				arg_len = htonl(read_size);
				memcpy(cmd_pos, &arg_len, 4); cmd_pos += 4;
				cmd_cnt = cmd_pos - command;
				write(accept_sockfd, command, cmd_cnt);
				
				pthread_mutex_lock(&file_mutex);
				
				{
					lseek(file_fd, offset, SEEK_SET);
					read(file_fd, buf, read_size);
				}
				
				pthread_mutex_unlock(&file_mutex);
				
				write(accept_sockfd, buf, read_size);
				
#ifdef MESSAGE
				printf("chunkNum[%5d] sent to ip[%s]port[%d]\n", offset / CHUNK_SIZE, inet_ntoa(accept_conn->accept_addr.sin_addr), ntohs(accept_conn->accept_addr.sin_port));
				//printf("do_listen sent data[%X][%X]chunk[%d]offset[%d]size[%d]\n", command[0], command[1], offset / CHUNK_SIZE, offset, read_size);
#endif
				
				close(accept_sockfd);
			}else{
				
				command[0] = 0x26;
				command[1] = 0x00;
				write(accept_sockfd, command, 2);
				
#ifdef DEBUG
				printf("do_listen sent[%X][%X]", command[0], command[1]);
#endif
				
				close(accept_sockfd);
			}
		}
	}
	
	free(accept_conn);
	
	pthread_exit(NULL);
	return;
}

void do_listen(unsigned short portno){
//all time alive listen pthread
	
#ifdef DEBUG
	printf("enter do_listen, port:[%d]\n", portno);
#endif
	
	listen_portno = portno;
	
	int my_sockfd, accept_sockfd;
	struct sockaddr_in my_addr, accept_addr;
	int sockaddrlen = sizeof(struct sockaddr_in);
	
	if((my_sockfd = socket(AF_INET, SOCK_STREAM, 0)) < 0){
		perror("do_listen Error socket()");
		return;
	}
	
	my_addr.sin_family = AF_INET;
	my_addr.sin_addr.s_addr = htonl(INADDR_ANY);
	my_addr.sin_port = htons(portno);
	
	if(bind(my_sockfd, (struct sockaddr*) &my_addr, sockaddrlen) < 0){
		perror("do_listen Error bind()");
		return;
	}
	
	if(listen(my_sockfd, 1024) < 0){
		perror("do_listen Error listen()");
		return;
	}
	
	listen_started = 1;
	pthread_t* accept_pthread;
	accept_struct* accept_conn;
	
	while(1){//require modification replace 1 with stop condition indicator
		accept_sockfd = accept(my_sockfd, (struct sockaddr*) &accept_addr, &sockaddrlen);
		if(accept_sockfd < 0){
			perror("do_listen Error accept()");
			return;
		}else{
#ifdef DEBUG
			printf("do_listen accepted[%s][%d]\n", inet_ntoa(accept_addr.sin_addr), ntohs(accept_addr.sin_port));
#endif
		}
		
		accept_conn = (accept_struct*) malloc(sizeof(accept_struct));
		
		accept_conn->accept_addr = accept_addr;
		accept_conn->accept_sockfd = accept_sockfd;
		
		accept_pthread = (pthread_t*) malloc(sizeof(pthread_t));
		
		pthread_create(accept_pthread, NULL, do_accept, accept_conn);
		
	}
	
#ifdef DEBUG
	printf("listen_pthread exit\n");
#endif
	pthread_exit(NULL);
	return;
	
}

int do_reg_peer(){
//register peer to tracker
	
#ifdef DEBUG
	printf("enter do_reg_peer\n");
#endif
	
	int tracker_sockfd;
	struct sockaddr_in tracker_addr;
	int sockaddrlen = sizeof(struct sockaddr_in);
	
	unsigned int arg_len;
	char* cmd_pos;
	char command[100];
	unsigned int cmd_cnt;
	char receive[100];
	int recv_cnt;
	
	if((tracker_sockfd = socket(AF_INET, SOCK_STREAM, 0)) < 0){
		perror("do_reg_peer Error socket()");
		return -1;
	}
	
	tracker_addr.sin_family = AF_INET;
	tracker_addr.sin_addr = tracker_ip;
	tracker_addr.sin_port = htons(tracker_port);
	
	struct in_addr myIP;
	inet_aton(peer_ip, &myIP);
	unsigned short myPort = htons(listen_portno);	
	
	if(connect(tracker_sockfd, (struct sockaddr*) &tracker_addr, sockaddrlen) < 0){
		perror("do_reg_peer Error connect()");
		close(tracker_sockfd);
		return -1;
	}else{
#ifdef DEBUG
		printf("do_reg_peer connected\n");
#endif
	}
	
	//construct message
	command[0] = 0x01;
	command[1] = 0x03;
	cmd_pos = command + 2;
	//write IP
	arg_len = htonl(4);
	memcpy(cmd_pos, &arg_len, 4); cmd_pos += 4;
	memcpy(cmd_pos, &myIP, 4); cmd_pos += 4;
	//write port no
	arg_len = htonl(2);
	memcpy(cmd_pos, &arg_len, 4); cmd_pos += 4;
	memcpy(cmd_pos, &myPort, 2); cmd_pos += 2;
	//write torrent id
	arg_len = htonl(4);
	memcpy(cmd_pos, &arg_len, 4); cmd_pos += 4;
	memcpy(cmd_pos, &torrent_id, 4); cmd_pos += 4;
	
	cmd_cnt = cmd_pos - command;
	
	write(tracker_sockfd, command, cmd_cnt);
	
	recv_cnt = read(tracker_sockfd, receive, 2);
	
	if(receive[0] == 0x11){
		//register peer success
#ifdef DEBUG
		printf("register peer to tracker success\n");
#endif
		close(tracker_sockfd);
		return 1;
	}
	
	if(receive[0] == 0x21){
		//register peer fail
		printf("register peer to tracker fail\n");
		close(tracker_sockfd);
		return -1;
	}
	
	printf("crazy tracker!!!\n");
	close(tracker_sockfd);
	
	return -1;
}

int do_ureg_peer(){
//unregister peer from tracker
	
#ifdef DEBUG
	printf("enter do_ureg_peer\n");
#endif
	
	int tracker_sockfd;
	struct sockaddr_in tracker_addr;
	int sockaddrlen = sizeof(struct sockaddr_in);
	
	unsigned int arg_len;
	char* cmd_pos;
	char command[100];
	unsigned int cmd_cnt;
	char receive[100];
	int recv_cnt;
	
	if((tracker_sockfd = socket(AF_INET, SOCK_STREAM, 0)) < 0){
		perror("do_ureg_peer Error socket()");
		return -1;
	}
	
	tracker_addr.sin_family = AF_INET;
	tracker_addr.sin_addr = tracker_ip;
	tracker_addr.sin_port = htons(tracker_port);
	
	struct in_addr myIP;
	inet_aton(peer_ip, &myIP);//require modification replace peer_ip to host ip
	unsigned short myPort = htons(listen_portno);	
	
	if(connect(tracker_sockfd, (struct sockaddr*) &tracker_addr, sockaddrlen) < 0){
		perror("do_ureg_peer Error connect()");
		close(tracker_sockfd);
		return -1;
	}else{
#ifdef DEBUG
		printf("do_ureg_peer connected\n");
#endif
	}
	
	//construct message
	command[0] = 0x03;
	command[1] = 0x03;
	cmd_pos = command + 2;
	//write IP
	arg_len = htonl(4);
	memcpy(cmd_pos, &arg_len, 4); cmd_pos += 4;
	memcpy(cmd_pos, &myIP, 4); cmd_pos += 4;
	//write port no
	arg_len = htonl(2);
	memcpy(cmd_pos, &arg_len, 4); cmd_pos += 4;
	memcpy(cmd_pos, &myPort, 2); cmd_pos += 2;
	//write torrent id
	arg_len = htonl(4);
	memcpy(cmd_pos, &arg_len, 4); cmd_pos += 4;
	memcpy(cmd_pos, &torrent_id, 4); cmd_pos += 4;
	
	cmd_cnt = cmd_pos - command;
	
	write(tracker_sockfd, command, cmd_cnt);
	
	recv_cnt = read(tracker_sockfd, receive, 2);
	
	if(receive[0] == 0x13){
		//register peer success
#ifdef DEBUG
		printf("unregister peer from tracker success\n");
#endif
		close(tracker_sockfd);
		return 1;
	}
	
	if(receive[0] == 0x23){
		//register peer fail
		printf("unregister peer from tracker fail\n");
		close(tracker_sockfd);
		return -1;
	}
	
	printf("crazy tracker!!!\n");
	close(tracker_sockfd);
	return -1;
}

int do_getpeerlist(struct sockaddr_in** peerlist_tmp){
//get peerlist from tracker
	
#ifdef DEBUG
	printf("enter do_getpeerlist\n");
#endif
	
	int tracker_sockfd;
	struct sockaddr_in tracker_addr;
	int sockaddrlen = sizeof(struct sockaddr_in);
	
	unsigned int arg_len;
	char* cmd_pos;
	char command[100];
	unsigned int cmd_cnt;
	char receive[100];
	int recv_cnt;
	
	if((tracker_sockfd = socket(AF_INET, SOCK_STREAM, 0)) < 0){
		perror("do_getpeerlist Error socket()");
		return 0;
	}
	
	tracker_addr.sin_family = AF_INET;
	tracker_addr.sin_addr = tracker_ip;
	tracker_addr.sin_port = htons(tracker_port);
	
	if(connect(tracker_sockfd, (struct sockaddr*) &tracker_addr, sockaddrlen) < 0){
		perror("do_getpeerlist Error connect()");
		close(tracker_sockfd);
		return 0;
	}else{
#ifdef DEBUG
		printf("do_getpeerlist connected\n");
#endif
	}
	
	
	//construct message
	command[0] = 0x04;
	command[1] = 0x01;
	cmd_pos = command + 2;
	//write torrent id
	arg_len = htonl(4);
	memcpy(cmd_pos, &arg_len, 4); cmd_pos += 4;
	memcpy(cmd_pos, &torrent_id, 4); cmd_pos += 4;
	
	cmd_cnt = cmd_pos - command;
	
	write(tracker_sockfd, command, cmd_cnt);
	
	recv_cnt = read(tracker_sockfd, receive, 2);
	
	if(receive[0] == 0x14){
		//get peerlist success
#ifdef DEBUG
		printf("get peerlist from tracker success[%X][%X]\n", receive[0], receive[1]);
#endif
		
		unsigned int numOfArg = (unsigned int) receive[1];
		int numOfPeer_tmp = (int) numOfArg;
		
		*peerlist_tmp = (struct sockaddr_in*) malloc(sizeof(struct sockaddr_in) * numOfPeer_tmp);
		
#ifdef DEBUG
		printf("numOfArg[%d]\n", numOfArg);
#endif
		int i;
		for(i = 0;i < numOfPeer_tmp;i++){
			recv_cnt = read(tracker_sockfd, receive, 4 + 6);
//			printf("[%X][%X][%X][%X][%X][%X]\n", receive[4], receive[5], receive[6], receive[7], receive[8], receive[9]);
			(*peerlist_tmp)[i].sin_family = AF_INET;
			memcpy(&(*peerlist_tmp)[i].sin_addr, receive + 4, 4);
			memcpy(&(*peerlist_tmp)[i].sin_port, receive + 4 + 4, 2);
#ifdef DEBUG
			printf("ip:[%s]port:[%d]\n", inet_ntoa((*peerlist_tmp)[i].sin_addr), ntohs((*peerlist_tmp)[i].sin_port));
#endif
		}
		
		close(tracker_sockfd);
		
#ifdef DEBUG
		printf("done get peerlist\n");
#endif
		
		return numOfPeer_tmp;
	}
	
	printf("crazy tracker!!!\n");
	close(tracker_sockfd);
	
	return 0;
}

char* do_getBitmap(struct sockaddr_in peer_addr){
//get bitmap from peer
	
#ifdef DEBUG
	printf("enter do_getBitmap\n");
#endif
	
	int peer_sockfd;
	int sockaddrlen = sizeof(struct sockaddr_in);
	
	unsigned int arg_len;
	char* cmd_pos;
	char command[100];
	unsigned int cmd_cnt;
	char receive[100];
	int recv_cnt;
		
	char* tmp;
	int i;
	
	if((peer_sockfd = socket(AF_INET, SOCK_STREAM, 0)) < 0){
		perror("do_getBitmap Error socket()");
		return NULL;
	}
	
	if(connect(peer_sockfd, (struct sockaddr*) &peer_addr, sockaddrlen) < 0){
		perror("do_getBitmap Error connect()");
		close(peer_sockfd);
		return NULL;
	}else{
#ifdef DEBUG
		printf("do_getBitmap connected\n");
#endif
	}
	//construct message
	command[0] = 0x05;
	command[1] = 0x01;
	cmd_pos = command + 2;
	//write torrent id
	arg_len = htonl(4);
	memcpy(cmd_pos, &arg_len, 4); cmd_pos += 4;
	memcpy(cmd_pos, &torrent_id, 4); cmd_pos += 4;
	
	cmd_cnt = cmd_pos - command;
	
	write(peer_sockfd, command, cmd_cnt);
	
	recv_cnt = read(peer_sockfd, receive, 2);
	
	if(receive[0] == 0x15){
		//get bitmap success
#ifdef DEBUG
		printf("get bitmap ok\n");
#endif
		
		recv_cnt = read(peer_sockfd, receive, 4);
		if(!(ntohl(*(unsigned int*)receive) == bitmap_size)){
			printf("peer return incorrect bitmap_size\n");
			return NULL;
		}
		
		tmp = (char*) malloc(sizeof(char) * bitmap_size);
		
		recv_cnt = read(peer_sockfd, tmp, bitmap_size);
#ifdef DEBUG
		
		printf("getbitmap:");
		for(i = 0;i < bitmap_size;i++){
			printf("[%X]", tmp[i]);
		}
		printf("\n");
		
#endif
		close(peer_sockfd);
#ifdef DEBUG
		printf("get bitmap success, tmp[%p]\n", tmp);
#endif
		return tmp;
	}
	
	if(receive[0] == 0x25){
		//get bitmap fail
		printf("get bitmap fail\n");
		close(peer_sockfd);
		return NULL;
	}
	
	printf("crazy peer\n");
	close(peer_sockfd);
	return NULL;
}

void do_getInfo(int period){
//getInfo pthread, get peerlist from tracker, get bitmap from peers
	
#ifdef DEBUG
	printf("enter do_getInfo\n");
#endif
	
	int i;
	
	struct sockaddr_in* peerlist_tmp = (struct sockaddr_in*) malloc(sizeof(struct sockaddr_in));
	char** bitmap_tmp;
	unsigned int numOfPeer_tmp = 0;
	
	struct sockaddr_in* peerlist_tmp2;
	char** bitmap_tmp2;
	unsigned int numOfPeer_last_round = 0;
	
	while(1){
		
		if(!stop){
			
#ifdef DEBUG
			printf("getting info...\n");
#endif
			
			numOfPeer_tmp = do_getpeerlist(&peerlist_tmp);
			if(numOfPeer_tmp == 0){
#ifdef DEBUG
				printf("tracker fault, retry...\n");
#endif
				sleep(10);
				continue;
			}
			
			if(!download_finished){
				
#ifdef DEBUG
				printf("update peerlist and bitmap begin...\n");
#endif
				
				bitmap_tmp = (char**) malloc(sizeof(char*) * numOfPeer_tmp);
				
				for(i = 0;i < numOfPeer_tmp;i++){

					//if it is myself, set NULL to bitmap_tmp[i]				
					if(!strcmp(inet_ntoa(peerlist_tmp[i].sin_addr), peer_ip) && listen_portno == ntohs(peerlist_tmp[i].sin_port)){
						bitmap_tmp[i] = NULL;
					}else{
						bitmap_tmp[i] = do_getBitmap(peerlist_tmp[i]);
					}
					
				}
				
				pthread_mutex_lock(&peerlist_and_bitmap_peer_mutex);
				
				{
					numOfPeer_last_round = numOfPeer;
					numOfPeer = numOfPeer_tmp;
					peerlist_tmp2 = peerlist;
					peerlist = peerlist_tmp;
					bitmap_tmp2 = bitmap_peer;
					bitmap_peer = bitmap_tmp;
				}
				
				pthread_mutex_unlock(&peerlist_and_bitmap_peer_mutex);
				
				free(peerlist_tmp2);
				
				for(i = 0;i < numOfPeer_last_round;i++){
					free(bitmap_tmp2[i]);
				}
				free(bitmap_tmp2);
				
#ifdef DEBUG
				printf("committed peerlist and bitmap\n");
#endif
				
			}else{
				
				pthread_mutex_lock(&peerlist_and_bitmap_peer_mutex);
				
				{
					numOfPeer = numOfPeer_tmp;
					peerlist_tmp2 = peerlist;
					peerlist = peerlist_tmp;
				}
				
				pthread_mutex_unlock(&peerlist_and_bitmap_peer_mutex);
				
				free(peerlist_tmp2);
				
#ifdef DEBUG
				printf("committed peerlist\n");
#endif
				
			}
			
			peerlist_and_bitmap_peer_updated = 1;
			first_getInfo = 1;
			
#ifdef DEBUG
			print_peerlist();
#endif
			
#ifdef MESSAGE
			printf("Going to have a nice 30sec sleep, 4430S:we want it too...\n");
#endif
			sleep(period);
		}
	}
	
#ifdef DEBUG
	printf("getInfo_pthread exit\n");
#endif
	pthread_exit(NULL);
	return;
}

void do_download(download_struct* download_conn){
//TO DO: reduce lines of code at the end of this function
//download pthread, free(download_conn), unlock all mutex before you call pthread exit
	
#ifdef DEBUG
	printf("enter do_download[%s][%d][%d]\n", inet_ntoa(download_conn->peer_addr.sin_addr), ntohs(download_conn->peer_addr.sin_port), download_conn->chunkNum);
#endif
	
	pthread_detach(pthread_self());
	
	downloadPeerLinkedList myNode;
	myNode.download_conn = *download_conn;
	myNode.next = NULL;
	
	//register this download pthread to tail of dlPeerLL
	int i;
	downloadPeerLinkedList* tmp;
	
	pthread_mutex_lock(&dlPeerLL_mutex);
	
	{
		tmp = &dlPeerLL;
		for(i = 0;i < numOfDownloadPeer;i++){
			tmp = tmp->next;
		}
		tmp->next = &myNode;
		numOfDownloadPeer++;
	}
	
	pthread_mutex_unlock(&dlPeerLL_mutex);
	
	pthread_mutex_lock(&internal_chunkmap_mutex);
	
	{
		internal_chunkmap[download_conn->chunkNum] = 2;
	}
	
	pthread_mutex_unlock(&internal_chunkmap_mutex);
	
	dl_pthread_started = 1;//message to main pthread: I start to download now
	
	char data[CHUNK_SIZE];
	unsigned int file_offset = download_conn->chunkNum * (CHUNK_SIZE);
	unsigned int data_size;
	unsigned int data_cnt = 0;
	
	if(download_conn->chunkNum == (numOfChunk - 1)){
		data_size = fsize % CHUNK_SIZE;
	}else{
		data_size = CHUNK_SIZE;
	}
	
	int peer_sockfd;
	int sockaddrlen = sizeof(struct sockaddr_in);
	
	unsigned int arg_len;
	char* cmd_pos;
	char command[100];
	unsigned int cmd_cnt;
	char receive[1448];
	int recv_cnt;
	
	if((peer_sockfd = socket(AF_INET, SOCK_STREAM, 0)) < 0){
		perror("do_download Error socket()");
		//unregister this download pthread from dlPeerLL
		pthread_mutex_lock(&dlPeerLL_mutex);
		
		{
			tmp = &dlPeerLL;
			while(tmp->next != &myNode){
				tmp = tmp->next;
			}
			tmp->next = myNode.next;
			numOfDownloadPeer--;
		}
		
		pthread_mutex_unlock(&dlPeerLL_mutex);
			
		pthread_mutex_lock(&internal_chunkmap_mutex);
		
		{
			internal_chunkmap[download_conn->chunkNum] = 0;
		}
		
		pthread_mutex_unlock(&internal_chunkmap_mutex);
			
		free(download_conn);
		pthread_exit(NULL);
	}
	
	if(connect(peer_sockfd, (struct sockaddr*) &(download_conn->peer_addr), sockaddrlen) < 0){
		perror("do_download Error connect()");
		close(peer_sockfd);
		//unregister this download pthread from dlPeerLL
		pthread_mutex_lock(&dlPeerLL_mutex);
		
		{
			tmp = &dlPeerLL;
			while(tmp->next != &myNode){
				tmp = tmp->next;
			}
			tmp->next = myNode.next;
			numOfDownloadPeer--;
		}
		
		pthread_mutex_unlock(&dlPeerLL_mutex);
			
		pthread_mutex_lock(&internal_chunkmap_mutex);
		
		{
			internal_chunkmap[download_conn->chunkNum] = 0;
		}
		
		pthread_mutex_unlock(&internal_chunkmap_mutex);
			
		free(download_conn);
		pthread_exit(NULL);
	}else{
#ifdef DEBUG
		printf("do_download connected\n");
#endif
	}
	
	
	//construct message
	command[0] = 0x06;
	command[1] = 0x02;
	cmd_pos = command + 2;
	//write torrent id
	arg_len = htonl(4);
	memcpy(cmd_pos, &arg_len, 4); cmd_pos += 4;
	memcpy(cmd_pos, &torrent_id, 4); cmd_pos += 4;
	//write offset
	arg_len = htonl(4);
	memcpy(cmd_pos, &arg_len, 4); cmd_pos += 4;
	unsigned int file_offset_tmp = htonl(file_offset);
	memcpy(cmd_pos, &file_offset_tmp, 4); cmd_pos += 4;
	
	cmd_cnt = cmd_pos - command;
	
	write(peer_sockfd, command, cmd_cnt);
	
	recv_cnt = read(peer_sockfd, receive, 2);
	
	if(receive[0] == 0x16){
		//get chunk ok
#ifdef DEBUG
		printf("get chunk ok\n");
#endif
		recv_cnt = read(peer_sockfd, receive, 4);
		
		//get data from peer for a chunk
		while(data_cnt < data_size){
			recv_cnt = read(peer_sockfd, receive, 1448);	//1448 is better!?
			if(recv_cnt < 0){
				break;
			}
			memcpy(data + data_cnt, receive, recv_cnt);
			data_cnt += recv_cnt;
		}
#ifdef DEBUG
		printf("data_cnt[%d]\n", data_cnt);
		
		printf("done get data, wait to flush...\n");
#endif
		
		int write_cnt;
		
		//possible buggy zone, mutex lock...
		pthread_mutex_lock(&file_mutex);
		pthread_mutex_lock(&bitmap_mutex);
		pthread_mutex_lock(&internal_chunkmap_mutex);
		
		{
			lseek(file_fd, file_offset, SEEK_SET);
			if(write(file_fd, data, data_cnt) < 0){
				perror("writing fault, cannot do write(), maybe lack of hdd space");
			}
/*
			if(upload){
				if(fsync(file_fd) < 0){
					perror("writing fault, cannot do fsync()");
				}
			}
*/
			internal_chunkmap[download_conn->chunkNum] = 1;
			bitmap[download_conn->chunkNum / 8] = bitmap[download_conn->chunkNum / 8] | (0x01 << (download_conn->chunkNum % 8));
		}
		
		pthread_mutex_unlock(&file_mutex);
		pthread_mutex_unlock(&bitmap_mutex);
		pthread_mutex_unlock(&internal_chunkmap_mutex);

#ifdef MESSAGE		
		printf("Finish download chunkNum[%5d]from ip[%s]port[%d]\n", download_conn->chunkNum, inet_ntoa(download_conn->peer_addr.sin_addr), ntohs(download_conn->peer_addr.sin_port));
#endif
		
		close(peer_sockfd);	//change it if turn to persistent connection
		
		//unregister this download pthread from dlPeerLL
		pthread_mutex_lock(&dlPeerLL_mutex);
		
		{
			tmp = &dlPeerLL;
			while(tmp->next != &myNode){
				tmp = tmp->next;
			}
			tmp->next = myNode.next;
			numOfDownloadPeer--;
		}
		
		pthread_mutex_unlock(&dlPeerLL_mutex);
		
#ifdef DEBUG
		printf("ready to quit[%s][%d]\n", inet_ntoa(download_conn->peer_addr.sin_addr), ntohs(download_conn->peer_addr.sin_port));
#endif
		free(download_conn);
		
#ifdef DEBUG
		printf("do_download pthread exit\n");
#endif
		pthread_exit(NULL);
	}
	
	if(receive[0] == 0x26){
		//get chunk fail
		printf("get chunk fail\n");
		close(peer_sockfd);
		
		//unregister this download pthread from dlPeerLL
		pthread_mutex_lock(&dlPeerLL_mutex);
		
		{
			tmp = &dlPeerLL;
			while(tmp->next != &myNode){
				tmp = tmp->next;
			}
			tmp->next = myNode.next;
			numOfDownloadPeer--;
		}
		
		pthread_mutex_unlock(&dlPeerLL_mutex);
		
		pthread_mutex_lock(&internal_chunkmap_mutex);
		
		{
			internal_chunkmap[download_conn->chunkNum] = 0;
		}
		
		pthread_mutex_unlock(&internal_chunkmap_mutex);
		
		free(download_conn);
		
#ifdef DEBUG
		printf("do_download pthread exit\n");
#endif
		pthread_exit(NULL);
	}
	
	printf("crazy peer\n");
	close(peer_sockfd);
	
	//unregister this download pthread from dlPeerLL
	pthread_mutex_lock(&dlPeerLL_mutex);
	
	{
		tmp = &dlPeerLL;
		while(tmp->next != &myNode){
			tmp = tmp->next;
		}
		tmp->next = myNode.next;
		numOfDownloadPeer--;
	}
	
	pthread_mutex_unlock(&dlPeerLL_mutex);
		
	pthread_mutex_lock(&internal_chunkmap_mutex);
	
	{
		internal_chunkmap[download_conn->chunkNum] = 0;
	}
	
	pthread_mutex_unlock(&internal_chunkmap_mutex);
		
	free(download_conn);
	
#ifdef DEBUG
	printf("do_download pthread exit\n");
#endif
	pthread_exit(NULL);
	return;
}

void do_getFile(){
//download file from peers
//may free NULL pointer in this function

#ifdef DEBUG
	printf("enter do_getFile\n");
#endif

	struct sockaddr_in* peerlist_tmp = NULL;
	char** bitmap_peer_tmp = NULL;
	unsigned int numOfPeer_tmp = 0;
	
	struct sockaddr_in* peerlist_tmp2 = NULL;
	char** bitmap_peer_tmp2 = NULL;
	unsigned int numOfPeer_last_round = 0;
	
	unsigned int* rarest = (unsigned int*) malloc(sizeof(unsigned int) * numOfChunk);
	unsigned int* internal_chunkmap_tmp = (unsigned int*) malloc(sizeof(unsigned int) * numOfChunk);
	
	int i, j, k, bitlen;
	double progress, progress_previous;
	
	while(!first_getInfo);
	
	while(1){
		//possible buggy zone, race condition with getInfo pthread
		
		if(!stop){
		
		progress = print_progress(0);
#ifdef PROGRESS
		if(progress_previous != progress){
			print_progress(1);
		}
		progress_previous = progress;
#endif
		if(progress == 100){
			printf("Download finished\n");
			download_finished = 1;
			break;
		}
			
			if(peerlist_and_bitmap_peer_updated){
				free(peerlist_tmp);
				for(i = 0;i < numOfPeer_tmp;i++){
					free(bitmap_peer_tmp[i]);
				}
				free(bitmap_peer_tmp);
				//make a copy of peerlist and bitmap_peer
				pthread_mutex_lock(&peerlist_and_bitmap_peer_mutex);
				
				{
					numOfPeer_tmp = numOfPeer;
					
					peerlist_tmp = (struct sockaddr_in*) malloc(sizeof(struct sockaddr_in) * numOfPeer_tmp);
					memcpy(peerlist_tmp, peerlist, sizeof(struct sockaddr_in) * numOfPeer_tmp);
					
					bitmap_peer_tmp = (char**) malloc(sizeof(char*) * numOfPeer_tmp);
					for(i = 0;i < numOfPeer_tmp;i++){
						if(bitmap_peer[i] == NULL){
							bitmap_peer_tmp[i] = NULL;
						}else{
							bitmap_peer_tmp[i] = (char*) malloc(sizeof(char) * bitmap_size);
							memcpy(bitmap_peer_tmp[i], bitmap_peer[i], sizeof(char) * bitmap_size);
						}
					}
				}
				
				pthread_mutex_unlock(&peerlist_and_bitmap_peer_mutex);
				
				peerlist_and_bitmap_peer_updated = 0;
				
				//clear rarest
				for(i = 0;i < numOfChunk;i++){
					rarest[i] = 0;
				}
				//calculate rarest
				for(i = 0;i < bitmap_size;i++){//foreach 8 chunks
					bitlen = (i == (bitmap_size - 1)?((numOfChunk%8 == 0)?8:numOfChunk%8):8);
					for(j = 0;j < bitlen;j++){//foreach chunk in 8 chunks
						for(k = 0;k < numOfPeer_tmp;k++){//foreach peer
							if(bitmap_peer_tmp[k] != NULL){//exclude invalid bitmap
								//printf("i[%d]j[%d]k[%d]bitlen[%d][%X][%X]\n", i, j, k, bitlen, bitmap_peer_tmp[k][i], ((bitmap_peer_tmp[k][i] >> j) & 0x01));
								if(((bitmap_peer_tmp[k][i] >> j) & 0x01) == 0x01){
									rarest[i * 8 + j]++;
								}
							}
						}
					}
				}
#ifdef DEBUG
				printf("chunkNum[rarest]");
				for(i = 0;i < numOfChunk;i++){
					printf("%d[%d]", i, rarest[i]);
				}
				printf("\n");
#endif
				
			}
			
			pthread_t* download_pthread = NULL;
			download_struct* download_conn = NULL;
			unsigned int chunkNum_tmp = 318342443; //318 & 342 & 443
			unsigned int rarest_tmp = 7610; //4430 + 3180
			unsigned int bitmap_pos = 0;
			
			int numOfDownloadPeer_tmp = 0;
			int max_conn = 0;
			int rand_num = rand() % 10000;
			int search_pnt = 0;
			
			pthread_mutex_lock(&internal_chunkmap_mutex);
			
			{
				memcpy(internal_chunkmap_tmp, internal_chunkmap, sizeof(unsigned int) * numOfChunk);
			}
			
			pthread_mutex_unlock(&internal_chunkmap_mutex);
			
			pthread_mutex_lock(&dlPeerLL_mutex);
			
			{
				numOfDownloadPeer_tmp = numOfDownloadPeer;
			}
			
			pthread_mutex_unlock(&dlPeerLL_mutex);
			
			max_conn = MAX_DL_PTHREAD - numOfDownloadPeer_tmp;
			
#ifdef DEBUG
			//printf("max_conn[%d]\n", max_conn);
#endif

			for(k = 0;k < numOfPeer_tmp && max_conn > 0;k++){//foreach peer
				chunkNum_tmp = 318342443;
				rarest_tmp = 7610;
				if(bitmap_peer_tmp[k] != NULL){//exclude invalid bitmap
					for(i = 0;i < bitmap_size;i++){//foreach 8 chunks
						search_pnt = (i + rand_num) % bitmap_size;
						bitlen = (search_pnt == (bitmap_size - 1)?((numOfChunk%8 == 0)?8:numOfChunk%8):8);
						for(j = 0;j < bitlen;j++){//foreach chunk in 8 chunks
							bitmap_pos = search_pnt * 8 + j;
							if(internal_chunkmap_tmp[bitmap_pos] == 0 && ((bitmap_peer_tmp[k][search_pnt] >> j) & 0x01) == 0x01 && rarest[bitmap_pos] < rarest_tmp){
								rarest_tmp = rarest[bitmap_pos];
								chunkNum_tmp = bitmap_pos;
#ifdef DEBUG
								printf("k[%d]rarest_tmp[%d]chunkNum_tmp[%d]\n", k, rarest_tmp, chunkNum_tmp);
#endif
							}
						}
					}
				}
				if(chunkNum_tmp != 318342443){
					download_pthread = (pthread_t*) malloc(sizeof(pthread_t));
					download_conn = (download_struct*) malloc(sizeof(download_struct));
					download_conn->peer_addr = peerlist_tmp[k];
					download_conn->chunkNum = chunkNum_tmp;
					
#ifdef DEBUG
					printf("getFile download_conn[%s][%d][%d]\n", inet_ntoa(download_conn->peer_addr.sin_addr), ntohs(download_conn->peer_addr.sin_port), download_conn->chunkNum);
#endif
					dl_pthread_started = 0;
					pthread_create(download_pthread, NULL, do_download, download_conn);
					while(!dl_pthread_started);
					
					max_conn--;
					
					pthread_mutex_lock(&internal_chunkmap_mutex);
					
					{
						memcpy(internal_chunkmap_tmp, internal_chunkmap, sizeof(unsigned int) * numOfChunk);
					}
					
					pthread_mutex_unlock(&internal_chunkmap_mutex);
					
				}
			}
			
			//sleep(1);
				
			if(max_conn <= 0){
#ifdef DEBUG
				printf("do_getFile going to have 3sec sleep\n");
#endif
				sleep(1);
			}
		}
	}
	
#ifdef DEBUG
	printf("do_getFile return\n");
#endif
	return;
}

int read_torrent(char* torrent, int mode){
//read torrent info
	int torrent_fd;
	
	torrent_fd = open(torrent, O_RDONLY);
	if(torrent_fd < 0){
		perror("torrent file not found");
		exit(1);
	}
	read(torrent_fd, (void*) &torrent_id, 4);
	torrent_id = ntohl(torrent_id);
	read(torrent_fd, (void*) &tracker_ip.s_addr, 4);
	tracker_ip.s_addr = ntohl(tracker_ip.s_addr);
	read(torrent_fd, (void*) &tracker_port, 2);
	//tracker_port = ntohs(tracker_port);
	read(torrent_fd, (void*) &fname_length, 4);
	//fname_length = ntohl(fname_length);
	fname = (char*) malloc(sizeof(char) * fname_length + 1);
	read(torrent_fd, (void*) fname, fname_length);
	fname[fname_length] = 0;
	read(torrent_fd, (void*) &fsize, 4);
	
#ifdef DEBUG
	printf("torrent_fd:[%d]\n", torrent_fd);
	printf("torrent_id:[%X]\n", ntohl(torrent_id));
	printf("tracker_ip:[%s]\n", inet_ntoa(tracker_ip));
	printf("fname_length:[%d]\n", fname_length);
	printf("track_port:[%d]\n", tracker_port);
	printf("fname:[%s]\n", fname);
	printf("fsize:[%d]\n", fsize);
#endif
	
	numOfChunk = ceil((double)fsize / (CHUNK_SIZE));
	bitmap_size = ceil((double)numOfChunk / 8);
	
	if(mode == 0){
		FILE* fptr;
		fptr = fopen(file_path, "w");
		close(fptr);
		file_fd = open(file_path, O_RDWR);
	}
	
	if(mode == 1){
		file_fd = open(file_path, O_RDONLY);
	}
	
	if(file_fd < 0){
		printf("cannot open file\n");
		exit(1);
	}
	
#ifdef DEBUG
	printf("file_fd[%d]numOfChunk[%d]bitmap_size[%d]\n", file_fd, numOfChunk, bitmap_size);
#endif
	
	return 1;
}

void do_exit(){
//terminate the peer process
#ifdef DEBUG
	printf("enter do_exit\n");
#endif
	if(!stop){
		while(do_ureg_peer() < 0){
			perror("fail to unregister peer from tracker, retry...\n");
			sleep(3);
		}
	}
	
#ifdef DEBUG
	printf("terminated by user\n");
#endif
	exit(0);
	return;
}

void do_gets(){
//all time alive gets_pthread
	
	char buf[100];
	
	while(1){
		gets(buf);
		
		if(!strcmp(buf, "stop")){
			if(!stop){
				stop = 1;
				do_ureg_peer();
			}else{
				printf("already stopped\n");
			}
		}
		
		if(!strcmp(buf, "resume")){
			if(stop){
				stop = 0;
				do_reg_peer();
			}else{
				printf("already running\n");
			}
		}
		
		if(!strcmp(buf, "progress")){
			print_progress(1);
		}
		
		if(!strcmp(buf, "peer")){
			print_peerlist();
		}
		
		if(!strcmp(buf, "exit")){
			do_exit();
		}
		
	}
	
#ifdef DEBUG
	printf("gets_pthread exit\n");
#endif
	pthread_exit(NULL);
	return;
}

void do_add(char* torrent, int mode){
//add torrent and start download

#ifdef DEBUG
	printf("enter do_add\n");
#endif
	
	if(mode == 1){
		upload = 0;
	}
	
	read_torrent(torrent, 0);
	
	//make a bitmap
	char* bitmap_tmp;
	char* bitmap_tmp2;
	
	bitmap_tmp = (char*) malloc(sizeof(char) * bitmap_size);
	
	int i;
	for(i = 0;i < bitmap_size;i++){
		bitmap_tmp[i] = 0x00;
	}
	
#ifdef DEBUG
	printf("bitmap:");
	for(i = 0;i < bitmap_size;i++){
		printf("[%X]", bitmap_tmp[i]);
	}
	printf("\n");
#endif
	
	pthread_mutex_lock(&bitmap_mutex);
	
	{
		bitmap_tmp2 = bitmap;
		bitmap = bitmap_tmp;
	}
	
	pthread_mutex_unlock(&bitmap_mutex);
	
	free(bitmap_tmp2);
	
	//make an internal_chunkmap
#ifdef DEBUG
	printf("construct internal_chunkmap\n");
#endif
	unsigned int* internal_chunkmap_tmp;
	unsigned int* internal_chunkmap_tmp2;
	
	internal_chunkmap_tmp = (unsigned int*) malloc(sizeof(unsigned int) * numOfChunk);
	
	for(i = 0;i < numOfChunk;i++){
		internal_chunkmap_tmp[i] = 0;
	}
	
	pthread_mutex_lock(&internal_chunkmap_mutex);
	
	{
		internal_chunkmap_tmp2 = internal_chunkmap;
		internal_chunkmap = internal_chunkmap_tmp;
	}
	
	pthread_mutex_unlock(&internal_chunkmap_mutex);
	
	free(internal_chunkmap_tmp2);
	
	pthread_t listen_pthread;
	pthread_t gets_pthread;
	pthread_t getInfo_pthread;
	
	pthread_create(&listen_pthread, NULL, do_listen, listen_portno);
#ifdef DEBUG
	printf("add listen_pthread created\n");
#endif
	
	while(!listen_started);
	
	pthread_create(&gets_pthread, NULL, do_gets, NULL);
#ifdef DEBUG
	printf("add gets_pthread created\n");
#endif
	
	while(do_reg_peer() < 0){
		perror("fail to register peer to tracker, retry...\n");
		sleep(3);
	}
	
	while(!done_test_by_tracker);
	
	pthread_create(&getInfo_pthread, NULL, do_getInfo, GETINFO_PERIOD);
#ifdef DEBUG
	printf("add getInfo_pthread created\n");
#endif

#ifdef TIMER
	c_start = times(&buf1);
#endif
	
	do_getFile();
#ifdef DEBUG
	printf("do_getFile() return\n");
#endif

#ifdef TIMER
	end = times(&buf2);
	printf("total download time:[%.2f]sec\n", ((double)end - (double)c_start)/100);
#endif
	
	
	pthread_join(listen_pthread, NULL);
#ifdef DEBUG
	printf("listen_pthread joined\n");
#endif
	pthread_join(gets_pthread, NULL);
#ifdef DEBUG
	printf("gets_pthread joined\n");
#endif
	pthread_join(getInfo_pthread, NULL);
#ifdef DEBUG
	printf("getInfo_pthread joined\n");
	printf("do_add return\n");
#endif
	return;
}

void do_seed(char* torrent){
//add torrent and start upload all chunks
	
#ifdef DEBUG
	printf("enter do_seed\n");
#endif
	
	read_torrent(torrent, 1);
	
	//make a bitmap
	char* bitmap_tmp;
	char* bitmap_tmp2;
	
	bitmap_tmp = (char*) malloc(sizeof(char) * bitmap_size);
	
	int i;
	for(i = 0;i < bitmap_size;i++){
		bitmap_tmp[i] = 0xff;
	}
	
	bitmap_tmp[bitmap_size - 1] &= 0xff >> ((numOfChunk % 8 == 0)?0:(8 - (numOfChunk % 8)));

#ifdef DEBUG	
	printf("bitmap_tmp:");
	for(i = 0;i < bitmap_size;i++){
		printf("[%X]", bitmap_tmp[i]);
	}
	printf("\n");
#endif
	
	pthread_mutex_lock(&bitmap_mutex);
	
	{
		bitmap_tmp2 = bitmap;
		bitmap = bitmap_tmp;
	}
	
	pthread_mutex_unlock(&bitmap_mutex);
	
	free(bitmap_tmp2);
	
	download_finished = 1;
	
	pthread_t listen_pthread;
	pthread_t gets_pthread;
	pthread_t getInfo_pthread;
	
	pthread_create(&listen_pthread, NULL, do_listen, listen_portno);
#ifdef DEBUG
	printf("seed listen_pthread created\n");
#endif
	
	while(!listen_started);
	
	pthread_create(&gets_pthread, NULL, do_gets, NULL);
#ifdef DEBUG
	printf("seed gets_pthread created\n");
#endif
	
	while(do_reg_peer() < 0){
		perror("fail to register peer to tracker, retry...\n");
		sleep(3);
	}
	
	while(!done_test_by_tracker);
	
	pthread_create(&getInfo_pthread, NULL, do_getInfo, GETINFO_PERIOD);
#ifdef DEBUG
	printf("seed getInfo_pthread created\n");
#endif
	
	pthread_join(listen_pthread, NULL);
#ifdef DEBUG
	printf("listen_pthread joined\n");
#endif
	pthread_join(gets_pthread, NULL);
#ifdef DEBUG
	printf("gets_pthread joined\n");
#endif
	pthread_join(getInfo_pthread, NULL);
#ifdef DEBUG
	printf("getInfo_pthread joined\n");
	printf("do_seed return\n");
#endif
	return;
}

void do_subseed(char* torrent){
//add torrent and start upload some chunks
	
#ifdef DEBUG
	printf("enter do_subseed\n");
#endif
	
	read_torrent(torrent, 1);
	
	//make a bitmap
	char* bitmap_tmp;
	char* bitmap_tmp2;
	
	numOfChunk = ceil((double)fsize / (CHUNK_SIZE));
	bitmap_size = ceil((double)numOfChunk / 8);
	
#ifdef DEBUG
	printf("numOfChunk[%d]bitmap_size[%d]\n", numOfChunk, bitmap_size);
#endif
	
	bitmap_tmp = (char*) malloc(sizeof(char) * bitmap_size);
	
	int i;
	for(i = 0;i < bitmap_size;i++){
		bitmap_tmp[i] = 0x00;
	}
	
	//interactive input and output
	printf("There are %d chunks in %s\n", numOfChunk, torrent);
	printf("Which chunks to upload? (start counting from 0)\n");
	
	int confirm = 0;
	char buf[100];
	char buf2[100];
	char* ch_ptr;
	int seq_start, seq_end, seq_cnt = 0;
	while(!confirm){
		printf("[type . to stop] >> ");
		gets(buf);
		ch_ptr = buf + strlen(buf) - 1;
		if(*ch_ptr == '.'){
			printf("Confirm (Y/N) >> ");
			gets(buf2);
			if(*buf2 == 'Y'){
				confirm = 1;
			}else{
				continue;
			}
		}
		ch_ptr = strtok(buf, "-.");
		if(ch_ptr != NULL){
			seq_start = atoi(ch_ptr);
		}else{
			seq_start = -1;
		}
		ch_ptr = strtok(NULL, "-.");
		if(ch_ptr != NULL){
			seq_end = atoi(ch_ptr);
		}else{
			seq_end = seq_start;
		}
		if(seq_start != -1){
			for(i = seq_start;i <= seq_end;i++){
				bitmap_tmp[i / 8] = bitmap_tmp[i / 8] | 0x01 << (i % 8);
				seq_cnt++;
			}
		}
#ifdef DEBUG
		printf("seq_start[%d]seq_end[%d]\n", seq_start, seq_end);
#endif
	}

#ifdef DEBUG	
	printf("bitmap_tmp:");
	for(i = 0;i < bitmap_size;i++){
		printf("[%X]", bitmap_tmp[i]);
	}
	printf("\n");
	
	printf("Start uploading %d chunk(s)\n", seq_cnt);
#endif
	
	pthread_mutex_lock(&bitmap_mutex);
	
	{
		bitmap_tmp2 = bitmap;
		bitmap = bitmap_tmp;
	}
	
	pthread_mutex_unlock(&bitmap_mutex);
	
	free(bitmap_tmp2);
	
	download_finished = 1;
	
	pthread_t listen_pthread;
	pthread_t gets_pthread;
	pthread_t getInfo_pthread;
	
	pthread_create(&listen_pthread, NULL, do_listen, listen_portno);
#ifdef DEBUG
	printf("do_subseed listen_pthread created\n");
#endif
	
	while(!listen_started);
	
	pthread_create(&gets_pthread, NULL, do_gets, NULL);
#ifdef DEBUG
	printf("do_subseed gets_pthread created\n");
#endif
	
	while(do_reg_peer() < 0){
		perror("fail to register peer to tracker, retry...\n");
		sleep(3);
	}
	
	while(!done_test_by_tracker);
	
	pthread_create(&getInfo_pthread, NULL, do_getInfo, GETINFO_PERIOD);
#ifdef DEBUG
	printf("do_subseed getInfo_pthread created\n");
#endif
	
	pthread_join(listen_pthread, NULL);
#ifdef DEBUG
	printf("listen_pthread joined\n");
#endif
	pthread_join(gets_pthread, NULL);
#ifdef DEBUG
	printf("gets_pthread joined\n");
#endif
	pthread_join(getInfo_pthread, NULL);
#ifdef DEBUG
	printf("getInfo_pthread joined\n");
	printf("do_subseed return\n");
#endif
	return;
}

int main(int argc, char** argv){
	
	if(argc != 5){
		printf("Usage: %s [add/vampire/seed/subseed] [torrent] [your ip] [file path]\n", argv[0]);
		exit(1);
	}
	
	peerlist = (struct sockaddr_in*) malloc(sizeof(struct sockaddr_in));
	bitmap = (char*) malloc(sizeof(char));
	internal_chunkmap = (unsigned int*) malloc(sizeof(unsigned int));
	bitmap_peer = (char **) malloc(sizeof(char*));
	
	peer_ip = argv[3];
	file_path = argv[4];
	
	srand(time(NULL));
	listen_portno = 9000 + (rand() % 1000);
	
	if(!strcmp(argv[1], "add")){
		do_add(argv[2], 0);
	}
	
	if(!strcmp(argv[1], "vampire")){
		do_add(argv[2], 1);
	}
	
	if(!strcmp(argv[1], "seed")){
		do_seed(argv[2]);
	}
	
	if(!strcmp(argv[1], "subseed")){
		do_subseed(argv[2]);
	}
	
	return 0;
}