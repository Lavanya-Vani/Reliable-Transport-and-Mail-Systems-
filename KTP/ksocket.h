/*
 Group Details:
  Member 1 Name: M Lekhya Reddy
  Member 1 Roll number: 23CS10039
  Member 2 Name: Y. Lavanya Vani
  Member 2 Roll number: 23CS10082
 */

#ifndef KSOCKET_H
#define KSOCKET_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/sem.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <signal.h>

#define T           5       
#define DROP_PROB   0.05f   

#define MAX_KTP_SOCKETS  10
#define MSG_SIZE         512
#define RECV_BUF_SIZE    10
#define SEND_BUF_SIZE    50   // sender-side app queue
#define MAX_SEQ_NUM      256  // 8-bit sequence space                              
#define INIT_SWND_SIZE   10   // initial swnd = RECV_BUF_SIZE per spec             

#define SHM_KEY   0x4B545031
#define SEM_KEY   0x4B545032

#define SOCK_KTP  100

#define ENOSPACE    200   // no free SM slot or send buffer full  
#define ENOTBOUND   201   // dst IP/Port doesn't match k_bind()   
#define ENOMESSAGE  202   // no message in recv buffer yet        

#define KTP_DATA    0x01
#define KTP_ACK     0x02
#define KTP_DUPACK  0x04

#define SLOT_FREE         0
#define SLOT_REQUESTED    1   // k_socket() posted, init must open UDP  
#define SLOT_READY        2   // init opened UDP socket                 
#define SLOT_BIND_NEEDED  3   // k_bind() posted addresses              
#define SLOT_BOUND        4   // init bound UDP socket, ready to use    
#define SLOT_CLOSING      5   // k_close() called                       

typedef struct {
    uint8_t type;       // KTP_DATA / KTP_ACK / KTP_DUPACK          
    uint8_t seq_num;    // seq# (data) or last in-order ACKed (ack) 
    uint8_t rwnd_size;  // piggybacked receiver window size          
    uint8_t pad;        // reserved, set to 0                        
} KTPHeader;

#define HEADER_SIZE     sizeof(KTPHeader)
#define TOTAL_MSG_SIZE  (HEADER_SIZE + MSG_SIZE)

typedef struct {
    int     occupied;       // 1 = slot has data, 0 = free                
    uint8_t seq_num;        // sequence number assigned by Thread S        
    int     sent;           // 1 implies Thread S has transmitted this slot      
    long    insert_order;   // FIFO for sender 
    char    data[MSG_SIZE];
} MsgSlot;

typedef struct {
    int     size;                   // max unACKed msgs; equals rwnd from receiver 
    int     next_seq;               // next seq number to assign (1..255, skips 0) 
    uint8_t unacked[MAX_SEQ_NUM];   // seq nums sent but not yet ACKed             
    int     unacked_count;          // count of unacked messages 
    time_t  last_sent_time;         // wall-clock of last sendto() for timeout     
} SWnd;


typedef struct {
    int     size;                   // free recv_buf slots = current rwnd    
    uint8_t expected_seq;           // next in-order seq expected            
    uint8_t received[MAX_SEQ_NUM];  // bitmap: received[s]=1 means s stored  
    uint8_t last_inorder_seq;       // last consecutively received seq       
} RWnd;

typedef struct {
    int      state;                  
    pid_t    pid;     
    struct sockaddr_in src_addr;   
    struct sockaddr_in dst_addr;  
    int      is_bound;
    MsgSlot  send_buf[SEND_BUF_SIZE];  
    MsgSlot  recv_buf[RECV_BUF_SIZE];   
    SWnd     swnd;
    RWnd     rwnd;
    int      nospace_flag;  // recv buf full; Thread R sends DUPACK on recovery 
    long     send_order;    // monotonic counter stamped on each k_sendto() call 
    uint8_t  deliver_seq;   // next seq k_recvfrom() should hand to the app      
} KTPSocket;


typedef struct {
    KTPSocket sockets[MAX_KTP_SOCKETS];
} SharedMem;

extern int k_errno;

int k_socket(int domain, int type, int protocol);
int k_bind(int ktp_fd, const char *src_ip, int src_port, const char *dst_ip, int dst_port);
int k_sendto(int ktp_fd, const void *buf, size_t len);
int k_recvfrom(int ktp_fd, void *buf, size_t len);
int k_close(int ktp_fd);

int dropMessage(float p);

SharedMem *get_shared_memory(int create);
int        get_semaphores(int create);
void       sem_lock(int semid, int idx);
void       sem_unlock(int semid, int idx);

#endif /* KSOCKET_H */
