#include <GL/gl.h>
#include <GL/glx.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum { size = 128 };

static int errors(const char *stage)
{
    GLenum e;
    int ok = 1;
    while ((e = glGetError()) != GL_NO_ERROR) {
        printf("FAIL %s GL error 0x%04x\n", stage, (unsigned)e);
        ok = 0;
    }
    return ok;
}

static int sample(const char *label, int x, int y, int r, int g, int b)
{
    GLubyte p[4] = {127, 127, 127, 127};
    glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, p);
    int ok = errors(label) && abs(p[0] - r) <= 4 && abs(p[1] - g) <= 4 &&
             abs(p[2] - b) <= 4;
    printf("%s %s (%d,%d) rgb=%u,%u,%u expected=%d,%d,%d\n",
           ok ? "PASS" : "FAIL", label, x, y, (unsigned)p[0],
           (unsigned)p[1], (unsigned)p[2], r, g, b);
    return ok;
}

int main(int argc, char **argv)
{
    int show = argc == 2 && strcmp(argv[1], "--show") == 0;
    if (argc != 1 && !show) {
        fprintf(stderr, "usage: %s [--show]\n", argv[0]);
        return 2;
    }
    Display *d = XOpenDisplay(NULL);
    if (!d) {
        fputs("SETUP FAIL: cannot open DISPLAY\n", stderr);
        return 2;
    }
    int attrs[] = {GLX_RGBA, GLX_DOUBLEBUFFER, GLX_RED_SIZE, 1,
                   GLX_GREEN_SIZE, 1, GLX_BLUE_SIZE, 1, None};
    XVisualInfo *vi = glXChooseVisual(d, DefaultScreen(d), attrs);
    int db = 0;
    if (!vi || glXGetConfig(d, vi, GLX_DOUBLEBUFFER, &db) != 0 || !db) {
        fputs("SETUP FAIL: no double-buffered RGB visual\n", stderr);
        return 2;
    }
    Window root = RootWindow(d, vi->screen);
    XSetWindowAttributes wa = {0};
    wa.colormap = XCreateColormap(d, root, vi->visual, AllocNone);
    wa.override_redirect = True;
    Window win = XCreateWindow(d, root, 0, 0, size, size, 0, vi->depth,
                               InputOutput, vi->visual,
                               CWBorderPixel | CWColormap | CWOverrideRedirect, &wa);
    XStoreName(d, win, "GL 1.0 baseline");
    XMapRaised(d, win);
    XSync(d, False);
    GLXContext ctx = glXCreateContext(d, vi, NULL, True);
    if (!ctx || !glXMakeCurrent(d, win, ctx)) {
        fputs("SETUP FAIL: cannot create or bind GLX context\n", stderr);
        return 2;
    }

    const GLubyte *vendor = glGetString(GL_VENDOR);
    const GLubyte *renderer = glGetString(GL_RENDERER);
    const GLubyte *version = glGetString(GL_VERSION);
    printf("display=%s visual=0x%lx doublebuffer=%d direct=%d\n"
           "GL_VENDOR=%s\nGL_RENDERER=%s\nGL_VERSION=%s\n",
           DisplayString(d), vi->visualid, db, glXIsDirect(d, ctx),
           vendor ? (const char *)vendor : "(null)",
           renderer ? (const char *)renderer : "(null)",
           version ? (const char *)version : "(null)");
    int ok = errors("identity") && vendor && renderer && version;
    glViewport(0, 0, size, size);
    glClearColor(0, 0, 1, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    ok &= errors("clear");
    ok &= sample("clear-center", 64, 64, 0, 0, 255);
    ok &= sample("clear-corner", 8, 8, 0, 0, 255);

    glColor3f(0, 1, 0);
    glBegin(GL_TRIANGLES);
    glVertex2f(-0.75f, -0.75f);
    glVertex2f( 0.75f, -0.75f);
    glVertex2f( 0.0f,   0.75f);
    glEnd();
    ok &= errors("triangle");
    ok &= sample("triangle-center", 64, 64, 0, 255, 0);
    ok &= sample("triangle-corner", 8, 8, 0, 0, 255);
    printf("%s pre-swap readback\n", ok ? "PASS" : "FAIL");
    fflush(stdout);
    if (show) {
        glXSwapBuffers(d, win);
        XSync(d, False);
        puts("OBSERVE: green triangle on blue for 5 seconds (not scored)");
        fflush(stdout);
        sleep(5);
    }
    glXMakeCurrent(d, None, NULL);
    glXDestroyContext(d, ctx);
    XDestroyWindow(d, win);
    XFreeColormap(d, wa.colormap);
    XFree(vi);
    XCloseDisplay(d);
    return ok ? 0 : 1;
}
