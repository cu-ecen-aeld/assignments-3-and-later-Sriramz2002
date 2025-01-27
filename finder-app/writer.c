#include <stdio.h>   // STANDARD INPUT-OUTPUT LIBRARY
#include <stdlib.h>  // STANDARD LIBRARY FUNCTIONS (MEMORY ALLOCATION, EXIT, ETC.)
#include <string.h>  // STRING MANIPULATION FUNCTIONS
#include <syslog.h>  // SYSLOG FOR SYSTEM LOGGING
#include <errno.h>   // ERROR HANDLING AND ERROR NUMBERS

// INITIALIZE SYSLOG FOR LOGGING
void initialize_syslog(const char *val)
{
    
    openlog(val, LOG_PID | LOG_CONS, LOG_USER); // OPEN SYSLOG WITH PROGRAM NAME AND LOG OPTIONS
}

// VALIDATE COMMAND-LINE ARGUMENTS
int validate_arguments(int count, char *arg[])
{
    
    if (count != 3) // CHECK IF ARGUMENT COUNT IS NOT 3
    {
        syslog(LOG_ERR, "INVALID ARGS. USAGE: %s <FILE> <STR>", arg[0]); // LOG ERROR
        fprintf(stderr, "ERROR: USAGE: %s <FILE> <STR>\n", arg[0]);      // PRINT USAGE ERROR TO STDERR
        return 1; // RETURN ERROR CODE
    }
    return 0; // RETURN SUCCESS CODE
}


int write_and_verify_file(const char *file_path, const char *write_str)// WRITE AND VERIFY CONTENT IN FILE
{
    FILE *file = fopen(file_path, "w+"); // OPEN FILE IN WRITE-AND-READ MODE

    if (!file) // CHECK IF FILE FAILED TO OPEN
    {
        syslog(LOG_ERR, "OPEN ERR %s: %s", file_path, strerror(errno)); // LOG OPEN ERROR
        fprintf(stderr, "ERROR: OPEN FAIL %s: %s\n", file_path, strerror(errno)); // PRINT ERROR TO STDERR
        return 1; // RETURN ERROR CODE
    }

    if (fprintf(file, "%s", write_str) < 0) // WRITE CONTENT TO FILE
    {
        syslog(LOG_ERR, "WRITE ERR %s: %s", file_path, strerror(errno)); // LOG WRITE ERROR
        
        fprintf(stderr, "ERROR: WRITE FAIL %s: %s\n", file_path, strerror(errno)); // PRINT WRITE ERROR
        fclose(file); // CLOSE FILE
        return 1; // RETURN ERROR CODE
    }

    if (fflush(file) != 0) // FLUSH WRITTEN CONTENT TO FILE
    {
        syslog(LOG_ERR, "FLUSH ERR %s: %s", file_path, strerror(errno)); // LOG FLUSH ERROR
        fprintf(stderr, "ERROR: FLUSH FAIL %s: %s\n", file_path, strerror(errno)); // PRINT FLUSH ERROR
        fclose(file); // CLOSE FILE
        return 1; // RETURN ERROR CODE
    }

    rewind(file); // MOVE FILE POINTER BACK TO THE BEGINNING

    char buffer[1000]; // TEMP BUFFER TO READ FILE CONTENT

    while (fgets(buffer, sizeof(buffer), file)) // READ CONTENT LINE-BY-LINE
    {
        if (strcmp(buffer, write_str) == 0) // CHECK IF CONTENT MATCHES
        {
            syslog(LOG_DEBUG, "VERIFY OK: %s", file_path); // LOG SUCCESSFUL VERIFICATION
            break; // EXIT LOOP IF VERIFIED
        }
        else // IF CONTENT DOES NOT MATCH
        {
            syslog(LOG_ERR, "VERIFY ERR %s", file_path); // LOG VERIFICATION ERROR
            
            fprintf(stderr, "ERROR: VERIFY FAIL %s\n", file_path); // PRINT VERIFICATION ERROR
            
            fclose(file); // CLOSE FILE
            
            
            return 1; // RETURN ERROR CODE
        }
    }

    if (fclose(file) != 0) // CLOSE FILE AFTER ALL OPERATIONS
    {
        syslog(LOG_ERR, "CLOSE ERR %s: %s", file_path, strerror(errno)); // LOG CLOSE ERROR
        
        fprintf(stderr, "ERROR: CLOSE FAIL %s: %s\n", file_path, strerror(errno));// PRINT CLOSE ERROR
        return 1; // RETURN ERROR CODE
    }

    return 0; // RETURN SUCCESS CODE
}

// MAIN FUNCTION
int main(int count, char *arg[])
{
    initialize_syslog("WRITER"); // INITIALIZE SYSLOG WITH PROGRAM NAME

    if (validate_arguments(count, arg)) // VALIDATE COMMAND-LINE ARGUMENTS
    {
        closelog(); // CLOSE SYSLOG IF INVALID
        return 1; // EXIT WITH ERROR
    }

    if (write_and_verify_file(arg[1], arg[2])) // WRITE TO FILE AND VERIFY
    {
        closelog(); // CLOSE SYSLOG ON FAILURE
        return 1; // EXIT WITH ERROR
    }

    closelog(); // CLOSE SYSLOG AFTER SUCCESS
    return 0; // EXIT WITH SUCCESS
}

