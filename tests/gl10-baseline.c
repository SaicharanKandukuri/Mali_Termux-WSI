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

static void triangle_z(GLfloat z)
{
    glBegin(GL_TRIANGLES);
    glVertex3f(-0.75f, -0.75f, z);
    glVertex3f( 0.75f, -0.75f, z);
    glVertex3f( 0.0f,   0.75f, z);
    glEnd();
}

static int component_textures(void)
{
    static const struct {
        const char *name;
        GLenum format;
        GLubyte data[4], rgb[3];
    } cases[] = {
        {"1/L",    GL_LUMINANCE,       {192},              {192,192,192}},
        {"2/LA",   GL_LUMINANCE_ALPHA, {192,128},          {96,96,223}},
        {"3/RGB",  GL_RGB,             {31,143,31},        {31,143,31}},
        {"4/RGBA", GL_RGBA,            {32,128,192,128},   {16,64,223}}
    };
    int ok = 1;
    glEnable(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glColor4f(1, 1, 1, 1);
    glClearColor(0, 0, 1, 1);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    for (int j = 0; j < 4; ++j) {
        glTexImage2D(GL_TEXTURE_2D, 0, j+1, 1, 1, 0, cases[j].format,
                     GL_UNSIGNED_BYTE, cases[j].data);
        ok &= errors(cases[j].name);
        glClear(GL_COLOR_BUFFER_BIT);
        triangle_z(0);
        ok &= sample(cases[j].name, 64, 64, cases[j].rgb[0], cases[j].rgb[1], cases[j].rgb[2]);
    }
    glDisable(GL_BLEND);
    glDisable(GL_TEXTURE_2D);
    return ok;
}

static int context_state(const char *label, int blend, int red, int blue)
{
    GLfloat color[4] = {-1, -1, -1, -1};
    glGetFloatv(GL_CURRENT_COLOR, color);
    int enabled = glIsEnabled(GL_BLEND);
    int ok = errors(label) && enabled == blend && color[0] == red &&
             color[1] == 1 && color[2] == blue && color[3] == 1;
    printf("%s %s blend=%d rgba=%g,%g,%g,%g expected=%d/%d,1,%d,1\n",
           ok ? "PASS" : "FAIL", label, enabled, color[0], color[1], color[2],
           color[3], blend, red, blue);
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
    int attrs[] = {GLX_RGBA, GLX_DOUBLEBUFFER, GLX_RED_SIZE, 8,
                   GLX_GREEN_SIZE, 8, GLX_BLUE_SIZE, 8, GLX_DEPTH_SIZE, 16, None};
    XVisualInfo *vi = glXChooseVisual(d, DefaultScreen(d), attrs);
    int db = 0;
    if (!vi || glXGetConfig(d, vi, GLX_DOUBLEBUFFER, &db) != 0 || !db) {
        fputs("SETUP FAIL: no double-buffered RGB8/depth16 visual\n", stderr);
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
    GLint depth = 0, red = 0, green = 0, blue = 0;
    glGetIntegerv(GL_DEPTH_BITS, &depth);
    glGetIntegerv(GL_RED_BITS, &red);
    glGetIntegerv(GL_GREEN_BITS, &green);
    glGetIntegerv(GL_BLUE_BITS, &blue);
    printf("GL_DEPTH_BITS=%d GL_RGB_BITS=%d,%d,%d\n", depth, red, green, blue);
    if (!errors("framebuffer bits") || depth < 16 || red < 8 || green < 8 || blue < 8) {
        fputs("SETUP FAIL: bound framebuffer does not satisfy RGB8/depth16\n", stderr);
        return 2;
    }
    glViewport(0, 0, size, size);
    glClearColor(0, 0, 1, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    ok &= errors("clear");
    ok &= sample("clear-center", 64, 64, 0, 0, 255);
    ok &= sample("clear-corner", 8, 8, 0, 0, 255);

    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glClearDepth(1);
    glClear(GL_DEPTH_BUFFER_BIT);
    glColor3f(1, 0, 0);
    triangle_z(0);
    ok &= errors("depth initial draw");
    ok &= sample("depth-first-accepted", 64, 64, 255, 0, 0);
    glColor3f(0, 1, 0);
    triangle_z(0.5f);
    ok &= errors("depth farther draw");
    ok &= sample("depth-farther-rejected", 64, 64, 255, 0, 0);
    triangle_z(-0.5f);
    ok &= errors("depth nearer draw");
    ok &= sample("depth-nearer-accepted", 64, 64, 0, 255, 0);
    glDisable(GL_DEPTH_TEST);

    glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_DITHER);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glColor4f(1, 0, 0, 0.5f);
    triangle_z(0);
    ok &= errors("blend draw");
    /* RGB8 or better, dithering off: half red plus half blue, allowing rounding. */
    ok &= sample("blend-half-red-over-blue", 64, 64, 128, 0, 128);
    ok &= sample("blend-background", 8, 8, 0, 0, 255);

    glColor4f(0, 1, 0, 1);
    ok &= context_state("context-A-before", 1, 0, 0);
    GLXContext other = glXCreateContext(d, vi, NULL, True);
    if (!other || !glXMakeCurrent(d, win, other)) {
        fputs("SETUP FAIL: cannot create or bind independent context B\n", stderr);
        return 2;
    }
    ok &= context_state("context-B-default", 0, 1, 1);
    if (!glXMakeCurrent(d, win, ctx)) {
        fputs("SETUP FAIL: cannot rebind original context A\n", stderr);
        return 2;
    }
    ok &= context_state("context-A-restored", 1, 0, 0);
    glXDestroyContext(d, other);
    glDisable(GL_BLEND);
    ok &= component_textures();
    glClearColor(0, 0, 1, 1);
    glClear(GL_COLOR_BUFFER_BIT);
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
