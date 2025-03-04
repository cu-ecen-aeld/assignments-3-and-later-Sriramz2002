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

static volatile sig_atomic_t server_exit_flag = 0;
static pthread_mutex_t file_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_t timestamp_thread;

// Server socket descriptor
int server_socket_fd = -1;

// SLIST for tracking client threads
typedef struct client_thread {
    pthread_t thread_id;
    int client_fd;
    struct sockaddr_in client_addr;
    SLIST_ENTRY(client_thread) entries;
} client_thread_t;

static SLIST_HEAD(client_list_head, client_thread) client_list = SLIST_HEAD_INITIALIZER(client_list);

// Signal handler function
void signal_handler(int signo) {
    (void)signo;
    server_exit_flag = 1;
    close(server_socket_fd);
}

// Function to run a periodic timestamp writer
void* timestamp_writer(void *arg) {
    (void)arg;

    while (!server_exit_flag) {
        sleep(10);


        time_t now = time(NULL);
        struct tm time_info;
        char timestamp_str[128];

        localtime_r(&now, &time_info);
        strftime(timestamp_str, sizeof(timestamp_str), "timestamp:%a, %d %b %Y %T %z\n", &time_info);

        // Write to file
        pthread_mutex_lock(&file_mutex);
        int fd = open("/var/tmp/aesdsocketdata", O_WRONLY | O_APPEND | O_CREAT, 0777);
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

// Client handler function
void* client_handler(void *arg) {
    client_thread_t *client_info = (client_thread_t*)arg;
    int client_fd = client_info->client_fd;

    syslog(LOG_INFO, "Accepted connection from %s", inet_ntoa(client_info->client_addr.sin_addr));

    // **Initial buffer size set to 2000 bytes**
    size_t buffer_size = 2000; 
    char *recvbuf = malloc(buffer_size);
    if (!recvbuf) {
        syslog(LOG_ERR, "Failed to allocate memory for client buffer");
        close(client_fd);
        pthread_exit(NULL);
    }

    ssize_t bytes_received;
    size_t total_received = 0;

    pthread_mutex_lock(&file_mutex);
    int file_fd = open("/var/tmp/aesdsocketdata", O_CREAT | O_WRONLY | O_APPEND, 0666);
    if (file_fd == -1) {
        syslog(LOG_ERR, "Failed to open file");
        close(client_fd);
        free(recvbuf);
        pthread_mutex_unlock(&file_mutex);
        pthread_exit(NULL);
    }

    while ((bytes_received = recv(client_fd, recvbuf + total_received, buffer_size - total_received - 1, 0)) > 0) {
        total_received += bytes_received;
        recvbuf[total_received] = '\0';

        // **Dynamically resize buffer if needed**
        if (total_received >= buffer_size - 1) {
            size_t new_size = buffer_size * 2; 
            char *new_buf = realloc(recvbuf, new_size);
            if (!new_buf) {
                syslog(LOG_ERR, "Failed to reallocate buffer");
                break;
            }
            recvbuf = new_buf;
            buffer_size = new_size;
        }

        // **Break if newline received**
        if (strchr(recvbuf, '\n')) {
            break;
        }
    }

    if (write(file_fd, recvbuf, total_received) == -1) {
        syslog(LOG_ERR, "Failed to write to file");
    }
    close(file_fd);
    pthread_mutex_unlock(&file_mutex);

    // Send back file contents
    pthread_mutex_lock(&file_mutex);
    file_fd = open("/var/tmp/aesdsocketdata", O_RDONLY);
    if (file_fd != -1) {
        while ((bytes_received = read(file_fd, recvbuf, buffer_size)) > 0) {
            send(client_fd, recvbuf, bytes_received, 0);
        }
        close(file_fd);
    }
    pthread_mutex_unlock(&file_mutex);

    syslog(LOG_INFO, "Closing connection from %s", inet_ntoa(client_info->client_addr.sin_addr));

    close(client_fd);
    free(recvbuf);
    pthread_exit(NULL);
}

// Daemonization function
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

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    server_socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket_fd < 0) {
        syslog(LOG_ERR, "Socket creation failed: %s", strerror(errno));
        return EXIT_FAILURE;
    }

    int optval = 1;
    setsockopt(server_socket_fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(9000);  // **Hardcoded port number**

    if (bind(server_socket_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        syslog(LOG_ERR, "Bind failed: %s", strerror(errno));
        close(server_socket_fd);
        return EXIT_FAILURE;
    }

    if (daemon_mode) {
        daemonize();
    }

    if (listen(server_socket_fd, 10) < 0) {  
        syslog(LOG_ERR, "Listen failed: %s", strerror(errno));
        close(server_socket_fd);
        return EXIT_FAILURE;
    }

    pthread_create(&timestamp_thread, NULL, timestamp_writer, NULL);
    syslog(LOG_INFO, "Server listening on port 9000");

    while (!server_exit_flag) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int client_fd = accept(server_socket_fd, (struct sockaddr*)&client_addr, &addr_len);

        if (server_exit_flag) break;
        if (client_fd < 0) continue;

        client_thread_t *client_thread = malloc(sizeof(client_thread_t));
        client_thread->client_fd = client_fd;
        client_thread->client_addr = client_addr;

        pthread_create(&client_thread->thread_id, NULL, client_handler, client_thread);
    }

    close(server_socket_fd);
    remove("/var/tmp/aesdsocketdata");
    closelog();
    return 0;
}

