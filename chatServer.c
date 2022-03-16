#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <signal.h>
#include <sys/time.h>
#include <sys/ioctl.h>
#include <netdb.h>
#include "chatServer.h"


static int end_server = 0;

void intHandler(int SIG_INT) {
   end_server=1;
}

conn_t* removeNode(int sd, conn_pool_t* pool) {//this fanction free all the nodes
    conn_t* temp=pool->conn_head;
    if(temp->fd==sd && pool->nr_conns==1){
        pool->conn_head=NULL;
        temp->next=NULL;
        pool->nr_conns--;
        return temp;
    }
    if(temp->fd==sd && pool->nr_conns>1) {//if thar is one item in the list to remove
        pool->conn_head=pool->conn_head->next;
        temp->next->prev=NULL;
        temp->next=NULL;
        pool->nr_conns--;
        return temp;
    }
    while (temp->next !=NULL){
        if(temp->fd==sd){
            temp->prev->next=temp->next;
            temp->next=temp->prev;
            pool->nr_conns--;
            return temp;
        }
        temp=temp->next;
    }

    temp->prev->next=NULL;
    temp->prev=NULL;
    pool->nr_conns--;
    return temp;
}

int findMaxFd (conn_pool_t* pool){
    int curr_max=0;
    conn_t* temp=pool->conn_head;
    while(temp !=NULL){
        if(temp->fd>curr_max){
            curr_max=temp->fd;
        }
        temp=temp->next;
    }

    return curr_max;
}

void freeNode (conn_t * conn ){
    msg_t* node=NULL;
   if(conn && conn->write_msg_head)
       node= conn->write_msg_head;

    while(node){
        msg_t* newNode=node;
        node=newNode->next;
        free(newNode->message);
        free(newNode);
    }

    if(conn && conn->write_msg_head)
        free(conn->write_msg_head);

    if(conn && conn->write_msg_tail)
        free(conn->write_msg_tail);

    if(conn){
        close(conn->fd);
        free(conn);
    }
}

int main (int argc, char *argv[])
{
    if(argc != 2){
        printf("Usage: chatServer <port>\n");
        exit(EXIT_FAILURE);
    }

    signal(SIGINT, intHandler);

    conn_pool_t* pool = malloc(sizeof(conn_pool_t));
    if(pool==NULL){
        perror("error: allocation failed\n");
        exit(EXIT_FAILURE);
    }
    init_pool(pool);

    int fd;		/* socket descriptor */
    if((fd = socket(PF_INET, SOCK_STREAM, 0)) < 0) {
        free(pool);
        perror("error: socket\n");
        exit(EXIT_FAILURE);
    }
    struct sockaddr_in peeraddr;
    peeraddr.sin_port= htons(atoi(argv[1]));
    peeraddr.sin_family = AF_INET;
    peeraddr.sin_addr.s_addr = htonl(INADDR_ANY);

    int on = 1;
   int rc = ioctl(fd, (int)FIONBIO, (char *)&on);
   if(rc<0){
       free(pool);
       perror("error:iotcl failed\n");
       exit(EXIT_FAILURE);
   }

    if(bind(fd, (struct sockaddr*) &peeraddr, sizeof(peeraddr)) < 0) {//bind
        free(pool);
        perror("error:bind failed\n");
        exit(EXIT_FAILURE);
    }
    if(listen(fd, 5) < 0){//listen
        free(pool);
        perror("error: listen failed\n");
        exit(EXIT_FAILURE);
    }
    FD_SET(fd,&pool->read_set);
    pool->maxfd=fd;

    do
    {

        pool->ready_read_set=pool->read_set;
        pool->ready_write_set=pool->write_set;

        printf("Waiting on select()...\nMaxFd %d\n", pool->maxfd);

        pool->nready=select(pool->maxfd+1,&pool->ready_read_set,&pool->ready_write_set,NULL,NULL);
        if(pool->nready<0){
            end_server=1;
        }


        for (int i=0; pool->nready>0 && i<pool->maxfd+1 ; i++)
        {
            if (FD_ISSET(i, &pool->ready_read_set)) {
                pool->nready--;
                if (fd == i) {
                    int toAdd = accept(i, NULL, NULL);
                    if (toAdd == -1) {
                        perror("accept is failed\n");
                        exit(EXIT_FAILURE);
                    }
                    add_conn(toAdd, pool);
                    printf("New incoming connection on sd %d\n", i);

                }

                else {
                    char *nmsg = malloc(BUFFER_SIZE);//Allocate memory for a new message
                    if (nmsg == NULL) {
                        perror("error: malloc is failed\n");
                        exit(EXIT_FAILURE);
                    }
                    memset(nmsg,'\0',BUFFER_SIZE);//Reset buffer
                    ssize_t read_size;
                    printf("Descriptor %d is readable\n", i);
                    read_size=read(i, nmsg, BUFFER_SIZE);
                    if(read_size==0){
                        remove_conn(i, pool);
                        if(pool->maxfd == 0)
                            pool->maxfd = fd;
                        printf("removing connection with sd %d \n", i);
                        printf("Connection closed for sd %d\n",i);
                    }

                    printf("%d bytes received from sd %d\n", (int)read_size, i);
                    add_msg(i,nmsg, (int)read_size, pool);
                    free(nmsg);
                }
                continue;
            }

            if(FD_ISSET(i, &pool->ready_write_set)){
                    write_to_client(i,pool);
                }
        }

    } while (end_server == 0);

    //free
    conn_t* current = pool->conn_head;
    conn_t * temp = NULL;
    while(current != NULL){
        temp = current->next;
        remove_conn(current->fd, pool);
        current = temp;
    }
    free(pool);
    return 0;
}

int init_pool(conn_pool_t* pool) {
    pool->maxfd=0;
    pool->nready=0;
    pool->nr_conns=0;
    pool->conn_head=NULL;
    FD_ZERO(&pool->read_set);
    FD_ZERO(&pool->ready_read_set);
    FD_ZERO(&pool->ready_write_set);
    FD_ZERO(&pool->write_set);
    return 0;
}

int add_conn(int sd, conn_pool_t* pool) {
    if(pool->maxfd<sd){
        pool->maxfd=sd;
    }

    conn_t* newConn=(conn_t*) malloc(sizeof (conn_t));
    if (newConn==NULL){
        perror("error: allocation failed\n");
        return -1;
    }
    bzero(newConn, sizeof(conn_t));
    newConn->fd=sd;
    newConn->write_msg_head=NULL;
    newConn->write_msg_tail=NULL;

    if(pool->nr_conns==0){//If there are no organs on the list
        newConn->next=NULL;
        newConn->prev=NULL;
        FD_SET(sd,&pool->read_set);
        pool->conn_head=newConn;
        pool->nr_conns++;
        return 0;
    }
    else{//There are organs in the list, the list is not empty
        conn_t* newConn2=pool->conn_head;
        while(newConn2->next !=NULL){
            newConn2=newConn2->next;
        }
        newConn->prev=newConn2;
        newConn2->next=newConn;
        FD_SET(sd,&pool->read_set);
        pool->nr_conns++;
    }

    return 0;
}

int remove_conn(int sd, conn_pool_t* pool) {
    conn_t* conn = removeNode(sd,pool);
    pool->maxfd= findMaxFd(pool);

    freeNode(conn);
    //add free
    FD_CLR(sd,&pool->read_set);
    FD_CLR(sd,&pool->write_set);

    return 0;

}

int add_msg(int sd,char* buffer,int len,conn_pool_t* pool) {//add new message
    for(conn_t* curr=pool->conn_head; curr!=NULL; curr=curr->next ){
        if(curr->fd==sd){
            continue;
        }
        msg_t* nMessage= (msg_t*) malloc(sizeof(msg_t));
        if(nMessage==NULL){
            perror("error: malloc failed\n");
            exit(EXIT_FAILURE);
        }

        nMessage->message=malloc(len+1);
        if(nMessage->message==NULL){
            perror("error: malloc failed\n");
            exit(EXIT_FAILURE);
        }
        memset(nMessage->message,'\0',len+1);
        nMessage->size=len;
        nMessage->next=NULL;
        strcpy(nMessage->message, buffer);
        msg_t* ptr=curr->write_msg_head;



        if(ptr==NULL){
            curr->write_msg_tail=nMessage;
            curr->write_msg_head=nMessage;
        }

        else{
            curr->write_msg_tail->next=nMessage;
            nMessage->prev=curr->write_msg_tail;
            curr->write_msg_tail=nMessage;
        }

        FD_SET(curr->fd, &pool->write_set);

    }

    return 0;
}

int write_to_client(int sd,conn_pool_t* pool) {//Writing for the client
msg_t* ptr;
ssize_t writeValue;

for(conn_t* curr=pool->conn_head; curr!=NULL; curr=curr->next ){
        if(curr->write_msg_head!=NULL ){ //If there is something to write
          if(curr->fd==sd){
              ptr=curr->write_msg_head;
              writeValue= write(sd, curr->write_msg_head->message, curr->write_msg_head->size);
              if(writeValue> -1){
                  curr->write_msg_head=curr->write_msg_head->next;
                  free(ptr->message);
                  free(ptr);
              }

              if(curr->write_msg_head==NULL){
                  curr->write_msg_tail=NULL;
                  FD_CLR(curr->fd, &pool->write_set);
              }
          }
          }

}

    return 0;
}






