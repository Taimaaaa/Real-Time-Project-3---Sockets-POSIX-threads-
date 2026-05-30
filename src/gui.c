// gui.c
// OpenGL visualization for the software update server monitor.
// Displays live server stats, active client indicators, and a scrolling event log.
// All displayed data comes from shared state updated by server threads in real time.

#include <GL/glut.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "common.h"

// global gui state definitions (declared extern in common.h)
int  gui_active_clients  = 0;
int  gui_total_updates   = 0;
int  gui_total_connected = 0;
char gui_log[MAX_LOG_LINES][128];
int  gui_log_count       = 0;
pthread_mutex_t gui_mutex = PTHREAD_MUTEX_INITIALIZER;

void gui_add_log(const char *msg)
{
    pthread_mutex_lock(&gui_mutex);

    if (gui_log_count < MAX_LOG_LINES)
    {
        strncpy(gui_log[gui_log_count], msg, 127);
        gui_log_count++;
    }
    else
    {
        // scroll up and add new entry at bottom
        for (int i = 0; i < MAX_LOG_LINES - 1; i++)
            strcpy(gui_log[i], gui_log[i + 1]);
        strncpy(gui_log[MAX_LOG_LINES - 1], msg, 127);
    }

    pthread_mutex_unlock(&gui_mutex);
}

static void draw_text(float x, float y, const char *text)
{
    glRasterPos2f(x, y);
    for (const char *c = text; *c != '\0'; c++)
        glutBitmapCharacter(GLUT_BITMAP_9_BY_15, *c);
}

static void draw_rect(float x, float y, float w, float h, float r, float g, float b)
{
    glColor3f(r, g, b);
    glBegin(GL_QUADS);
        glVertex2f(x,     y);
        glVertex2f(x + w, y);
        glVertex2f(x + w, y + h);
        glVertex2f(x,     y + h);
    glEnd();
}

static void draw_circle(float cx, float cy, float radius, float r, float g, float b)
{
    glColor3f(r, g, b);
    glBegin(GL_POLYGON);
    for (int i = 0; i < 30; i++)
    {
        float angle = 2.0f * 3.14159f * i / 30;
        glVertex2f(cx + radius * cosf(angle), cy + radius * sinf(angle));
    }
    glEnd();
}

void display(void)
{
    glClear(GL_COLOR_BUFFER_BIT);

    // title bar
    draw_rect(0, 560, 800, 40, 0.1f, 0.1f, 0.3f);
    glColor3f(1.0f, 1.0f, 1.0f);
    draw_text(10, 572, "Distributed Software Update Framework -- Server Monitor");

    // stats panel background
    draw_rect(10, 460, 370, 90, 0.15f, 0.15f, 0.15f);

    // server status indicator
    draw_circle(35, 535, 10, 0.2f, 0.9f, 0.3f); // green dot = running
    glColor3f(0.4f, 0.8f, 1.0f);
    draw_text(55, 530, "SERVER STATUS: RUNNING");

    // live stats from shared state
    char buf[128];
    glColor3f(1.0f, 1.0f, 1.0f);

    pthread_mutex_lock(&gui_mutex);
    sprintf(buf, "Active Clients  : %d", gui_active_clients);
    draw_text(20, 510, buf);
    sprintf(buf, "Total Connected : %d", gui_total_connected);
    draw_text(20, 492, buf);
    sprintf(buf, "Updates Sent    : %d", gui_total_updates);
    draw_text(20, 474, buf);
    pthread_mutex_unlock(&gui_mutex);

    // active client circles panel
    draw_rect(390, 460, 400, 90, 0.15f, 0.15f, 0.15f);
    glColor3f(0.7f, 0.7f, 0.7f);
    draw_text(400, 535, "Active Client Threads:");

    pthread_mutex_lock(&gui_mutex);
    for (int i = 0; i < gui_active_clients && i < 8; i++)
    {
        float cx = 420 + i * 45;
        float cy = 492;
        draw_circle(cx, cy, 14, 0.2f, 0.8f, 0.4f); // green circle per active client

        // draw thread number inside circle
        char num[4];
        sprintf(num, "%d", i + 1);
        glColor3f(0.0f, 0.0f, 0.0f);
        draw_text(cx - 4, cy - 5, num);
    }

    // grey circles for empty slots
    for (int i = gui_active_clients; i < 8; i++)
    {
        float cx = 420 + i * 45;
        float cy = 492;
        draw_circle(cx, cy, 14, 0.3f, 0.3f, 0.3f);
    }
    pthread_mutex_unlock(&gui_mutex);

    // event log panel
    draw_rect(10, 10, 780, 440, 0.1f, 0.1f, 0.1f);
    glColor3f(0.5f, 0.5f, 0.5f);
    draw_text(20, 432, "Event Log:");

    // horizontal divider
    glColor3f(0.2f, 0.2f, 0.2f);
    glBegin(GL_LINES);
        glVertex2f(10,  420);
        glVertex2f(790, 420);
    glEnd();

    pthread_mutex_lock(&gui_mutex);
    for (int i = 0; i < gui_log_count; i++)
    {
        // newer entries brighter
        float brightness = 0.4f + 0.6f * ((float)i / MAX_LOG_LINES);
        glColor3f(0.0f, brightness, brightness * 0.6f);
        draw_text(20, 408 - i * 18, gui_log[gui_log_count - 1 - i]);
    }
    pthread_mutex_unlock(&gui_mutex);

    glutSwapBuffers();
}

void timer(int value)
{
    glutPostRedisplay();
    glutTimerFunc(100, timer, 0); // refresh every 100ms for smoother updates
}

void reshape(int w, int h)
{
    glViewport(0, 0, w, h);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    gluOrtho2D(0, w, 0, h);
    glMatrixMode(GL_MODELVIEW);
}

void init_gui(int *argc, char **argv)
{
    glutInit(argc, argv);
    glutInitDisplayMode(GLUT_DOUBLE | GLUT_RGB);
    glutInitWindowSize(800, 600);
    glutCreateWindow("Software Update Server Monitor");

    glClearColor(0.08f, 0.08f, 0.08f, 1.0f);

    glutDisplayFunc(display);
    glutReshapeFunc(reshape);
    glutTimerFunc(100, timer, 0);

    glutMainLoop(); // blocks — this is why we run it in its own thread
}