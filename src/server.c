/* =====================================================================
 * server.c
 * ---------------------------------------------------------------------
 * Concurrent update server.
 *
 *   - Binds the configured port and listens.
 *   - Spawns ONE detached worker thread per accepted client, so clients
 *     never block one another.
 *   - Each worker: optional token authentication -> version compare ->
 *     either "up to date" or stream the update package framed with a
 *     size + CRC32 header so the client can verify integrity.
 *   - Supports resumable downloads (client may request a byte offset).
 *   - Maintains live statistics for the OpenGL monitor.
 *   - Installs a SIGINT/SIGTERM handler for a clean shutdown that logs
 *     the event, closes the listening socket, and flushes the log.
 *   - SIGPIPE is ignored process-wide; all socket writes use send_all()
 *     with MSG_NOSIGNAL, so a client vanishing mid-transfer can never
 *     crash the server.
 *
 * Build with GUI (default) or with -DNO_GUI for a headless server.
 * ===================================================================== */

#include "common.h"
#include <signal.h>
#include <fcntl.h>
#include <dirent.h>

static int             active_clients = 0;
static pthread_mutex_t clients_mutex  = PTHREAD_MUTEX_INITIALIZER;

static volatile sig_atomic_t keep_running = 1;
static int                   listen_fd    = -1;

/* ---------------------------------------------------------------------
 * Signal handling: flip the run flag and shut down the listening socket
 * so accept() unblocks and the main loop exits cleanly.
 * ------------------------------------------------------------------- */
static void handle_signal(int sig)
{
    (void)sig;
    keep_running = 0;
    if (listen_fd >= 0) shutdown(listen_fd, SHUT_RDWR);
}

/* Stream the update file to the client starting at `offset` (resume
 * support). Returns bytes sent, or -1 on error. */
static long stream_file(int client_fd, int thread_id, uint64_t offset)
{
    FILE *f = fopen(config.update_file_path, "rb");
    if (!f) { log_event("ERROR: update file not found", thread_id); return -1; }

    if (offset > 0 && fseek(f, (long)offset, SEEK_SET) != 0) {
        log_event("ERROR: could not seek to resume offset", thread_id);
        fclose(f);
        return -1;
    }

    char  *buffer = malloc(config.buffer_size);
    if (!buffer) { fclose(f); return -1; }

    long   total = 0;
    size_t n;
    while ((n = fread(buffer, 1, config.buffer_size, f)) > 0) {
        if (send_all(client_fd, buffer, n) < 0) {
            log_event("ERROR: client disconnected during transfer", thread_id);
            free(buffer); fclose(f);
            return -1;
        }
        total += (long)n;
        if (config.artificial_delay_ms > 0)        /* demo throttle */
            usleep(config.artificial_delay_ms * 1000);
    }
    free(buffer);
    fclose(f);
    return total;
}

static const char *peer_str(struct sockaddr_in *a, char *out, size_t n)
{
    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &a->sin_addr, ip, sizeof(ip));
    snprintf(out, n, "%s:%d", ip, ntohs(a->sin_port));
    return out;
}

/* Optional shared-secret authentication handshake. Returns 1 if the
 * client may proceed, 0 if it must be rejected. */
static int authenticate(int client_fd, int thread_id)
{
    if (!config.require_auth) return 1;

    MsgHeader h;
    if (recv_header(client_fd, &h) != 0 || h.type != MSG_AUTH) {
        log_event("AUTH: malformed or missing auth message", thread_id);
        return 0;
    }
    MsgHeader resp; memset(&resp, 0, sizeof(resp));
    if (strncmp(h.auth_token, config.auth_token, MAX_TOKEN_LEN) == 0) {
        resp.type = MSG_AUTH_OK;
        send_header(client_fd, &resp);
        log_event("AUTH: client authenticated", thread_id);
        return 1;
    }
    resp.type = MSG_AUTH_FAIL;
    send_header(client_fd, &resp);
    log_event("AUTH: authentication FAILED", thread_id);
    return 0;
}

/* ---------------------------------------------------------------------
 * Per-client worker thread.
 * ------------------------------------------------------------------- */
static void *handle_client(void *arg)
{
    ClientArgs *args      = (ClientArgs *)arg;
    int         client_fd = args->client_fd;
    int         thread_id = args->thread_id;
    char        peer[80];
    peer_str(&args->address, peer, sizeof(peer));
    free(arg);

    pthread_mutex_lock(&gui_mutex);
    gui_active_clients++;
    gui_total_connected++;
    pthread_mutex_unlock(&gui_mutex);
    gui_set_slot(thread_id, 1);

    log_eventf(thread_id, "Client connected from %s", peer);
    { char m[128]; snprintf(m,sizeof(m),"[T%d] connected %s", thread_id, peer); gui_add_log(m); }

    int counted_fail = 0;   /* avoid double-counting failures */

    /* --- authentication --- */
    if (!authenticate(client_fd, thread_id)) {
        gui_add_log("[auth] rejected a client");
        goto cleanup;
    }

    /* --- receive version --- */
    MsgHeader req;
    if (recv_header(client_fd, &req) != 0 || req.type != MSG_VERSION) {
        log_event("ERROR: failed to receive version request", thread_id);
        pthread_mutex_lock(&gui_mutex); gui_total_failed++; pthread_mutex_unlock(&gui_mutex);
        counted_fail = 1;
        goto cleanup;
    }
    int client_version = (int)req.version;
    log_eventf(thread_id, "Version request received: client is on v%d", client_version);
    { char m[128]; snprintf(m,sizeof(m),"[T%d] reports v%d", thread_id, client_version); gui_add_log(m); }

    /* --- decision --- */
    if (client_version >= config.latest_version) {
        MsgHeader resp; memset(&resp, 0, sizeof(resp));
        resp.type    = MSG_UP_TO_DATE;
        resp.version = config.latest_version;
        send_header(client_fd, &resp);

        pthread_mutex_lock(&gui_mutex); gui_total_uptodate++; pthread_mutex_unlock(&gui_mutex);
        log_event("Decision: client is UP TO DATE", thread_id);
        { char m[128]; snprintf(m,sizeof(m),"[T%d] up to date", thread_id); gui_add_log(m); }
        goto cleanup;
    }

    /* --- update required: compute checksum + size, offer the package --- */
    uint32_t crc; uint64_t fsize;
    if (crc32_file(config.update_file_path, &crc, &fsize) != 0) {
        MsgHeader e; memset(&e,0,sizeof(e)); e.type = MSG_ERROR;
        send_header(client_fd, &e);
        log_event("ERROR: cannot read update file for checksum", thread_id);
        pthread_mutex_lock(&gui_mutex); gui_total_failed++; pthread_mutex_unlock(&gui_mutex);
        counted_fail = 1;
        goto cleanup;
    }

    MsgHeader offer; memset(&offer, 0, sizeof(offer));
    offer.type      = MSG_UPDATE_AVAIL;
    offer.version   = config.latest_version;
    offer.file_size = fsize;
    offer.crc32     = crc;
    send_header(client_fd, &offer);
    log_eventf(thread_id, "Decision: UPDATE -> offering v%d (%llu bytes, crc=%08x)",
               config.latest_version, (unsigned long long)fsize, crc);
    { char m[128]; snprintf(m,sizeof(m),"[T%d] sending v%d", thread_id, config.latest_version); gui_add_log(m); }

    /* The client may now request a resume offset before we stream. */
    MsgHeader ready;
    if (recv_header(client_fd, &ready) != 0) {
        log_event("ERROR: client did not confirm download readiness", thread_id);
        pthread_mutex_lock(&gui_mutex); gui_total_failed++; pthread_mutex_unlock(&gui_mutex);
        counted_fail = 1;
        goto cleanup;
    }
    uint64_t start = (ready.type == MSG_RESUME) ? ready.resume_offset : 0;
    if (start > 0) log_eventf(thread_id, "Resuming transfer from byte %llu",
                              (unsigned long long)start);

    long sent = stream_file(client_fd, thread_id, start);
    if (sent < 0) {
        pthread_mutex_lock(&gui_mutex); gui_total_failed++; pthread_mutex_unlock(&gui_mutex);
        counted_fail = 1;
        goto cleanup;
    }

    /* --- wait for the client's verdict on the checksum --- */
    MsgHeader verdict;
    if (recv_header(client_fd, &verdict) == 0 && verdict.type == MSG_ACK) {
        pthread_mutex_lock(&gui_mutex);
        gui_total_updates++;
        gui_bytes_sent += sent;
        pthread_mutex_unlock(&gui_mutex);
        log_eventf(thread_id, "Transfer complete and ACKed (%ld bytes)", sent);
        { char m[128]; snprintf(m,sizeof(m),"[T%d] update OK (%ld B)", thread_id, sent); gui_add_log(m); }
    } else {
        pthread_mutex_lock(&gui_mutex); gui_total_failed++; pthread_mutex_unlock(&gui_mutex);
        counted_fail = 1;
        log_event("ERROR: client reported checksum mismatch (NACK)", thread_id);
        { char m[128]; snprintf(m,sizeof(m),"[T%d] FAILED checksum", thread_id); gui_add_log(m); }
    }

cleanup:
    (void)counted_fail;
    log_eventf(thread_id, "Closing connection to %s", peer);
    { char m[128]; snprintf(m,sizeof(m),"[T%d] disconnected", thread_id); gui_add_log(m); }
    close(client_fd);

    gui_set_slot(thread_id, 0);
    pthread_mutex_lock(&clients_mutex); active_clients--; pthread_mutex_unlock(&clients_mutex);
    pthread_mutex_lock(&gui_mutex);     gui_active_clients--; pthread_mutex_unlock(&gui_mutex);
    pthread_exit(NULL);
}

#ifndef NO_GUI
static void *gui_thread_func(void *arg)
{
    (void)arg;
    int argc = 1; char *argv[] = { (char*)"server", NULL };
    init_gui(&argc, argv);   /* blocks in glutMainLoop */
    return NULL;
}
#endif

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <config-file>\n", argv[0]);
        return EXIT_FAILURE;
    }

    /* SIGPIPE would otherwise kill us when a client vanishes mid-send. */
    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT,  handle_signal);
    signal(SIGTERM, handle_signal);

    load_config(argv[1]);

    /* --- automatic cleanup of stale state from previous runs --- */
    {
        /* Remove any leftover partial downloads in the downloads/ folder.
         * Each run is independent; the server doesn't need files left behind
         * by prior client processes (the clients clean their own up on
         * success, but a killed client may leave a partial behind). */
        char *dir = strdup(config.download_path);
        if (dir) {
            char *slash = strrchr(dir, '/');
            if (slash) {
                *slash = '\0';
                DIR *d = opendir(dir);
                if (d) {
                    struct dirent *ent;
                    int removed = 0;
                    while ((ent = readdir(d)) != NULL) {
                        if (ent->d_name[0] == '.') continue;
                        if (strstr(ent->d_name, ".pkg") == NULL) continue;
                        char path[512];
                        snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);
                        if (unlink(path) == 0) removed++;
                    }
                    closedir(d);
                    if (removed > 0)
                        printf("Cleaned %d stale download file(s) from %s/\n",
                               removed, dir);
                }
            }
            free(dir);
        }
    }

    pthread_mutex_lock(&gui_mutex);
    gui_server_running  = 1;
    gui_active_clients  = 0;
    gui_total_connected = 0;
    gui_total_updates   = 0;
    gui_total_uptodate  = 0;
    gui_total_failed    = 0;
    gui_bytes_sent      = 0;
    gui_log_count       = 0;
    pthread_mutex_unlock(&gui_mutex);
    gui_reset_slots();   /* clears slot_used[] and slot_owner[] */

    log_event("===== Server starting up =====", -1);
    log_eventf(-1, "Config: port=%d latest=v%d max_clients=%d auth=%s",
               config.server_port, config.latest_version, config.max_clients,
               config.require_auth ? "on" : "off");

#ifndef NO_GUI
    pthread_t gui_tid;
    pthread_create(&gui_tid, NULL, gui_thread_func, NULL);
    pthread_detach(gui_tid);
#endif

    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) { perror("socket"); return EXIT_FAILURE; }

    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(config.server_port);

    if (bind(listen_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind"); return EXIT_FAILURE;
    }
    if (listen(listen_fd, config.max_clients) < 0) {
        perror("listen"); return EXIT_FAILURE;
    }

    log_eventf(-1, "Server listening on port %d", config.server_port);
    { char m[64]; snprintf(m,sizeof(m),"Listening on port %d", config.server_port); gui_add_log(m); }

    int thread_counter = 0;
    while (keep_running) {
        ClientArgs *cargs   = malloc(sizeof(ClientArgs));
        if (!cargs) { log_event("ERROR: out of memory", -1); continue; }
        socklen_t addr_len  = sizeof(cargs->address);

        cargs->client_fd = accept(listen_fd, (struct sockaddr*)&cargs->address, &addr_len);
        if (cargs->client_fd < 0) {
            free(cargs);
            if (!keep_running) break;          /* shutdown requested */
            if (errno == EINTR) continue;
            perror("accept");
            continue;
        }

        pthread_mutex_lock(&clients_mutex);
        if (active_clients >= config.max_clients) {
            pthread_mutex_unlock(&clients_mutex);
            log_event("Max clients reached -- connection refused", -1);
            gui_add_log("Refused: server full");
            close(cargs->client_fd);
            free(cargs);
            continue;
        }
        active_clients++;
        pthread_mutex_unlock(&clients_mutex);

        cargs->thread_id = ++thread_counter;
        log_eventf(cargs->thread_id, "Incoming connection accepted");

        pthread_t tid;
        if (pthread_create(&tid, NULL, handle_client, cargs) != 0) {
            perror("pthread_create");
            close(cargs->client_fd);
            free(cargs);
            pthread_mutex_lock(&clients_mutex); active_clients--; pthread_mutex_unlock(&clients_mutex);
            continue;
        }
        pthread_detach(tid);
    }

    /* ----- graceful shutdown ----- */
    log_event("===== Server shutting down =====", -1);
    pthread_mutex_lock(&gui_mutex); gui_server_running = 0; pthread_mutex_unlock(&gui_mutex);

    if (listen_fd >= 0) close(listen_fd);

    /* give in-flight workers a brief moment to finish logging */
    for (int i = 0; i < 20; i++) {
        pthread_mutex_lock(&clients_mutex);
        int a = active_clients;
        pthread_mutex_unlock(&clients_mutex);
        if (a == 0) break;
        usleep(100 * 1000);
    }

    log_eventf(-1, "Final stats: connected=%d updates=%d uptodate=%d failed=%d bytes=%ld",
               gui_total_connected, gui_total_updates, gui_total_uptodate,
               gui_total_failed, gui_bytes_sent);
    close_log();
    printf("Server stopped cleanly.\n");
    fflush(stdout);
    /* GLUT's atexit handler can race with our shutdown and emit a
     * harmless X BadAccess error. Bypass libc cleanup with _exit so
     * the return code stays 0 and the terminal output is clean. */
    _exit(EXIT_SUCCESS);
}
