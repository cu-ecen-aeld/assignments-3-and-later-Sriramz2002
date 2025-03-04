#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <netdb.h>
#include <syslog.h>
#include <signal.h>
#include <sys/wait.h>
#include <stdbool.h>
#include <errno.h>
#include <pthread.h>
#include <sys/queue.h>
#include <time.h>

#define SERVER_PORT 9000
#define SOCKET_DATA_FILE "/var/tmp/aesdsocketdata"

// SLIST for tracking threads
typedef struct client_thread_s client_thread_t;
struct client_thread_s {
    pthread_t worker_thread;  
    int client_socket;
    bool completed;
    struct sockaddr_in *client_addr;
    SLIST_ENTRY(client_thread_s) entries;
};
SLIST_HEAD(thread_list_head, client_thread_s) active_threads = SLIST_HEAD_INITIALIZER(active_threads);

int server_socket;
volatile sig_atomic_t terminate_flag = 0;
pthread_t time_logger_thread;
pthread_mutex_t file_access_mutex = PTHREAD_MUTEX_INITIALIZER;

void handle_signal(int signal) {
    syslog(LOG_INFO, "Received signal %d, shutting down server.", signal);
    terminate_flag = 1;
    remove(SOCKET_DATA_FILE);

    if (server_socket != -1) {
        close(server_socket);
    }

    pthread_join(time_logger_thread, NULL);
    pthread_mutex_destroy(&file_access_mutex);
    closelog();
    exit(0);
}

void daemonize_process() {
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

void* timestamp_logger(void* arg) {
    while (!terminate_flag) {
        sleep(10);

        time_t now = time(NULL);
        struct tm *tm_info = localtime(&now);
        char timestamp[100];
        strftime(timestamp, sizeof(timestamp), "timestamp:%a, %d %b %Y %H:%M:%S %z\n", tm_info);

        pthread_mutex_lock(&file_access_mutex);
        int file_fd = open(SOCKET_DATA_FILE, O_WRONLY | O_APPEND | O_CREAT, 0644);
        if (file_fd != -1) {
            write(file_fd, timestamp, strlen(timestamp));
            close(file_fd);
        } else {
            syslog(LOG_ERR, "Error writing timestamp to file");
        }
        pthread_mutex_unlock(&file_access_mutex);
    }
    return NULL;
}

void* client_handler(void* arg) {
    client_thread_t *client_info = (client_thread_t *)arg;
    struct sockaddr_in *addr = client_info->client_addr;
    int client_socket = client_info->client_socket;
    int file_fd;

    syslog(LOG_INFO, "Connected to %s", inet_ntoa(addr->sin_addr));

    size_t buffer_size = 1024;
    char *buffer = (char *)malloc(buffer_size);
    if (!buffer) {
        syslog(LOG_ERR, "Memory allocation failed");
        close(client_socket);
        return NULL;
    }

    size_t received_data = 0;
    ssize_t bytes_received;

    pthread_mutex_lock(&file_access_mutex);
    file_fd = open(SOCKET_DATA_FILE, O_CREAT | O_WRONLY | O_APPEND, 0644);
    if (file_fd == -1) {
        syslog(LOG_ERR, "Failed to open socket data file");
        close(client_socket);
        free(buffer);
        pthread_mutex_unlock(&file_access_mutex);
        return NULL;
    }

    while ((bytes_received = recv(client_socket, buffer + received_data, buffer_size - received_data - 1, 0)) > 0) {
        received_data += bytes_received;
        buffer[received_data] = '\0';

        if (received_data >= buffer_size - 1) {
            buffer_size *= 2;
            char *new_buffer = realloc(buffer, buffer_size);
            if (!new_buffer) {
                syslog(LOG_ERR, "Memory reallocation failed");
                break;
            }
            buffer = new_buffer;
        }

        if (strchr(buffer, '\n')) {
            break;
        }
    }

    if (write(file_fd, buffer, received_data) == -1) {
        syslog(LOG_ERR, "Failed to write client data to file");
    }

    fsync(file_fd);
    close(file_fd);
    pthread_mutex_unlock(&file_access_mutex);

    pthread_mutex_lock(&file_access_mutex);
    file_fd = open(SOCKET_DATA_FILE, O_RDONLY);
    if (file_fd != -1) {
        while ((bytes_received = read(file_fd, buffer, buffer_size)) > 0) {
            send(client_socket, buffer, bytes_received, 0);
        }
        close(file_fd);
    }
    pthread_mutex_unlock(&file_access_mutex);

    syslog(LOG_INFO, "Disconnected from %s", inet_ntoa(addr->sin_addr));
    close(client_socket);
    free(buffer);
    client_info->completed = true;
    return NULL;
}

int main(int argc, char *argv[]) {
    int status, client_socket;
    struct addrinfo hints, *server_info;
    int daemon_mode = 0;
    struct sockaddr_storage client_addr;
    socklen_t addr_len;

    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    if (argc == 2 && strcmp(argv[1], "-d") == 0) {
        daemon_mode = 1;
    }

    openlog("aesdsocket", LOG_PID, LOG_USER);
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    if ((status = getaddrinfo(NULL, "9000", &hints, &server_info)) != 0) {
        syslog(LOG_ERR, "getaddrinfo error: %s", gai_strerror(status));
        exit(EXIT_FAILURE);
    }

    server_socket = socket(server_info->ai_family, server_info->ai_socktype, server_info->ai_protocol);
    if (server_socket == -1) {
        syslog(LOG_ERR, "Socket creation failed");
        freeaddrinfo(server_info);
        exit(EXIT_FAILURE);
    }

    int optval = 1;
    if (setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) == -1) {
        syslog(LOG_ERR, "Failed to set socket options");
        freeaddrinfo(server_info);
        close(server_socket);
        exit(EXIT_FAILURE);
    }

    if (bind(server_socket, server_info->ai_addr, server_info->ai_addrlen) == -1) {
        syslog(LOG_ERR, "Binding socket failed");
        freeaddrinfo(server_info);
        close(server_socket);
        exit(EXIT_FAILURE);
    }
    freeaddrinfo(server_info);

    if (daemon_mode) {
        daemonize_process();
        syslog(LOG_INFO, "Daemonized server");
    }

    if (listen(server_socket, 10) == -1) {
        syslog(LOG_ERR, "Listening failed");
        close(server_socket);
        exit(EXIT_FAILURE);
    }

    syslog(LOG_INFO, "Server running on port %d", SERVER_PORT);
    pthread_create(&time_logger_thread, NULL, timestamp_logger, NULL);

    while (!terminate_flag) {
        addr_len = sizeof client_addr;
        client_socket = accept(server_socket, (struct sockaddr *)&client_addr, &addr_len);
        if (client_socket == -1) {
            syslog(LOG_ERR, "Accept failed");
            continue;
        }

        client_thread_t *new_thread = (client_thread_t *)malloc(sizeof(client_thread_t));
        new_thread->client_socket = client_socket;
        new_thread->client_addr = (struct sockaddr_in *)&client_addr;
        new_thread->completed = false;

        pthread_create(&new_thread->worker_thread, NULL, client_handler, (void *)new_thread);
        SLIST_INSERT_HEAD(&active_threads, new_thread, entries);
    }

    close(server_socket);
    remove(SOCKET_DATA_FILE);
    closelog();
    return 0;
}

