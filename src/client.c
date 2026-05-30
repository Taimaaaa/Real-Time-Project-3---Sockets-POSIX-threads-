// client.c
// Client application that connects to the update server.
// Sends current software version and downloads update if one is available.

#include "common.h"

void download_update(int server_fd, int thread_id, int new_version)
{
    FILE *f = fopen("update_files/received_update.pkg", "wb");
    if (!f)
    {
        log_event("ERROR: could not open file for writing", thread_id);
        return;
    }

    char buffer[1024];
    int bytes_received;
    while ((bytes_received = recv(server_fd, buffer, sizeof(buffer), 0)) > 0)
        fwrite(buffer, 1, bytes_received, f);

    fclose(f);
    log_event("Update downloaded and saved to update_files/received_update.pkg", thread_id);

    // simulate executing the update
    log_event("Simulating update installation...", thread_id);
    sleep(1);
    log_event("Update installed successfully", thread_id);

    // update local version file
    FILE *vf = fopen("config/version.txt", "w");
    if (vf)
    {
        fprintf(vf, "%d", new_version); // use version number received from server
        fclose(vf);
        log_event("Local version updated", thread_id);
    }
}

void CheckForUpdate(int thread_id)
{
    log_event("Checking for update...", thread_id);

    // create socket
    int sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_fd < 0) { perror("socket"); exit(-1); }

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port   = htons(config.server_port);

    // connect to server on localhost
    if (inet_pton(AF_INET, "127.0.0.1", &server_addr.sin_addr) <= 0)
    {
        perror("inet_pton");
        exit(-1);
    }

    if (connect(sock_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0)
    {
        log_event("ERROR: could not connect to server", thread_id);
        close(sock_fd);
        return;
    }

    log_event("Connected to server", thread_id);

    // send current version
    Message msg;
    msg.version = get_current_version();
    msg.update_available = 0;

    char event[128];
    sprintf(event, "Sending current version: %d", msg.version);
    log_event(event, thread_id);

    send(sock_fd, &msg, sizeof(msg), 0);

    // wait for server response
    if (recv(sock_fd, &msg, sizeof(msg), 0) <= 0)
    {
        log_event("ERROR: no response from server", thread_id);
        close(sock_fd);
        return;
    }
    printf("DEBUG: update_available=%d, msg.version=%d\n", msg.update_available, msg.version);


    int new_version = msg.version; // server puts latest version here
    if (msg.update_available)
    {
        log_event("Update available — downloading...", thread_id);
        download_update(sock_fd, thread_id, new_version);
    }
    else
    {
        log_event("Already up to date — no update needed", thread_id);
    }

    log_event("Closing connection", thread_id);
    close(sock_fd);
}

int main(int argc, char **argv)
{
    if (argc != 2)
    {
        printf("Usage: ./client config/config.txt\n");
        exit(-1);
    }

    load_config(argv[1]);
    log_event("Client starting up", -1);

    CheckForUpdate(0);

    return 0;
}