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

#define PORT (9000)
#define DATAFILE "/var/tmp/aesdsocketdata"

static volatile sig_atomic_t g_exit_flag = 0; 

// Mutex for file access synchronization
static pthread_mutex_t g_file_mutex = PTHREAD_MUTEX_INITIALIZER;

static pthread_t g_timer_thread; 

typedef struct {
    int listen_fd;
} global_resources_t;

static global_resources_t g_resources = {
    .listen_fd = -1
};

typedef struct client_thread_s {
    pthread_t thread_id;
    int client_fd;
    struct sockaddr_in client_addr;
    SLIST_ENTRY(client_thread_s) entries;
} client_thread_t;

static SLIST_HEAD(client_list_head, client_thread_s) g_client_list =
    SLIST_HEAD_INITIALIZER(g_client_list);

static void signal_handler(int signo)
{
    (void)signo;
    g_exit_flag = 1;

    if (g_resources.listen_fd >= 0) 
    {
        close(g_resources.listen_fd);
        g_resources.listen_fd = -1;
    }
}

static void* timestamp_thread_func(void *arg)
{
    (void)arg; 

    while (!g_exit_flag) {
        // Sleep for 10 seconds between timestamp writes
        for (int i = 0; i < 10 && !g_exit_flag; i++) {
            sleep(1); 
        }

        if (g_exit_flag) break;  

        time_t now = time(NULL);
        struct tm t;
        localtime_r(&now, &t);

        char timestamp_str[128];
        strftime(timestamp_str, sizeof(timestamp_str),
                 "timestamp:%a, %d %b %Y %T %z\n", &t);

        pthread_mutex_lock(&g_file_mutex);

        // Open file for writing timestamp
        int fd = open(DATAFILE, O_CREAT | O_WRONLY | O_APPEND, 0666);
        if (fd < 0) {
            syslog(LOG_ERR, "Failed to open file for timestamp: %s", strerror(errno));
            pthread_mutex_unlock(&g_file_mutex);
            continue;
        }

        // Write timestamp
        if (write(fd, timestamp_str, strlen(timestamp_str)) < 0) {
            syslog(LOG_ERR, "Failed to write timestamp: %s", strerror(errno));
        }
        
        // Close file
        close(fd);
        pthread_mutex_unlock(&g_file_mutex);
    }
    pthread_exit(NULL);
}

static void* client_thread_func(void *arg)
{
    client_thread_t *client_info = (client_thread_t*)arg;

    syslog(LOG_INFO, "Accepted connection from %s",
           inet_ntoa(client_info->client_addr.sin_addr));

    // Dynamic buffer allocation with initial size
    size_t bufsize = 1024;
    char *recvbuf = malloc(bufsize);
    if (!recvbuf) {
        syslog(LOG_ERR, "malloc() failed for client buffer");
        goto done;
    }
    memset(recvbuf, 0, bufsize);

    size_t total_recv = 0;
    ssize_t rc;

    // Receive data with dynamic buffer resizing
    while (!g_exit_flag && (rc = recv(client_info->client_fd, recvbuf + total_recv, bufsize - total_recv - 1, 0)) > 0) 
    {
        total_recv += rc;
        recvbuf[total_recv] = '\0'; 

        // Dynamically resize buffer if needed
        if (total_recv >= bufsize - 1) {
            size_t newsize = bufsize * 2;
            char *tmp = realloc(recvbuf, newsize);
            if (!tmp) {
                syslog(LOG_ERR, "realloc failed");
                goto done;
            }
            recvbuf = tmp;
            bufsize = newsize;
        }

        // Check for newline to complete message
        if (memchr(recvbuf, '\n', total_recv)) {
            break;
        }
    }

    // Process received data
    if (recvbuf && total_recv > 0) {
        pthread_mutex_lock(&g_file_mutex);

        // Open file for writing data
        int write_fd = open(DATAFILE, O_CREAT | O_WRONLY | O_APPEND, 0666);
        if (write_fd < 0) {
            syslog(LOG_ERR, "Failed to open file for writing: %s", strerror(errno));
            pthread_mutex_unlock(&g_file_mutex);
            goto done;
        }

        // Write received data
        if (write(write_fd, recvbuf, total_recv) != total_recv) {
            syslog(LOG_ERR, "Error writing data to file: %s", strerror(errno));
        }
        
        // Ensure data is written to disk
        fsync(write_fd);
        
        // Close write file descriptor
        close(write_fd);

        // Open file for reading
        int read_fd = open(DATAFILE, O_RDONLY);
        if (read_fd < 0) {
            syslog(LOG_ERR, "Failed to open file for reading: %s", strerror(errno));
            pthread_mutex_unlock(&g_file_mutex);
            goto done;
        }

        // Prepare a separate buffer for reading
        char *readbuf = malloc(bufsize);
        if (!readbuf) {
            syslog(LOG_ERR, "malloc() failed for read buffer");
            close(read_fd);
            pthread_mutex_unlock(&g_file_mutex);
            goto done;
        }

        // Read and send file contents
        ssize_t bytes_read;
        while ((bytes_read = read(read_fd, readbuf, bufsize - 1)) > 0) {
            readbuf[bytes_read] = '\0';
            send(client_info->client_fd, readbuf, bytes_read, 0);
        }

        // Clean up read resources
        free(readbuf);
        close(read_fd);
        pthread_mutex_unlock(&g_file_mutex);
    }

done:
    syslog(LOG_INFO, "Closing connection from %s",
           inet_ntoa(client_info->client_addr.sin_addr));

    if (client_info->client_fd >= 0) {
        close(client_info->client_fd);
        client_info->client_fd = -1;
    }
    if (recvbuf) {
        free(recvbuf);
    }
    pthread_exit(NULL);
}

int main(int argc, char *argv[])
{
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

    // Create socket
    g_resources.listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (g_resources.listen_fd < 0) {
        syslog(LOG_ERR, "socket() failed: %s", strerror(errno));
        return EXIT_FAILURE;
    }

    // Set socket options
    int optval = 1;
    setsockopt(g_resources.listen_fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

    // Bind socket
    struct sockaddr_in servaddr;
    memset(&servaddr, 0, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = INADDR_ANY;
    servaddr.sin_port = htons(PORT);

    if (bind(g_resources.listen_fd, (struct sockaddr *)&servaddr, sizeof(servaddr)) < 0) {
        syslog(LOG_ERR, "bind() failed: %s", strerror(errno));
        close(g_resources.listen_fd);
        return EXIT_FAILURE;
    }

    // Daemon mode
    if (daemon_mode) {
        pid_t pid = fork();
        if (pid < 0) {
            syslog(LOG_ERR, "fork failed: %s", strerror(errno));
            close(g_resources.listen_fd);
            return EXIT_FAILURE;
        } else if (pid > 0) {
            return EXIT_SUCCESS;
        }

        umask(0);
        if (setsid() < 0) {
            syslog(LOG_ERR, "setsid failed: %s", strerror(errno));
            close(g_resources.listen_fd);
            return EXIT_FAILURE;
        }

        // Redirect standard files
        freopen("/dev/null", "r", stdin);
        freopen("/dev/null", "w", stdout);
        freopen("/dev/null", "w", stderr);
    }

    // Listen on socket
    if (listen(g_resources.listen_fd, 10) < 0) {
        syslog(LOG_ERR, "listen() failed: %s", strerror(errno));
        close(g_resources.listen_fd);
        return EXIT_FAILURE;
    }

    // Create timestamp thread
    if (pthread_create(&g_timer_thread, NULL, timestamp_thread_func, NULL) != 0) {
        syslog(LOG_ERR, "pthread_create for timer thread failed");
        close(g_resources.listen_fd);
        return EXIT_FAILURE;
    }

    syslog(LOG_INFO, "aesdsocket started, listening on port %d", PORT);

    // Accept connections
    while (!g_exit_flag) {
        struct sockaddr_in clientaddr;
        socklen_t addrlen = sizeof(clientaddr);

        int client_fd = accept(g_resources.listen_fd,
                               (struct sockaddr*)&clientaddr,
                               &addrlen);
        if (g_exit_flag) break;
        
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            syslog(LOG_ERR, "accept failed: %s", strerror(errno));
            break;
        }

        // Create client thread
        client_thread_t *ct = calloc(1, sizeof(client_thread_t));
        if (!ct) {
            syslog(LOG_ERR, "calloc() for client_thread_t failed");
            close(client_fd);
            continue;
        }
        ct->client_fd = client_fd;
        ct->client_addr = clientaddr;

        SLIST_INSERT_HEAD(&g_client_list, ct, entries);

        if (pthread_create(&ct->thread_id, NULL, client_thread_func, ct) != 0) {
            syslog(LOG_ERR, "pthread_create for client failed");
            close(client_fd);
            SLIST_REMOVE(&g_client_list, ct, client_thread_s, entries);
            free(ct);
        }
    }

    // Cleanup
    syslog(LOG_INFO, "shutting down aesdsocket");

    if (g_resources.listen_fd >= 0) {
        close(g_resources.listen_fd);
        g_resources.listen_fd = -1;
    }

    // Join and cleanup threads
    client_thread_t *it = NULL;
    client_thread_t *tmp = NULL;

    for (it = SLIST_FIRST(&g_client_list); it != NULL; it = tmp) {
        tmp = SLIST_NEXT(it, entries);

        pthread_join(it->thread_id, NULL);

        SLIST_REMOVE(&g_client_list, it, client_thread_s, entries);
        if (it->client_fd >= 0) close(it->client_fd);
        free(it);
    }

    // Join timestamp thread
    pthread_join(g_timer_thread, NULL);

    // Remove data file
    remove(DATAFILE);

    syslog(LOG_INFO, "aesdsocket clean exit");
    closelog();

    return 0;
}
