/* =====================================================================
 * gui.c
 * ---------------------------------------------------------------------
 * OpenGL/GLUT monitor for the update server. Shows, in real time:
 *   - server status (running / stopped),
 *   - active client count and per-thread "slot" indicators,
 *   - cumulative stats (connected, updates sent, up-to-date, failed,
 *     total bytes transferred),
 *   - a scrolling event log.
 *
 * All numbers come from shared state in common.c, updated by the server
 * worker threads and guarded by gui_mutex. The GUI never touches sockets;
 * it is a pure read-only view, which keeps the concurrency story simple.
 *
 * When compiled with -DNO_GUI this file provides no-op stubs for the GUI
 * entry points plus the shared-state definitions, so the headless server
 * still links and the log-driven counters still work.
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

/* Map thread_id -> slot index. Assignment table ensures the same thread
 * always lights/clears the SAME slot regardless of how high thread_id
 * climbs across multiple client runs without restarting the server. */
#define NO_SLOT (-1)
static int slot_owner[MAX_SLOTS];   /* thread_id owning each slot, or NO_SLOT */

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
        /* find a free slot */
        int idx = NO_SLOT;
        for (int i = 0; i < MAX_SLOTS; i++) {
            if (slot_owner[i] == NO_SLOT) { idx = i; break; }
        }
        if (idx != NO_SLOT) {
            slot_owner[idx]  = thread_id;
            gui_slot_used[idx] = 1;
        }
    } else {
        /* find the slot this thread owns and clear it */
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

static void draw_text(float x, float y, const char *text)
{
    glRasterPos2f(x, y);
    for (const char *c = text; *c; c++)
        glutBitmapCharacter(GLUT_BITMAP_9_BY_15, *c);
}

static void draw_rect(float x, float y, float w, float h, float r, float g, float b)
{
    glColor3f(r, g, b);
    glBegin(GL_QUADS);
        glVertex2f(x, y); glVertex2f(x+w, y);
        glVertex2f(x+w, y+h); glVertex2f(x, y+h);
    glEnd();
}

static void draw_circle(float cx, float cy, float rad, float r, float g, float b)
{
    glColor3f(r, g, b);
    glBegin(GL_POLYGON);
    for (int i = 0; i < 32; i++) {
        float a = 2.0f * 3.14159265f * i / 32;
        glVertex2f(cx + rad * cosf(a), cy + rad * sinf(a));
    }
    glEnd();
}

static void display(void)
{
    glClear(GL_COLOR_BUFFER_BIT);

    /* title bar */
    draw_rect(0, 560, 800, 40, 0.10f, 0.10f, 0.32f);
    glColor3f(1,1,1);
    draw_text(12, 572, "Distributed Software Update Framework  --  Server Monitor");

    /* status + stats panel */
    draw_rect(10, 430, 370, 120, 0.15f, 0.15f, 0.15f);

    pthread_mutex_lock(&gui_mutex);
    int running = gui_server_running;
    int ac = gui_active_clients, tc = gui_total_connected, tu = gui_total_updates;
    int ud = gui_total_uptodate, tf = gui_total_failed;
    long bytes = gui_bytes_sent;
    int slots[MAX_SLOTS]; memcpy(slots, gui_slot_used, sizeof(slots));
    int log_count = gui_log_count;
    char logbuf[MAX_LOG_LINES][128];
    for (int i = 0; i < log_count; i++) strcpy(logbuf[i], gui_log[i]);
    pthread_mutex_unlock(&gui_mutex);

    if (running) draw_circle(35, 535, 10, 0.2f, 0.9f, 0.3f);
    else         draw_circle(35, 535, 10, 0.9f, 0.2f, 0.2f);
    glColor3f(0.4f, 0.8f, 1.0f);
    draw_text(55, 530, running ? "SERVER STATUS: RUNNING" : "SERVER STATUS: STOPPED");

    char buf[128];
    glColor3f(1,1,1);
    sprintf(buf, "Active clients  : %d", ac);      draw_text(20, 508, buf);
    sprintf(buf, "Total connected : %d", tc);      draw_text(20, 490, buf);
    sprintf(buf, "Updates sent    : %d", tu);      draw_text(20, 472, buf);
    sprintf(buf, "Up to date      : %d", ud);      draw_text(200,490, buf);
    sprintf(buf, "Failed          : %d", tf);      draw_text(200,472, buf);
    sprintf(buf, "Bytes transferred: %ld", bytes); draw_text(20, 448, buf);

    /* active-thread slots */
    draw_rect(390, 430, 400, 120, 0.15f, 0.15f, 0.15f);
    glColor3f(0.7f,0.7f,0.7f);
    draw_text(400, 535, "Worker thread slots:");
    for (int i = 0; i < MAX_SLOTS; i++) {
        float cx = 420 + i * 45, cy = 490;
        if (slots[i]) draw_circle(cx, cy, 15, 0.2f, 0.8f, 0.4f);
        else          draw_circle(cx, cy, 15, 0.28f,0.28f,0.28f);
        char num[4]; sprintf(num, "%d", i+1);
        glColor3f(0,0,0); draw_text(cx-4, cy-5, num);
    }
    glColor3f(0.5f,0.5f,0.5f);
    draw_text(400, 448, "(green = busy, grey = idle)");

    /* event log */
    draw_rect(10, 10, 780, 410, 0.10f, 0.10f, 0.10f);
    glColor3f(0.55f,0.55f,0.55f);
    draw_text(20, 400, "Event Log:");
    glColor3f(0.2f,0.2f,0.2f);
    glBegin(GL_LINES); glVertex2f(10,390); glVertex2f(790,390); glEnd();

    for (int i = 0; i < log_count; i++) {
        float brightness = 0.45f + 0.55f * ((float)i / MAX_LOG_LINES);
        glColor3f(0.0f, brightness, brightness * 0.65f);
        draw_text(20, 372 - i * 22, logbuf[log_count - 1 - i]);
    }

    glutSwapBuffers();
}

static void timer(int v) { (void)v; glutPostRedisplay(); glutTimerFunc(100, timer, 0); }

static void reshape(int w, int h)
{
    glViewport(0, 0, w, h);
    glMatrixMode(GL_PROJECTION); glLoadIdentity();
    gluOrtho2D(0, 800, 0, 600);
    glMatrixMode(GL_MODELVIEW);
}

void init_gui(int *argc, char **argv)
{
    glutInit(argc, argv);
    glutInitDisplayMode(GLUT_DOUBLE | GLUT_RGB);
    glutInitWindowSize(800, 600);
    glutCreateWindow("Software Update Server Monitor");
    glClearColor(0.07f, 0.07f, 0.07f, 1.0f);
    glutDisplayFunc(display);
    glutReshapeFunc(reshape);
    glutTimerFunc(100, timer, 0);
    glutMainLoop();   /* blocks; runs in its own thread */
}

#else  /* NO_GUI: headless stub */

void init_gui(int *argc, char **argv) { (void)argc; (void)argv; }

#endif
