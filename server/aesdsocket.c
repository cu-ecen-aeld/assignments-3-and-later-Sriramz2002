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
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <stdint.h>
#include <netdb.h>

#define SERVER_PORT "9000"
#define BACKLOG 10
#define BUFSIZE 1024
#define TIMESTAMP_INTERVAL 10

#define USE_AESD_CHAR_DEVICE 1

#if USE_AESD_CHAR_DEVICE
#define DATA_STORAGE "/dev/aesdchar"
#else
#define DATA_STORAGE "/var/tmp/aesdsocketdata"
#endif

#define IOCTL_CMD_PREFIX "AESDCHAR_IOCSEEKTO:"
#define IOCTL_CMD_PREFIX_LEN (sizeof(IOCTL_CMD_PREFIX) - 1)
#define AESDCHAR_IOC_MAGIC 0x16

struct aesd_seekto {
    uint32_t write_cmd;
    uint32_t write_cmd_offset;
};
#define AESDCHAR_IOCSEEKTO _IOWR(AESDCHAR_IOC_MAGIC, 1, struct aesd_seekto)

// Global variables
static volatile sig_atomic_t exit_requested = 0;
static pthread_mutex_t storage_lock = PTHREAD_MUTEX_INITIALIZER;
static int server_fd = -1;

// Thread management structure
typedef struct thread_tracker {
    pthread_t tid;
    int connection_fd;
    struct sockaddr_in client_info;
    struct thread_tracker *next;
} thread_tracker_t;

static thread_tracker_t *thread_list_head = NULL;

// Function prototypes
void setup_signal_handlers(void);
int initialize_server(void);
void *process_connection(void *connection_data);
void cleanup_resources(void);
void *timestamp_generator(void *arg);
void run_as_daemon(void);
void *get_client_addr(struct sockaddr *sa);

// Signal handler function
void handle_signals(int signo) {
    (void)signo; // Suppress unused parameter warning
    exit_requested = 1;
    // Closing the server socket will cause accept() to return with an error
    if (server_fd >= 0) {
        close(server_fd);
    }
}

// Sets up signal handling for graceful termination
void setup_signal_handlers(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_signals;
    sigemptyset(&sa.sa_mask);
    
    if (sigaction(SIGINT, &sa, NULL) == -1 || sigaction(SIGTERM, &sa, NULL) == -1) {
        syslog(LOG_ERR, "Failed to set up signal handlers: %s", strerror(errno));
        exit(EXIT_FAILURE);
    }
}

// Helper function to get socket address
void *get_client_addr(struct sockaddr *sa) {
    if (sa->sa_family == AF_INET) {
        return &(((struct sockaddr_in*)sa)->sin_addr);
    }
    return &(((struct sockaddr_in6*)sa)->sin6_addr);
}

// Initialize and set up server socket
int initialize_server(void) {
    int status, socket_opt = 1;
    struct addrinfo hints, *servinfo, *p;
    
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    
    if ((status = getaddrinfo(NULL, SERVER_PORT, &hints, &servinfo)) != 0) {
        syslog(LOG_ERR, "getaddrinfo error: %s", gai_strerror(status));
        return -1;
    }
    
    // Find valid address to bind to
    for (p = servinfo; p != NULL; p = p->ai_next) {
        if ((server_fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol)) == -1) {
            continue;
        }
        
        if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &socket_opt, sizeof(socket_opt)) == -1) {
            syslog(LOG_ERR, "setsockopt failed: %s", strerror(errno));
            close(server_fd);
            freeaddrinfo(servinfo);
            return -1;
        }
        
        if (bind(server_fd, p->ai_addr, p->ai_addrlen) == -1) {
            close(server_fd);
            continue;
        }
        
        break;
    }
    
    freeaddrinfo(servinfo);
    
    if (p == NULL) {
        syslog(LOG_ERR, "Failed to bind to any address");
        return -1;
    }
    
    if (listen(server_fd, BACKLOG) == -1) {
        syslog(LOG_ERR, "Listen failed: %s", strerror(errno));
        close(server_fd);
        return -1;
    }
    
    return server_fd;
}

// Process client connection in a separate thread
void *process_connection(void *connection_data) {
    thread_tracker_t *client_data = (thread_tracker_t *)connection_data;
    int client_fd = client_data->connection_fd;
    char *recv_buffer = malloc(BUFSIZE);
    char client_ip[INET6_ADDRSTRLEN];
    
    if (!recv_buffer) {
        syslog(LOG_ERR, "Memory allocation failed for connection buffer");
        close(client_fd);
        free(client_data);
        return NULL;
    }
    
    // Get client IP address
    inet_ntop(client_data->client_info.sin_family, 
              get_client_addr((struct sockaddr*)&client_data->client_info),
              client_ip, sizeof(client_ip));
    
    syslog(LOG_INFO, "Connection accepted from %s", client_ip);
    
    ssize_t bytes_read;
    size_t total_bytes = 0;
    int storage_fd;
    int is_ioctl_cmd = 0;
    
    // Receive data from client
    while ((bytes_read = recv(client_fd, recv_buffer + total_bytes, 
                             BUFSIZE - total_bytes - 1, 0)) > 0) {
        total_bytes += bytes_read;
        recv_buffer[total_bytes] = '\0';
        
        // Resize buffer if needed
        if (total_bytes >= BUFSIZE - 1) {
            char *new_buffer = realloc(recv_buffer, total_bytes * 2);
            if (!new_buffer) {
                syslog(LOG_ERR, "Failed to resize receive buffer");
                break;
            }
            recv_buffer = new_buffer;
        }
        
        // Check for newline to end command
        if (strchr(recv_buffer, '\n')) break;
    }
    
    pthread_mutex_lock(&storage_lock);
    
    if (total_bytes > 0) {
        // Check if it's an IOCTL command
        if (strncmp(recv_buffer, IOCTL_CMD_PREFIX, IOCTL_CMD_PREFIX_LEN) == 0) {
            unsigned int cmd_index = 0, cmd_offset = 0;
            
            if (sscanf(recv_buffer, "AESDCHAR_IOCSEEKTO:%u,%u", &cmd_index, &cmd_offset) == 2) {
                syslog(LOG_INFO, "IOCTL seek request: cmd=%u offset=%u", cmd_index, cmd_offset);
                
                struct aesd_seekto seek_params;
                seek_params.write_cmd = cmd_index;
                seek_params.write_cmd_offset = cmd_offset;
                
                storage_fd = open(DATA_STORAGE, O_RDWR);
                if (storage_fd == -1) {
                    syslog(LOG_ERR, "Failed to open storage file: %s", strerror(errno));
                    pthread_mutex_unlock(&storage_lock);
                    close(client_fd);
                    free(recv_buffer);
                    free(client_data);
                    return NULL;
                }
                
                if (ioctl(storage_fd, AESDCHAR_IOCSEEKTO, &seek_params) == -1) {
                    syslog(LOG_ERR, "IOCTL seek operation failed: %s", strerror(errno));
                }
                is_ioctl_cmd = 1;
            } else {
                syslog(LOG_ERR, "Invalid IOCTL command format");
                pthread_mutex_unlock(&storage_lock);
                close(client_fd);
                free(recv_buffer);
                free(client_data);
                return NULL;
            }
        } else {
            // Normal write operation
            storage_fd = open(DATA_STORAGE, O_RDWR | O_CREAT | O_APPEND, 0666);
            if (storage_fd == -1) {
                syslog(LOG_ERR, "Failed to open storage file: %s", strerror(errno));
                pthread_mutex_unlock(&storage_lock);
                close(client_fd);
                free(recv_buffer);
                free(client_data);
                return NULL;
            }
            
            if (write(storage_fd, recv_buffer, total_bytes) == -1) {
                syslog(LOG_ERR, "Failed to write to storage: %s", strerror(errno));
            }
        }
        
        // Send file content back to client
        char read_buffer[BUFSIZE];
        ssize_t read_bytes;
        
        if (!is_ioctl_cmd) {
            lseek(storage_fd, 0, SEEK_SET);
        }
        
        while ((read_bytes = read(storage_fd, read_buffer, sizeof(read_buffer))) > 0) {
            send(client_fd, read_buffer, read_bytes, 0);
        }
        
        close(storage_fd);
    }
    
    pthread_mutex_unlock(&storage_lock);
    
    // Clean up connection
    syslog(LOG_INFO, "Connection closed from %s", client_ip);
    close(client_fd);
    free(recv_buffer);
    free(client_data);
    return NULL;
}

// Timestamp thread function
void *timestamp_generator(void *arg) {
    (void)arg; // Suppress unused parameter warning
    
    while (!exit_requested) {
        sleep(TIMESTAMP_INTERVAL);
        
        time_t current_time = time(NULL);
        struct tm time_info;
        char timestamp[BUFSIZE];
        
        localtime_r(&current_time, &time_info);
        strftime(timestamp, sizeof(timestamp), "timestamp:%Y-%m-%d %H:%M:%S\n", &time_info);
        
        pthread_mutex_lock(&storage_lock);
        int fd = open(DATA_STORAGE, O_WRONLY | O_APPEND | O_CREAT, 0666);
        if (fd != -1) {
            write(fd, timestamp, strlen(timestamp));
            close(fd);
        } else {
            syslog(LOG_ERR, "Failed to write timestamp: %s", strerror(errno));
        }
        pthread_mutex_unlock(&storage_lock);
    }
    
    return NULL;
}

// Clean up resources before exiting
void cleanup_resources(void) {
    // Join all client threads
    thread_tracker_t *current = thread_list_head;
    thread_tracker_t *next;
    
    while (current != NULL) {
        pthread_join(current->tid, NULL);
        next = current->next;
        free(current);
        current = next;
    }
    
#if !USE_AESD_CHAR_DEVICE
    // Remove data file if not using char device
    remove(DATA_STORAGE);
#endif
    
    // Close server socket if still open
    if (server_fd >= 0) {
        close(server_fd);
    }
}

// Run as a daemon process
void run_as_daemon(void) {
    pid_t pid = fork();
    
    if (pid < 0) {
        syslog(LOG_ERR, "Daemon creation failed: %s", strerror(errno));
        exit(EXIT_FAILURE);
    }
    
    if (pid > 0) {
        // Parent process exits
        exit(EXIT_SUCCESS);
    }
    
    // Create new session
    if (setsid() < 0) {
        syslog(LOG_ERR, "Failed to set session ID: %s", strerror(errno));
        exit(EXIT_FAILURE);
    }
    
    // Second fork to prevent TTY acquisition
    pid = fork();
    
    if (pid < 0) {
        syslog(LOG_ERR, "Second fork failed: %s", strerror(errno));
        exit(EXIT_FAILURE);
    }
    
    if (pid > 0) {
        // Parent process exits
        exit(EXIT_SUCCESS);
    }
    
    // Change working directory and reset file creation mask
    umask(0);
    chdir("/");
    
    // Close standard file descriptors
    close(STDIN_FILENO);
    close(STDOUT_FILENO);
    close(STDERR_FILENO);
}

// Main function
int main(int argc, char *argv[]) {
    int daemon_mode = 0;
    
    // Check for daemon mode flag
    if (argc > 1 && strcmp(argv[1], "-d") == 0) {
        daemon_mode = 1;
    }
    
    // Initialize syslog
    openlog("aesdsocket", LOG_PID, LOG_USER);
    
    // Set up signal handlers
    setup_signal_handlers();
    
    // Initialize server socket
    server_fd = initialize_server();
    if (server_fd < 0) {
        syslog(LOG_ERR, "Server initialization failed");
        exit(EXIT_FAILURE);
    }
    
    // Run as daemon if requested
    if (daemon_mode) {
        run_as_daemon();
    }
    
#if !USE_AESD_CHAR_DEVICE
    // Start timestamp thread if not using char device
    pthread_t timestamp_tid;
    pthread_create(&timestamp_tid, NULL, timestamp_generator, NULL);
#endif
    
    syslog(LOG_INFO, "Server started, waiting for connections");
    
    // Main server loop
    while (!exit_requested) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        
        int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &addr_len);
        
        if (exit_requested) break;
        
        if (client_fd < 0) {
            if (errno != EINTR) {
                syslog(LOG_ERR, "Accept failed: %s", strerror(errno));
            }
            continue;
        }
        
        // Create client thread structure
        thread_tracker_t *client_thread = malloc(sizeof(thread_tracker_t));
        if (!client_thread) {
            syslog(LOG_ERR, "Failed to allocate memory for thread tracking");
            close(client_fd);
            continue;
        }
        
        // Fill in client information
        client_thread->connection_fd = client_fd;
        memcpy(&client_thread->client_info, &client_addr, sizeof(client_addr));
        
        // Create thread to handle client
        if (pthread_create(&client_thread->tid, NULL, process_connection, client_thread) != 0) {
            syslog(LOG_ERR, "Failed to create client thread: %s", strerror(errno));
            close(client_fd);
            free(client_thread);
            continue;
        }
        
        // Add to thread list
        client_thread->next = thread_list_head;
        thread_list_head = client_thread;
    }
    
    syslog(LOG_INFO, "Server shutting down");
    
#if !USE_AESD_CHAR_DEVICE
    // Clean up timestamp thread
    pthread_cancel(timestamp_tid);
    pthread_join(timestamp_tid, NULL);
#endif
    
    // Clean up resources
    cleanup_resources();
    
    closelog();
    return EXIT_SUCCESS;
}
