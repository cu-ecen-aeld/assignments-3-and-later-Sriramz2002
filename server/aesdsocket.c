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


#define PATH "/var/tmp/aesdsocketdata"




typedef struct 
{

    int server_fd;
    int client_fd;
    FILE *data_file;
    char *buffer;
    size_t buffer_size;
} server_resources_t;

server_resources_t *resources = NULL;

void cleanup(int signum) 
{
    syslog(LOG_INFO, "Caught signal %d, cleaning up and exiting", signum);
    if (resources)
    
     {
        if (resources->client_fd > 0) close(resources->client_fd);
        if (resources->server_fd > 0) close(resources->server_fd);
        if (resources->data_file) fclose(resources->data_file);
        if (resources->buffer) free(resources->buffer);
        free(resources);
    }
    remove(PATH);
    closelog();
    exit(EXIT_SUCCESS);
}

void setup_signal_handlers() 
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    
    sa.sa_handler = cleanup;
    sigaction(SIGINT, &sa, NULL);
    
    sigaction(SIGTERM, &sa, NULL);
}

void daemonize() 
{
    pid_t pid = fork();

    if (pid < 0) 
    {
        syslog(LOG_ERR, "fork failed: %s", strerror(errno));
        exit(EXIT_FAILURE);
    }

    if (pid > 0) 
    {
        // Parent exits
        exit(EXIT_SUCCESS); 
    }

    umask(0);

    if (setsid() < 0)
    {
        syslog(LOG_ERR, "failed to create new session: %s", strerror(errno));
        exit(EXIT_FAILURE);
    }

    // Redirect standard files to /dev/null
    freopen("/dev/null", "r", stdin);
    freopen("/dev/null", "w", stdout);
    freopen("/dev/null", "w", stderr);

    if (chdir("/") < 0) 
    {
        syslog(LOG_ERR, "failed to change directory: %s", strerror(errno));
        exit(EXIT_FAILURE);
    }


    FILE *pid_file = fopen("/var/run/aesdsocket.pid", "w");
    if (pid_file) {
        fprintf(pid_file, "%d\n", getpid());
        fclose(pid_file);
    } else {
        syslog(LOG_ERR, "failed to write PID file: %s", strerror(errno));
        exit(EXIT_FAILURE);
    }
}

int main(int argc, char *argv[]) 
{
    struct sockaddr_in address, client_addr ;
    
    socklen_t client_addr_len = sizeof(client_addr );
    ssize_t bytes_read;
    int daemon= 0  ;

    openlog("aesdsocket", LOG_PID | LOG_CONS, LOG_USER );


    if (argc == 2 && strcmp(argv[1], "-d") == 0  ) 
    {
        daemon= 1 ;
    }


    setup_signal_handlers();

    resources = calloc(1, sizeof(server_resources_t));
    if (!resources) {
        syslog(LOG_ERR, "failed to allocate memory for resources");
        exit(EXIT_FAILURE);
    }


    resources->buffer_size = 1000;
    resources->buffer = malloc(resources->buffer_size);
    if (!resources->buffer) 
    {
        syslog(LOG_ERR, "failed to allocate initial buffer memory");
        cleanup(SIGTERM);
    }

    // Create the socket
    resources->server_fd = socket(AF_INET, SOCK_STREAM, 0);
    
    if (resources->server_fd == -1) 
    {
        syslog(LOG_ERR, "failed to create socket: %s", strerror(errno));
        cleanup(SIGTERM);
    }

    // Allow reuse of the port
    int opt = 1;
    if (setsockopt(resources->server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == -1) {
        syslog(LOG_ERR, "setsockopt failed: %s", strerror(errno));
        cleanup(SIGTERM);
    }

    // Bind the socket
    memset(&address, 0, sizeof(address));
    
    address.sin_family = AF_INET;
    
    address.sin_addr.s_addr = INADDR_ANY;
    
    address.sin_port = htons(PORT);

    if (bind(resources->server_fd, (struct sockaddr *)&address, sizeof(address)) == -1) 
    {
        syslog(LOG_ERR, "Bind failed: %s", strerror(errno));
        
        cleanup(SIGTERM);
    }


    if (daemon) 
    {
        daemonize();
    }


    if (listen(resources->server_fd, 10) == -1) 
    {   syslog(LOG_ERR, "listen failed: %s", strerror(errno));
        cleanup(SIGTERM);
    }


    resources->data_file = fopen(PATH, "a+");
    if (!resources->data_file) {
        syslog(LOG_ERR, "failed to open file: %s", strerror(errno));
        cleanup(SIGTERM);
    }

    // Main loop for accepting connections
    while (1) 
    {
        resources->client_fd = accept(resources->server_fd , (struct sockaddr *)&client_addr, &client_addr_len);
        if (resources->client_fd == -1) {
            syslog(LOG_ERR, "Accept failed: %s", strerror(errno));
            continue;
        }

        syslog(LOG_INFO, "connection accepted:- %s", inet_ntoa(client_addr.sin_addr));

        size_t total_received = 0;
        ssize_t received;


        memset(resources->buffer, 0, resources->buffer_size);


        while ((received = recv(resources->client_fd , resources->buffer + total_received  , resources->buffer_size - total_received - 1  , 0) ) >  0) 
        {
            total_received += received;


            if (total_received >= resources->buffer_size - 1) 
            {
            
                size_t new_size = resources->buffer_size * 2;
                char *new_buffer = realloc(resources->buffer, new_size);
                if (!new_buffer) {
                    syslog(LOG_ERR, "failed_realloc ");
                    cleanup(SIGTERM);
                }
                resources->buffer = new_buffer;
                resources->buffer_size = new_size;
            }


            if (memchr(resources->buffer, '\n', total_received)) {
                break;
            }
        }

        if (received == -1) {
            syslog(LOG_ERR, "receive failed: %s", strerror(errno));
        } else if (total_received > 0) {

            if (fwrite(resources->buffer, 1, total_received, resources->data_file) != total_received) {
                syslog(LOG_ERR, "write to file failed: %s", strerror(errno));
            }
            fflush(resources->data_file);


            fseek(resources->data_file, 0, SEEK_SET);
            while (fgets(resources->buffer, resources->buffer_size, resources->data_file)) {
                send(resources->client_fd, resources->buffer, strlen(resources->buffer), 0);
            }
            fseek(resources->data_file, 0, SEEK_END);
        }

        syslog(LOG_INFO, "connection clsed:- %s", inet_ntoa(client_addr.sin_addr));
        close(resources->client_fd);
        resources->client_fd = -1;
    }

    cleanup(SIGTERM);
    return 0;
}

