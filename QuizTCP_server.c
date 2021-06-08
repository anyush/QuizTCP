#define _GNU_SOURCE 
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <signal.h>
#include <netdb.h>
#include <fcntl.h>
#include <time.h>
#define ERR(source) (perror(source),\
		fprintf(stderr,"%s:%d\n",__FILE__,__LINE__),\
		exit(EXIT_FAILURE))

#define MAXMSGSIZE 2000
#define NSECWAIT 330000000

volatile sig_atomic_t do_work=1 ;
volatile sig_atomic_t server_state = 0;

typedef struct client
{
	int fd;
	int q_nr;
	int alr_sent;
} client_t;

void sigint_handler(int sig) {
	do_work=0;
}

void sigusr1_handler(int sig) {
	server_state = (server_state == 0 ? 1 : server_state);
}

int sethandler( void (*f)(int), int sigNo) {
	struct sigaction act;
	memset(&act, 0, sizeof(struct sigaction));
	act.sa_handler = f;
	if (-1==sigaction(sigNo, &act, NULL))
		return -1;
	return 0;
}
int make_socket(int domain, int type){
	int sock;
	sock = socket(domain,type,0);
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

int bind_tcp_socket(char* ip, char* port, int n){
	struct sockaddr_in addr;
	int socketfd,t=1;
	socketfd = make_socket(PF_INET,SOCK_STREAM);
	addr = make_address(ip, port);
	if (setsockopt(socketfd, SOL_SOCKET, SO_REUSEADDR,&t, sizeof(t))) ERR("setsockopt");
	if(bind(socketfd,(struct sockaddr*) &addr,sizeof(addr)) < 0)  ERR("bind");
	if(listen(socketfd, n) < 0) ERR("listen");
	return socketfd;
}

int add_new_client(int sfd){
	int nfd;
	if((nfd=TEMP_FAILURE_RETRY(accept(sfd,NULL,NULL)))<0) {
		if(EAGAIN==errno||EWOULDBLOCK==errno) return -1;
		ERR("accept");
	}
	return nfd;
}
void usage(char * name){
	fprintf(stderr,"USAGE: %s address port number_of_clients name_of_file\n", name);
}

void add_nsec(struct timespec* tspc, long nsecs)
{
	while (tspc->tv_nsec + nsecs > 999999999)
	{
		tspc->tv_sec += 1;
		nsecs -= 1000000000;
	}
	tspc->tv_nsec += nsecs;
}

struct timespec get_timeout(struct timespec last)
{
	struct timespec timeout = {0, 0};
	struct timespec now;
	clock_gettime(CLOCK_REALTIME, &now);
	add_nsec(&last, NSECWAIT);
	if (last.tv_sec > now.tv_sec || (last.tv_sec == now.tv_sec && last.tv_nsec > now.tv_nsec))
	{
		long nsecs = (last.tv_sec - now.tv_sec) * 1000000000 + last.tv_nsec - now.tv_nsec;
		add_nsec(&timeout, nsecs);
	}
	return timeout;
}

void saveClient(int cfd, client_t* clients, int* clients_alr, int qn, fd_set* base_rfds)
{
	clients[*clients_alr].fd = cfd;
	clients[*clients_alr].q_nr = rand() % qn;
	clients[*clients_alr].alr_sent = 0;
	(*clients_alr)++;
	FD_SET(cfd, base_rfds);
}

void refuseClient(int cfd)
{
	char* data = "NIE";
	write(cfd, data, 3*sizeof(char));
	if(TEMP_FAILURE_RETRY(close(cfd))<0)ERR("close");
}

void processNewClient(int fdT, client_t* clients, int* clients_alr, int max_clients, int qn, fd_set* base_rfds, int* mfd)
{
	int cfd = add_new_client(fdT);
	if(cfd>=0)
	{
		if (*clients_alr < max_clients)
		{
			saveClient(cfd, clients, clients_alr, qn, base_rfds);
			*mfd = *mfd < cfd ? cfd : *mfd;
		}
		else
			refuseClient(cfd);
	}
}

ssize_t bulk_write(int fd, char *buf, size_t count){
	int c;
	size_t len=0;
	do{
		c=TEMP_FAILURE_RETRY(write(fd,buf,count));
		if(c<0) return c;
		buf+=c;
		len+=c;
		count-=c;
	}while(count>0);
	return len ;
}

int readFromClient(client_t* client, int qn)
{
	char answ;
	int rd;
	if ((rd = read(client->fd, &answ, sizeof(char))) == -1)
	{
		if (errno == EPIPE)
			return 0;
	}
	else if (rd == 0) return 0;
	client->q_nr = rand() % qn;
	client->alr_sent = 0;
	return 1;
}

int sendQuestion(client_t* client, char* question)
{
	int to_send = rand() % (strlen(question) + 1 - client->alr_sent + 1);
	to_send = MAXMSGSIZE < to_send ? MAXMSGSIZE : to_send;
	int sent;
	if ((sent = bulk_write(client->fd, question + client->alr_sent, to_send*sizeof(char))) == -1)
	{
		if (errno == EPIPE)
			return 0;
	}
	else
		client->alr_sent += sent;
	return 1;
}

void sendKoniec(client_t* clients, int clients_n)
{
    char msg[6] = "Koniec";
    for (int i = 0; i < clients_n; i++)
        bulk_write(clients[i].fd, msg, 6*sizeof(char));
}

void removeClient(int nr, client_t* clients, int* clients_n, fd_set* base_rfds)
{
	FD_CLR(clients[nr].fd, base_rfds);
	if(TEMP_FAILURE_RETRY(close(clients[nr].fd))<0)ERR("close");
	for (int i = nr; i < *clients_n - 1; i++)
		clients[i] = clients[i+1];
	(*clients_n)--;
}

void processClientsRead(client_t* clients, int* clients_n, char** questions, int qn, fd_set* rfds, fd_set* base_rfds)
{
	int stillConnected;
	for (int i = 0; i < *clients_n; i++)
	{
		if (!FD_ISSET(clients[i].fd, rfds)) continue;
		if (strlen(questions[clients[i].q_nr]) + 1 >= clients[i].alr_sent)
			stillConnected = readFromClient(&clients[i], qn);
		if (!stillConnected)
		{
			removeClient(i, clients, clients_n, base_rfds);
			i--;
		}
	}
}

void processClientsWrite(client_t* clients, int* clients_n, char** questions, int qn, fd_set* base_rfds)
{
	int stillConnected;
	for (int i = 0; i < *clients_n; i++)
	{
		if (strlen(questions[clients[i].q_nr]) + 1 > clients[i].alr_sent)
			stillConnected = sendQuestion(&clients[i], questions[clients[i].q_nr]);
		if (!stillConnected)
		{
			removeClient(i, clients, clients_n, base_rfds);
			i--;
		}
	}
}

void setMasks(sigset_t* mask, sigset_t* oldmask)
{
	sigemptyset(mask);
	sigaddset(mask, SIGINT);
	sigaddset(mask, SIGUSR1);
	sigprocmask (SIG_BLOCK, mask, oldmask);
}

void checkState(int fdT, fd_set* base_rfds)
{
	switch (server_state)
	{
		case 0:
			break;
		
		case 1:
			if(TEMP_FAILURE_RETRY(close(fdT))<0)ERR("close");
			FD_CLR(fdT, base_rfds);
			server_state = 2;
			break;
		default:
			break;
	}
}

void freeClients(client_t* clients, int clients_n)
{
	for (int i = 0; i < clients_n; i++)
		if(TEMP_FAILURE_RETRY(close(clients[i].fd))<0)ERR("close");
	free(clients);
}

void doServer(int fdT, int n, char** questions, int qn){
	srand(time(NULL));
	fd_set base_rfds, rfds;
	FD_ZERO(&base_rfds);
	FD_SET(fdT, &base_rfds);
	sigset_t mask, oldmask;
	setMasks(&mask, &oldmask);
	int mfd = fdT;
	
	struct timespec last;
	clock_gettime(CLOCK_REALTIME, &last);
    struct timespec timeout = get_timeout(last);
    
	client_t* clients = (client_t*) malloc(n*sizeof(client_t));
	int clients_alr = 0;
	while(do_work)
	{
		checkState(fdT, &base_rfds);
		rfds=base_rfds;
		if(pselect(mfd+1,&rfds,NULL,NULL,&timeout,&oldmask)>0)
		{
			if (FD_ISSET(fdT, &rfds))
                processNewClient(fdT, clients, &clients_alr, n, qn, &base_rfds, &mfd);
			else
				processClientsRead(clients, &clients_alr, questions, qn, &rfds, &base_rfds);
		}
		else
		{
            processClientsWrite(clients, &clients_alr, questions, qn, &base_rfds);
			clock_gettime(CLOCK_REALTIME, &last);
		}
		timeout = get_timeout(last);
	}
    sendKoniec(clients, clients_alr);
	sigprocmask(SIG_UNBLOCK, &mask, NULL);
	checkState(fdT, &base_rfds);
	freeClients(clients, clients_alr);
}

char** splitFile(char*file_text, int* qn)
{
	int fl = strlen(file_text);
	*qn = 0;
	for (int i = 0; i < fl; i++)
		if (file_text[i] == '\n')
			(*qn)++;
	if (*qn == 0) return NULL;
	(*qn)++;
	char** questions = (char**)malloc((*qn)*sizeof(char*));
	char* pch = strtok(file_text, "\n");
	for (int i = 0; i < *qn; i++)
	{
		questions[i] = (char*)malloc((strlen(pch)+1)*sizeof(char));
		strcpy(questions[i], pch);
		pch = strtok(NULL, "\n");
	}
	free(file_text);
	return questions;
}

char** readFile(char* filename, int* qn)
{
	int fd;
	if ((fd = open(filename, O_RDONLY)) == -1) ERR("open");
	char* file_text = (char*) malloc(1000*sizeof(char));
	int offset = 0;
	int ready_to_save = 1000;
	int saved;
	while ((saved = read(fd, file_text + offset, ready_to_save*sizeof(char*))) == ready_to_save)
	{
		char* incr_text = (char*) malloc(2*ready_to_save*sizeof(char));
		strncpy(incr_text, file_text, ready_to_save);
		free(file_text);
		file_text = incr_text;
		offset = ready_to_save;
		ready_to_save *= 2;
	}
	close(fd);
	char* stripped_text = (char*)malloc((offset + saved)*sizeof(char));
	strncpy(stripped_text, file_text, (offset + saved)*sizeof(char));
	free(file_text);
	return (splitFile(stripped_text, qn));
}

void freeQuestions(char** questions, int qn)
{
	for (int i = 0; i < qn; i++)
		free(questions[i]);
	free(questions);
}

int main(int argc, char** argv) {
	int fdT;
	int new_flags;
    int n;
	if(argc!=5) {
		usage(argv[0]);
		return EXIT_FAILURE;
	}
    n = atoi(argv[3]);
	int qn;
	char** questions = readFile(argv[4], &qn);
	if(sethandler(SIG_IGN,SIGPIPE)) ERR("Seting SIGPIPE:");
	if(sethandler(sigint_handler,SIGINT)) ERR("Seting SIGINT:");
	if(sethandler(sigusr1_handler,SIGUSR1)) ERR("Seting SIGUSR1:");
	fdT=bind_tcp_socket(argv[1], argv[2], n+1);
	new_flags = fcntl(fdT, F_GETFL) | O_NONBLOCK;
	fcntl(fdT, F_SETFL, new_flags);
	doServer(fdT, n, questions, qn);
	freeQuestions(questions, qn);
	fprintf(stderr,"Server has terminated.\n");
	return EXIT_SUCCESS;
}