#define _GNU_SOURCE 
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <signal.h>
#include <netdb.h>
#define ERR(source) (perror(source),\
		fprintf(stderr,"%s:%d\n",__FILE__,__LINE__),\
		exit(EXIT_FAILURE))

#define MAXANSWLEN 100
#define MAXMSGSIZE 2000

volatile sig_atomic_t do_work=1;

typedef struct server
{
	int fd;
	char* data;
	int data_size;
	int offset;
	char* ip;
	char* port;
} server_t;

void sigint_handler(int sig) {
	do_work=0;
}

int sethandler( void (*f)(int), int sigNo) {
	struct sigaction act;
	memset(&act, 0, sizeof(struct sigaction));
	act.sa_handler = f;
	if (-1==sigaction(sigNo, &act, NULL))
		return -1;
	return 0;
}
int make_socket(void){
	int sock;
	sock = socket(PF_INET,SOCK_STREAM,0);
	if(sock < 0) ERR("socket");
	return sock;
}
struct sockaddr_in make_address(char *address, char *port){
	int ret;
	struct sockaddr_in addr;
	struct addrinfo *result;
	struct addrinfo hints = {};
	hints.ai_family = AF_INET;
	if((ret=getaddrinfo(address,port, &hints, &result))){
		fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(ret));
		exit(EXIT_FAILURE);
	}
	addr = *(struct sockaddr_in *)(result->ai_addr);
	freeaddrinfo(result);
	return addr;
}
int connect_socket(char *name, char *port){
	struct sockaddr_in addr;
	int socketfd;
	socketfd = make_socket();
	addr=make_address(name,port);
	if(connect(socketfd,(struct sockaddr*) &addr,sizeof(struct sockaddr_in)) < 0){
		if(errno!=EINTR) ERR("connect");
		else { 
			fd_set wfds ;
			int status;
			socklen_t size = sizeof(int);
			FD_ZERO(&wfds);
			FD_SET(socketfd, &wfds);
			if(TEMP_FAILURE_RETRY(select(socketfd+1,NULL,&wfds,NULL,NULL))<0) ERR("select");
			if(getsockopt(socketfd,SOL_SOCKET,SO_ERROR,&status,&size)<0) ERR("getsockopt");
			if(0!=status) ERR("connect");
		}
	}
	return socketfd;
}

void setMasks(sigset_t* mask, sigset_t* oldmask)
{
	sigemptyset(mask);
	sigaddset(mask, SIGINT);
	sigprocmask (SIG_BLOCK, mask, oldmask);
}

void addFds(fd_set* base_rfds, server_t* servers, int n)
{
	FD_SET(STDIN_FILENO, base_rfds);
	for (int i = 0; i < n; i++)
		FD_SET(servers[i].fd, base_rfds);
}

int maxFd(server_t* servers, int n)
{
	int res = servers[0].fd;
	for (int i = 1; i < n; i++)
		res = (res < servers[i].fd ? servers[i].fd : res);
	return res;
}

void readAnswer(char* answ, int answ_to, int* answ_ready)
{
	char buf[MAXANSWLEN];
	if (read(STDIN_FILENO, buf, MAXANSWLEN*sizeof(char)) > 0 && answ_to != -1)
	{
		*answ = buf[0];
		*answ_ready = 1;
	}
	else
		printf("Nie teraz!\n");
}

int readServer(server_t* server)
{
	int rd = read(server->fd, server->data + server->offset, 
				  (server->data_size - server->offset)*sizeof(char));
	if (rd < 1) return 0;
	server->offset += rd;
	if (server->offset > server->data_size / 2)
	{
		server->data_size *= 2;
		char* buf = (char*) malloc(server->data_size*sizeof(char));
		memcpy(buf, server->data, server->offset);
		free(server->data);
		server->data = buf;
	}
	return 1;
}

void answerServer(server_t* server, char* answ, int* answ_to, int* answ_ready)
{
	write(server->fd, answ, sizeof(char));
	server->offset = 0;
	*answ_to = -1;
	*answ_ready = 0;
}

void changeReceiver(int nr, server_t* servers, int* answ_to)
{
    if (nr == *answ_to) return;
	char chr = 0;
	if (*answ_to != -1)
	{
		TEMP_FAILURE_RETRY(write(servers[*answ_to].fd, &chr, sizeof(char)));
		servers[*answ_to].offset = 0;
		printf("[no answer]\n");
	}
	printf("\n[%s:%s]\nQuestion: %s\nAnswer: ", servers[nr].ip, servers[nr].port, servers[nr].data);
	fflush(stdout);
	*answ_to = nr;
}

void removeServer(int nr, server_t* servers, int* servers_n, fd_set* base_rfds, int* answ_to)
{
    if (nr == *answ_to)
    {
        printf("[no answer]");
        *answ_to = -1;
    }
    printf("\n");
    printf("\n[%s:%s] terminated!\n", servers[nr].ip, servers[nr].port);
    if (*answ_to != -1) printf("\n[%s:%s]\nQuestion: %s\nAnswer: ", servers[*answ_to].ip, servers[*answ_to].port, servers[*answ_to].data);
    fflush(stdout);
	if (*answ_to > nr) (*answ_to)--;
    FD_CLR(servers[nr].fd, base_rfds);
	if(TEMP_FAILURE_RETRY(close(servers[nr].fd))<0)ERR("close");
	for (int i = nr; i < *servers_n - 1; i++)
		servers[i] = servers[i+1];
	(*servers_n)--;
}

void doClient(server_t* servers, int* n)
{
	fd_set base_rfds, rfds;
	FD_ZERO(&base_rfds);
	addFds(&base_rfds, servers, *n);
	sigset_t mask, oldmask;
	setMasks(&mask, &oldmask);
	int mfs = maxFd(servers, *n);
	int answ_to = -1;
	char answ;
	int answ_ready = 0;
	while(do_work && *n > 0)
	{
		rfds=base_rfds;
		if(pselect(mfs+1,&rfds,NULL,NULL,NULL,&oldmask)>0)
		{
			if (FD_ISSET(STDIN_FILENO, &rfds))
				readAnswer(&answ, answ_to, &answ_ready);
			for (int i = 0; i < *n; i++)
			{
				if (i == answ_to && answ_ready)
				{
					answerServer(&servers[i], &answ, &answ_to, &answ_ready);
					continue;
				}
				if (FD_ISSET(servers[i].fd, &rfds))
				{
					if (!readServer(&servers[i]))
					{
						removeServer(i, servers, n, &base_rfds, &answ_to);
						i--;
						continue;
					}
					if (servers[i].data[servers[i].offset-1] == '\0')
						changeReceiver(i, servers, &answ_to);
				}
			}
		}
	}
	sigprocmask(SIG_UNBLOCK, &mask, NULL);
}

void usage(char * name){
	fprintf(stderr,"USAGE: %s domain port [domain port] ...\n",name);
}

void connectServers(server_t* servers, int n, char** argv)
{
	for (int i = 0; i < n; i++)
	{
		servers[i].fd = connect_socket(argv[2*i+1], argv[2*i+2]);
		servers[i].data_size = MAXMSGSIZE;
		servers[i].data = (char*)malloc(servers[i].data_size*sizeof(char));
		servers[i].offset = 0;
		servers[i].ip = argv[2*i+1];
		servers[i].port = argv[2*i+2];
	}
}

void freeServers(server_t* servers, int n)
{
	for (int i = 0; i < n; i++)
	{
		if(TEMP_FAILURE_RETRY(close(servers[i].fd))<0)ERR("close");
		free(servers[i].data);
	}
	free(servers);
}

int main(int argc, char** argv) {
	if(argc < 3 || argc % 2 == 0) {
		usage(argv[0]);
		return EXIT_FAILURE;
	}
	int n = argc/2;
	if(sethandler(SIG_IGN,SIGPIPE)) ERR("Seting SIGPIPE:");
	if(sethandler(sigint_handler,SIGINT)) ERR("Seting SIGINT:");
	server_t* servers = (server_t*)malloc(n*sizeof(server_t));;
	connectServers(servers, n, argv);
	doClient(servers, &n);
	freeServers(servers, n);
	return EXIT_SUCCESS;
}
