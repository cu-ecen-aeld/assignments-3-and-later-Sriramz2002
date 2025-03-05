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
#include <stdbool.h>  // Fix for bool, true, false

#define PORT (9000)
#define DATAFILE "/var/tmp/aesdsocketdata"
#define PIDFILE "/var/run/aesdsocket.pid"

static volatile sig_atomic_t g_exit_flag = 0;
static pthread_mutex_t g_file_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_t g_timer_thread;

typedef struct client_thread_s {
    pthread_t thread_id;
    int client_fd;
    struct sockaddr_in client_addr;
    bool completed;
    SLIST_ENTRY(client_thread_s) entries;  // Fix: Ensure this field exists
} client_thread_t;

static SLIST_HEAD(client_list_head, client_thread_s) g_client_list =
    SLIST_HEAD_INITIALIZER(g_client_list);

static void signal_handler(int signo)
{
    g_exit_flag = 1;

    // Clean up resources before exiting
    syslog(LOG_INFO, "Caught signal %d, shutting down...", signo);

    remove(PIDFILE); // Ensure PID file is removed on exit
    remove(DATAFILE); // Delete the file if it exists
    closelog();

    exit(0);
}

static void* timestamp_thread_func(void *arg)
{
    (void)arg;  // Fix unused parameter warning

    while (!g_exit_flag) {
        sleep(10);

        time_t now = time(NULL);
        struct tm t;
        localtime_r(&now, &t);

        char timestamp_str[128];
        strftime(timestamp_str, sizeof(timestamp_str),
                 "timestamp:%a, %d %b %Y %T %z\n", &t);

        pthread_mutex_lock(&g_file_mutex);
        int file_fd = open(DATAFILE, O_WRONLY | O_APPEND | O_CREAT, 0666);
        if (file_fd != -1) {
            write(file_fd, timestamp_str, strlen(timestamp_str));
            fsync(file_fd);
            close(file_fd);
        }
        pthread_mutex_unlock(&g_file_mutex);
    }
    return NULL;
}

static void* client_thread_func(void *arg)
{
    client_thread_t *client_info = (client_thread_t*)arg;
    syslog(LOG_INFO, "Accepted connection from %s",
           inet_ntoa(client_info->client_addr.sin_addr));

    char *recvbuf = malloc(1024);
    if (!recvbuf) {
        syslog(LOG_ERR, "malloc() failed");
        close(client_info->client_fd);
        pthread_exit(NULL);
    }

    size_t total_recv = 0;
    ssize_t rc;

    while (!g_exit_flag && (rc = recv(client_info->client_fd, recvbuf + total_recv, 1024 - total_recv - 1, 0)) > 0) {
        total_recv += rc;
        recvbuf[total_recv] = '\0';

        if (strchr(recvbuf, '\n')) break;
    }

    pthread_mutex_lock(&g_file_mutex);
    int file_fd = open(DATAFILE, O_WRONLY | O_APPEND | O_CREAT, 0666);
    if (file_fd != -1) {
        write(file_fd, recvbuf, total_recv);
        fsync(file_fd);
        close(file_fd);
    }
    pthread_mutex_unlock(&g_file_mutex);

    pthread_mutex_lock(&g_file_mutex);
    file_fd = open(DATAFILE, O_RDONLY);
    if (file_fd != -1) {
        char readbuf[1024];
        ssize_t read_bytes;
        while ((read_bytes = read(file_fd, readbuf, sizeof(readbuf))) > 0) {
            send(client_info->client_fd, readbuf, read_bytes, 0);
        }
        close(file_fd);
    }
    pthread_mutex_unlock(&g_file_mutex);

    syslog(LOG_INFO, "Closing connection from %s",
           inet_ntoa(client_info->client_addr.sin_addr));

    close(client_info->client_fd);
    free(recvbuf);
    client_info->completed = true;
    pthread_exit(NULL);
}

int main(int argc, char *argv[])
{
    int daemon_mode = (argc == 2 && strcmp(argv[1], "-d") == 0);

    openlog("aesdsocket", LOG_PID, LOG_USER);

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        syslog(LOG_ERR, "socket() failed: %s", strerror(errno));
        return EXIT_FAILURE;
    }

    int optval = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

    struct sockaddr_in servaddr = {0};
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = INADDR_ANY;
    servaddr.sin_port = htons(PORT);

    if (bind(listen_fd, (struct sockaddr *)&servaddr, sizeof(servaddr)) < 0) {
        syslog(LOG_ERR, "bind() failed: %s", strerror(errno));
        close(listen_fd);
        return EXIT_FAILURE;
    }

    if (daemon_mode) {
        pid_t pid = fork();
        if (pid < 0) return EXIT_FAILURE;
        if (pid > 0) return EXIT_SUCCESS;
        umask(0);
        setsid();
        chdir("/");
        freopen("/dev/null", "r", stdin);
        freopen("/dev/null", "w", stdout);
        freopen("/dev/null", "w", stderr);

        FILE *pidfp = fopen(PIDFILE, "w");
        if (pidfp) {
            fprintf(pidfp, "%d\n", getpid());
            fclose(pidfp);
        }
    }

    if (listen(listen_fd, 10) < 0) {
        syslog(LOG_ERR, "listen() failed: %s", strerror(errno));
        close(listen_fd);
        return EXIT_FAILURE;
    }

    pthread_create(&g_timer_thread, NULL, timestamp_thread_func, NULL);

    while (!g_exit_flag) {
        struct sockaddr_in clientaddr;
        socklen_t addrlen = sizeof(clientaddr);
        int client_fd = accept(listen_fd, (struct sockaddr*)&clientaddr, &addrlen);
        if (client_fd < 0) continue;

        client_thread_t *ct = calloc(1, sizeof(client_thread_t));
        ct->client_fd = client_fd;
        ct->client_addr = clientaddr;
        ct->completed = false;

        SLIST_INSERT_HEAD(&g_client_list, ct, entries);
        pthread_create(&ct->thread_id, NULL, client_thread_func, ct);

        client_thread_t *it, *tmp;
        SLIST_FOREACH(it, &g_client_list, entries) {
            if (it->completed) {
                pthread_join(it->thread_id, NULL);
                SLIST_REMOVE(&g_client_list, it, client_thread_s, entries);
                free(it);
            }
        }
    }

    close(listen_fd);
    remove(DATAFILE);
    remove(PIDFILE);
    closelog();
    return 0;
}

