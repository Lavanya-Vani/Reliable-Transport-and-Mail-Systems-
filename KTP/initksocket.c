/*
 Group Details:
  Member 1 Name: M Lekhya Reddy
  Member 1 Roll number: 23CS10039
  Member 2 Name: Y. Lavanya Vani
  Member 2 Roll number: 23CS10082
 */


#include "ksocket.h"
#include <sys/wait.h>
#include <sys/select.h>

static int        udp_fds[MAX_KTP_SOCKETS];
static SharedMem *sm    = NULL;
static int        semid = -1;


static int count_free_recv(KTPSocket *ks)
{
    int n = 0;
    for (int i = 0; i < RECV_BUF_SIZE; i++)
        if (!ks->recv_buf[i].occupied) n++;
    return n;
}

static void service_handshakes(void)
{
    for (int i = 0; i < MAX_KTP_SOCKETS; i++) {

        sem_lock(semid, i);
        int st = sm->sockets[i].state;
        sem_unlock(semid, i);

        if (st == SLOT_REQUESTED) {
            if (udp_fds[i] > 0) { close(udp_fds[i]); udp_fds[i] = -1; }
            int fd = socket(AF_INET, SOCK_DGRAM, 0);
            sem_lock(semid, i);
            if (fd >= 0) {
                udp_fds[i] = fd;
                sm->sockets[i].state = SLOT_READY;
                printf("[init] Slot %d: UDP fd=%d created\n", i, fd);
            } else {
                perror("[init] socket");
            }
            sem_unlock(semid, i);
        }
        else if (st == SLOT_BIND_NEEDED) {
            sem_lock(semid, i);
            struct sockaddr_in addr = sm->sockets[i].src_addr;
            sem_unlock(semid, i);

            int r = bind(udp_fds[i], (struct sockaddr *)&addr, sizeof(addr));

            sem_lock(semid, i);
            if (r < 0) {
                perror("[init] bind");
            } else {
                sm->sockets[i].is_bound = 1;
                sm->sockets[i].state    = SLOT_BOUND;
                char ip[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip));
                printf("[init] Slot %d: bound %s:%d\n",
                       i, ip, ntohs(addr.sin_port));
            }
            sem_unlock(semid, i);
        }
        else if (st == SLOT_CLOSING) {
            if (udp_fds[i] > 0) { close(udp_fds[i]); udp_fds[i] = -1; }
            sem_lock(semid, i);
            memset(&sm->sockets[i], 0, sizeof(KTPSocket));
            sm->sockets[i].state = SLOT_FREE;
            sem_unlock(semid, i);
            printf("[init] Slot %d: freed\n", i);
        }
    }
}

// Thread R - receives UDP messages, delivers data and send ACKs
void *thread_R(void *arg)
{
    (void)arg;
    char pkt[TOTAL_MSG_SIZE + 64];
    srand((unsigned int)time(NULL) ^ (unsigned int)pthread_self());

    while (1) {
        service_handshakes();

        fd_set rset;
        FD_ZERO(&rset);
        int maxfd = -1;
        for (int i = 0; i < MAX_KTP_SOCKETS; i++) {
            if (udp_fds[i] > 0 && sm->sockets[i].state == SLOT_BOUND) {
                FD_SET(udp_fds[i], &rset);
                if (udp_fds[i] > maxfd) maxfd = udp_fds[i];
            }
        }
        if (maxfd < 0) {
            struct timespec ts = { 0, 50000000L };
            nanosleep(&ts, NULL);
            continue;
        }

        struct timeval tv = { 1, 0 };  
        int ready = select(maxfd + 1, &rset, NULL, NULL, &tv);
        if (ready < 0) { if (errno == EINTR) continue; perror("[R] select"); continue; }

        if (ready == 0) {
            for (int i = 0; i < MAX_KTP_SOCKETS; i++) {
                if (udp_fds[i] <= 0) continue;

                int     do_send  = 0;
                uint8_t seq_out  = 0, rwnd_out = 0;
                struct sockaddr_in dst;

                sem_lock(semid, i);
                KTPSocket *ks = &sm->sockets[i];
                if (ks->state == SLOT_BOUND && ks->nospace_flag) {
                    int f = count_free_recv(ks);
                    if (f > 0) {
                        ks->rwnd.size = f;
                        // nospace_flag cleared only when next DATA arrives, so a lost DUPACK doesn't cause permanent deadlock 
                        seq_out  = ks->rwnd.last_inorder_seq;
                        rwnd_out = (uint8_t)f;
                        dst      = ks->dst_addr;
                        do_send  = 1;
                        printf("[R] Slot %d: nospace recovery rwnd=%d\n", i, f);
                    }
                }
                sem_unlock(semid, i);

                if (do_send) {
                    KTPHeader h = { KTP_DUPACK, seq_out, rwnd_out, 0 };
                    sendto(udp_fds[i], &h, HEADER_SIZE, 0,
                           (struct sockaddr *)&dst, sizeof(dst));
                }
            }
            continue;
        }

        // processes each ready socket 
        for (int i = 0; i < MAX_KTP_SOCKETS; i++) {
            if (udp_fds[i] <= 0 || !FD_ISSET(udp_fds[i], &rset)) continue;

            struct sockaddr_in from;
            socklen_t fromlen = sizeof(from);
            ssize_t n = recvfrom(udp_fds[i], pkt, sizeof(pkt), 0,
                                 (struct sockaddr *)&from, &fromlen);
            if (n < (ssize_t)HEADER_SIZE) continue;

            // packet dropping 
            if (dropMessage(DROP_PROB)) {
                printf("[R] Slot %d: simulated drop\n", i);
                continue;
            }

            KTPHeader *hdr = (KTPHeader *)pkt;

            int     do_ack   = 0;
            uint8_t ack_seq  = 0;
            uint8_t ack_rwnd = 0;
            struct sockaddr_in dst_copy;

            sem_lock(semid, i);
            KTPSocket *ks = &sm->sockets[i];

            if (hdr->type == KTP_DATA) {
                uint8_t seq = hdr->seq_num;

                // A packet arrived — safe to clear nospace now 
                ks->nospace_flag = 0;

                // Buffer full — drop, mark flag, sender will retry 
                if (ks->rwnd.size == 0) {
                    ks->nospace_flag = 1;
                    sem_unlock(semid, i);
                    continue;
                }

                uint8_t dist = (uint8_t)(seq - ks->rwnd.expected_seq);

                // dist >= 128: packet is from a previous window (old retransmit).
                // Re-ACK last good seq so sender can advance, then drop. 
                if (dist >= 128) {
                    ack_seq  = ks->rwnd.last_inorder_seq;
                    ack_rwnd = (uint8_t)count_free_recv(ks);
                    dst_copy = ks->dst_addr;
                    sem_unlock(semid, i);
                    KTPHeader h = { KTP_ACK, ack_seq, ack_rwnd, 0 };
                    sendto(udp_fds[i], &h, HEADER_SIZE, 0,
                           (struct sockaddr *)&dst_copy, sizeof(dst_copy));
                    continue;
                }

                // dist >= rwnd.size: beyond current window, drop silently 
                if (dist >= (uint8_t)ks->rwnd.size) {
                    sem_unlock(semid, i);
                    continue;
                }

                // Duplicate within current window: re-ACK so sender slides 
                if (ks->rwnd.received[seq]) {
                    ack_seq  = ks->rwnd.last_inorder_seq;
                    ack_rwnd = (uint8_t)count_free_recv(ks);
                    dst_copy = ks->dst_addr;
                    sem_unlock(semid, i);
                    KTPHeader h = { KTP_ACK, ack_seq, ack_rwnd, 0 };
                    sendto(udp_fds[i], &h, HEADER_SIZE, 0,
                           (struct sockaddr *)&dst_copy, sizeof(dst_copy));
                    continue;
                }

                // Record whether this is the in-order packet 
                int is_in_order = (seq == ks->rwnd.expected_seq);

                /* Find a free recv slot */
                int slot = -1;
                for (int b = 0; b < RECV_BUF_SIZE; b++) {
                    if (!ks->recv_buf[b].occupied) { slot = b; break; }
                }
                if (slot < 0) {
                    ks->nospace_flag = 1;
                    sem_unlock(semid, i);
                    continue;
                }

                // Store message payload 
                ks->recv_buf[slot].occupied = 1;
                ks->recv_buf[slot].seq_num  = seq;
                size_t plen = (size_t)(n - (ssize_t)HEADER_SIZE);
                memset(ks->recv_buf[slot].data, 0, MSG_SIZE);
                memcpy(ks->recv_buf[slot].data, pkt + HEADER_SIZE,
                       plen < MSG_SIZE ? plen : MSG_SIZE);

                
                ks->rwnd.received[seq] = 1;

                while (ks->rwnd.received[ks->rwnd.expected_seq]) {
                    ks->rwnd.received[ks->rwnd.expected_seq] = 0;
                    ks->rwnd.last_inorder_seq = ks->rwnd.expected_seq;
                    ks->rwnd.expected_seq++;
                    /* Wrap: sender skips 0, so receiver must too */
                    if (ks->rwnd.expected_seq == 0)
                        ks->rwnd.expected_seq = 1;
                }

                int free_slots = count_free_recv(ks);
                ks->rwnd.size = free_slots;
                if (free_slots == 0) ks->nospace_flag = 1;

                ack_seq  = ks->rwnd.last_inorder_seq;
                ack_rwnd = (uint8_t)free_slots;
                dst_copy = ks->dst_addr;
                do_ack   = 1;
                sem_unlock(semid, i);

                // ACK only for in-order messages
                if (is_in_order) {
                    KTPHeader h = { KTP_ACK, ack_seq, ack_rwnd, 0 };
                    sendto(udp_fds[i], &h, HEADER_SIZE, 0,
                           (struct sockaddr *)&dst_copy, sizeof(dst_copy));
                }
                (void)do_ack;
            }

            else if (hdr->type == KTP_ACK) {
                uint8_t acked    = hdr->seq_num;
                uint8_t new_rwnd = hdr->rwnd_size;

                // Cumulative ACK
                int j = 0;
                while (j < ks->swnd.unacked_count) {
                    uint8_t s    = ks->swnd.unacked[j];
                    uint8_t dist = (uint8_t)(acked - s);
                    if (dist <= 128) {
                        for (int b = 0; b < SEND_BUF_SIZE; b++) {
                            if (ks->send_buf[b].occupied &&
                                ks->send_buf[b].seq_num == s) {
                                ks->send_buf[b].occupied     = 0;
                                ks->send_buf[b].sent         = 0;
                                ks->send_buf[b].seq_num      = 0;
                                ks->send_buf[b].insert_order = 0;
                                break;
                            }
                        }
                        memmove(&ks->swnd.unacked[j],
                                &ks->swnd.unacked[j + 1],
                                (size_t)(ks->swnd.unacked_count - j - 1));
                        ks->swnd.unacked_count--;
                    } else {
                        j++;
                    }
                }
                // Update swnd from piggybacked rwnd; cap at RECV_BUF_SIZE 
                ks->swnd.size = (new_rwnd < RECV_BUF_SIZE)
                                ? new_rwnd : RECV_BUF_SIZE;
                sem_unlock(semid, i);
            }

            // Duplicate ACK
            else if (hdr->type == KTP_DUPACK) {
                uint8_t new_rwnd = hdr->rwnd_size;
                ks->swnd.size = (new_rwnd < RECV_BUF_SIZE)
                                ? new_rwnd : RECV_BUF_SIZE;
                sem_unlock(semid, i);
            }
            else {
                sem_unlock(semid, i);
            }
        }
    }
    return NULL;
}

 // Thread S — send new messages + retransmit on timeout

void *thread_S(void *arg)
{
    (void)arg;

    while (1) {
        struct timespec ts = { 0, 50000000L }; //50 ms
        nanosleep(&ts, NULL);

        time_t now = time(NULL);

        for (int i = 0; i < MAX_KTP_SOCKETS; i++) {
            if (udp_fds[i] <= 0) continue;

            char   pkts[SEND_BUF_SIZE][TOTAL_MSG_SIZE];
            int    npkts   = 0;
            int    do_rexmt = 0;
            struct sockaddr_in dst;

            sem_lock(semid, i);
            KTPSocket *ks = &sm->sockets[i];
            if (ks->state != SLOT_BOUND) { sem_unlock(semid, i); continue; }
            dst = ks->dst_addr;

            // Retransmission on timeout 
            if (ks->swnd.unacked_count > 0 && ks->swnd.last_sent_time > 0 &&
                difftime(now, ks->swnd.last_sent_time) >= (double)T) {

                printf("[S] Slot %d: TIMEOUT — retransmitting %d msgs\n",
                       i, ks->swnd.unacked_count);
                do_rexmt = 1;

                for (int u = 0; u < ks->swnd.unacked_count && npkts < SEND_BUF_SIZE; u++) {
                    uint8_t seq = ks->swnd.unacked[u];
                    for (int b = 0; b < SEND_BUF_SIZE; b++) {
                        if (ks->send_buf[b].occupied &&
                            ks->send_buf[b].seq_num == seq) {
                            KTPHeader *h = (KTPHeader *)pkts[npkts];
                            h->type      = KTP_DATA;
                            h->seq_num   = seq;
                            h->rwnd_size = 0;
                            h->pad       = 0;
                            memcpy(pkts[npkts] + HEADER_SIZE,
                                   ks->send_buf[b].data, MSG_SIZE);
                            npkts++;
                            break;
                        }
                    }
                }
                ks->swnd.last_sent_time = now;
            }
// Sends new unsent messages in-order
            if (!do_rexmt) {
                int can_send = ks->swnd.size - ks->swnd.unacked_count;

                while (can_send > 0) {
                    int  best_b     = -1;
                    long min_order  = -1;
                    for (int b = 0; b < SEND_BUF_SIZE; b++) {
                        if (ks->send_buf[b].occupied && !ks->send_buf[b].sent) {
                            if (best_b < 0 ||
                                ks->send_buf[b].insert_order < min_order) {
                                best_b    = b;
                                min_order = ks->send_buf[b].insert_order;
                            }
                        }
                    }
                    if (best_b < 0) break;

                    uint8_t seq = (uint8_t)ks->swnd.next_seq;
                    ks->swnd.next_seq++;
                    if (ks->swnd.next_seq == 0 ||
                        ks->swnd.next_seq >= MAX_SEQ_NUM)
                        ks->swnd.next_seq = 1;

                    ks->send_buf[best_b].seq_num = seq;
                    ks->send_buf[best_b].sent    = 1;

                    KTPHeader *h = (KTPHeader *)pkts[npkts];
                    h->type      = KTP_DATA;
                    h->seq_num   = seq;
                    h->rwnd_size = 0;
                    h->pad       = 0;
                    memcpy(pkts[npkts] + HEADER_SIZE,
                           ks->send_buf[best_b].data, MSG_SIZE);
                    npkts++;

                    ks->swnd.unacked[ks->swnd.unacked_count++] = seq;
                    ks->swnd.last_sent_time = now;
                    can_send--;
                }
            }

            sem_unlock(semid, i);  

            for (int p = 0; p < npkts; p++) {
                sendto(udp_fds[i], pkts[p], TOTAL_MSG_SIZE, 0,
                       (struct sockaddr *)&dst, sizeof(dst));
            }
        }
    }
    return NULL;
}

static void garbage_collector(void)
{
    while (1) {
        sleep(5);
        for (int i = 0; i < MAX_KTP_SOCKETS; i++) {
            sem_lock(semid, i);
            KTPSocket *ks = &sm->sockets[i];
            if (ks->state != SLOT_FREE && ks->pid > 0) {
                if (kill(ks->pid, 0) < 0 && errno == ESRCH) {
                    printf("[GC] Slot %d: pid %d dead, cleaning up\n",
                           i, ks->pid);
                    ks->state = SLOT_CLOSING;
                }
            }
            sem_unlock(semid, i);
        }
    }
}

static void cleanup(int sig)
{
    (void)sig;
    printf("\n[init] Shutting down...\n");
    for (int i = 0; i < MAX_KTP_SOCKETS; i++)
        if (udp_fds[i] > 0) close(udp_fds[i]);
    int shmid = shmget(SHM_KEY, sizeof(SharedMem), 0666);
    if (sm) shmdt(sm);
    if (shmid >= 0) shmctl(shmid, IPC_RMID, NULL);
    if (semid >= 0) semctl(semid, 0, IPC_RMID);
    exit(0);
}

int main(void)
{
    srand((unsigned int)time(NULL));
    printf("[init] KTP init PID=%d\n", getpid());
    signal(SIGINT, cleanup);
    signal(SIGTERM, cleanup);

    for (int i = 0; i < MAX_KTP_SOCKETS; i++) udp_fds[i] = -1;

    int old_shm = shmget(SHM_KEY, sizeof(SharedMem), 0666);
    if (old_shm >= 0) { shmdt(NULL); shmctl(old_shm, IPC_RMID, NULL); }
    int old_sem = semget(SEM_KEY, MAX_KTP_SOCKETS, 0666);
    if (old_sem >= 0) semctl(old_sem, 0, IPC_RMID);

    sm = get_shared_memory(1);
    if (!sm) { fprintf(stderr, "[init] shm failed\n"); exit(1); }
    for (int i = 0; i < MAX_KTP_SOCKETS; i++) sm->sockets[i].state = SLOT_FREE;

    semid = get_semaphores(1);
    if (semid < 0) { fprintf(stderr, "[init] sem failed\n"); exit(1); }
    printf("[init] IPC ready.\n");

    pid_t gc = fork();
    if (gc < 0) { perror("fork"); exit(1); }
    if (gc == 0) { garbage_collector(); exit(0); }
    printf("[init] GC pid=%d\n", gc);

    pthread_t tid_R, tid_S;
    pthread_create(&tid_R, NULL, thread_R, NULL);
    pthread_create(&tid_S, NULL, thread_S, NULL);
    printf("[init] Threads R and S running. Ready.\n");

    pthread_join(tid_R, NULL);
    pthread_join(tid_S, NULL);
    waitpid(gc, NULL, 0);
    return 0;
}
