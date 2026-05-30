// common.c
// Implements shared utility functions used by both server and client.
// Handles config file parsing, event logging with timestamps, and reading the current installed version.

#include "common.h"

Config config;
static FILE *log_file = NULL;
static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;

void load_config(const char *filename)
{
    FILE *f = fopen(filename, "r");
    if (!f) { perror("fopen config"); exit(-1); }

    char line[256];
    while (fgets(line, sizeof(line), f))
    {
        // skip comments and empty lines
        if (line[0] == '#' || line[0] == '\n') continue;

        // strip inline comments
        char *comment = strchr(line, '#');
        if (comment) *comment = '\0';

        // trim trailing whitespace
        char *end = line + strlen(line) - 1;
        while (end > line && (*end == ' ' || *end == '\t' || *end == '\n')) *end-- = '\0';
        char key[128], value[128];
        if (sscanf(line, " %[^=]=%s", key, value) != 2) continue;

        if      (strcmp(key, "SERVER_PORT")       == 0) config.server_port       = atoi(value);
        else if (strcmp(key, "LATEST_VERSION")    == 0) config.latest_version    = atoi(value);
        else if (strcmp(key, "UPDATE_FILE_PATH")  == 0) strncpy(config.update_file_path, value, 255);
        else if (strcmp(key, "LOG_FILE_PATH")     == 0) strncpy(config.log_file_path,    value, 255);
        else if (strcmp(key, "MAX_CLIENTS")       == 0) config.max_clients       = atoi(value);
        else if (strcmp(key, "BUFFER_SIZE")       == 0) config.buffer_size       = atoi(value);
    }

    fclose(f);

    // open log file after config is loaded
    log_file = fopen(config.log_file_path, "a");
    if (!log_file) { perror("fopen log"); exit(-1); }
}

void log_event(const char *event, int thread_id)
{
    time_t now = time(NULL);
    char timestamp[64];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", localtime(&now));

    pthread_mutex_lock(&log_mutex);

    // print to terminal and write to log file
    if (thread_id >= 0)
        printf("[%s] [Thread %d] %s\n", timestamp, thread_id, event);
    else
        printf("[%s] %s\n", timestamp, event);

    if (log_file)
    {
        if (thread_id >= 0)
            fprintf(log_file, "[%s] [Thread %d] %s\n", timestamp, thread_id, event);
        else
            fprintf(log_file, "[%s] %s\n", timestamp, event);
        fflush(log_file);
    }

    pthread_mutex_unlock(&log_mutex);
}

int get_current_version()
{
    // simulate reading installed version from a local file
    FILE *f = fopen("config/version.txt", "r");
    if (!f) return 1; // default to version 1 if file not found

    int version = 1;
    fscanf(f, "%d", &version);
    fclose(f);
    return version;
}