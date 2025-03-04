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
#define DATA_FILE "/var/tmp/aesdsocketdata"
#define INIT_BUFFER_SIZE 2000  // Initial buffer size
#define BUFFER_INCREMENT 1024  // Buffer growth chunk
#define MAX_PENDING_CONNECTIONS 10  // Hardcoded backlog

typedef struct client_thread_data client_thread_data_t;
struct client_thread_data {
    pthread_t thread_id;
    int client_socket;
    bool is_complete;
    struct sockaddr_in *client_addr;
    SLIST_ENTRY(client_thread_data) thread_entry;
};

SLIST_HEAD(thread_list, client_thread_data) active_threads = SLIST_HEAD_INITIALIZER(active_threads);

int server_socket_fd;
volatile sig_atomic_t server_terminate_flag = 0;
pthread_t periodic_logger_thread;
pthread_mutex_t file_mutex = PTHREAD_MUTEX_INITIALIZER;

// Signal handler using switch-case
void handle_signal(int signal) {
    switch (signal) {
        case SIGINT:
        case SIGTERM:
            syslog(LOG_INFO, "Received signal %d, shutting down server.", signal);
            server_terminate_flag = 1;
            remove(DATA_FILE);

            if (server_socket_fd != -1) {
                close(server_socket_fd);
            }

            pthread_join(periodic_logger_thread, NULL);
            pthread_mutex_destroy(&file_mutex);
            closelog();
            exit(0);
            break;

        default:
            syslog(LOG_WARNING, "Unhandled signal: %d", signal);
            break;
    }
}

// Daemonizing function
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

// Periodically writes a timestamp every 10 seconds
void* timestamp_logger(void* arg) {
    while (!server_terminate_flag) {
        sleep(10);

        time_t current_time = time(NULL);
        struct tm *tm_info = localtime(&current_time);
        char timestamp[128];
        strftime(timestamp, sizeof(timestamp), "timestamp:%a, %d %b %Y %H:%M:%S %z\n", tm_info);

        pthread_mutex_lock(&file_mutex);
        int file_descriptor = open(DATA_FILE, O_WRONLY | O_APPEND | O_CREAT, 0644);
        if (file_descriptor != -1) {
            write(file_descriptor, timestamp, strlen(timestamp));
            close(file_descriptor);
        } else {
            syslog(LOG_ERR, "Error writing timestamp to file.");
        }
        pthread_mutex_unlock(&file_mutex);
    }
    return NULL;
}

// Handles client requests, receives data, writes to file, and responds
void* client_handler(void* arg) {
    client_thread_data_t *client_data = (client_thread_data_t *)arg;
    struct sockaddr_in *addr = client_data->client_addr;
    int client_socket = client_data->client_socket;
    int file_descriptor;

    syslog(LOG_INFO, "Connected to %s", inet_ntoa(addr->sin_addr));

    size_t buffer_capacity = INIT_BUFFER_SIZE;
    char *buffer = (char *)malloc(buffer_capacity);
    if (!buffer) {
        syslog(LOG_ERR, "Memory allocation failed.");
        close(client_socket);
        return NULL;
    }

    size_t received_bytes = 0;
    ssize_t bytes_read;

    pthread_mutex_lock(&file_mutex);
    file_descriptor = open(DATA_FILE, O_CREAT | O_WRONLY | O_APPEND, 0644);
    if (file_descriptor == -1) {
        syslog(LOG_ERR, "Failed to open file.");
        close(client_socket);
        free(buffer);
        pthread_mutex_unlock(&file_mutex);
        return NULL;
    }

    while ((bytes_read = recv(client_socket, buffer + received_bytes, buffer_capacity - received_bytes - 1, 0)) > 0) {
        received_bytes += bytes_read;
        buffer[received_bytes] = '\0';

        // Increase buffer size dynamically by fixed increments
        if (received_bytes >= buffer_capacity - 1) {
            size_t new_size = buffer_capacity + BUFFER_INCREMENT;
            char *temp_buffer = realloc(buffer, new_size);
            if (!temp_buffer) {
                syslog(LOG_ERR, "Memory reallocation failed.");
                break;
            }
            buffer = temp_buffer;
            buffer_capacity = new_size;
        }

        if (strchr(buffer, '\n')) {
            break;
        }
    }

    if (write(file_descriptor, buffer, received_bytes) == -1) {
        syslog(LOG_ERR, "Failed to write client data to file.");
    }

    fsync(file_descriptor);
    close(file_descriptor);
    pthread_mutex_unlock(&file_mutex);

    pthread_mutex_lock(&file_mutex);
    file_descriptor = open(DATA_FILE, O_RDONLY);
    if (file_descriptor != -1) {
        while ((bytes_read = read(file_descriptor, buffer, buffer_capacity)) > 0) {
            send(client_socket, buffer, bytes_read, 0);
        }
        close(file_descriptor);
    }
    pthread_mutex_unlock(&file_mutex);

    syslog(LOG_INFO, "Disconnected from %s", inet_ntoa(addr->sin_addr));
    close(client_socket);
    free(buffer);
    client_data->is_complete = true;
    return NULL;
}

// Main function to set up server, signal handling, and client connections
int main(int argc, char *argv[]) {
    int status, client_socket;
    struct addrinfo hints, *server_info;
    int run_as_daemon = 0;
    struct sockaddr_storage client_addr;
    socklen_t addr_len;

    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    if (argc == 2 && strcmp(argv[1], "-d") == 0) {
        run_as_daemon = 1;
    }

    openlog("aesdsocket", LOG_PID, LOG_USER);
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    if ((status = getaddrinfo(NULL, "9000", &hints, &server_info)) != 0) {
        syslog(LOG_ERR, "getaddrinfo error: %s", gai_strerror(status));
        exit(EXIT_FAILURE);
    }

    server_socket_fd = socket(server_info->ai_family, server_info->ai_socktype, server_info->ai_protocol);
    if (server_socket_fd == -1) {
        syslog(LOG_ERR, "Socket creation failed.");
        freeaddrinfo(server_info);
        exit(EXIT_FAILURE);
    }

    int option_value = 1;
    if (setsockopt(server_socket_fd, SOL_SOCKET, SO_REUSEADDR, &option_value, sizeof(option_value)) == -1) {
        syslog(LOG_ERR, "Failed to set socket options.");
        freeaddrinfo(server_info);
        close(server_socket_fd);
        exit(EXIT_FAILURE);
    }

    if (bind(server_socket_fd, server_info->ai_addr, server_info->ai_addrlen) == -1) {
        syslog(LOG_ERR, "Binding socket failed.");
        freeaddrinfo(server_info);
        close(server_socket_fd);
        exit(EXIT_FAILURE);
    }
    freeaddrinfo(server_info);

    if (run_as_daemon) {
        daemonize_process();
        syslog(LOG_INFO, "Server daemonized.");
    }

    if (listen(server_socket_fd, MAX_PENDING_CONNECTIONS) == -1) {
        syslog(LOG_ERR, "Listening failed.");
        close(server_socket_fd);
        exit(EXIT_FAILURE);
    }

    syslog(LOG_INFO, "Server running on port %d.", SERVER_PORT);
    pthread_create(&periodic_logger_thread, NULL, timestamp_logger, NULL);

    while (!server_terminate_flag) {
        addr_len = sizeof client_addr;
        client_socket = accept(server_socket_fd, (struct sockaddr *)&client_addr, &addr_len);
        if (client_socket == -1) {
            syslog(LOG_ERR, "Accept failed.");
            continue;
        }

        client_thread_data_t *new_client = (client_thread_data_t *)malloc(sizeof(client_thread_data_t));
        new_client->client_socket = client_socket;
        new_client->client_addr = (struct sockaddr_in *)&client_addr;
        new_client->is_complete = false;

        pthread_create(&new_client->thread_id, NULL, client_handler, (void *)new_client);
        SLIST_INSERT_HEAD(&active_threads, new_client, thread_entry);
    }

    close(server_socket_fd);
    remove(DATA_FILE);
    closelog();
    return 0;
}

