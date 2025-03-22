#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <syslog.h>
#include <pthread.h>
#include <fcntl.h>
#include <sys/queue.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include "aesd_ioctl.h"

#define SERVER_PORT 9000
#define BACKLOG 10
#define BUFFER_SIZE 1024

#define USE_AESD_CHAR_DEVICE 1

#if USE_AESD_CHAR_DEVICE
#define FILE_PATH "/dev/aesdchar"
#else
#define FILE_PATH "/var/tmp/aesdsocketdata"
#endif
#define IOCTL_CMD_STR "AESDCHAR_IOCSEEKTO:"

char *seek_cmd = IOCTL_CMD_STR;
size_t seek_cmd_len = strlen(seek_cmd);

// Global exit flag and file mutex
static volatile sig_atomic_t server_exit_flag = 0;
static pthread_mutex_t file_mutex = PTHREAD_MUTEX_INITIALIZER;

#if !USE_AESD_CHAR_DEVICE
static pthread_t timestamp_thread;
#endif

typedef struct client_thread {
    pthread_t thread_id;
    int client_fd;
    struct sockaddr_in client_addr;
    SLIST_ENTRY(client_thread) entries;
} client_thread_t;

// SLIST for active client threads
static SLIST_HEAD(client_list_head, client_thread) client_list = SLIST_HEAD_INITIALIZER(client_list);

int server_socket_fd = -1;

// Signal handler
void signal_handler(int signo) {
    (void)signo;
    server_exit_flag = 1;
    close(server_socket_fd);
}

#if !USE_AESD_CHAR_DEVICE
void* timestamp_writer(void *arg) {
    (void)arg;
    while (!server_exit_flag) {
        sleep(10);
        time_t now = time(NULL);
        struct tm time_info;
        char timestamp_str[128];

        localtime_r(&now, &time_info);
        strftime(timestamp_str, sizeof(timestamp_str), "timestamp:%a, %d %b %Y %T %z\n", &time_info);

        pthread_mutex_lock(&file_mutex);
        int fd = open(FILE_PATH, O_WRONLY | O_APPEND | O_CREAT, 0666);
        if (fd >= 0) {
            write(fd, timestamp_str, strlen(timestamp_str));
            close(fd);
        } else {
            syslog(LOG_ERR, "Failed to write timestamp: %s", strerror(errno));
        }
        pthread_mutex_unlock(&file_mutex);
    }
    return NULL;
}
#endif

// Client handler
void* client_handler(void *arg) {
    client_thread_t *client_info = (client_thread_t*)arg;
    int client_fd = client_info->client_fd;
    syslog(LOG_INFO, "Accepted connection from %s", inet_ntoa(client_info->client_addr.sin_addr));

    char *recvbuf = malloc(BUFFER_SIZE);
    if (!recvbuf) {
        syslog(LOG_ERR, "Memory allocation failed");
        close(client_fd);
        free(client_info);
        pthread_exit(NULL);
    }

    ssize_t bytes_received;
    size_t total_received = 0;
    int file_fd;

#if USE_AESD_CHAR_DEVICE
    pthread_mutex_lock(&file_mutex);
    file_fd = open(FILE_PATH, O_RDWR);
#else
    pthread_mutex_lock(&file_mutex);
    file_fd = open(FILE_PATH, O_CREAT | O_RDWR | O_APPEND, 0666);
#endif

    if (file_fd == -1) {
        syslog(LOG_ERR, "File open failed: %s", strerror(errno));
        pthread_mutex_unlock(&file_mutex);
        close(client_fd);
        free(recvbuf);
        free(client_info);
        pthread_exit(NULL);
    }

    // Receive full message (until newline)
    while ((bytes_received = recv(client_fd, recvbuf + total_received, BUFFER_SIZE - total_received - 1, 0)) > 0) {
        total_received += bytes_received;
        recvbuf[total_received] = '\0';

        if (total_received >= BUFFER_SIZE - 1) {
            char *new_buf = realloc(recvbuf, total_received * 2);
            if (!new_buf) {
                syslog(LOG_ERR, "Buffer reallocation failed");
                break;
            }
            recvbuf = new_buf;
        }

        if (strchr(recvbuf, '\n')) break;
    }

    if (total_received > 0) {
        // Check if IOCTL command
        if (strncmp(recvbuf, seek_cmd, seek_cmd_len) == 0) {
            // Detected IOCTL Command
            unsigned int cmd_idx = 0, cmd_offset = 0;
            char *x_ptr = recvbuf + seek_cmd_len;
            char *y_ptr = strchr(x_ptr, ',');

            if (y_ptr) {
                *y_ptr = '\0';
                y_ptr++;

                cmd_idx = (unsigned int) strtoul(x_ptr, NULL, 10);
                cmd_offset = (unsigned int) strtoul(y_ptr, NULL, 10);

                syslog(LOG_INFO, "IOCTL Seek Request: Command=%u Offset=%u", cmd_idx, cmd_offset);

                struct aesd_seekto seekto_params;
                seekto_params.write_cmd = cmd_idx;
                seekto_params.write_cmd_offset = cmd_offset;

                if (ioctl(file_fd, AESDCHAR_IOCSEEKTO, &seekto_params) == -1) {
                    syslog(LOG_ERR, "ioctl AESDCHAR_IOCSEEKTO failed: %s", strerror(errno));
                }
            }
        } else {
            // Normal write
            if (write(file_fd, recvbuf, total_received) == -1) {
                syslog(LOG_ERR, "Write failed: %s", strerror(errno));
            }
        }
    }

    // Read and send back file content
    lseek(file_fd, 0, SEEK_SET);
    char read_buf[BUFFER_SIZE];
    ssize_t read_bytes;
    while ((read_bytes = read(file_fd, read_buf, sizeof(read_buf))) > 0) {
        send(client_fd, read_buf, read_bytes, 0);
    }

    close(file_fd);
    pthread_mutex_unlock(&file_mutex);

    syslog(LOG_INFO, "Closed connection from %s", inet_ntoa(client_info->client_addr.sin_addr));

    close(client_fd);
    free(recvbuf);
    free(client_info);
    pthread_exit(NULL);
}

// Daemonize process
void daemonize() {
    pid_t pid = fork();
    if (pid < 0) exit(EXIT_FAILURE);
    if (pid > 0) exit(EXIT_SUCCESS);

    if (setsid() < 0) exit(EXIT_FAILURE);

    signal(SIGHUP, SIG_IGN);
    pid = fork();
    if (pid < 0) exit(EXIT_FAILURE);
    if (pid > 0) exit(EXIT_SUCCESS);

    umask(0);
    chdir("/");
    close(STDIN_FILENO);
    close(STDOUT_FILENO);
    close(STDERR_FILENO);
}

// Main function
int main(int argc, char *argv[]) {
    int daemon_mode = 0;
    if ((argc == 2) && (strcmp(argv[1], "-d") == 0)) {
        daemon_mode = 1;
    }

    openlog("aesdsocket", LOG_PID, LOG_USER);

    // Signal setup
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    // Socket setup
    server_socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket_fd < 0) {
        syslog(LOG_ERR, "Socket failed: %s", strerror(errno));
        return EXIT_FAILURE;
    }

    int optval = 1;
    setsockopt(server_socket_fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(SERVER_PORT);

    if (bind(server_socket_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        syslog(LOG_ERR, "Bind failed: %s", strerror(errno));
        close(server_socket_fd);
        return EXIT_FAILURE;
    }

    if (daemon_mode) {
        daemonize();
    }

    if (listen(server_socket_fd, BACKLOG) < 0) {
        syslog(LOG_ERR, "Listen failed: %s", strerror(errno));
        close(server_socket_fd);
        return EXIT_FAILURE;
    }

#if !USE_AESD_CHAR_DEVICE
    pthread_create(&timestamp_thread, NULL, timestamp_writer, NULL);
#endif

    syslog(LOG_INFO, "Server started on port %d", SERVER_PORT);

    // Accept client connections
    while (!server_exit_flag) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int client_fd = accept(server_socket_fd, (struct sockaddr*)&client_addr, &addr_len);
        if (server_exit_flag) break;
        if (client_fd < 0) continue;

        client_thread_t *client_info = malloc(sizeof(client_thread_t));
        client_info->client_fd = client_fd;
        client_info->client_addr = client_addr;

        pthread_create(&client_info->thread_id, NULL, client_handler, client_info);
    }

    close(server_socket_fd);

#if !USE_AESD_CHAR_DEVICE
    pthread_cancel(timestamp_thread);
    pthread_join(timestamp_thread, NULL);
    remove(FILE_PATH);
#endif

    closelog();
    return 0;
}

