/*
 Group Details:
  Member 1 Name: M Lekhya Reddy
  Member 1 Roll number: 23CS10039
  Member 2 Name: Y. Lavanya Vani
  Member 2 Roll number: 23CS10082
 */

#include "ksocket.h"

int k_errno = 0;

SharedMem *get_shared_memory(int create)
{
    int shmid = create
        ? shmget(SHM_KEY, sizeof(SharedMem), IPC_CREAT | 0666)
        : shmget(SHM_KEY, sizeof(SharedMem), 0666);
    if (shmid < 0) { perror("shmget"); return NULL; }
    SharedMem *sm = (SharedMem *)shmat(shmid, NULL, 0);
    if (sm == (SharedMem *)-1) { perror("shmat"); return NULL; }
    return sm;
}

int get_semaphores(int create)
{
    int semid;
    if (create) {
        semid = semget(SEM_KEY, MAX_KTP_SOCKETS, IPC_CREAT | 0666);
        if (semid < 0) { perror("semget"); return -1; }
        union { int val; struct semid_ds *buf; unsigned short *array; } arg;
        unsigned short vals[MAX_KTP_SOCKETS];
        for (int i = 0; i < MAX_KTP_SOCKETS; i++) vals[i] = 1;
        arg.array = vals;
        semctl(semid, 0, SETALL, arg);
    } else {
        semid = semget(SEM_KEY, MAX_KTP_SOCKETS, 0666);
    }
    return semid;
}

void sem_lock(int semid, int idx)
{
    struct sembuf sb = { (unsigned short)idx, -1, 0 };
    semop(semid, &sb, 1);
}

void sem_unlock(int semid, int idx)
{
    struct sembuf sb = { (unsigned short)idx, 1, 0 };
    semop(semid, &sb, 1);
}


int dropMessage(float p)
{
    return ((float)rand() / (float)RAND_MAX) < p ? 1 : 0;
}

int k_socket(int domain, int type, int protocol)
{
    (void)domain; (void)protocol;
    if (type != SOCK_KTP) { k_errno = EINVAL; return -1; }

    SharedMem *sm = get_shared_memory(0);
    if (!sm) { k_errno = ENOSPACE; return -1; }
    int semid = get_semaphores(0);
    if (semid < 0) { k_errno = ENOSPACE; return -1; }

    int slot = -1;
    for (int i = 0; i < MAX_KTP_SOCKETS; i++) {
        sem_lock(semid, i);
        if (sm->sockets[i].state == SLOT_FREE) {
            memset(&sm->sockets[i], 0, sizeof(KTPSocket));
            sm->sockets[i].state             = SLOT_REQUESTED;
            sm->sockets[i].pid               = getpid();
            sm->sockets[i].swnd.next_seq     = 1;
            sm->sockets[i].swnd.size         = INIT_SWND_SIZE;
            sm->sockets[i].rwnd.size         = RECV_BUF_SIZE;
            sm->sockets[i].rwnd.expected_seq = 1;
            sm->sockets[i].deliver_seq       = 1;
            sm->sockets[i].send_order        = 0;
            slot = i;
            sem_unlock(semid, i);
            break;
        }
        sem_unlock(semid, i);
    }
    if (slot < 0) { k_errno = ENOSPACE; return -1; }

    while (1) {
        sem_lock(semid, slot);
        int st = sm->sockets[slot].state;
        sem_unlock(semid, slot);
        if (st == SLOT_READY) break;
        struct timespec ts = { 0, 20000000L }; /* 20 ms */
        nanosleep(&ts, NULL);
    }
    return slot;
}

int k_bind(int ktp_fd, const char *src_ip, int src_port,
           const char *dst_ip, int dst_port)
{
    if (ktp_fd < 0 || ktp_fd >= MAX_KTP_SOCKETS) { errno = EBADF; return -1; }
    SharedMem *sm = get_shared_memory(0);
    if (!sm) return -1;
    int semid = get_semaphores(0);
    if (semid < 0) return -1;

    sem_lock(semid, ktp_fd);
    KTPSocket *ks = &sm->sockets[ktp_fd];
    if (ks->state == SLOT_FREE) { sem_unlock(semid, ktp_fd); errno = EBADF; return -1; }

    memset(&ks->src_addr, 0, sizeof(ks->src_addr));
    ks->src_addr.sin_family = AF_INET;
    ks->src_addr.sin_port   = htons((uint16_t)src_port);
    if (inet_pton(AF_INET, src_ip, &ks->src_addr.sin_addr) <= 0) {
        sem_unlock(semid, ktp_fd); return -1;
    }

    memset(&ks->dst_addr, 0, sizeof(ks->dst_addr));
    ks->dst_addr.sin_family = AF_INET;
    ks->dst_addr.sin_port   = htons((uint16_t)dst_port);
    if (inet_pton(AF_INET, dst_ip, &ks->dst_addr.sin_addr) <= 0) {
        sem_unlock(semid, ktp_fd); return -1;
    }

    ks->state = SLOT_BIND_NEEDED;
    sem_unlock(semid, ktp_fd);

    while (1) {
        sem_lock(semid, ktp_fd);
        int st = sm->sockets[ktp_fd].state;
        sem_unlock(semid, ktp_fd);
        if (st == SLOT_BOUND) break;
        struct timespec ts = { 0, 20000000L };
        nanosleep(&ts, NULL);
    }
    return 0;
}

int k_sendto(int ktp_fd, const void *buf, size_t len)
{
    if (ktp_fd < 0 || ktp_fd >= MAX_KTP_SOCKETS) { k_errno = EBADF; return -1; }
    SharedMem *sm = get_shared_memory(0);
    if (!sm) return -1;
    int semid = get_semaphores(0);
    if (semid < 0) return -1;

    sem_lock(semid, ktp_fd);
    KTPSocket *ks = &sm->sockets[ktp_fd];

    if (ks->state != SLOT_BOUND) {
        sem_unlock(semid, ktp_fd); k_errno = ENOTBOUND; return -1;
    }

    int slot = -1;
    for (int i = 0; i < SEND_BUF_SIZE; i++) {
        if (!ks->send_buf[i].occupied) { slot = i; break; }
    }
    if (slot < 0) {
        sem_unlock(semid, ktp_fd); k_errno = ENOSPACE; return -1;
    }

    size_t n = (len < MSG_SIZE) ? len : MSG_SIZE;
    memset(ks->send_buf[slot].data, 0, MSG_SIZE);
    memcpy(ks->send_buf[slot].data, buf, n);
    ks->send_buf[slot].occupied     = 1;
    ks->send_buf[slot].sent         = 0;
    ks->send_buf[slot].seq_num      = 0;
    ks->send_buf[slot].insert_order = ++ks->send_order; /* FIFO ordering */

    sem_unlock(semid, ktp_fd);
    return (int)n;
}


int k_recvfrom(int ktp_fd, void *buf, size_t len)
{
    if (ktp_fd < 0 || ktp_fd >= MAX_KTP_SOCKETS) { k_errno = EBADF; return -1; }
    SharedMem *sm = get_shared_memory(0);
    if (!sm) return -1;
    int semid = get_semaphores(0);
    if (semid < 0) return -1;

    sem_lock(semid, ktp_fd);
    KTPSocket *ks = &sm->sockets[ktp_fd];

    int slot = -1;
    for (int i = 0; i < RECV_BUF_SIZE; i++) {
        if (ks->recv_buf[i].occupied &&
            ks->recv_buf[i].seq_num == ks->deliver_seq) {
            slot = i;
            break;
        }
    }

    if (slot < 0) {
        sem_unlock(semid, ktp_fd);
        k_errno = ENOMESSAGE;
        return -1;
    }

    size_t n = (len < MSG_SIZE) ? len : MSG_SIZE;
    memcpy(buf, ks->recv_buf[slot].data, n);

    ks->recv_buf[slot].occupied     = 0;
    ks->recv_buf[slot].insert_order = 0;

    ks->deliver_seq++;
    if (ks->deliver_seq == 0) ks->deliver_seq = 1;

    ks->rwnd.size++;
    if (ks->rwnd.size > RECV_BUF_SIZE) ks->rwnd.size = RECV_BUF_SIZE;

    sem_unlock(semid, ktp_fd);
    return (int)n;
}


int k_close(int ktp_fd)
{
    if (ktp_fd < 0 || ktp_fd >= MAX_KTP_SOCKETS) { errno = EBADF; return -1; }
    SharedMem *sm = get_shared_memory(0);
    if (!sm) return -1;
    int semid = get_semaphores(0);
    if (semid < 0) return -1;

    sem_lock(semid, ktp_fd);
    if (sm->sockets[ktp_fd].state != SLOT_FREE)
        sm->sockets[ktp_fd].state = SLOT_CLOSING;
    sem_unlock(semid, ktp_fd);

    while (1) {
        sem_lock(semid, ktp_fd);
        int st = sm->sockets[ktp_fd].state;
        sem_unlock(semid, ktp_fd);
        if (st == SLOT_FREE) break;
        struct timespec ts = { 0, 20000000L };
        nanosleep(&ts, NULL);
    }
    return 0;
}
