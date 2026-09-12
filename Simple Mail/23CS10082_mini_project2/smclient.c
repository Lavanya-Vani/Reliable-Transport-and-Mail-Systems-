/*
  Mini Project- 2
 Name : Yernagula Lavanya Vani
 Roll No. : 23CS10082
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

#define MAX_LINE    512
#define MAX_USERNAME 21
#define MAX_PASSWORD 31
#define NONCE_LEN    8



static char server_ip[64];
static int  server_port;


static int  g_fd = -1;
static char g_buf[MAX_LINE * 4];
static int  g_buf_len = 0;

static int connect_to_server(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return -1; }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(server_port);

    if (inet_pton(AF_INET, server_ip, &addr.sin_addr) <= 0) {
        struct hostent *he = gethostbyname(server_ip);
        if (!he) {
            fprintf(stderr, "ERROR: Cannot resolve '%s'\n", server_ip);
            close(fd);
            return -1;
        }
        memcpy(&addr.sin_addr, he->h_addr_list[0], he->h_length);
    }

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect");
        close(fd);
        return -1;
    }

    return fd;
}


static void net_send(int fd, const char *line) {
    char buf[MAX_LINE + 3];
    int len = snprintf(buf, sizeof(buf), "%s\r\n", line);
    int sent = 0;
    while (sent < len) {
        int n = send(fd, buf + sent, len - sent, 0);
        if (n <= 0) break;
        sent += n;
    }
}


static int net_recv_line(int fd, char *out, int max_out) {
    while (1) {
        /* Check if a complete line is already in the buffer */
        for (int i = 0; i < g_buf_len - 1; i++) {
            if (g_buf[i] == '\r' && g_buf[i+1] == '\n') {
                int copy = i < max_out - 1 ? i : max_out - 1;
                memcpy(out, g_buf, copy);
                out[copy] = '\0';
                /* Shift buffer */
                int consumed = i + 2;
                memmove(g_buf, g_buf + consumed, g_buf_len - consumed);
                g_buf_len -= consumed;
                return 1;
            }
        }

        /* Need more data */
        int space = (int)sizeof(g_buf) - g_buf_len - 1;
        if (space <= 0) return 0; /* buffer full – shouldn't happen */

        int n = recv(fd, g_buf + g_buf_len, space, 0);
        if (n <= 0) return 0;
        g_buf_len += n;
        g_buf[g_buf_len] = '\0';
    }
}


static int open_session(const char *mode) {
    g_buf_len = 0;
    int fd = connect_to_server();
    if (fd < 0) return -1;
    g_fd = fd;

    // Read WELCOME 
    char line[MAX_LINE];
    if (!net_recv_line(fd, line, sizeof(line))) {
        fprintf(stderr, "ERROR: No greeting from server\n");
        close(fd);
        return -1;
    }
    if (strncmp(line, "WELCOME", 7) != 0) {
        fprintf(stderr, "ERROR: Unexpected greeting: %s\n", line);
        close(fd);
        return -1;
    }

    // Send mode 
    net_send(fd, mode);
    if (!net_recv_line(fd, line, sizeof(line))) {
        fprintf(stderr, "ERROR: No response to MODE\n");
        close(fd);
        return -1;
    }
    if (strncmp(line, "OK", 2) != 0) {
        fprintf(stderr, "ERROR: Server rejected mode: %s\n", line);
        close(fd);
        return -1;
    }
    return fd;
}

// Close a session cleanly 
static void close_session(int fd) {
    if (fd >= 0) close(fd);
    g_fd      = -1;
    g_buf_len = 0;
}



static unsigned long djb2(const char *str) {
    unsigned long hash = 5381;
    int c;
    while ((c = (unsigned char)*str++))
        hash = ((hash << 5) + hash) + c;
    return hash;
}



static char *read_stdin(char *buf, int sz) {
    if (!fgets(buf, sz, stdin)) return NULL;
    buf[strcspn(buf, "\r\n")] = '\0';
    return buf;
}



static void send_mail_session(void) {
    int fd = open_session("MODE SEND");
    if (fd < 0) {
        printf("ERROR: Could not connect to server.\n");
        return;
    }

    char line[MAX_LINE];
    char input[MAX_LINE-6];

    printf("From (your name): ");
    fflush(stdout);
    if (!read_stdin(input, sizeof(input))) { close_session(fd); return; }

    char cmd[MAX_LINE];
    snprintf(cmd, sizeof(cmd), "FROM %s", input);
    net_send(fd, cmd);

    if (!net_recv_line(fd, line, sizeof(line))) { close_session(fd); return; }
    if (strncmp(line, "OK", 2) != 0) {
        printf("ERROR: %s\n", line);
        close_session(fd);
        return;
    }

    int accepted = 0;
    while (1) {
        printf("To (recipient username, empty line to finish): ");
        fflush(stdout);
        if (!read_stdin(input, sizeof(input))) break;
        if (strlen(input) == 0) break;

        snprintf(cmd, sizeof(cmd), "TO %s", input);
        net_send(fd, cmd);

        if (!net_recv_line(fd, line, sizeof(line))) { close_session(fd); return; }
        if (strncmp(line, "OK", 2) == 0) {
            printf("-> Recipient '%s' accepted.\n", input);
            accepted++;
        } else {
            printf("-> Error: user '%s' does not exist on this server.\n", input);
        }
    }

    if (accepted == 0) {
        printf("ERROR: No valid recipients. Mail not sent.\n");
        net_send(fd, "QUIT");
        net_recv_line(fd, line, sizeof(line)); /* read BYE */
        close_session(fd);
        return;
    }


    printf("Subject: ");
    fflush(stdout);
    if (!read_stdin(input, sizeof(input))) { close_session(fd); return; }
    snprintf(cmd, sizeof(cmd), "SUB %s", input);
    net_send(fd, cmd);

    if (!net_recv_line(fd, line, sizeof(line))) { close_session(fd); return; }
    if (strncmp(line, "OK", 2) != 0) {
        printf("ERROR: %s\n", line);
        close_session(fd);
        return;
    }

    net_send(fd, "BODY");
    if (!net_recv_line(fd, line, sizeof(line))) { close_session(fd); return; }
    if (strncmp(line, "OK", 2) != 0) {
        printf("ERROR: %s\n", line);
        close_session(fd);
        return;
    }

    printf("Body (type '.' on a line by itself to finish):\n");
    fflush(stdout);

    while (1) {
        if (!read_stdin(input, sizeof(input))) {
            // EOF from user – send dot to finish 
            net_send(fd, ".");
            break;
        }
        if (strcmp(input, ".") == 0) {
            net_send(fd, ".");
            break;
        }
        // Dot-stuffing: if line starts with '.', prepend an extra '.' 
        if (input[0] == '.') {
            char stuffed[MAX_LINE + 2];
            stuffed[0] = '.';
            strncpy(stuffed + 1, input, sizeof(stuffed) - 2);
            net_send(fd, stuffed);
        } else {
            net_send(fd, input);
        }
    }

    if (!net_recv_line(fd, line, sizeof(line))) { close_session(fd); return; }
    if (strncmp(line, "OK Delivered", 12) == 0) {
        int n = 0;
        sscanf(line, "OK Delivered to %d", &n);
        printf("Mail delivered to %d recipient%s.\n", n, n == 1 ? "" : "s");
    } else if (strncmp(line, "ERR Body too large", 18) == 0) {
        printf("ERROR: Mail body is too large (max 65536 bytes).\n");
    } else {
        printf("ERROR: %s\n", line);
    }

    net_send(fd, "QUIT");
    net_recv_line(fd, line, sizeof(line)); /* read BYE */
    close_session(fd);
}




static void smp_list(int fd) {
    net_send(fd, "LIST");

    char line[MAX_LINE];
    if (!net_recv_line(fd, line, sizeof(line))) return;

    if (strncmp(line, "OK", 2) != 0) {
        printf("ERROR: %s\n", line);
        return;
    }

    int count = 0;
    sscanf(line, "OK %d", &count);

    if (count == 0) {
        printf("  (No messages)\n");
        net_recv_line(fd, line, sizeof(line)); /* consume the '.' */
        return;
    }

   
    printf("\n  %-6s %-25s %-30s %s\n", "ID", "From", "Subject", "Date");
    printf("  %-6s %-25s %-30s %s\n",
           "------", "-------------------------",
           "------------------------------", "-------------------");

    while (1) {
        if (!net_recv_line(fd, line, sizeof(line))) break;
        if (strcmp(line, ".") == 0) break;

        /* Format: <id>\t<from>\t<subject>\t<date> */
        char id_s[16]     = "";
        char from_s[128]  = "";
        char subj_s[256]  = "";
        char date_s[32]   = "";

        char *tok = strtok(line, "\t");
        if (tok) { strncpy(id_s,   tok, sizeof(id_s)   - 1); tok = strtok(NULL, "\t"); }
        if (tok) { strncpy(from_s, tok, sizeof(from_s) - 1); tok = strtok(NULL, "\t"); }
        if (tok) { strncpy(subj_s, tok, sizeof(subj_s) - 1); tok = strtok(NULL, "\t"); }
        if (tok) { strncpy(date_s, tok, sizeof(date_s) - 1); }

        printf("  %-6s %-25.25s %-30.30s %s\n",
               id_s, from_s, subj_s, date_s);
    }
    printf("\n");
}


static void smp_read(int fd) {
    char input[32];
    printf("Enter message ID: ");
    fflush(stdout);
    if (!read_stdin(input, sizeof(input))) return;

    char cmd[64];
    snprintf(cmd, sizeof(cmd), "READ %s", input);
    net_send(fd, cmd);

    char line[MAX_LINE];
    if (!net_recv_line(fd, line, sizeof(line))) return;

    if (strncmp(line, "OK", 2) != 0) {
        printf("ERROR: %s\n", line);
        return;
    }

    printf("\n");
    // Read and print mail lines until '.' (apply dot-unstuffing) 
    while (1) {
        if (!net_recv_line(fd, line, sizeof(line))) break;
        if (strcmp(line, ".") == 0) break;
        /* Dot-unstuffing: if line starts with '..', strip one dot */
        const char *text = line;
        if (line[0] == '.' && line[1] == '.') text = line + 1;
        printf("  %s\n", text);
    }
    printf("\n");
}


static void smp_delete(int fd) {
    char input[32];
    printf("Enter message ID: ");
    fflush(stdout);
    if (!read_stdin(input, sizeof(input))) return;

    char cmd[64];
    snprintf(cmd, sizeof(cmd), "DELETE %s", input);
    net_send(fd, cmd);

    char line[MAX_LINE];
    if (!net_recv_line(fd, line, sizeof(line))) return;

    if (strncmp(line, "OK", 2) == 0)
        printf("Message %s deleted.\n", input);
    else
        printf("ERROR: %s\n", line);
}

static void check_mailbox_session(void) {
    int fd = open_session("MODE RECV");
    if (fd < 0) {
        printf("ERROR: Could not connect to server.\n");
        return;
    }

    char line[MAX_LINE];
    char username[MAX_USERNAME];
    char password[MAX_PASSWORD];

    if (!net_recv_line(fd, line, sizeof(line))) { close_session(fd); return; }
    if (strncmp(line, "AUTH REQUIRED ", 14) != 0) {
        printf("ERROR: Expected AUTH REQUIRED, got: %s\n", line);
        close_session(fd);
        return;
    }
    char nonce[NONCE_LEN + 1];
    strncpy(nonce, line + 14, NONCE_LEN);
    nonce[NONCE_LEN] = '\0';

    int logged_in = 0;
    for (int attempt = 0; attempt < 3; attempt++) {
        printf("Username: ");
        fflush(stdout);
        if (!read_stdin(username, sizeof(username))) { close_session(fd); return; }

        printf("Password: ");
        fflush(stdout);
        if (!read_stdin(password, sizeof(password))) { close_session(fd); return; }

        char combined[MAX_PASSWORD + NONCE_LEN + 1];
        snprintf(combined, sizeof(combined), "%s%s", password, nonce);
        unsigned long hash = djb2(combined);

        char cmd[MAX_LINE];
        snprintf(cmd, sizeof(cmd), "AUTH %s %lu", username, hash);
        net_send(fd, cmd);

        if (!net_recv_line(fd, line, sizeof(line))) { close_session(fd); return; }

        if (strncmp(line, "OK", 2) == 0) {
            logged_in = 1;
            printf("Welcome, %s!\n", username);
            break;
        }

        if (strncmp(line, "ERR Too many", 12) == 0) {
            printf("ERROR: Too many failed attempts. Connection closed.\n");
            close_session(fd);
            return;
        }
        printf("ERROR: Authentication failed. Please try again.\n");
    }

    if (!logged_in) { close_session(fd); return; }

    char choice[8];
    while (1) {
        net_send(fd, "COUNT");
        int msg_count = 0;
        if (net_recv_line(fd, line, sizeof(line)))
            sscanf(line, "OK %d", &msg_count);

        printf("\nMailbox for %s (%d message%s)\n",
               username, msg_count, msg_count == 1 ? "" : "s");
        printf("1. List all messages\n");
        printf("2. Read a message\n");
        printf("3. Delete a message\n");
        printf("4. Logout\n");
        printf("> ");
        fflush(stdout);

        if (!read_stdin(choice, sizeof(choice))) break;

        if (strcmp(choice, "1") == 0) {
            smp_list(fd);
        } else if (strcmp(choice, "2") == 0) {
            smp_read(fd);
        } else if (strcmp(choice, "3") == 0) {
            smp_delete(fd);
        } else if (strcmp(choice, "4") == 0) {
            net_send(fd, "QUIT");
            net_recv_line(fd, line, sizeof(line)); /* read BYE */
            printf("Logged out.\n");
            break;
        } else {
            printf("Invalid choice. Please enter 1-4.\n");
        }
    }

    close_session(fd);
}



int main(int argc, char *argv[]) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <server_ip> <port>\n", argv[0]);
        return 1;
    }

    strncpy(server_ip, argv[1], sizeof(server_ip) - 1);
    server_port = atoi(argv[2]);

    if (server_port <= 0 || server_port > 65535) {
        fprintf(stderr, "ERROR: Invalid port number\n");
        return 1;
    }

    
    {
        int test_fd = connect_to_server();
        if (test_fd < 0) {
            fprintf(stderr, "ERROR: Cannot connect to %s:%d\n",
                    server_ip, server_port);
            return 1;
        }
        close(test_fd);
        g_buf_len = 0;
    }

    printf("Connected to SimpleMail server.\n");

    char choice[8];
    while (1) {
        printf("\n1. Send a mail\n");
        printf("2. Check my mailbox\n");
        printf("3. Quit\n");
        printf("> ");
        fflush(stdout);

        if (!read_stdin(choice, sizeof(choice))) break;

        if (strcmp(choice, "1") == 0) {
            send_mail_session();
        } else if (strcmp(choice, "2") == 0) {
            check_mailbox_session();
        } else if (strcmp(choice, "3") == 0) {
            printf("Goodbye.\n");
            break;
        } else {
            printf("Invalid choice. Please enter 1, 2, or 3.\n");
        }
    }

    return 0;
}
