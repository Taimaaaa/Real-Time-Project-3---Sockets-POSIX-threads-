// server.c
// Update server that listens for incoming client connections.
// Spawns a new thread per client to handle version checks and file transfers concurrently.
// Updates shared GUI state so the OpenGL monitor reflects real activity.

#include "common.h"

static int             active_clients = 0;
static pthread_mutex_t clients_mutex  = PTHREAD_MUTEX_INITIALIZER;

void send_file(int client_fd, int thread_id)
{
    FILE *f = fopen(config.update_file_path, "rb");
    if (!f)
    {
        log_event("ERROR: update file not found", thread_id);
        return;
    }

    char buffer[1024];
    int  bytes_read;
    while ((bytes_read = fread(buffer, 1, sizeof(buffer), f)) > 0)
        send(client_fd, buffer, bytes_read, 0);

    fclose(f);

    // update gui counter
    pthread_mutex_lock(&gui_mutex);
    gui_total_updates++;
    pthread_mutex_unlock(&gui_mutex);

    char msg[128];
    sprintf(msg, "[Thread %d] Update file sent", thread_id);
    gui_add_log(msg);
    log_event("Update file sent successfully", thread_id);
}

void *handle_client(void *arg)
{
    ClientArgs *args      = (ClientArgs *)arg;
    int         client_fd = args->client_fd;
    int         thread_id = args->thread_id;
    free(arg);

    // update gui: new client connected
    pthread_mutex_lock(&gui_mutex);
    gui_active_clients++;
    gui_total_connected++;
    pthread_mutex_unlock(&gui_mutex);

    char gui_msg[128];
    sprintf(gui_msg, "[Thread %d] Client connected", thread_id);
    gui_add_log(gui_msg);
    log_event("Client connected", thread_id);

    // receive client version
    Message msg;
    if (recv(client_fd, &msg, sizeof(msg), 0) <= 0)
    {
        log_event("ERROR: failed to receive version", thread_id);
        close(client_fd);

        pthread_mutex_lock(&clients_mutex);
        active_clients--;
        pthread_mutex_unlock(&clients_mutex);

        pthread_mutex_lock(&gui_mutex);
        gui_active_clients--;
        pthread_mutex_unlock(&gui_mutex);

        pthread_exit(NULL);
    }

    char event[128];
    sprintf(event, "Client version received: %d", msg.version);
    log_event(event, thread_id);

    sprintf(gui_msg, "[Thread %d] Version: %d", thread_id, msg.version);
    gui_add_log(gui_msg);

    // compare version and respond
    if (msg.version < config.latest_version)
    {
        msg.update_available = 1;
        msg.version          = config.latest_version;
        send(client_fd, &msg, sizeof(msg), 0);

        sprintf(gui_msg, "[Thread %d] Sending update v%d", thread_id, config.latest_version);
        gui_add_log(gui_msg);
        log_event("Update required — sending file", thread_id);

        send_file(client_fd, thread_id);
    }
    else
    {
        msg.update_available = 0;
        send(client_fd, &msg, sizeof(msg), 0);

        sprintf(gui_msg, "[Thread %d] Already up to date", thread_id);
        gui_add_log(gui_msg);
        log_event("Client is up to date", thread_id);
    }

    log_event("Client disconnected", thread_id);

    sprintf(gui_msg, "[Thread %d] Client disconnected", thread_id);
    gui_add_log(gui_msg);

    close(client_fd);

    // decrement both counters on disconnect
    pthread_mutex_lock(&clients_mutex);
    active_clients--;
    pthread_mutex_unlock(&clients_mutex);

    pthread_mutex_lock(&gui_mutex);
    gui_active_clients--;
    pthread_mutex_unlock(&gui_mutex);

    pthread_exit(NULL);
}

// gui thread entry point — runs glutMainLoop which blocks forever
void *gui_thread_func(void *arg)
{
    int   argc = 0;
    char *argv[] = { NULL };
    init_gui(&argc, argv);
    return NULL;
}

int main(int argc, char **argv)
{
    if (argc != 2)
    {
        printf("Usage: ./server config/config.txt\n");
        exit(-1);
    }

    load_config(argv[1]);
    log_event("Server starting up", -1);

    // start gui in its own thread
    pthread_t gui_tid;
    pthread_create(&gui_tid, NULL, gui_thread_func, NULL);
    pthread_detach(gui_tid);

    // create server socket
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) { perror("socket"); exit(-1); }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family      = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port        = htons(config.server_port);

    if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0)
    {
        perror("bind");
        exit(-1);
    }

    if (listen(server_fd, config.max_clients) < 0)
    {
        perror("listen");
        exit(-1);
    }

    log_event("Server listening for connections", -1);
    gui_add_log("Server listening on port 8080");

    int thread_counter = 0;

    while (1)
    {
        ClientArgs *args   = malloc(sizeof(ClientArgs));
        socklen_t addr_len = sizeof(args->address);

        args->client_fd = accept(server_fd, (struct sockaddr *)&args->address, &addr_len);
        if (args->client_fd < 0)
        {
            perror("accept");
            free(args);
            continue;
        }

        pthread_mutex_lock(&clients_mutex);
        if (active_clients >= config.max_clients)
        {
            log_event("Max clients reached — connection refused", -1);
            gui_add_log("Connection refused: max clients reached");
            close(args->client_fd);
            free(args);
            pthread_mutex_unlock(&clients_mutex);
            continue;
        }
        active_clients++;
        pthread_mutex_unlock(&clients_mutex);

        args->thread_id = ++thread_counter;
        log_event("Incoming connection accepted", args->thread_id);

        pthread_t tid;
        if (pthread_create(&tid, NULL, handle_client, args) != 0)
        {
            perror("pthread_create");
            free(args);
            continue;
        }

        pthread_detach(tid);
    }

    close(server_fd);
    return 0;
}