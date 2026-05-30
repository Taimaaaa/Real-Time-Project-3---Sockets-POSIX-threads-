// server.c
// Update server that listens for incoming client connections.
// Spawns a new thread per client to handle version checks and file transfers concurrently.

#include "common.h"

static int active_clients = 0;
static pthread_mutex_t clients_mutex = PTHREAD_MUTEX_INITIALIZER;


//Opens the update file, reads it in chunks of 1024 bytes and sends each chunk over the socket to the client. 
//Chunking is important because files can be larger than what fits in one send call.

void send_file(int client_fd, int thread_id)
{
    FILE *f = fopen(config.update_file_path, "rb");
    if (!f)
    {
        log_event("ERROR: update file not found", thread_id);
        return;
    }

    char buffer[1024];
    int bytes_read;
    while ((bytes_read = fread(buffer, 1, sizeof(buffer), f)) > 0)
        send(client_fd, buffer, bytes_read, 0);

    fclose(f);
    log_event("Update file sent successfully", thread_id);
}

//Every client gets their own thread
void *handle_client(void *arg)
{
    ClientArgs *args = (ClientArgs *)arg;
    int client_fd    = args->client_fd;
    int thread_id    = args->thread_id;
    free(arg);

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
        pthread_exit(NULL);
    }
    printf("DEBUG SERVER: latest_version=%d\n", config.latest_version);
    

    char event[128];
    sprintf(event, "Client version received: %d", msg.version);
    log_event(event, thread_id);

    // compare version and respond
    if (msg.version < config.latest_version)
    {
        msg.update_available = 1;
        msg.version = config.latest_version; // tell client what the latest version is
        send(client_fd, &msg, sizeof(msg), 0);
        log_event("Update required — sending file", thread_id);
        send_file(client_fd, thread_id);
    }
    else
    {
        msg.update_available = 0;
        send(client_fd, &msg, sizeof(msg), 0);
        log_event("Client is up to date", thread_id);
    }

    log_event("Client disconnected", thread_id);
    close(client_fd);

    pthread_mutex_lock(&clients_mutex);
    active_clients--;
    pthread_mutex_unlock(&clients_mutex);

    pthread_exit(NULL);
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

    // create server socket
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) { perror("socket"); exit(-1); }

    // allow port reuse
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

    int thread_counter = 0;

    // main accept loop
    while (1)
    {
        ClientArgs *args = malloc(sizeof(ClientArgs));
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

        pthread_detach(tid); // no need to join, thread cleans itself up
    }

    close(server_fd);
    return 0;
}