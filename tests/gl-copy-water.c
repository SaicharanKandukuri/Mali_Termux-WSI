#include <GL/gl.h>
#include <GL/glx.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { side = 16, src_x = 8, src_y = 32 };
static int failed;

static void errors(const char *stage)
{
    GLenum e;
    while ((e = glGetError()) != GL_NO_ERROR) {
        printf("FAIL %s GL error=0x%x\n", stage, (unsigned)e);
        failed = 1;
    }
}

static void pair(const char *stage, int x, int y, int width, int blue)
{
    for (int i = 0; i < 2; ++i) {
        GLubyte p[4] = {0};
        int px = x + (i ? 3 : 1) * width / 4, py = y + width / 2;
        int r = (!blue && !i) * 255, g = (!blue && i) * 255, b = blue * 255;
        glReadPixels(px, py, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, p);
        int ok = abs(p[0]-r) <= 4 && abs(p[1]-g) <= 4 && abs(p[2]-b) <= 4;
        printf("%s %s (%d,%d) rgb=%d,%d,%d expected=%d,%d,%d\n",
               ok ? "PASS" : "FAIL", stage, px, py, p[0], p[1], p[2], r, g, b);
        failed |= !ok;
    }
    errors(stage);
}

static void draw(const char *stage, int width, int blue)
{
    glViewport(0, 0, width, width);
    glClearColor(1, 1, 1, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glBegin(GL_TRIANGLE_STRIP);
    glTexCoord2f(0,0); glVertex2f(-1,-1);
    glTexCoord2f(1,0); glVertex2f( 1,-1);
    glTexCoord2f(0,1); glVertex2f(-1, 1);
    glTexCoord2f(1,1); glVertex2f( 1, 1);
    glEnd();
    pair(stage, 0, 0, width, blue);
}

int main(int argc, char **argv)
{
    const char *symbol = "glGenerateMipmapEXT";
    if (argc == 2 && !strcmp(argv[1], "--oes")) symbol = "glGenerateMipmapOES";
    else if (argc != 1) { fputs("usage: gl-copy-water [--oes]\n", stderr); return 2; }
    Display *d = XOpenDisplay(NULL);
    int attrs[] = {GLX_RGBA, GLX_DOUBLEBUFFER, GLX_RED_SIZE, 8,
                   GLX_GREEN_SIZE, 8, GLX_BLUE_SIZE, 8, None};
    XVisualInfo *vi = d ? glXChooseVisual(d, DefaultScreen(d), attrs) : NULL;
    if (!vi) { fputs("SETUP FAIL: no display or RGB8 GLX visual\n", stderr); return 2; }
    Window root = RootWindow(d, vi->screen);
    XSetWindowAttributes wa = {0};
    wa.colormap = XCreateColormap(d, root, vi->visual, AllocNone);
    wa.override_redirect = True;
    Window win = XCreateWindow(d, root, 0, 0, 64, 64, 0, vi->depth, InputOutput,
                               vi->visual, CWColormap|CWBorderPixel|CWOverrideRedirect, &wa);
    XMapRaised(d, win);
    XSync(d, False);
    GLXContext ctx = glXCreateContext(d, vi, NULL, True);
    if (!ctx || !glXMakeCurrent(d, win, ctx)) {
        fputs("SETUP FAIL: cannot bind context\n", stderr); return 2;
    }
    printf("renderer=%s GL_VERSION=%s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION));
    GLuint tex;
    GLubyte pixels[side*side][4];
    for (int i = 0; i < side*side; ++i) {
        pixels[i][0] = (i % side < side/2) * 255;
        pixels[i][1] = 255 - pixels[i][0];
        pixels[i][2] = 0; pixels[i][3] = 255;
    }
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, side, side, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    glEnable(GL_TEXTURE_2D);
    draw("upload control", side, 0);

    for (int i = 0; i < side*side; ++i) {
        pixels[i][0] = pixels[i][1] = 0; pixels[i][2] = 255;
    }
    for (int lvl = 0, n = side; n; ++lvl, n /= 2)
        glTexImage2D(GL_TEXTURE_2D, lvl, GL_RGBA, n, n, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    errors("allocate blue levels");
    glEnable(GL_SCISSOR_TEST);
    glScissor(src_x, src_y, side/2, side); glClearColor(1,0,0,1); glClear(GL_COLOR_BUFFER_BIT);
    glScissor(src_x+side/2, src_y, side/2, side); glClearColor(0,1,0,1); glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_SCISSOR_TEST);
    pair("source", src_x, src_y, side, 0);
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, src_x, src_y, side, side);
    errors("copy");
    draw("copy level 0", side, 0);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_NEAREST);
    draw("seeded mip before generate", 4, 1);
    typedef void (*gen_fn)(GLenum);
    printf("Testing %s\n", symbol);
    gen_fn gen = (gen_fn)glXGetProcAddressARB((const GLubyte *)symbol);
    if (gen) {
        gen(GL_TEXTURE_2D);
        errors(symbol);
        draw("generated mip", 4, 0);
    } else {
        printf("UNAVAILABLE %s\n", symbol);
    }
    glDeleteTextures(1, &tex);
    glXMakeCurrent(d, None, NULL);
    glXDestroyContext(d, ctx);
    XDestroyWindow(d, win);
    XFreeColormap(d, wa.colormap);
    XFree(vi);
    XCloseDisplay(d);
    return failed ? 1 : (gen ? 0 : 2);
}
