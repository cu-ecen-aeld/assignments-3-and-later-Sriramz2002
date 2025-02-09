#include "threading.h"
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>

// Optional: use these functions to add debug or error prints to your application
#define DEBUG_LOG(msg,...)
//#define DEBUG_LOG(msg,...) printf("threading: " msg "\n" , ##__VA_ARGS__)
#define ERROR_LOG(msg,...) printf("threading ERROR: " msg "\n" , ##__VA_ARGS__)
thread_data_t* create_thread_data(pthread_mutex_t *mutex, int wait_to_obtain_ms, int wait_to_release_ms)
{
    thread_data_t* data = (thread_data_t*) malloc(sizeof(thread_data_t));
    if (!data) {
        ERROR_LOG("Failed to allocate memory for thread_data");
        return NULL;
    }
    data->mutex = mutex;
    data->wait_to_obtain_ms = wait_to_obtain_ms;
    data->wait_to_release_ms = wait_to_release_ms;
    data->thread_complete_success = false;
    return data;
}
void* threadfunc(void* thread_param)
{

    // TODO: wait, obtain mutex, wait, release mutex as described by thread_data structure
    
thread_data_t* data = (thread_data_t*) thread_param;
    if (!data) return NULL;

    // Sleep before obtaining the mutex
    usleep(data->wait_to_obtain_ms * 1000);

    // Try to lock the mutex
    if (pthread_mutex_lock(data->mutex) != 0) {
        ERROR_LOG("Failed to obtain mutex");
        data->thread_complete_success = false;
        return data;
    }

    // Sleep while holding the mutex
    usleep(data->wait_to_release_ms * 1000);

    // Unlock the mutex
    if (pthread_mutex_unlock(data->mutex) != 0) {
        ERROR_LOG("Failed to release mutex");
        data->thread_complete_success = false;
        return data;
    }

    // Indicate successful execution
    data->thread_complete_success = true;
    return data;
}


bool start_thread_obtaining_mutex(pthread_t *thread, pthread_mutex_t *mutex,int wait_to_obtain_ms, int wait_to_release_ms)
{
    /**
     * TODO: allocate memory for thread_data, setup mutex and wait arguments, pass thread_data to created thread
     * using threadfunc() as entry point.
     *
     * return true if successful.
     *
     * See implementation details in threading.h file comment block
     */
 thread_data_t* data = create_thread_data(mutex, wait_to_obtain_ms, wait_to_release_ms);
    if (!data) return false;

    // Create the thread and pass thread_data as argument
    if (pthread_create(thread, NULL, threadfunc, data) != 0) {
        ERROR_LOG("Failed to create thread");
        free(data);
        return false;
    }

    return true;
}

