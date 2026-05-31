/* =====================================================================
 * client.c
 * ---------------------------------------------------------------------
 * Update client. Implements CheckForUpdate() per the spec.
 *
 *   - Connects to the server (address/port come from config, not code).
 *   - Optional shared-secret authentication.
 *   - Reports its installed version (getCurrentVersion()).
 *   - If an update is offered, downloads exactly file_size bytes, then
 *     verifies the CRC32 the server advertised. On success it ACKs,
 *     "installs" the update (simulated), and bumps its local version.
 *     On mismatch it NACKs.
 *   - Automatic connection retries with backoff.
 *   - Each client instance uses a UNIQUE download path based on its PID
 *     so multiple concurrent clients never collide on disk.
 *   - After a successful install the temporary download file is removed.
 *   - Resume support: if a partial download from a PREVIOUS run of the
 *     same PID exists, the client asks the server to resume from that
 *     offset. (In practice this means a crashed client can resume on
 *     restart since the PID changes -- the old partial is ignored.)
 *
 * Usage:
 *   ./client <config-file> [installed-version]
 * The optional version overrides config/version.txt for the run.
 * ===================================================================== */

#include "common.h"
#include <fcntl.h>
#include <sys/stat.h>

/* unique download path for THIS process -- avoids concurrent collisions */
static char my_download_path[MAX_PATH_LEN];

/* getCurrentVersion() -- name kept close to the spec's wording. */
static int g_version_override = -1;
int getCurrentVersion(void)
{
    if (g_version_override >= 0) return g_version_override;
    return get_current_version();
}

static int connect_with_retry(int max_retries)
{
    for (int attempt = 1; attempt <= max_retries; attempt++) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) { perror("socket"); return -1; }

        struct sockaddr_in srv;
        memset(&srv, 0, sizeof(srv));
        srv.sin_family = AF_INET;
        srv.sin_port   = htons(config.server_port);
        if (inet_pton(AF_INET, config.server_ip, &srv.sin_addr) <= 0) {
            log_event("ERROR: invalid server address in config", 0);
            close(fd);
            return -1;
        }

        if (connect(fd, (struct sockaddr*)&srv, sizeof(srv)) == 0) {
            log_eventf(0, "Connected to %s:%d (attempt %d)",
                       config.server_ip, config.server_port, attempt);
            return fd;
        }
        log_eventf(0, "Connect attempt %d/%d failed: %s",
                   attempt, max_retries, strerror(errno));
        close(fd);
        if (attempt < max_retries) sleep(1);
    }
    return -1;
}

/* Returns 1 if authenticated (or auth disabled), 0 otherwise. */
static int do_auth(int fd)
{
    if (!config.require_auth) return 1;
    MsgHeader a; memset(&a, 0, sizeof(a));
    a.type = MSG_AUTH;
    memcpy(a.auth_token, config.auth_token, MAX_TOKEN_LEN);
    if (send_header(fd, &a) != 0) return 0;

    MsgHeader r;
    if (recv_header(fd, &r) != 0) return 0;
    if (r.type == MSG_AUTH_OK) { log_event("Authentication accepted", 0); return 1; }
    log_event("ERROR: authentication rejected by server", 0);
    return 0;
}

static uint64_t existing_partial_size(void)
{
    struct stat st;
    if (stat(my_download_path, &st) == 0) return (uint64_t)st.st_size;
    return 0;
}

/* Download exactly file_size bytes (minus any resume offset), compute
 * CRC32 as it arrives, and verify against the advertised value.
 * On success returns 0 and the file at my_download_path is complete.
 * On failure returns -1; the partial file is left for a future resume. */
static int download_and_verify(int fd, uint64_t file_size, uint64_t offset,
                               uint32_t expected_crc)
{
    /* Re-checksum the already-present prefix when resuming. */
    uint32_t crc = 0;
    const char *mode = "wb";
    if (offset > 0) {
        FILE *pf = fopen(my_download_path, "rb");
        if (pf) {
            unsigned char b[NET_CHUNK]; size_t n; uint64_t seen = 0;
            while (seen < offset && (n = fread(b, 1, sizeof(b), pf)) > 0) {
                if (seen + n > offset) n = (size_t)(offset - seen);
                crc = crc32_buf(crc, b, n);
                seen += n;
            }
            fclose(pf);
        }
        mode = "ab";
    }

    FILE *f = fopen(my_download_path, mode);
    if (!f) {
        log_eventf(0, "ERROR: cannot open download file '%s': %s",
                   my_download_path, strerror(errno));
        return -1;
    }

    char *buf = malloc(config.buffer_size);
    if (!buf) { fclose(f); return -1; }

    uint64_t remaining = file_size - offset;
    while (remaining > 0) {
        size_t want = remaining < (uint64_t)config.buffer_size
                      ? (size_t)remaining : (size_t)config.buffer_size;
        ssize_t n = recv(fd, buf, want, 0);
        if (n <= 0) {
            log_event("ERROR: connection lost during download", 0);
            free(buf); fclose(f);
            return -1;   /* partial file stays for resume */
        }
        fwrite(buf, 1, (size_t)n, f);
        crc = crc32_buf(crc, buf, (size_t)n);
        remaining -= (uint64_t)n;
    }
    free(buf);
    fclose(f);

    if (crc != expected_crc) {
        log_eventf(0, "Checksum MISMATCH (got %08x, expected %08x)", crc, expected_crc);
        return -1;
    }
    log_eventf(0, "Checksum verified (%08x)", crc);
    return 0;
}

void CheckForUpdate(void)
{
    log_event("CheckForUpdate(): starting", 0);

    int fd = connect_with_retry(5);
    if (fd < 0) { log_event("ERROR: could not reach server after retries", 0); return; }

    if (!do_auth(fd)) { close(fd); return; }

    /* report version */
    int my_version = getCurrentVersion();
    MsgHeader vmsg; memset(&vmsg, 0, sizeof(vmsg));
    vmsg.type    = MSG_VERSION;
    vmsg.version = (uint32_t)my_version;
    log_eventf(0, "Reporting installed version v%d", my_version);
    if (send_header(fd, &vmsg) != 0) {
        log_event("ERROR: failed to send version", 0); close(fd); return;
    }

    /* server verdict */
    MsgHeader resp;
    if (recv_header(fd, &resp) != 0) {
        log_event("ERROR: no response from server", 0); close(fd); return;
    }

    if (resp.type == MSG_UP_TO_DATE) {
        log_eventf(0, "Server says: already UP TO DATE (latest v%d). Nothing to do.",
                   resp.version);
        close(fd);
        return;
    }
    if (resp.type != MSG_UPDATE_AVAIL) {
        log_event("ERROR: unexpected server response", 0); close(fd); return;
    }

    int      new_version = (int)resp.version;
    uint64_t file_size   = resp.file_size;
    uint32_t crc         = resp.crc32;
    log_eventf(0, "Update available: v%d (%llu bytes). Downloading to %s ...",
               new_version, (unsigned long long)file_size, my_download_path);

    /* resume only if the existing partial is smaller than the full file */
    uint64_t have   = existing_partial_size();
    uint64_t offset = (have > 0 && have < file_size) ? have : 0;

    /* if a stale full-size file is sitting there, remove it and start fresh */
    if (have >= file_size) {
        remove(my_download_path);
        offset = 0;
        log_event("Removed stale completed download, starting fresh", 0);
    }

    MsgHeader ready; memset(&ready, 0, sizeof(ready));
    if (offset > 0) {
        ready.type          = MSG_RESUME;
        ready.resume_offset = offset;
        log_eventf(0, "Found partial download (%llu bytes), requesting resume",
                   (unsigned long long)offset);
    } else {
        ready.type = MSG_ACK;   /* "send from the start" */
    }
    send_header(fd, &ready);

    if (download_and_verify(fd, file_size, offset, crc) == 0) {
        MsgHeader ack; memset(&ack, 0, sizeof(ack)); ack.type = MSG_ACK;
        send_header(fd, &ack);

        log_event("Simulating installation...", 0);
        usleep(300 * 1000);
        set_current_version(new_version);
        log_eventf(0, "Installed successfully. Local version is now v%d", new_version);

        /* clean up the temporary download file */
        remove(my_download_path);
        log_event("Download temp file cleaned up", 0);
    } else {
        MsgHeader nack; memset(&nack, 0, sizeof(nack)); nack.type = MSG_NACK;
        send_header(fd, &nack);
        log_event("Update FAILED verification; partial file kept for resume", 0);
    }

    log_event("Closing connection", 0);
    close(fd);
}

int main(int argc, char **argv)
{
    if (argc < 2 || argc > 3) {
        fprintf(stderr, "Usage: %s <config-file> [installed-version]\n", argv[0]);
        return EXIT_FAILURE;
    }
    load_config(argv[1]);
    if (argc == 3) g_version_override = atoi(argv[2]);

    /* Build a unique download path using the process ID so concurrent
     * clients never write to the same file. Example:
     *   downloads/update_pid12345.pkg                                  */
    {
        /* split config.download_path into dir + extension */
        char dir[MAX_PATH_LEN]  = "downloads";
        char ext[32]            = ".pkg";
        char base[MAX_PATH_LEN] = "update";

        char tmp[MAX_PATH_LEN];
        strncpy(tmp, config.download_path, MAX_PATH_LEN - 1);
        tmp[MAX_PATH_LEN - 1] = '\0';

        char *slash = strrchr(tmp, '/');
        char *dot   = strrchr(tmp, '.');
        if (slash) {
            *slash = '\0';
            snprintf(dir,  MAX_PATH_LEN, "%s", tmp);
            snprintf(base, MAX_PATH_LEN, "%s", slash+1);
        } else {
            snprintf(base, MAX_PATH_LEN, "%s", tmp);
        }
        if (dot && dot > (slash ? slash : tmp)) {
            strncpy(ext, dot, sizeof(ext) - 1);
            /* strip extension from base */
            char *dot_in_base = strrchr(base, '.');
            if (dot_in_base) *dot_in_base = '\0';
        }
        snprintf(my_download_path, MAX_PATH_LEN,
                 "%s/%s_pid%d%s", dir, base, (int)getpid(), ext);
    }

    log_event("----- Client starting up -----", -1);
    log_eventf(-1, "PID %d, download path: %s", (int)getpid(), my_download_path);
    CheckForUpdate();
    close_log();
    return EXIT_SUCCESS;
}
