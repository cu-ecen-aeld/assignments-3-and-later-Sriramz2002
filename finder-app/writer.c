#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <errno.h>

int main(int argc, char *argv[]) {
    // Open syslog with LOG_USER facility
    openlog("writer", LOG_PID, LOG_USER);

    // Validate the number of arguments
    if (argc != 3) {
        syslog(LOG_ERR, "Invalid number of arguments. Usage: %s <file> <string>", argv[0]);
        fprintf(stderr, "Usage: %s <file> <string>\n", argv[0]);
        closelog();
        return 1;
    }

    const char *file_path = argv[1];
    const char *write_str = argv[2];

    // Open the file for writing
    FILE *file = fopen(file_path, "w");
    if (!file) {
        syslog(LOG_ERR, "Failed to open file %s: %s", file_path, strerror(errno));
        fprintf(stderr, "Error: Could not open file %s: %s\n", file_path, strerror(errno));
        closelog();
        return 1;
    }

    // Write the string to the file
    if (fprintf(file, "%s", write_str) < 0) {
        syslog(LOG_ERR, "Failed to write to file %s: %s", file_path, strerror(errno));
        fprintf(stderr, "Error: Could not write to file %s: %s\n", file_path, strerror(errno));
        fclose(file);
        closelog();
        return 1;
    }

    // Log success with LOG_DEBUG
    syslog(LOG_DEBUG, "Writing '%s' to '%s'", write_str, file_path);

    // Close the file
    if (fclose(file) != 0) {
        syslog(LOG_ERR, "Failed to close file %s: %s", file_path, strerror(errno));
        fprintf(stderr, "Error: Could not close file %s: %s\n", file_path, strerror(errno));
        closelog();
        return 1;
    }

    // Close syslog
    closelog();

    return 0;
}

