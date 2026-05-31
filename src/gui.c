/* =====================================================================
 * gui.c
 * ---------------------------------------------------------------------
 * OpenGL/GLUT monitor for the update server -- enhanced edition.
 *
 *   Modern dashboard layout with:
 *     - color-coded status indicator (pulsing green when running)
 *     - large stat tiles with distinct colors per metric
 *     - 8 worker thread slots with status rings and IDs
 *     - real-time throughput bar showing bytes transferred
 *     - success/failure ratio bar
 *     - colored event log with severity-based highlighting
 *     - subtle grid background for that "control room" feel
 *
 *   All numbers come from shared state in common.c, updated by the server
 *   worker threads and guarded by gui_mutex. The GUI never touches sockets.
 *
 *   When compiled with -DNO_GUI this file provides no-op stubs for the
 *   GUI entry points plus the shared-state definitions.
 * ===================================================================== */

#include "common.h"

/* ---- shared monitor state (declared extern in common.h) ---- */
int  gui_active_clients  = 0;
int  gui_total_connected = 0;
int  gui_total_updates   = 0;
int  gui_total_uptodate  = 0;
int  gui_total_failed    = 0;
long gui_bytes_sent      = 0;
int  gui_slot_used[MAX_SLOTS] = {0};
char gui_log[MAX_LOG_LINES][128];
int  gui_log_count       = 0;
int  gui_server_running  = 0;
pthread_mutex_t gui_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Append a line to the scrolling log (thread-safe). */
void gui_add_log(const char *msg)
{
    pthread_mutex_lock(&gui_mutex);
    if (gui_log_count < MAX_LOG_LINES) {
        strncpy(gui_log[gui_log_count], msg, 127);
        gui_log[gui_log_count][127] = '\0';
        gui_log_count++;
    } else {
        for (int i = 0; i < MAX_LOG_LINES - 1; i++)
            strcpy(gui_log[i], gui_log[i + 1]);
        strncpy(gui_log[MAX_LOG_LINES - 1], msg, 127);
        gui_log[MAX_LOG_LINES - 1][127] = '\0';
    }
    pthread_mutex_unlock(&gui_mutex);
}

/* ---- per-thread slot tracking (slot_owner is module-private) ---- */
#define NO_SLOT (-1)
static int slot_owner[MAX_SLOTS];

void gui_reset_slots(void)
{
    pthread_mutex_lock(&gui_mutex);
    memset(gui_slot_used, 0, sizeof(gui_slot_used));
    for (int i = 0; i < MAX_SLOTS; i++) slot_owner[i] = NO_SLOT;
    pthread_mutex_unlock(&gui_mutex);
}

void gui_set_slot(int thread_id, int used)
{
    pthread_mutex_lock(&gui_mutex);
    if (used) {
        int idx = NO_SLOT;
        for (int i = 0; i < MAX_SLOTS; i++)
            if (slot_owner[i] == NO_SLOT) { idx = i; break; }
        if (idx != NO_SLOT) {
            slot_owner[idx]    = thread_id;
            gui_slot_used[idx] = 1;
        }
    } else {
        for (int i = 0; i < MAX_SLOTS; i++) {
            if (slot_owner[i] == thread_id) {
                slot_owner[i]    = NO_SLOT;
                gui_slot_used[i] = 0;
                break;
            }
        }
    }
    pthread_mutex_unlock(&gui_mutex);
}

#ifndef NO_GUI
#include <GL/glut.h>

/* ---------- color palette (dark dashboard theme) ---------- */
typedef struct { float r, g, b; } Color;
static const Color C_BG        = {0.06f, 0.07f, 0.10f};
static const Color C_PANEL     = {0.12f, 0.14f, 0.20f};
static const Color C_PANEL_HI  = {0.18f, 0.20f, 0.28f};
static const Color C_BORDER    = {0.30f, 0.35f, 0.45f};
static const Color C_TITLE     = {0.20f, 0.25f, 0.45f};
static const Color C_ACCENT    = {0.30f, 0.85f, 1.00f};   /* cyan */
static const Color C_GREEN     = {0.30f, 0.90f, 0.45f};
static const Color C_RED       = {0.95f, 0.35f, 0.40f};
static const Color C_YELLOW    = {0.95f, 0.80f, 0.30f};
static const Color C_PURPLE    = {0.70f, 0.45f, 0.95f};
static const Color C_ORANGE    = {1.00f, 0.55f, 0.20f};
static const Color C_GREY_TXT  = {0.65f, 0.70f, 0.78f};
static const Color C_DIM       = {0.40f, 0.45f, 0.55f};
static const Color C_WHITE     = {0.95f, 0.97f, 1.00f};

static int frame_counter = 0;   /* drives the pulse animation */

/* ---------- drawing helpers ---------- */
static void set_color(Color c) { glColor3f(c.r, c.g, c.b); }

static void draw_text(float x, float y, const char *text, Color c)
{
    set_color(c);
    glRasterPos2f(x, y);
    for (const char *p = text; *p; p++) glutBitmapCharacter(GLUT_BITMAP_9_BY_15, *p);
}
static void draw_text_big(float x, float y, const char *text, Color c)
{
    set_color(c);
    glRasterPos2f(x, y);
    for (const char *p = text; *p; p++) glutBitmapCharacter(GLUT_BITMAP_TIMES_ROMAN_24, *p);
}
static void draw_text_small(float x, float y, const char *text, Color c)
{
    set_color(c);
    glRasterPos2f(x, y);
    for (const char *p = text; *p; p++) glutBitmapCharacter(GLUT_BITMAP_8_BY_13, *p);
}

static void draw_rect(float x, float y, float w, float h, Color c)
{
    set_color(c);
    glBegin(GL_QUADS);
        glVertex2f(x, y); glVertex2f(x+w, y);
        glVertex2f(x+w, y+h); glVertex2f(x, y+h);
    glEnd();
}

static void draw_rect_outline(float x, float y, float w, float h, Color c)
{
    set_color(c);
    glLineWidth(1.5f);
    glBegin(GL_LINE_LOOP);
        glVertex2f(x, y); glVertex2f(x+w, y);
        glVertex2f(x+w, y+h); glVertex2f(x, y+h);
    glEnd();
}

static void draw_circle(float cx, float cy, float rad, Color c)
{
    set_color(c);
    glBegin(GL_POLYGON);
    for (int i = 0; i < 40; i++) {
        float a = 2.0f * 3.14159265f * i / 40;
        glVertex2f(cx + rad * cosf(a), cy + rad * sinf(a));
    }
    glEnd();
}

static void draw_ring(float cx, float cy, float rad, float thick, Color c)
{
    set_color(c);
    glLineWidth(thick);
    glBegin(GL_LINE_LOOP);
    for (int i = 0; i < 40; i++) {
        float a = 2.0f * 3.14159265f * i / 40;
        glVertex2f(cx + rad * cosf(a), cy + rad * sinf(a));
    }
    glEnd();
}

/* Decorative panel with title bar.
 * Layout: a thick accent strip across the TOP of the panel holds the
 * title text in white; the body below is reserved entirely for content.
 * This means callers can draw freely from (x+10, y+h-24) downwards. */
static void draw_panel(float x, float y, float w, float h, const char *title, Color titleColor)
{
    /* main panel body */
    draw_rect(x, y, w, h, C_PANEL);
    draw_rect_outline(x, y, w, h, C_BORDER);
    /* accent strip across the top */
    draw_rect(x, y + h - 20, w, 20, titleColor);
    if (title) {
        /* darken the title text so colored bars stay readable */
        Color titleText = {0.05f, 0.07f, 0.12f};
        draw_text_small(x + 10, y + h - 14, title, titleText);
    }
}

/* Stat tile: title in a thin colored bar at the top, large number below. */
static void draw_stat_tile(float x, float y, float w, float h,
                           const char *label, int value, Color valueColor)
{
    draw_rect(x, y, w, h, C_PANEL_HI);
    draw_rect_outline(x, y, w, h, C_BORDER);
    /* top accent strip with the label */
    draw_rect(x, y + h - 16, w, 16, valueColor);
    Color labelText = {0.05f, 0.07f, 0.12f};
    draw_text_small(x + 8, y + h - 11, label, labelText);
    /* big number centered in remaining area */
    char buf[32]; snprintf(buf, sizeof(buf), "%d", value);
    draw_text_big(x + 14, y + 14, buf, valueColor);
}
static void draw_stat_tile_long(float x, float y, float w, float h,
                                const char *label, long value, Color valueColor)
{
    draw_rect(x, y, w, h, C_PANEL_HI);
    draw_rect_outline(x, y, w, h, C_BORDER);
    draw_rect(x, y + h - 16, w, 16, valueColor);
    Color labelText = {0.05f, 0.07f, 0.12f};
    draw_text_small(x + 8, y + h - 11, label, labelText);
    char buf[32];
    if (value >= 1048576)      snprintf(buf, sizeof(buf), "%.1fM", value / 1048576.0);
    else if (value >= 1024)    snprintf(buf, sizeof(buf), "%.1fK", value / 1024.0);
    else                       snprintf(buf, sizeof(buf), "%ld", value);
    draw_text_big(x + 14, y + 14, buf, valueColor);
}

/* Subtle grid background */
static void draw_grid(void)
{
    glColor4f(C_BORDER.r, C_BORDER.g, C_BORDER.b, 0.06f);
    glLineWidth(1.0f);
    glBegin(GL_LINES);
    for (int x = 0; x < 1000; x += 40) {
        glVertex2f((float)x, 0); glVertex2f((float)x, 700);
    }
    for (int y = 0; y < 700; y += 40) {
        glVertex2f(0, (float)y); glVertex2f(1000, (float)y);
    }
    glEnd();
}

/* Horizontal bar chart of two values (e.g., success vs fail) */
static void draw_ratio_bar(float x, float y, float w, float h,
                           int a, int b, Color ca, Color cb)
{
    draw_rect(x, y, w, h, C_PANEL_HI);
    int total = a + b;
    if (total > 0) {
        float aw = w * ((float)a / total);
        draw_rect(x, y, aw, h, ca);
        draw_rect(x + aw, y, w - aw, h, cb);
    }
    draw_rect_outline(x, y, w, h, C_BORDER);
}

static int colored_log_color(const char *msg, Color *out)
{
    if (strstr(msg, "FAIL") || strstr(msg, "fail") || strstr(msg, "ERROR")
        || strstr(msg, "Refused")) { *out = C_RED;    return 1; }
    if (strstr(msg, "update OK") || strstr(msg, "authenticated"))
                                  { *out = C_GREEN;  return 1; }
    if (strstr(msg, "up to date")) { *out = C_YELLOW; return 1; }
    if (strstr(msg, "sending"))    { *out = C_ACCENT; return 1; }
    if (strstr(msg, "connected") || strstr(msg, "reports"))
                                  { *out = C_PURPLE; return 1; }
    if (strstr(msg, "disconnected"))
                                  { *out = C_DIM;    return 1; }
    if (strstr(msg, "Listening")) { *out = C_ORANGE; return 1; }
    *out = C_GREY_TXT;
    return 0;
}

/* ---------- main display ---------- */
static void display(void)
{
    glClear(GL_COLOR_BUFFER_BIT);
    draw_grid();

    /* ===== title bar ===== */
    draw_rect(0, 670, 1000, 40, C_TITLE);
    draw_rect(0, 666, 1000, 4, C_ACCENT);
    draw_text_big(14, 680, "Software Update Framework", C_WHITE);
    draw_text(420, 686, "// distributed real-time monitor //", C_ACCENT);

    /* clock */
    time_t now = time(NULL);
    char ts[32];
    strftime(ts, sizeof(ts), "%H:%M:%S", localtime(&now));
    draw_text_big(880, 680, ts, C_WHITE);

    /* ===== snapshot shared state ===== */
    pthread_mutex_lock(&gui_mutex);
    int running = gui_server_running;
    int ac = gui_active_clients, tc = gui_total_connected, tu = gui_total_updates;
    int ud = gui_total_uptodate, tf = gui_total_failed;
    long bytes = gui_bytes_sent;
    int slots[MAX_SLOTS]; memcpy(slots, gui_slot_used, sizeof(slots));
    int owners[MAX_SLOTS]; memcpy(owners, slot_owner, sizeof(owners));
    int log_count = gui_log_count;
    char logbuf[MAX_LOG_LINES][128];
    for (int i = 0; i < log_count; i++) strcpy(logbuf[i], gui_log[i]);
    pthread_mutex_unlock(&gui_mutex);

    /* ===== status banner (top-left) ===== */
    /* panel body y=580..635 (title bar at 635-655) */
    draw_panel(15, 580, 310, 75, "SYSTEM STATUS", running ? C_GREEN : C_RED);
    float pulse = running ? (0.7f + 0.3f * sinf(frame_counter * 0.08f)) : 1.0f;
    Color statCol = running
        ? (Color){C_GREEN.r * pulse, C_GREEN.g * pulse, C_GREEN.b * pulse}
        : C_RED;
    draw_circle(45, 605, 12, statCol);
    draw_ring(45, 605, 14, 2.0f, running ? C_GREEN : C_RED);
    draw_text_big(70, 595, running ? "ONLINE" : "OFFLINE",
                  running ? C_GREEN : C_RED);
    char portbuf[64];
    snprintf(portbuf, sizeof(portbuf), "port %d  |  max %d clients",
             config.server_port, config.max_clients);
    draw_text_small(70, 588, portbuf, C_DIM);

    /* ===== big stat tiles (top row) ===== */
    float tx = 340, ty = 580, tw = 155, th = 75;
    draw_stat_tile(tx,           ty, tw, th, "ACTIVE CLIENTS",   ac, C_ACCENT);
    draw_stat_tile(tx+tw+10,     ty, tw, th, "TOTAL CONNECTED",  tc, C_PURPLE);
    draw_stat_tile(tx+(tw+10)*2, ty, tw, th, "UPDATES SENT",     tu, C_GREEN);
    draw_stat_tile(tx+(tw+10)*3, ty, 145, th, "FAILURES",        tf, tf>0 ? C_RED : C_DIM);

    /* second row */
    draw_stat_tile     (15,  490, 200, 75, "UP TO DATE",       ud,    C_YELLOW);
    draw_stat_tile_long(225, 490, 200, 75, "BYTES TRANSFERRED",bytes, C_ORANGE);

    /* success ratio bar */
    draw_panel(435, 490, 550, 75, "SUCCESS / FAILURE RATIO", C_GREEN);
    /* panel body: y=490 to y=545 (title bar at 545-565) */
    draw_ratio_bar(450, 515, 520, 18, tu + ud, tf, C_GREEN, C_RED);
    char ratiobuf[96];
    int total_attempts = tu + ud + tf;
    if (total_attempts > 0) {
        snprintf(ratiobuf, sizeof(ratiobuf),
                 "successful: %d   failed: %d   ratio: %.1f%%",
                 tu + ud, tf, 100.0 * (tu + ud) / total_attempts);
    } else {
        snprintf(ratiobuf, sizeof(ratiobuf), "no client activity yet");
    }
    draw_text_small(450, 500, ratiobuf, C_GREY_TXT);

    /* ===== worker thread slots ===== */
    draw_panel(15, 340, 470, 140, "WORKER THREAD POOL", C_ACCENT);
    draw_text_small(30, 442, "8 detached pthreads -- one per concurrent client", C_DIM);
    for (int i = 0; i < MAX_SLOTS; i++) {
        float cx = 55 + i * 55, cy = 388;
        if (slots[i]) {
            float p = 0.75f + 0.25f * sinf((frame_counter + i * 8) * 0.10f);
            Color busy = {C_GREEN.r * p, C_GREEN.g * p, C_GREEN.b * p};
            draw_circle(cx, cy, 20, busy);
            draw_ring(cx, cy, 23, 2.5f, C_GREEN);
            char tid[8]; snprintf(tid, sizeof(tid), "T%d", owners[i]);
            draw_text(cx - 9, cy - 5, tid, C_BG);
        } else {
            draw_circle(cx, cy, 20, C_PANEL_HI);
            draw_ring(cx, cy, 23, 1.5f, C_BORDER);
            char num[4]; snprintf(num, sizeof(num), "%d", i + 1);
            draw_text(cx - 4, cy - 5, num, C_DIM);
        }
    }
    /* legend */
    draw_circle(40, 354, 5, C_GREEN);
    draw_text_small(50, 350, "busy", C_GREY_TXT);
    draw_circle(110, 354, 5, C_PANEL_HI);
    draw_ring(110, 354, 6, 1.5f, C_BORDER);
    draw_text_small(120, 350, "idle", C_GREY_TXT);

    /* ===== framework info (right side) ===== */
    /* panel body y=340..460 (title bar at 460-480) */
    draw_panel(495, 340, 490, 140, "FRAMEWORK INFO", C_PURPLE);

    /* Left column: protocol + reliability */
    draw_text_small(510, 442, "PROTOCOL", C_PURPLE);
    draw_text_small(510, 426, "  framed binary, net byte order", C_GREY_TXT);
    draw_text_small(510, 410, "  CRC32 integrity check", C_GREY_TXT);
    draw_text_small(510, 394, "  resumable downloads", C_GREY_TXT);
    draw_text_small(510, 378, "  shared-secret auth", C_GREY_TXT);
    draw_text_small(510, 360, "RELIABILITY", C_ORANGE);
    draw_text_small(510, 344, "  send_all / recv_all loops", C_GREY_TXT);

    /* vertical divider */
    set_color(C_BORDER);
    glLineWidth(1.0f);
    glBegin(GL_LINES); glVertex2f(745, 350); glVertex2f(745, 450); glEnd();

    /* Right column: live config */
    draw_text_small(755, 442, "CONFIGURATION", C_YELLOW);
    char vbuf[64];
    snprintf(vbuf, sizeof(vbuf), "  port:    %d", config.server_port);
    draw_text_small(755, 426, vbuf, C_GREY_TXT);
    snprintf(vbuf, sizeof(vbuf), "  latest:  v%d", config.latest_version);
    draw_text_small(755, 410, vbuf, C_GREY_TXT);
    snprintf(vbuf, sizeof(vbuf), "  max:     %d clients", config.max_clients);
    draw_text_small(755, 394, vbuf, C_GREY_TXT);
    snprintf(vbuf, sizeof(vbuf), "  buffer:  %d B", config.buffer_size);
    draw_text_small(755, 378, vbuf, C_GREY_TXT);
    snprintf(vbuf, sizeof(vbuf), "  auth:    %s", config.require_auth ? "ON" : "off");
    draw_text_small(755, 360, vbuf, config.require_auth ? C_GREEN : C_DIM);

    /* ===== event log (large bottom panel) ===== */
    /* panel body y=15..305 (title bar at 305-325) */
    draw_panel(15, 15, 970, 310, "LIVE EVENT LOG", C_ACCENT);
    char hdr[64];
    snprintf(hdr, sizeof(hdr), "showing last %d events (newest at top)", log_count);
    draw_text_small(30, 290, hdr, C_DIM);

    /* faint horizontal separator */
    set_color(C_BORDER);
    glBegin(GL_LINES); glVertex2f(25, 280); glVertex2f(975, 280); glEnd();

    for (int i = 0; i < log_count; i++) {
        Color lc;
        const char *line = logbuf[log_count - 1 - i];
        colored_log_color(line, &lc);
        float fade = 1.0f - (i * 0.04f);
        if (fade < 0.55f) fade = 0.55f;
        Color faded = { lc.r * fade, lc.g * fade, lc.b * fade };

        float row_y = 260 - i * 20;
        if (i % 2 == 1) {
            glColor4f(1, 1, 1, 0.02f);
            glBegin(GL_QUADS);
            glVertex2f(25,  row_y + 6); glVertex2f(975, row_y + 6);
            glVertex2f(975, row_y - 12); glVertex2f(25, row_y - 12);
            glEnd();
        }
        draw_circle(35, row_y, 3, faded);
        draw_text(48, row_y - 4, line, faded);
    }

    /* footer hint */
    draw_text_small(15, 3, "Distributed Software Update Framework  -  ENCS4330", C_DIM);

    glutSwapBuffers();
}

static void timer(int v) { (void)v; frame_counter++; glutPostRedisplay(); glutTimerFunc(60, timer, 0); }

static void reshape(int w, int h)
{
    glViewport(0, 0, w, h);
    glMatrixMode(GL_PROJECTION); glLoadIdentity();
    gluOrtho2D(0, 1000, 0, 710);
    glMatrixMode(GL_MODELVIEW);
}

void init_gui(int *argc, char **argv)
{
    glutInit(argc, argv);
    glutInitDisplayMode(GLUT_DOUBLE | GLUT_RGBA);
    glutInitWindowSize(1000, 710);
    glutCreateWindow("Software Update Server Monitor");
    glClearColor(C_BG.r, C_BG.g, C_BG.b, 1.0f);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_LINE_SMOOTH);
    glHint(GL_LINE_SMOOTH_HINT, GL_NICEST);
    glutDisplayFunc(display);
    glutReshapeFunc(reshape);
    glutTimerFunc(60, timer, 0);
    glutMainLoop();
}

#else  /* NO_GUI: headless stub */
void init_gui(int *argc, char **argv) { (void)argc; (void)argv; }
#endif
