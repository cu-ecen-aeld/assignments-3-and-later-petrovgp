#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <syslog.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/queue.h>
#include <time.h>
#include <stdbool.h>
#include <errno.h>

#define PORT 9000
#define DATA_FILE "/var/tmp/aesdsocketdata"
#define BUFFER_SIZE 1024

//Compatibility Macro
#ifndef SLIST_FOREACH_SAFE
#define SLIST_FOREACH_SAFE(var, head, field, tvar)           \
    for ((var) = SLIST_FIRST((head));                        \
        (var) && ((tvar) = SLIST_NEXT((var), field), 1);    \
        (var) = (tvar))
#endif

struct thread_data_t {
    pthread_t thread_id;
    int client_fd;
    struct sockaddr_in client_addr;
    bool thread_complete;
    SLIST_ENTRY(thread_data_t) entries;
};

int server_fd = -1;
volatile sig_atomic_t caught_sig = 0;
pthread_mutex_t file_mutex = PTHREAD_MUTEX_INITIALIZER;
SLIST_HEAD(slisthead, thread_data_t) head = SLIST_HEAD_INITIALIZER(head);

void signal_handler(int sig) {
    if (sig == SIGINT || sig == SIGTERM) {
        syslog(LOG_INFO, "Caught signal, exiting");
        caught_sig = 1;
        if (server_fd != -1) {
            shutdown(server_fd, SHUT_RDWR);
        }
    }
}

void* timestamp_handler(void* arg) {
    while (!caught_sig) {
        // Sleep in 1s intervals to remain responsive to caught_sig
        for (int i = 0; i < 10 && !caught_sig; i++) {
            sleep(1);
        }
        if (caught_sig) break;

        time_t rawtime;
        struct tm *info;
        char time_buffer[100];
        time(&rawtime);
        info = localtime(&rawtime);
        // RFC 2822: %a, %d %b %Y %H:%M:%S %z
        strftime(time_buffer, sizeof(time_buffer), "timestamp:%a, %d %b %Y %H:%M:%S %z\n", info);

        pthread_mutex_lock(&file_mutex);
        FILE *fp = fopen(DATA_FILE, "a");
        if (fp) {
            fputs(time_buffer, fp);
            fclose(fp);
        }
        pthread_mutex_unlock(&file_mutex);
    }
    return NULL;
}

void* thread_handle_connection(void* thread_param) {
    struct thread_data_t *data = (struct thread_data_t *)thread_param;
    char buffer[BUFFER_SIZE];
    ssize_t bytes_recv;

    while (!caught_sig) {
        bytes_recv = recv(data->client_fd, buffer, BUFFER_SIZE, 0);
        if (bytes_recv <= 0) break;

        pthread_mutex_lock(&file_mutex);
        FILE *fp = fopen(DATA_FILE, "a+");
        if (fp) {
            fwrite(buffer, 1, bytes_recv, fp);
            fclose(fp);
        }
        pthread_mutex_unlock(&file_mutex);

        if (memchr(buffer, '\n', bytes_recv)) break;
    }

    pthread_mutex_lock(&file_mutex);
    FILE *fp = fopen(DATA_FILE, "r");
    if (fp) {
        while (fgets(buffer, BUFFER_SIZE, fp)) {
            send(data->client_fd, buffer, strlen(buffer), 0);
        }
        fclose(fp);
    }
    pthread_mutex_unlock(&file_mutex);

    close(data->client_fd);
    data->thread_complete = true;
    return thread_param;
}

void cleanup() {
    struct thread_data_t *datap, *tmp;
    SLIST_FOREACH_SAFE(datap, &head, entries, tmp) {
        pthread_join(datap->thread_id, NULL);
        SLIST_REMOVE(&head, datap, thread_data_t, entries);
        free(datap);
    }
    if (server_fd != -1) close(server_fd);
    pthread_mutex_destroy(&file_mutex);
    remove(DATA_FILE);
    closelog();
}

int main(int argc, char *argv[]) {
    openlog("aesdsocket", LOG_PID, LOG_USER);

    remove(DATA_FILE);

    struct sigaction sa = {0};
    sa.sa_handler = signal_handler;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in address = { .sin_family = AF_INET, .sin_port = htons(PORT), .sin_addr.s_addr = INADDR_ANY };
    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) return -1;

    if (argc > 1 && strcmp(argv[1], "-d") == 0) {
        pid_t pid = fork();
        if (pid < 0) return -1;
        if (pid > 0) exit(0);
        setsid();
        chdir("/");
        int devnull = open("/dev/null", O_RDWR);
        dup2(devnull, STDIN_FILENO); dup2(devnull, STDOUT_FILENO); dup2(devnull, STDERR_FILENO);
        close(devnull);
    }

    if (listen(server_fd, 10) < 0) return -1;

    pthread_t timer_thread;
    pthread_create(&timer_thread, NULL, timestamp_handler, NULL);

    while (!caught_sig) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &addr_len);
        
        if (caught_sig) break;
        if (client_fd == -1) continue;

        struct thread_data_t *new_thread = malloc(sizeof(struct thread_data_t));
        new_thread->client_fd = client_fd;
        new_thread->client_addr = client_addr;
        new_thread->thread_complete = false;

        if (pthread_create(&new_thread->thread_id, NULL, thread_handle_connection, new_thread) != 0) {
            close(client_fd);
            free(new_thread);
        } else {
            SLIST_INSERT_HEAD(&head, new_thread, entries);
        }

        struct thread_data_t *datap, *tvar;
        SLIST_FOREACH_SAFE(datap, &head, entries, tvar) {
            if (datap->thread_complete) {
                pthread_join(datap->thread_id, NULL);
                SLIST_REMOVE(&head, datap, thread_data_t, entries);
                free(datap);
            }
        }
    }

    pthread_join(timer_thread, NULL);
    cleanup();
    return 0;
}