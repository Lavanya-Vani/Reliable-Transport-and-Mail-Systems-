/*
 Mini Project- 2
 Name : Yernagula Lavanya Vani
 Roll No. : 23CS10082
*/

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <strings.h>  
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <ctype.h>
#include <errno.h>
#include <time.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define MAX_USERS        100
#define MAX_USERNAME     21      
#define MAX_PASSWORD     31     
#define MAX_LINE         512     /* max SMTP2/SMP protocol line length */
#define MAX_BODY         65536   /* max mail body size after de-stuffing */
#define MAX_CLIENTS      64      /* max simultaneous connections via select() */
#define MODE_TIMEOUT     30      /* seconds to wait for MODE declaration */
#define MAX_AUTH_TRIES   3       /* max SMP authentication attempts */
#define NONCE_LEN        8       /* length of SMP challenge nonce */
#define MAX_RECIPIENTS 50
#define MAILBOX_DIR      "mailboxes"

//  State machine states 
typedef enum {
    STATE_INIT,          /* waiting for MODE */
    // SMTP2 states 
    STATE_SMTP_FROM,     /* waiting for FROM */
    STATE_SMTP_TO,       /* waiting for TO / SUB */
    STATE_SMTP_SUB,      /* waiting for SUB */
    STATE_SMTP_BODY_CMD, /* waiting for BODY command */
    STATE_SMTP_BODY,     /* reading body lines */
    // SMP states 
    STATE_SMP_AUTH,      /* waiting for AUTH */
    STATE_SMP_CMD,       /* authenticated, waiting for commands */
} State;


typedef struct {
    int  fd;
    State state;
    time_t connect_time;    /* for MODE_TIMEOUT */
    int  auth_tries;        /* SMP auth attempt counter */
    char nonce[NONCE_LEN + 1];

     
    char from[MAX_LINE];
    char recipients[MAX_RECIPIENTS][MAX_USERNAME]; /* accepted recipients */
    int  rcpt_count;
    char subject[MAX_LINE];
    char body[MAX_BODY + 1];
    int  body_len;

    
    char sm_user[MAX_USERNAME]; /* authenticated username */


    char buf[MAX_LINE * 2];
    int  buf_len;
} Client;

typedef struct {
    char username[MAX_USERNAME];
    char password[MAX_PASSWORD];
} User;

static User  users[MAX_USERS];
static int   user_count = 0;

static Client clients[MAX_CLIENTS];


// Timestamped log line 
static void log_msg(const char *fmt, ...) {
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", t);
    printf("[%s] ", ts);
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    fflush(stdout);
}

// current time stamp string for mail header
static void timestamp_str(char *out, size_t sz) {
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    strftime(out, sz, "%Y-%m-%d %H:%M:%S", t);
}

static int find_user(const char *username) {
    for (int i = 0; i < user_count; i++) {
        if (strcasecmp(users[i].username, username) == 0)
            return i;
    }
    return -1;
}

// DJB2 hash 
static unsigned long djb2(const char *str) {
    unsigned long hash = 5381;
    int c;
    while ((c = (unsigned char)*str++))
        hash = ((hash << 5) + hash) + c;
    return hash;
}

static void gen_nonce(char *out) {
    static const char charset[] =
        "abcdefghijklmnopqrstuvwxyz"
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "0123456789";
    for (int i = 0; i < NONCE_LEN; i++)
        out[i] = charset[rand() % (sizeof(charset) - 1)];
    out[NONCE_LEN] = '\0';
}

static void send_line(int fd, const char *line) {
    char buf[MAX_LINE + 3];
    int  len = snprintf(buf, sizeof(buf), "%s\r\n", line);
    int sent = 0;
    while (sent < len) {
        int n = send(fd, buf + sent, len - sent, 0);
        if (n <= 0) break;
        sent += n;
    }
}


static void ensure_mailbox_dir(const char *username) {
    char path[256];
    snprintf(path, sizeof(path), "%s/%s", MAILBOX_DIR, username);
    mkdir(path, 0755); 
}


static int next_mail_id(const char *username) {
    char path[256];
    snprintf(path, sizeof(path), "%s/%s", MAILBOX_DIR, username);
    DIR *d = opendir(path);
    if (!d) return 1;

    int max_id = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        /* file names are like "3.txt" */
        int id = atoi(de->d_name);
        if (id > max_id) max_id = id;
    }
    closedir(d);
    return max_id + 1;
}

/*
  Store a mail file for one recipient.
  Returns the mail ID used, or -1 on error.
 */
static int store_mail(const char *username, const char *from,
                      const char *to_list, const char *subject,
                      const char *body)
{
    ensure_mailbox_dir(username);
    int id = next_mail_id(username);

    char path[300];
    snprintf(path, sizeof(path), "%s/%s/%d.txt", MAILBOX_DIR, username, id);
    FILE *f = fopen(path, "w");
    if (!f) return -1;

    char date[32];
    timestamp_str(date, sizeof(date));

    fprintf(f, "From: %s\r\n", from);
    fprintf(f, "To: %s\r\n", to_list);
    fprintf(f, "Subject: %s\r\n", subject);
    fprintf(f, "Date: %s\r\n", date);
    fprintf(f, "---\r\n");
    fprintf(f, "%s", body);
    fclose(f);
    return id;
}


static int mail_path(const char *username, int id, char *out, size_t sz) {
    snprintf(out, sz, "%s/%s/%d.txt", MAILBOX_DIR, username, id);
    struct stat st;
    return stat(out, &st);
}

// counts mail in user's mailbox
static int count_mails(const char *username) {
    char path[256];
    snprintf(path, sizeof(path), "%s/%s", MAILBOX_DIR, username);
    DIR *d = opendir(path);
    if (!d) return 0;
    int count = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (atoi(de->d_name) > 0) count++;
    }
    closedir(d);
    return count;
}

static int extract_line(Client *c, char *line, int max_len) {
    for (int i = 0; i < c->buf_len - 1; i++) {
        if (c->buf[i] == '\r' && c->buf[i+1] == '\n') {
            int copy = i < max_len - 1 ? i : max_len - 1;
            memcpy(line, c->buf, copy);
            line[copy] = '\0';
            /* shift buffer */
            int consumed = i + 2;
            memmove(c->buf, c->buf + consumed, c->buf_len - consumed);
            c->buf_len -= consumed;
            return 1;
        }
    }
    return 0;
}


static void init_client(Client *c, int fd) {
    memset(c, 0, sizeof(*c));
    c->fd           = fd;
    c->state        = STATE_INIT;
    c->connect_time = time(NULL);
}

static void close_client(Client *c, const char *ip) {
    log_msg("Client disconnected (fd=%d, ip=%s)", c->fd, ip ? ip : "?");
    close(c->fd);
    c->fd = -1;
}

static void reset_smtp_mail(Client *c) {
    memset(c->from,       0, sizeof(c->from));
    memset(c->recipients, 0, sizeof(c->recipients));
    c->rcpt_count = 0;
    memset(c->subject, 0, sizeof(c->subject));
    memset(c->body,    0, sizeof(c->body));
    c->body_len = 0;
}



static void handle_smtp2_line(Client *c, const char *line, const char *ip) {

    if (strcmp(line, "QUIT") == 0) {
        send_line(c->fd, "BYE");
        close_client(c, ip);
        return;
    }

    switch (c->state) {

    case STATE_SMTP_FROM: {
        if (strncmp(line, "FROM ", 5) != 0) {
            send_line(c->fd, "ERR Bad sequence");
            return;
        }
        const char *name = line + 5;
        strncpy(c->from, name, sizeof(c->from) - 1);
        send_line(c->fd, "OK Sender accepted");
        c->state = STATE_SMTP_TO;
        break;
    }

    case STATE_SMTP_TO: {
        if (strncmp(line, "TO ", 3) == 0) {
            const char *uname = line + 3;
            // trim leading/trailing whitespace from username 
            while (*uname == ' ') uname++;

            if (find_user(uname) < 0) {
                send_line(c->fd, "ERR No such user");
            } else {
                if (c->rcpt_count < MAX_RECIPIENTS) {
                    strncpy(c->recipients[c->rcpt_count], uname,
                            MAX_USERNAME - 1);
                    c->rcpt_count++;
                }
                send_line(c->fd, "OK Recipient accepted");
            }
        } else if (strncmp(line, "SUB", 3) == 0) {
            if (c->rcpt_count == 0) {
                send_line(c->fd, "ERR Bad sequence");
                return;
            }
            
            if (strlen(line) > 4)
                strncpy(c->subject, line + 4, sizeof(c->subject) - 1);
            else
                strncpy(c->subject, "(no subject)", sizeof(c->subject) - 1);

            send_line(c->fd, "OK Subject accepted");
            c->state = STATE_SMTP_BODY_CMD;
        } else {
            send_line(c->fd, "ERR Bad sequence");
        }
        break;
    }

    case STATE_SMTP_BODY_CMD: {
        if (strcmp(line, "BODY") != 0) {
            send_line(c->fd, "ERR Bad sequence");
            return;
        }
        if (c->rcpt_count == 0) {
            send_line(c->fd, "ERR No valid recipients");
            c->state = STATE_SMTP_FROM;
            reset_smtp_mail(c);
            return;
        }
        send_line(c->fd, "OK Send body, end with CRLF.CRLF");
        c->body_len = 0;
        c->state    = STATE_SMTP_BODY;
        break;
    }

    case STATE_SMTP_BODY: {
       
        if (strcmp(line, ".") == 0) {

            char to_list[MAX_RECIPIENTS * MAX_USERNAME];
            to_list[0] = '\0';
            for (int i = 0; i < c->rcpt_count; i++) {
                if (i > 0) strncat(to_list, ", ",
                                   sizeof(to_list) - strlen(to_list) - 1);
                strncat(to_list, c->recipients[i],
                        sizeof(to_list) - strlen(to_list) - 1);
            }

            for (int i = 0; i < c->rcpt_count; i++) {
                int id = store_mail(c->recipients[i], c->from,
                                    to_list, c->subject, c->body);
                if (id > 0)
                    log_msg("Mail delivered from \"%s\" to [%s] (msg id %d)",
                            c->from, c->recipients[i], id);
            }

            char resp[64];
            snprintf(resp, sizeof(resp), "OK Delivered to %d mailboxes",
                     c->rcpt_count);
            send_line(c->fd, resp);

            reset_smtp_mail(c);
            c->state = STATE_SMTP_FROM;
            return;
        }

        const char *text = line;
        if (line[0] == '.') text = line + 1;

        int text_len = strlen(text);

      
        if (c->body_len + text_len + 2 > MAX_BODY) {
            send_line(c->fd, "ERR Body too large");
            reset_smtp_mail(c);
            c->state = STATE_SMTP_FROM;
            return;
        }

        memcpy(c->body + c->body_len, text, text_len);
        c->body_len += text_len;
        c->body[c->body_len++] = '\r';
        c->body[c->body_len++] = '\n';
        c->body[c->body_len]   = '\0';
        break;
    }

    default:
        send_line(c->fd, "ERR Bad sequence");
        break;
    }
}



static void handle_smp_line(Client *c, const char *line, const char *ip) {

    switch (c->state) {

    case STATE_SMP_AUTH: {
        char uname[MAX_USERNAME];
        char hash_str[64];

        if (sscanf(line, "AUTH %20s %63s", uname, hash_str) != 2) {
            c->auth_tries++;
            send_line(c->fd, "ERR Authentication failed");
            if (c->auth_tries >= MAX_AUTH_TRIES) {
                send_line(c->fd, "ERR Too many failures");
                close_client(c, ip);
            }
            return;
        }

        int idx = find_user(uname);
        if (idx < 0) {
            c->auth_tries++;
            log_msg("Auth failed for unknown user '%s' (fd=%d)", uname, c->fd);
            send_line(c->fd, "ERR Authentication failed");
            if (c->auth_tries >= MAX_AUTH_TRIES) {
                send_line(c->fd, "ERR Too many failures");
                close_client(c, ip);
            }
            return;
        }

        char combined[MAX_PASSWORD + NONCE_LEN + 1];
        snprintf(combined, sizeof(combined), "%s%s",
                 users[idx].password, c->nonce);
        unsigned long expected = djb2(combined);

        unsigned long got = strtoul(hash_str, NULL, 10);

        if (got != expected) {
            c->auth_tries++;
            log_msg("Auth failed for user '%s' (fd=%d)", uname, c->fd);
            send_line(c->fd, "ERR Authentication failed");
            if (c->auth_tries >= MAX_AUTH_TRIES) {
                send_line(c->fd, "ERR Too many failures");
                close_client(c, ip);
            }
            return;
        }

        strncpy(c->sm_user, users[idx].username, MAX_USERNAME - 1);
        log_msg("Authentication successful for user %s", c->sm_user);
        char resp[64];
        snprintf(resp, sizeof(resp), "OK Welcome %s", c->sm_user);
        send_line(c->fd, resp);
        c->state = STATE_SMP_CMD;
        break;
    }

    case STATE_SMP_CMD: {

        // QUIT
        if (strcmp(line, "QUIT") == 0) {
            send_line(c->fd, "BYE");
            close_client(c, ip);
            return;
        }

        // COUNT 
        if (strcmp(line, "COUNT") == 0) {
            int n = count_mails(c->sm_user);
            char resp[32];
            snprintf(resp, sizeof(resp), "OK %d", n);
            send_line(c->fd, resp);
            log_msg("User %s COUNT: %d mails", c->sm_user, n);
            return;
        }

        // LIST 
        if (strcmp(line, "LIST") == 0) {
            char dir_path[256];
            snprintf(dir_path, sizeof(dir_path), "%s/%s",
                     MAILBOX_DIR, c->sm_user);
            DIR *d = opendir(dir_path);

        
            int ids[1024];
            int id_count = 0;
            if (d) {
                struct dirent *de;
                while ((de = readdir(d)) != NULL) {
                    int id = atoi(de->d_name);
                    if (id > 0 && id_count < 1024)
                        ids[id_count++] = id;
                }
                closedir(d);
            }

            
            for (int i = 0; i < id_count - 1; i++)
                for (int j = i+1; j < id_count; j++)
                    if (ids[i] > ids[j]) {
                        int tmp = ids[i]; ids[i] = ids[j]; ids[j] = tmp;
                    }

            char resp[64];
            snprintf(resp, sizeof(resp), "OK %d messages", id_count);
            send_line(c->fd, resp);

            for (int i = 0; i < id_count; i++) {
                char path[300];
                snprintf(path, sizeof(path), "%s/%s/%d.txt",
                         MAILBOX_DIR, c->sm_user, ids[i]);
                FILE *f = fopen(path, "r");
                if (!f) continue;

                char from_val[101]    = "";
                char subject_val[201] = "";
                char date_val[21]    = "";
                char fline[MAX_LINE];

                while (fgets(fline, sizeof(fline), f)) {
                    /* strip trailing \r\n */
                    fline[strcspn(fline, "\r\n")] = '\0';
                    if (strncmp(fline, "From: ", 6) == 0)
                        strncpy(from_val, fline + 6, sizeof(from_val) - 1);
                    else if (strncmp(fline, "Subject: ", 9) == 0)
                        strncpy(subject_val, fline + 9, sizeof(subject_val)-1);
                    else if (strncmp(fline, "Date: ", 6) == 0)
                        strncpy(date_val, fline + 6, sizeof(date_val) - 1);
                    else if (strcmp(fline, "---") == 0)
                        break;
                }
                fclose(f);

                
                char list_line[MAX_LINE];
                snprintf(list_line, sizeof(list_line), "%d\t%s\t%s\t%s",
                         ids[i], from_val, subject_val, date_val);
                send_line(c->fd, list_line);
            }

            send_line(c->fd, ".");
            log_msg("User %s LIST: %d messages", c->sm_user, id_count);
            return;
        }

    
        if (strncmp(line, "READ ", 5) == 0) {
            int id = atoi(line + 5);
            char path[300];
            if (id <= 0 || mail_path(c->sm_user, id, path, sizeof(path)) != 0) {
                send_line(c->fd, "ERR No such message");
                return;
            }

            FILE *f = fopen(path, "r");
            if (!f) { send_line(c->fd, "ERR No such message"); return; }

            send_line(c->fd, "OK");

            
            char fline[MAX_LINE];
            while (fgets(fline, sizeof(fline), f)) {
                fline[strcspn(fline, "\r\n")] = '\0'; 
                if (fline[0] == '.') {
                    char stuffed[MAX_LINE + 2];
                    stuffed[0] = '.';
                    strncpy(stuffed + 1, fline, sizeof(stuffed) - 2);
                    send_line(c->fd, stuffed);
                } else {
                    send_line(c->fd, fline);
                }
            }
            fclose(f);
            send_line(c->fd, ".");
            log_msg("User %s READ message %d", c->sm_user, id);
            return;
        }

      
        if (strncmp(line, "DELETE ", 7) == 0) {
            int id = atoi(line + 7);
            char path[300];
            if (id <= 0 || mail_path(c->sm_user, id, path, sizeof(path)) != 0) {
                send_line(c->fd, "ERR No such message");
                return;
            }
            if (unlink(path) == 0) {
                send_line(c->fd, "OK Deleted");
                log_msg("User %s DELETE message %d", c->sm_user, id);
            } else {
                send_line(c->fd, "ERR No such message");
            }
            return;
        }

      
        send_line(c->fd, "ERR Unknown command");
        break;
    }

    default:
        send_line(c->fd, "ERR Unknown command");
        break;
    }
}



static void process_client_data(Client *c, const char *ip) {
    char line[MAX_LINE];

    while (extract_line(c, line, sizeof(line))) {

        if (c->state == STATE_INIT) {
            if (strcmp(line, "MODE SEND") == 0) {
                send_line(c->fd, "OK");
                log_msg("Client (fd=%d, ip=%s) selected MODE SEND", c->fd, ip);
                c->state = STATE_SMTP_FROM;
                reset_smtp_mail(c);
            } else if (strcmp(line, "MODE RECV") == 0) {
                send_line(c->fd, "OK");
                log_msg("Client (fd=%d, ip=%s) selected MODE RECV", c->fd, ip);
                gen_nonce(c->nonce);
                char auth_line[64];
                snprintf(auth_line, sizeof(auth_line),
                         "AUTH REQUIRED %s", c->nonce);
                send_line(c->fd, auth_line);
                c->state = STATE_SMP_AUTH;
            } else {
                send_line(c->fd, "ERR Unknown mode");
            }
            continue;
        }

        if (c->state >= STATE_SMTP_FROM && c->state <= STATE_SMTP_BODY) {
            handle_smtp2_line(c, line, ip);
        } else {
            handle_smp_line(c, line, ip);
        }

        if (c->fd < 0) return;
    }
}



static int load_users(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "ERROR: Cannot open user file '%s': %s\n",
                path, strerror(errno));
        return -1;
    }

    char line[128];
    int lineno = 0;
    while (fgets(line, sizeof(line), f)) {
        lineno++;
        line[strcspn(line, "\r\n")] = '\0';
        if (strlen(line) == 0) continue; /* skip blank lines */

        char uname[MAX_USERNAME], pwd[MAX_PASSWORD];
        if (sscanf(line, "%20s %30s", uname, pwd) != 2) {
            fprintf(stderr, "ERROR: Malformed user file at line %d\n", lineno);
            fclose(f);
            return -1;
        }

        for (char *p = uname; *p; p++) *p = tolower((unsigned char)*p);

        if (user_count >= MAX_USERS) {
            fprintf(stderr, "ERROR: Too many users (max %d)\n", MAX_USERS);
            fclose(f);
            return -1;
        }

        strncpy(users[user_count].username, uname, MAX_USERNAME - 1);
        strncpy(users[user_count].password, pwd,   MAX_PASSWORD - 1);
        user_count++;
    }

    fclose(f);
    return 0;
}



int main(int argc, char *argv[]) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <port> <userfile>\n", argv[0]);
        return 1;
    }

    int port = atoi(argv[1]);
    if (port <= 0 || port > 65535) {
        fprintf(stderr, "ERROR: Invalid port number\n");
        return 1;
    }

    if (load_users(argv[2]) != 0) return 1;
    if (user_count == 0) {
        fprintf(stderr, "ERROR: No users loaded from '%s'\n", argv[2]);
        return 1;
    }

    mkdir(MAILBOX_DIR, 0755);
    for (int i = 0; i < user_count; i++)
        ensure_mailbox_dir(users[i].username);

    srand((unsigned)time(NULL));

    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) { perror("socket"); return 1; }

    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(port);

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); close(listen_fd); return 1;
    }
    if (listen(listen_fd, 10) < 0) {
        perror("listen"); close(listen_fd); return 1;
    }

    log_msg("Server started on port %d", port);
    log_msg("Loaded %d users from %s", user_count, argv[2]);

    for (int i = 0; i < MAX_CLIENTS; i++) clients[i].fd = -1;

    while (1) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(listen_fd, &readfds);
        int max_fd = listen_fd;

        time_t now = time(NULL);

        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i].fd < 0) continue;

            if (clients[i].state == STATE_INIT &&
                (now - clients[i].connect_time) >= MODE_TIMEOUT) {
                log_msg("Client fd=%d timed out (no MODE within %ds)",
                        clients[i].fd, MODE_TIMEOUT);
                close(clients[i].fd);
                clients[i].fd = -1;
                continue;
            }

            FD_SET(clients[i].fd, &readfds);
            if (clients[i].fd > max_fd) max_fd = clients[i].fd;
        }

        struct timeval tv = {1, 0};
        int ready = select(max_fd + 1, &readfds, NULL, NULL, &tv);
        if (ready < 0) {
            if (errno == EINTR) continue;
            perror("select");
            break;
        }

        if (FD_ISSET(listen_fd, &readfds)) {
            struct sockaddr_in cli_addr;
            socklen_t cli_len = sizeof(cli_addr);
            int new_fd = accept(listen_fd,
                                (struct sockaddr *)&cli_addr, &cli_len);
            if (new_fd < 0) {
                perror("accept");
            } else {
                char ip_str[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &cli_addr.sin_addr, ip_str, sizeof(ip_str));
                log_msg("New connection from %s:%d (fd=%d)",
                        ip_str, ntohs(cli_addr.sin_port), new_fd);

                int slot = -1;
                for (int i = 0; i < MAX_CLIENTS; i++) {
                    if (clients[i].fd < 0) { slot = i; break; }
                }

                if (slot < 0) {
                    log_msg("Connection rejected: server full");
                    send(new_fd, "ERR Server full\r\n", 17, 0);
                    close(new_fd);
                } else {
                    init_client(&clients[slot], new_fd);
                    send_line(new_fd, "WELCOME SimpleMail v1.0");
                }
            }
        }

        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i].fd < 0) continue;
            if (!FD_ISSET(clients[i].fd, &readfds)) continue;

            struct sockaddr_in peer;
            socklen_t plen = sizeof(peer);
            char ip_str[INET_ADDRSTRLEN] = "?";
            if (getpeername(clients[i].fd,
                            (struct sockaddr *)&peer, &plen) == 0)
                inet_ntop(AF_INET, &peer.sin_addr, ip_str, sizeof(ip_str));

            int space = (int)sizeof(clients[i].buf) - clients[i].buf_len - 1;
            if (space <= 0) {
                /* Buffer overflow – disconnect */
                log_msg("Client fd=%d buffer overflow, closing", clients[i].fd);
                close_client(&clients[i], ip_str);
                continue;
            }

            int n = recv(clients[i].fd,
                         clients[i].buf + clients[i].buf_len, space, 0);
            if (n <= 0) {
                log_msg("Client fd=%d disconnected (recv returned %d)",
                        clients[i].fd, n);
                close(clients[i].fd);
                clients[i].fd = -1;
                continue;
            }
            clients[i].buf_len += n;
            clients[i].buf[clients[i].buf_len] = '\0';

            process_client_data(&clients[i], ip_str);
        }
    }

    close(listen_fd);
    return 0;
}
