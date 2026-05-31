/* =====================================================================
 * common.h
 * ---------------------------------------------------------------------
 * Shared definitions for the Distributed Software Update Framework.
 *
 * This header is included by BOTH the server and the client. It defines:
 *   - the runtime configuration loaded from a text file (no hard-coding),
 *   - the wire protocol (message types + a fixed binary header that is
 *     byte-order safe so the protocol works across machines),
 *   - per-client thread arguments,
 *   - logging / config / version helpers,
 *   - CRC32 checksum helper for integrity verification,
 *   - reliable send/recv helpers that handle partial transfers,
 *   - the shared state the OpenGL monitor reads from.
 *
 * Design notes:
 *   - Every multi-byte field that travels over the network is converted
 *     to network byte order (htonl/ntohl) so the framework is portable
 *     between little- and big-endian hosts.
 *   - All blocking socket I/O goes through send_all()/recv_all() which
 *     loop until the full payload is transferred or the peer goes away,
 *     fixing the classic "short read" bug.
 * ===================================================================== */

#ifndef COMMON_H
#define COMMON_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <time.h>
#include <math.h>

/* ---------- limits ---------- */
#define MAX_PATH_LEN   256
#define MAX_IP_LEN     64
#define MAX_TOKEN_LEN  128
#define NET_CHUNK      4096   /* socket I/O chunk size */

/* =====================================================================
 * Runtime configuration (loaded from config/config.txt).
 * Adding a new tunable means: add a field here + one strcmp in
 * load_config(); nothing else needs recompiling logic changes.
 * ===================================================================== */
typedef struct {
    int  server_port;                       /* TCP port the server binds */
    char server_ip[MAX_IP_LEN];             /* address the client dials  */
    int  latest_version;                    /* newest version on server  */
    char update_file_path[MAX_PATH_LEN];    /* file the server serves    */
    char download_path[MAX_PATH_LEN];       /* where the client saves it */
    char version_file_path[MAX_PATH_LEN];   /* client's installed version*/
    char log_file_path[MAX_PATH_LEN];       /* event log file            */
    int  max_clients;                       /* concurrent connection cap */
    int  buffer_size;                       /* preferred I/O chunk size  */
    char auth_token[MAX_TOKEN_LEN];         /* shared secret for auth    */
    int  require_auth;                      /* 1 = enforce token check   */
    int  artificial_delay_ms;               /* throttle to demo concurrency */
} Config;

/* =====================================================================
 * Wire protocol
 * ---------------------------------------------------------------------
 * Communication is framed. Every exchange begins with a fixed-size
 * MsgHeader (always sent in network byte order). The header's "type"
 * field says what, if anything, follows.
 *
 * Flow:
 *   client -> server : MSG_AUTH      (header.auth_token populated)
 *   server -> client : MSG_AUTH_OK | MSG_AUTH_FAIL
 *   client -> server : MSG_VERSION   (header.version = installed version)
 *   server -> client : MSG_UP_TO_DATE
 *                      OR
 *                      MSG_UPDATE_AVAIL (header.version = latest,
 *                                        header.file_size, header.crc32)
 *                      followed by file_size raw bytes of the package.
 *   client -> server : MSG_ACK on success / MSG_NACK on checksum failure
 * ===================================================================== */
typedef enum {
    MSG_AUTH         = 1,   /* client presents credentials            */
    MSG_AUTH_OK      = 2,   /* server accepts                         */
    MSG_AUTH_FAIL    = 3,   /* server rejects                         */
    MSG_VERSION      = 4,   /* client reports installed version       */
    MSG_UP_TO_DATE   = 5,   /* server: nothing to do                  */
    MSG_UPDATE_AVAIL = 6,   /* server: update follows this header     */
    MSG_ACK          = 7,   /* client confirms good download          */
    MSG_NACK         = 8,   /* client reports bad checksum            */
    MSG_RESUME       = 9,   /* client asks to resume from an offset   */
    MSG_ERROR        = 10   /* generic error notification             */
} MsgType;

/* Fixed 32-byte header. All integer members are transmitted big-endian
 * via header_hton()/header_ntoh(); the token is a fixed char array. */
typedef struct {
    uint32_t type;                       /* one of MsgType            */
    uint32_t version;                    /* version number in play    */
    uint64_t file_size;                  /* bytes of payload to follow*/
    uint32_t crc32;                      /* checksum of the payload   */
    uint64_t resume_offset;              /* for resumable downloads   */
    char     auth_token[MAX_TOKEN_LEN];  /* shared secret on MSG_AUTH */
} MsgHeader;

/* =====================================================================
 * Per-client thread context (server side).
 * ===================================================================== */
typedef struct {
    int                client_fd;   /* connected socket               */
    struct sockaddr_in address;     /* peer address (for logging)     */
    int                thread_id;   /* monotonic id used in logs/GUI  */
} ClientArgs;

/* =====================================================================
 * Shared monitor state — written by worker threads, read by the GUI.
 * Protected by gui_mutex.
 * ===================================================================== */
#define MAX_LOG_LINES 12
#define MAX_SLOTS      8
extern int  gui_active_clients;
extern int  gui_total_connected;
extern int  gui_total_updates;
extern int  gui_total_uptodate;
extern int  gui_total_failed;
extern long gui_bytes_sent;
extern int  gui_slot_used[MAX_SLOTS];   /* which thread occupies a slot */
extern char gui_log[MAX_LOG_LINES][128];
extern int  gui_log_count;
extern int  gui_server_running;         /* drives the status indicator */
extern pthread_mutex_t gui_mutex;

/* =====================================================================
 * Globals
 * ===================================================================== */
extern Config config;

/* ---------- configuration / logging / version ---------- */
void load_config(const char *filename);
void close_log(void);
void log_event(const char *event, int thread_id);
void log_eventf(int thread_id, const char *fmt, ...);   /* printf-style */
int  get_current_version(void);
void set_current_version(int v);

/* ---------- byte-order safe header marshalling ---------- */
void header_hton(MsgHeader *h);   /* host  -> network, in place */
void header_ntoh(MsgHeader *h);   /* network -> host,  in place */

/* ---------- reliable socket I/O (handles partial transfers) ---------- */
ssize_t send_all(int fd, const void *buf, size_t len);
ssize_t recv_all(int fd, void *buf, size_t len);
int     send_header(int fd, MsgHeader *h);          /* marshals + sends */
int     recv_header(int fd, MsgHeader *h);          /* recvs + unmarshals */

/* ---------- integrity ---------- */
uint32_t crc32_buf(uint32_t crc, const void *buf, size_t len);
int      crc32_file(const char *path, uint32_t *out_crc, uint64_t *out_size);

/* ---------- GUI (defined in gui.c; stubbed when GUI disabled) ---------- */
void gui_add_log(const char *msg);
void gui_set_slot(int thread_id, int used);
void gui_reset_slots(void);   /* call on server startup to clear all slots */
void init_gui(int *argc, char **argv);

#endif /* COMMON_H */
