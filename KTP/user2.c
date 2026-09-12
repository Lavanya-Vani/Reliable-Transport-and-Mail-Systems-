/*
 Group Details:
  Member 1 Name: M Lekhya Reddy
  Member 1 Roll number: 23CS10039
  Member 2 Name: Y. Lavanya Vani
  Member 2 Roll number: 23CS10082
 */

#include "ksocket.h"

// Shared exit flag between receive thread and stdin thread 
static volatile int exit_flag = 0;


static void *stdin_watcher(void *arg)
{
    (void)arg;
    char line[64];
    while (1) {
        if (fgets(line, sizeof(line), stdin) == NULL) {
            exit_flag = 1;
            break;
        }
        line[strcspn(line, "\r\n")] = '\0';
        if (strcasecmp(line, "EXIT") == 0) {
            printf("\n[user2] EXIT received. Closing...\n");
            exit_flag = 1;
            break;
        }
    }
    return NULL;
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
        fprintf(stderr, "[user2] k_socket failed (k_errno=%d)\n", k_errno);
        return 1;
    }
    printf("[user2] k_socket OK fd=%d\n", kfd);

    if (k_bind(kfd, src_ip, src_port, dst_ip, dst_port) < 0) {
        fprintf(stderr, "[user2] k_bind failed\n");
        k_close(kfd);
        return 1;
    }
    printf("[user2] k_bind OK: %s:%d -> %s:%d\n",
           src_ip, src_port, dst_ip, dst_port);

    FILE *outfp = fopen("received_output.txt", "wb");
    if (!outfp) { perror("fopen"); k_close(kfd); return 1; }

    printf("\n[user2] Waiting for data. Type EXIT to quit.\n");
    printf("============================================================\n");
    printf("  RECEIVED DATA\n");
    printf("============================================================\n");

    pthread_t tid;
    pthread_create(&tid, NULL, stdin_watcher, NULL);

    char   buf[MSG_SIZE + 1];
    int    total_msgs  = 0;
    long   total_bytes = 0;

    while (!exit_flag) {
        memset(buf, 0, sizeof(buf));
        int ret = k_recvfrom(kfd, buf, MSG_SIZE);

        if (ret < 0) {
            struct timespec ts = { 0, 10000000L }; /* 10 ms */
            nanosleep(&ts, NULL);
            continue;
        }

        total_msgs++;
        total_bytes += MSG_SIZE;

        fwrite(buf, 1, MSG_SIZE, outfp);
        fflush(outfp);

        printf("[Msg #%4d] ", total_msgs);
        for (int c = 0; c < MSG_SIZE && buf[c] != '\0'; c++) {
            unsigned char ch = (unsigned char)buf[c];
            if (ch >= 32 && ch < 127)                        putchar(ch);
            else if (ch == '\n' || ch == '\r' || ch == '\t') putchar(ch);
            else                                              putchar('.');
        }
        putchar('\n');
        fflush(stdout);

        if (total_msgs % 20 == 0)
            printf("--- [%d msgs / %ld bytes received so far] ---\n",
                   total_msgs, total_bytes);
    }

    fclose(outfp);
    pthread_join(tid, NULL);

    printf("\n============================================================\n");
    printf("[user2] Total: %d messages, %ld bytes\n", total_msgs, total_bytes);
    printf("[user2] Saved to: received_output.txt\n");
    printf("============================================================\n");

    k_close(kfd);
    return 0;
}
