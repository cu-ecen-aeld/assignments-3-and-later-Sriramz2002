#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <syslog.h>
#include <signal.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>

#define PORT 9000
#define BUFFER_SIZE 1024
#define FILE_PATH "/var/tmp/aesdsocketdata"

int server_fd, client_fd;
FILE *data_file;

void cleanup(int signum) {
    syslog(LOG_INFO, "Caught signal, exiting");
    if (client_fd > 0) close(client_fd);
    if (server_fd > 0) close(server_fd);
    if (data_file) fclose(data_file);
    remove(FILE_PATH);
    closelog();
    exit(EXIT_SUCCESS);
}

void daemonize() {
    pid_t pid = fork();

    if (pid < 0) {
        syslog(LOG_ERR, "Fork failed: %s", strerror(errno));
        exit(EXIT_FAILURE);
    }

    if (pid > 0) {
        // Parent process exits
        exit(EXIT_SUCCESS);
    }

    // Child process continues
    umask(0);

    if (setsid() < 0) {
        syslog(LOG_ERR, "Failed to create new session: %s", strerror(errno));
        exit(EXIT_FAILURE);
    }

    // Redirect standard files to /dev/null
    freopen("/dev/null", "r", stdin);
    freopen("/dev/null", "w", stdout);
    freopen("/dev/null", "w", stderr);

    // Change working directory
    if (chdir("/") < 0) {
        syslog(LOG_ERR, "Failed to change directory: %s", strerror(errno));
        exit(EXIT_FAILURE);
    }
}

int main(int argc, char *argv[]) {
    struct sockaddr_in address, client_addr;
    socklen_t client_addr_len = sizeof(client_addr);
    char buffer[BUFFER_SIZE];
    ssize_t bytes_read;
    int daemon_mode = 0;

    // Open syslog
    openlog("aesdsocket", LOG_PID | LOG_CONS, LOG_USER);

    // Check for -d flag
    if (argc == 2 && strcmp(argv[1], "-d") == 0) {
        daemon_mode = 1;
    }

    // Handle signals
    signal(SIGINT, cleanup);
    signal(SIGTERM, cleanup);

    // Create socket
    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
        syslog(LOG_ERR, "Failed to create socket: %s", strerror(errno));
        exit(EXIT_FAILURE);
    }

    // Bind socket
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(PORT);

    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) == -1) {
        syslog(LOG_ERR, "Bind failed: %s", strerror(errno));
        cleanup(SIGTERM);
    }

    if (daemon_mode) {
        daemonize();
    }

    // Listen
    if (listen(server_fd, 10) == -1) {
        syslog(LOG_ERR, "Listen failed: %s", strerror(errno));
        cleanup(SIGTERM);
    }

    // Open file for appending
    data_file = fopen(FILE_PATH, "a+");
    if (!data_file) {
        syslog(LOG_ERR, "Failed to open file: %s", strerror(errno));
        cleanup(SIGTERM);
    }

    while (1) {
        // Accept connection
        client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_addr_len);
        if (client_fd == -1) {
            syslog(LOG_ERR, "Accept failed: %s", strerror(errno));
            continue;
        }

        syslog(LOG_INFO, "Accepted connection from %s", inet_ntoa(client_addr.sin_addr));

        // Receive and write data
        while ((bytes_read = recv(client_fd, buffer, BUFFER_SIZE - 1, 0)) > 0) {
            buffer[bytes_read] = '\0';

            // Write to file
            if (fputs(buffer, data_file) == EOF) {
                syslog(LOG_ERR, "Write to file failed: %s", strerror(errno));
            }
            fflush(data_file);

            // Check for newline to send data back
            if (strchr(buffer, '\n')) {
                fseek(data_file, 0, SEEK_SET);
                while (fgets(buffer, BUFFER_SIZE, data_file)) {
                    send(client_fd, buffer, strlen(buffer), 0);
                }
                fseek(data_file, 0, SEEK_END); // Reset file pointer to end for further appends
            }
        }

        syslog(LOG_INFO, "Closed connection from %s", inet_ntoa(client_addr.sin_addr));
        close(client_fd);
    }

    cleanup(SIGTERM);
    return 0;
}

