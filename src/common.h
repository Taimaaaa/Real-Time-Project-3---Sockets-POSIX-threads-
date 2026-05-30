// common.h
// Shared definitions, structs, and function declarations used by both server and client.
// Includes config struct, message struct, client thread args, and utility function prototypes.

#ifndef COMMON_H
#define COMMON_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <time.h>
#include <math.h>

// config values loaded from config.txt
typedef struct {
    int  server_port;
    int  latest_version;
    char update_file_path[256];
    char log_file_path[256];
    int  max_clients;
    int  buffer_size;
} Config;

// info passed to each client thread
typedef struct {
    int                client_fd;   // socket file descriptor for this client
    struct sockaddr_in address;     // client address info
    int                thread_id;   // thread identifier for logging
} ClientArgs;

// message exchanged between client and server
typedef struct {
    int version;          // client's current software version
    int update_available; // server sets this: 1=update needed, 0=up to date
} Message;

// global config instance
extern Config config;

// shared GUI state — updated by server threads, read by gui
#define MAX_LOG_LINES 10
extern int  gui_active_clients;
extern int  gui_total_updates;
extern int  gui_total_connected;
extern char gui_log[MAX_LOG_LINES][128];
extern int  gui_log_count;
extern pthread_mutex_t gui_mutex;

// function declarations
void load_config(const char *filename);
void log_event(const char *event, int thread_id);
int  get_current_version();
void gui_add_log(const char *msg);
void init_gui(int *argc, char **argv);

#endif