/* =====================================================================
 * common.c
 * ---------------------------------------------------------------------
 * Implementation of everything shared by server and client:
 *   - config file parsing (key=value, comments stripped),
 *   - thread-safe timestamped logging to file + stdout,
 *   - installed-version read/write,
 *   - byte-order safe header marshalling,
 *   - reliable send_all/recv_all that survive partial transfers,
 *   - a self-contained CRC32 implementation (no external libs).
 * ===================================================================== */

#include "common.h"
#include <stdarg.h>

Config config;

static FILE           *log_file  = NULL;
static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Bounded copy that always null-terminates (avoids strncpy truncation
 * pitfalls). Copies at most cap-1 bytes then terminates. */
static void safe_copy(char *dst, const char *src, size_t cap)
{
    if (cap == 0) return;
    size_t i = 0;
    for (; i + 1 < cap && src[i]; i++) dst[i] = src[i];
    dst[i] = '\0';
}

/* ---------------------------------------------------------------------
 * Config defaults: applied first so a sparse config file still works.
 * ------------------------------------------------------------------- */
static void set_defaults(void)
{
    config.server_port        = 8080;
    safe_copy(config.server_ip,   "127.0.0.1", MAX_IP_LEN);
    config.latest_version     = 1;
    safe_copy(config.update_file_path,  "update_files/update.pkg", MAX_PATH_LEN);
    safe_copy(config.download_path,     "downloads/received_update.pkg", MAX_PATH_LEN);
    safe_copy(config.version_file_path, "config/version.txt", MAX_PATH_LEN);
    safe_copy(config.log_file_path,     "logs/server.log", MAX_PATH_LEN);
    config.max_clients        = 10;
    config.buffer_size        = NET_CHUNK;
    safe_copy(config.auth_token,  "", MAX_TOKEN_LEN);
    config.require_auth       = 0;
    config.artificial_delay_ms = 0;
}

void load_config(const char *filename)
{
    set_defaults();

    FILE *f = fopen(filename, "r");
    if (!f) { perror("fopen config"); exit(EXIT_FAILURE); }

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == '\n') continue;

        char *comment = strchr(line, '#');     /* strip inline comments */
        if (comment) *comment = '\0';

        /* trim trailing whitespace */
        char *end = line + strlen(line) - 1;
        while (end > line && (*end==' '||*end=='\t'||*end=='\n'||*end=='\r'))
            *end-- = '\0';

        char key[128], value[256];
        if (sscanf(line, " %127[^=]=%255s", key, value) != 2) continue;

        if      (!strcmp(key,"SERVER_PORT"))        config.server_port        = atoi(value);
        else if (!strcmp(key,"SERVER_IP"))          safe_copy(config.server_ip, value, MAX_IP_LEN);
        else if (!strcmp(key,"LATEST_VERSION"))     config.latest_version     = atoi(value);
        else if (!strcmp(key,"UPDATE_FILE_PATH"))   safe_copy(config.update_file_path,  value, MAX_PATH_LEN);
        else if (!strcmp(key,"DOWNLOAD_PATH"))      safe_copy(config.download_path,     value, MAX_PATH_LEN);
        else if (!strcmp(key,"VERSION_FILE_PATH"))  safe_copy(config.version_file_path, value, MAX_PATH_LEN);
        else if (!strcmp(key,"LOG_FILE_PATH"))      safe_copy(config.log_file_path,     value, MAX_PATH_LEN);
        else if (!strcmp(key,"MAX_CLIENTS"))        config.max_clients        = atoi(value);
        else if (!strcmp(key,"BUFFER_SIZE"))        config.buffer_size        = atoi(value);
        else if (!strcmp(key,"AUTH_TOKEN"))         safe_copy(config.auth_token, value, MAX_TOKEN_LEN);
        else if (!strcmp(key,"REQUIRE_AUTH"))       config.require_auth       = atoi(value);
        else if (!strcmp(key,"ARTIFICIAL_DELAY_MS"))config.artificial_delay_ms= atoi(value);
    }
    fclose(f);

    if (config.buffer_size <= 0 || config.buffer_size > (1<<20))
        config.buffer_size = NET_CHUNK;

    log_file = fopen(config.log_file_path, "a");
    if (!log_file) { perror("fopen log"); exit(EXIT_FAILURE); }
}

void close_log(void)
{
    pthread_mutex_lock(&log_mutex);
    if (log_file) { fflush(log_file); fclose(log_file); log_file = NULL; }
    pthread_mutex_unlock(&log_mutex);
}

/* ---------------------------------------------------------------------
 * Thread-safe logging. Writes to stdout and the log file with a
 * timestamp and (optionally) a thread id. -1 thread_id = system event.
 * ------------------------------------------------------------------- */
void log_event(const char *event, int thread_id)
{
    time_t now = time(NULL);
    char ts[64];
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", localtime(&now));

    pthread_mutex_lock(&log_mutex);
    if (thread_id >= 0) {
        printf("[%s] [Thread %d] %s\n", ts, thread_id, event);
        if (log_file) fprintf(log_file, "[%s] [Thread %d] %s\n", ts, thread_id, event);
    } else {
        printf("[%s] %s\n", ts, event);
        if (log_file) fprintf(log_file, "[%s] %s\n", ts, event);
    }
    fflush(stdout);
    if (log_file) fflush(log_file);
    pthread_mutex_unlock(&log_mutex);
}

/* printf-style convenience wrapper around log_event. */
void log_eventf(int thread_id, const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    log_event(buf, thread_id);
}

/* ---------------------------------------------------------------------
 * Installed version, simulated by a small text file on the client side.
 * ------------------------------------------------------------------- */
int get_current_version(void)
{
    FILE *f = fopen(config.version_file_path, "r");
    if (!f) return 1;                 /* assume v1 if nothing recorded */
    int v = 1;
    if (fscanf(f, "%d", &v) != 1) v = 1;
    fclose(f);
    return v;
}

void set_current_version(int v)
{
    FILE *f = fopen(config.version_file_path, "w");
    if (f) { fprintf(f, "%d\n", v); fclose(f); }
}

/* =====================================================================
 * Header marshalling: convert every integer member to/from network
 * byte order in place. 64-bit fields are split into two 32-bit halves
 * so we don't depend on a (non-portable) htonll.
 * ===================================================================== */
static uint64_t hton64(uint64_t v)
{
    uint32_t hi = (uint32_t)(v >> 32);
    uint32_t lo = (uint32_t)(v & 0xFFFFFFFFULL);
    /* place big-endian(hi) in the low memory word, big-endian(lo) in the
     * high memory word, so the raw bytes on the wire are big-endian(v). */
    return ((uint64_t)htonl(hi)) | ((uint64_t)htonl(lo) << 32);
}
static uint64_t ntoh64(uint64_t v)
{
    uint32_t low_word  = (uint32_t)(v & 0xFFFFFFFFULL); /* holds htonl(hi) */
    uint32_t high_word = (uint32_t)(v >> 32);           /* holds htonl(lo) */
    return ((uint64_t)ntohl(low_word) << 32) | (uint64_t)ntohl(high_word);
}

void header_hton(MsgHeader *h)
{
    h->type          = htonl(h->type);
    h->version       = htonl(h->version);
    h->crc32         = htonl(h->crc32);
    h->file_size     = hton64(h->file_size);
    h->resume_offset = hton64(h->resume_offset);
    /* auth_token is a byte array; no conversion needed */
}
void header_ntoh(MsgHeader *h)
{
    h->type          = ntohl(h->type);
    h->version       = ntohl(h->version);
    h->crc32         = ntohl(h->crc32);
    h->file_size     = ntoh64(h->file_size);
    h->resume_offset = ntoh64(h->resume_offset);
}

/* =====================================================================
 * Reliable I/O. recv()/send() may move fewer bytes than requested;
 * these loop until the whole buffer is handled, the peer closes, or a
 * real error occurs. EINTR is retried transparently.
 * ===================================================================== */
ssize_t send_all(int fd, const void *buf, size_t len)
{
    const char *p = buf;
    size_t left = len;
    while (left > 0) {
        ssize_t n = send(fd, p, left, MSG_NOSIGNAL);  /* no SIGPIPE */
        if (n < 0) { if (errno == EINTR) continue; return -1; }
        if (n == 0) return -1;
        p    += n;
        left -= (size_t)n;
    }
    return (ssize_t)len;
}

ssize_t recv_all(int fd, void *buf, size_t len)
{
    char *p = buf;
    size_t left = len;
    while (left > 0) {
        ssize_t n = recv(fd, p, left, 0);
        if (n < 0) { if (errno == EINTR) continue; return -1; }
        if (n == 0) return 0;          /* peer closed before completion */
        p    += n;
        left -= (size_t)n;
    }
    return (ssize_t)len;
}

int send_header(int fd, MsgHeader *h)
{
    MsgHeader net = *h;
    header_hton(&net);
    return send_all(fd, &net, sizeof(net)) == (ssize_t)sizeof(net) ? 0 : -1;
}

int recv_header(int fd, MsgHeader *h)
{
    ssize_t r = recv_all(fd, h, sizeof(*h));
    if (r != (ssize_t)sizeof(*h)) return -1;
    header_ntoh(h);
    return 0;
}

/* =====================================================================
 * CRC32 (IEEE 802.3, polynomial 0xEDB88820) — self-contained, no libs.
 * Used to verify update packages survive transfer intact.
 * ===================================================================== */
static uint32_t crc_table[256];
static int      crc_table_built = 0;
static pthread_mutex_t crc_mutex = PTHREAD_MUTEX_INITIALIZER;

static void build_crc_table(void)
{
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? (0xEDB88820U ^ (c >> 1)) : (c >> 1);
        crc_table[i] = c;
    }
    crc_table_built = 1;
}

uint32_t crc32_buf(uint32_t crc, const void *buf, size_t len)
{
    if (!crc_table_built) {
        pthread_mutex_lock(&crc_mutex);
        if (!crc_table_built) build_crc_table();
        pthread_mutex_unlock(&crc_mutex);
    }
    const unsigned char *p = buf;
    crc ^= 0xFFFFFFFFU;
    for (size_t i = 0; i < len; i++)
        crc = crc_table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFU;
}

int crc32_file(const char *path, uint32_t *out_crc, uint64_t *out_size)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    uint32_t crc = 0;
    uint64_t total = 0;
    unsigned char buf[NET_CHUNK];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        crc    = crc32_buf(crc, buf, n);
        total += n;
    }
    fclose(f);
    if (out_crc)  *out_crc  = crc;
    if (out_size) *out_size = total;
    return 0;
}
