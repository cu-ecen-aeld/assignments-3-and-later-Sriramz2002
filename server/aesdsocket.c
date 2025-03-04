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

#define PORT 9000
#define DATAFILE "/var/tmp/aesdsocketdata"

static volatile sig_atomic_t g_exit_flag = 0;
static pthread_mutex_t g_file_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_t g_timer_thread;

// Global resource structure (FILE pointer removed)
typedef struct {
    int listen_fd;
} global_resources_t;

static global_resources_t g_resources = {
    .listen_fd = -1
};

// Structure to track client threads using SLIST
typedef struct client_thread_s {
    pthread_t thread_id;
    int client_fd;
    struct sockaddr_in client_addr;
    SLIST_ENTRY(client_thread_s) entries;
} client_thread_t;

static SLIST_HEAD(client_list_head, client_thread_s) g_client_list =
    SLIST_HEAD_INITIALIZER(g_client_list);

// Signal handler: sets exit flag and closes the listen socket.
static void signal_handler(int signo)
{
    (void)signo;
    g_exit_flag = 1;
    if (g_resources.listen_fd >= 0) {
        close(g_resources.listen_fd);
        g_resources.listen_fd = -1;
    }
}

// Timestamp thread: every 10 seconds, append a timestamp to the data file.
// Uses open() with mode 0644 (owner read/write; group/others read).
static void* timestamp_thread_func(void *arg)
{
    (void)arg;
    while (!g_exit_flag) {
        // Wait 10 seconds (checking exit flag each second)
        for (int i = 0; i < 10 && !g_exit_flag; i++) {
            sleep(1);
        }
        if (g_exit_flag)
            break;
        time_t now = time(NULL);
        struct tm t;
        localtime_r(&now, &t);
        char timestamp_str[128];
        strftime(timestamp_str, sizeof(timestamp_str),
                 "timestamp:%a, %d %b %Y %T %z\n", &t);

        pthread_mutex_lock(&g_file_mutex);
        int fd = open(DATAFILE, O_WRONLY | O_APPEND | O_CREAT, 0644);
        if (fd >= 0) {
            write(fd, timestamp_str, strlen(timestamp_str));
            close(fd);
        } else {
            syslog(LOG_ERR, "Failed to write timestamp: %s", strerror(errno));
        }
        pthread_mutex_unlock(&g_file_mutex);
    }
    pthread_exit(NULL);
}

// Client handler: reads data from the client until newline,
// writes it to the file, then reads back the entire file and sends it to the client.
// The initial dynamic buffer size is hardcoded to 2000 bytes.
static void* client_thread_func(void *arg)
{
    client_thread_t *client_info = (client_thread_t*)arg;
    int client_fd = client_info->client_fd;
    syslog(LOG_INFO, "Accepted connection from %s",
           inet_ntoa(client_info->client_addr.sin_addr));

    size_t buffer_size = 2000;  // hardcoded initial buffer size
    char *recvbuf = malloc(buffer_size);
    if (!recvbuf) {
        syslog(LOG_ERR, "malloc() failed for client buffer");
        close(client_fd);
        pthread_exit(NULL);
    }
    memset(recvbuf, 0, buffer_size);

    size_t total_received = 0;
    ssize_t rc;

    // Write client data to file (using open/close for each operation)
    pthread_mutex_lock(&g_file_mutex);
    int fd = open(DATAFILE, O_WRONLY | O_APPEND | O_CREAT, 0644);
    if (fd == -1) {
        syslog(LOG_ERR, "Failed to open file");
        close(client_fd);
        free(recvbuf);
        pthread_mutex_unlock(&g_file_mutex);
        pthread_exit(NULL);
    }
    while ((rc = recv(client_fd, recvbuf + total_received,
                      buffer_size - total_received - 1, 0)) > 0) {
        total_received += rc;
        recvbuf[total_received] = '\0';

        // Dynamically resize buffer if full
        if (total_received >= buffer_size - 1) {
            size_t new_size = buffer_size * 2;
            char *tmp = realloc(recvbuf, new_size);
            if (!tmp) {
                syslog(LOG_ERR, "realloc failed");
                break;
            }
            recvbuf = tmp;
            buffer_size = new_size;
        }
        if (memchr(recvbuf, '\n', total_received))
            break;
    }
    if (write(fd, recvbuf, total_received) != (ssize_t)total_received) {
        syslog(LOG_ERR, "Error writing data to file");
    }
    fsync(fd);
    close(fd);
    pthread_mutex_unlock(&g_file_mutex);

    // Read back entire file and send to client
    pthread_mutex_lock(&g_file_mutex);
    fd = open(DATAFILE, O_RDONLY);
    if (fd != -1) {
        while ((rc = read(fd, recvbuf, buffer_size)) > 0) {
            send(client_fd, recvbuf, rc, 0);
        }
        close(fd);
    }
    pthread_mutex_unlock(&g_file_mutex);

    syslog(LOG_INFO, "Closing connection from %s",
           inet_ntoa(client_info->client_addr.sin_addr));
    close(client_fd);
    free(recvbuf);
    pthread_exit(NULL);
}

// Daemonization: forks to detach from terminal.
static void daemonize(void)
{
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

// Main function: creates the server socket, sets up signal handling, spawns the timestamp thread,
// and enters the accept loop. Client threads are tracked via an SLIST.
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

    g_resources.listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (g_resources.listen_fd < 0) {
        syslog(LOG_ERR, "socket() failed: %s", strerror(errno));
        return EXIT_FAILURE;
    }

    int optval = 1;
    setsockopt(g_resources.listen_fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

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

    if (daemon_mode) {
        daemonize();
        syslog(LOG_INFO, "Daemonized the server");
    }

    // Hardcoded backlog value (10) in the listen() call.
    if (listen(g_resources.listen_fd, 10) < 0) {
        syslog(LOG_ERR, "listen() failed: %s", strerror(errno));
        close(g_resources.listen_fd);
        return EXIT_FAILURE;
    }

    pthread_create(&g_timer_thread, NULL, timestamp_thread_func, NULL);
    syslog(LOG_INFO, "aesdsocket started, listening on port %d", PORT);

    while (!g_exit_flag) {
        struct sockaddr_in clientaddr;
        socklen_t addrlen = sizeof(clientaddr);
        int client_fd = accept(g_resources.listen_fd, (struct sockaddr*)&clientaddr, &addrlen);
        if (g_exit_flag)
            break;
        if (client_fd < 0) {
            if (errno == EINTR)
                continue;
            syslog(LOG_ERR, "accept failed: %s", strerror(errno));
            break;
        }
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

    syslog(LOG_INFO, "shutting down aesdsocket...");
    if (g_resources.listen_fd >= 0) {
        close(g_resources.listen_fd);
        g_resources.listen_fd = -1;
    }

    client_thread_t *it = NULL;
    client_thread_t *temp = NULL;
    for (it = SLIST_FIRST(&g_client_list); it != NULL; it = temp) {
        temp = SLIST_NEXT(it, entries);
        pthread_join(it->thread_id, NULL);
        SLIST_REMOVE(&g_client_list, it, client_thread_s, entries);
        if (it->client_fd >= 0)
            close(it->client_fd);
        free(it);
    }

    pthread_join(g_timer_thread, NULL);
    remove("/var/tmp/aesdsocketdata");
    syslog(LOG_INFO, "aesdsocket clean exit");
    closelog();
    return 0;
}

