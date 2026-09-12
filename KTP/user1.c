/*
 Group Details:
  Member 1 Name: M Lekhya Reddy
  Member 1 Roll number: 23CS10039
  Member 2 Name: Y. Lavanya Vani
  Member 2 Roll number: 23CS10082
 */


#include "ksocket.h"

// Send file
static int send_file(int kfd, const char *fname)
{
    FILE *fp = fopen(fname, "rb");
    if (!fp) { perror("fopen"); return -1; }

    fseek(fp, 0, SEEK_END);
    long fsize = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    long total_msgs = (fsize + MSG_SIZE - 1) / MSG_SIZE;
    if (total_msgs == 0) total_msgs = 1;

    printf("[user1] Sending '%s': %ld bytes, %ld messages\n",
           fname, fsize, total_msgs);

    char buf[MSG_SIZE];
    long sent = 0;

    while (sent < total_msgs) {
        size_t n = fread(buf, 1, MSG_SIZE, fp);
        if (n == 0) {
            if (sent == 0) { memset(buf, 0, MSG_SIZE); n = MSG_SIZE; }
            else break;
        }
        if (n < MSG_SIZE) memset(buf + n, 0, MSG_SIZE - n);

        sent++;

        int ret;
        do {
            ret = k_sendto(kfd, buf, MSG_SIZE);
            if (ret < 0 && k_errno == ENOSPACE) {
                struct timespec ts = { 0, 10000000L }; 
                nanosleep(&ts, NULL);
            }
        } while (ret < 0 && k_errno == ENOSPACE);

        if (ret < 0) {
            fprintf(stderr, "[user1] k_sendto error (k_errno=%d) at msg %ld\n",
                    k_errno, sent);
            fclose(fp);
            return -1;
        }

        if (sent % 50 == 0 || sent == total_msgs)
            printf("[user1] Queued %ld / %ld messages\n", sent, total_msgs);
    }
    fclose(fp);

    printf("[user1] All %ld messages queued. Waiting for ACKs...\n", sent);

    // Wait until send buffer is fully drained (all ACKed) 
    SharedMem *shm = get_shared_memory(0);
    int wsem = get_semaphores(0);

    while (1) {
        sem_lock(wsem, kfd);
        KTPSocket *ks = &shm->sockets[kfd];
        int pending = 0;
        for (int i = 0; i < SEND_BUF_SIZE; i++)
            if (ks->send_buf[i].occupied) pending++;
        int unacked = ks->swnd.unacked_count;
        sem_unlock(wsem, kfd);

        if (pending == 0 && unacked == 0) break;

        printf("[user1] Draining: %d buffered, %d unACKed...\n",
               pending, unacked);
        sleep(1);
    }

    printf("[user1] '%s' sent and fully acknowledged.\n\n", fname);
    return 0;
}

int main(int argc, char *argv[])
{
    if (argc < 5) {
        fprintf(stderr,
                "Usage: %s <src_ip> <src_port> <dst_ip> <dst_port>\n",
                argv[0]);
        return 1;
    }

    const char *src_ip   = argv[1];
    int         src_port = atoi(argv[2]);
    const char *dst_ip   = argv[3];
    int         dst_port = atoi(argv[4]);

    int kfd = k_socket(AF_INET, SOCK_KTP, 0);
    if (kfd < 0) {
        fprintf(stderr, "[user1] k_socket failed (k_errno=%d)\n", k_errno);
        return 1;
    }
    printf("[user1] k_socket OK fd=%d\n", kfd);

    if (k_bind(kfd, src_ip, src_port, dst_ip, dst_port) < 0) {
        fprintf(stderr, "[user1] k_bind failed\n");
        k_close(kfd);
        return 1;
    }
    printf("[user1] k_bind OK: %s:%d -> %s:%d\n",
           src_ip, src_port, dst_ip, dst_port);

    printf("\n[user1] Ready. Type a filename to send, or EXIT to quit.\n\n");

    char input[512];
    while (1) {
        printf("Enter filename: ");
        fflush(stdout);

        if (fgets(input, sizeof(input), stdin) == NULL) {
            break;
        }

        input[strcspn(input, "\r\n")] = '\0';

        if (strcasecmp(input, "EXIT") == 0) {
            printf("[user1] Exiting.\n");
            break;
        }

        if (strlen(input) == 0) continue;

        send_file(kfd, input);
    }

    k_close(kfd);
    return 0;
}
