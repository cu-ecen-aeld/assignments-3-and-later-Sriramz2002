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
#include <netdb.h>

#define PORT (9000)
#define DATAFILE "/var/tmp/aesdsocketdata"

static volatile sig_atomic_t g_exit_flag = 0;

// Mutex to protect file I/O to DATAFILE
static pthread_mutex_t g_file_mutex = PTHREAD_MUTEX_INITIALIZER;

static pthread_t g_timer_thread;

typedef struct {
    int listen_fd;
    FILE *data_fp;      // File pointer for DATAFILE
} global_resources_t;

static global_resources_t g_resources = {
    .listen_fd = -1,
    .data_fp   = NULL
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
    if (g_resources.listen_fd >= 0) {
        close(g_resources.listen_fd);
        g_resources.listen_fd = -1;
    }
}

static void* timestamp_thread_func(void *arg)
{
    (void)arg;
    while (!g_exit_flag) {
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
        // Ensure we write at the end of file
        fseek(g_resources.data_fp, 0, SEEK_END);
        fwrite(timestamp_str, 1, strlen(timestamp_str), g_resources.data_fp);
        fflush(g_resources.data_fp);
        fsync(fileno(g_resources.data_fp));
        pthread_mutex_unlock(&g_file_mutex);
    }
    pthread_exit(NULL);
}

static void* client_thread_func(void *arg)
{
    client_thread_t *client_info = (client_thread_t*)arg;

    syslog(LOG_INFO, "Accepted connection from %s",
           inet_ntoa(client_info->client_addr.sin_addr));

    size_t bufsize = 1024;
    char *recvbuf = malloc(bufsize);
    if (!recvbuf) {
        syslog(LOG_ERR, "malloc() failed for client buffer");
        goto done;
    }
    memset(recvbuf, 0, bufsize);

    size_t total_recv = 0;
    ssize_t rc;

    while (!g_exit_flag && (rc = recv(client_info->client_fd,
                                      recvbuf + total_recv,
                                      bufsize - total_recv - 1, 0)) > 0) {
        total_recv += rc;
        recvbuf[total_recv] = '\0';

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
        if (memchr(recvbuf, '\n', total_recv)) {
            break;
        }
    }

    if (recvbuf && total_recv > 0) {
        pthread_mutex_lock(&g_file_mutex);

        // Move to end and write the received data
        fseek(g_resources.data_fp, 0, SEEK_END);
        if (fwrite(recvbuf, 1, total_recv, g_resources.data_fp) != total_recv) {
            syslog(LOG_ERR, "Error writing data to file");
        }
        fflush(g_resources.data_fp);
        fsync(fileno(g_resources.data_fp));

        // Read from beginning to send back all file data
        fseek(g_resources.data_fp, 0, SEEK_SET);
        memset(recvbuf, 0, bufsize);
        while (fgets(recvbuf, bufsize, g_resources.data_fp) != NULL) {
            send(client_info->client_fd, recvbuf, strlen(recvbuf), 0);
        }
        // Reset pointer for future writes
        fseek(g_resources.data_fp, 0, SEEK_END);

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

    // Use getaddrinfo for protocol-independent binding.
    struct addrinfo hints, *servinfo;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC; // IPv4 or IPv6
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    int status;
    if ((status = getaddrinfo(NULL, "9000", &hints, &servinfo)) != 0) {
        syslog(LOG_ERR, "getaddrinfo error: %s", gai_strerror(status));
        return EXIT_FAILURE;
    }

    g_resources.listen_fd = socket(servinfo->ai_family, servinfo->ai_socktype, servinfo->ai_protocol);
    if (g_resources.listen_fd < 0) {
        syslog(LOG_ERR, "socket() failed: %s", strerror(errno));
        freeaddrinfo(servinfo);
        return EXIT_FAILURE;
    }

    int optval = 1;
    setsockopt(g_resources.listen_fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

    if (bind(g_resources.listen_fd, servinfo->ai_addr, servinfo->ai_addrlen) < 0) {
        syslog(LOG_ERR, "bind() failed: %s", strerror(errno));
        freeaddrinfo(servinfo);
        close(g_resources.listen_fd);
        return EXIT_FAILURE;
    }
    
    freeaddrinfo(servinfo);

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
        freopen("/dev/null", "r", stdin);
        freopen("/dev/null", "w", stdout);
        freopen("/dev/null", "w", stderr);

        FILE *pidfp = fopen("/var/run/aesdsocket.pid", "w");
        if (pidfp) {
            fprintf(pidfp, "%d\n", getpid());
            fclose(pidfp);
        } else {
            syslog(LOG_ERR, "could not open /var/run/aesdsocket.pid for writing");
        }
    }

    if (listen(g_resources.listen_fd, 10) < 0) {
        syslog(LOG_ERR, "listen() failed: %s", strerror(errno));
        close(g_resources.listen_fd);
        return EXIT_FAILURE;
    }

    // Open the file in append mode so that existing content isn’t lost.
    g_resources.data_fp = fopen(DATAFILE, "a+");
    if (!g_resources.data_fp) {
        syslog(LOG_ERR, "fopen(%s) failed: %s", DATAFILE, strerror(errno));
        close(g_resources.listen_fd);
        return EXIT_FAILURE;
    }

    if (pthread_create(&g_timer_thread, NULL, timestamp_thread_func, NULL) != 0) {
        syslog(LOG_ERR, "pthread_create for timer thread failed");
    }

    syslog(LOG_INFO, "aesdsocket started, listening on port %d", PORT);

    while (!g_exit_flag) {
        struct sockaddr_in clientaddr;
        socklen_t addrlen = sizeof(clientaddr);
        int client_fd = accept(g_resources.listen_fd,
                               (struct sockaddr*)&clientaddr,
                               &addrlen);
        if (g_exit_flag) {
            break;
        }
        if (client_fd < 0) {
            if (errno == EINTR) continue;
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

    syslog(LOG_INFO, "shutting down aesdsocket...........................................");

    if (g_resources.listen_fd >= 0) {
        close(g_resources.listen_fd);
        g_resources.listen_fd = -1;
    }

    client_thread_t *it = NULL;
    client_thread_t *tmp = NULL;
    for (it = SLIST_FIRST(&g_client_list); it != NULL; it = tmp) {
        tmp = SLIST_NEXT(it, entries);
        pthread_join(it->thread_id, NULL);
        SLIST_REMOVE(&g_client_list, it, client_thread_s, entries);
        if (it->client_fd >= 0)
            close(it->client_fd);
        free(it);
    }

    pthread_join(g_timer_thread, NULL);

    if (g_resources.data_fp) {
        fclose(g_resources.data_fp);
        g_resources.data_fp = NULL;
    }

    remove(DATAFILE);

    syslog(LOG_INFO, "aesdsocket clean exit");
    closelog();

    return 0;
}

