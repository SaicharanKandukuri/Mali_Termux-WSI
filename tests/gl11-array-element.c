/* OpenArena/ioquake3 checks desktop GL 1.1 functions via SDL before loading a map. */
#define GL_GLEXT_PROTOTYPES
#include <SDL.h>
#include <GL/gl.h>
#include <GL/glx.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>

typedef void (APIENTRY *array_element_fn)(GLint);
typedef void (APIENTRY *texture_unit_fn)(GLenum);

static int check_pixels(const char *label)
{
    GLubyte mid[4] = {0}, corner[4] = {0};
    glReadPixels(32, 32, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, mid);
    glReadPixels(2, 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, corner);
    GLenum err = glGetError();
    int ok = !err && mid[0] < 8 && mid[1] > 247 && mid[2] < 8 &&
             corner[0] < 8 && corner[1] < 8 && corner[2] > 247;
    printf("%s %s center=%u,%u,%u corner=%u,%u,%u glError=0x%x\n",
           ok ? "PASS" : "FAIL", label, mid[0], mid[1], mid[2],
           corner[0], corner[1], corner[2], err);
    return ok;
}

int main(void)
{
    const char *path = getenv("SDL_VIDEO_GL_DRIVER");
    if (!path || !*path) { fputs("Set SDL_VIDEO_GL_DRIVER to the libGL under test\n", stderr); return 2; }
    if (SDL_Init(SDL_INIT_VIDEO) || SDL_GL_LoadLibrary(path)) {
        fprintf(stderr, "SDL setup: %s\n", SDL_GetError()); return 2;
    }
    SDL_Window *win = SDL_CreateWindow("GL 1.1 array probe", 0, 0, 64, 64, SDL_WINDOW_OPENGL);
    SDL_GLContext ctx = win ? SDL_GL_CreateContext(win) : NULL;
    if (!ctx) { fprintf(stderr, "SDL context: %s\n", SDL_GetError()); return 2; }

    void *lib = dlopen(path, RTLD_NOW);
    if (!lib) { fprintf(stderr, "libGL: %s\n", dlerror()); return 2; }
    array_element_fn element = (array_element_fn)SDL_GL_GetProcAddress("glArrayElement");
    printf("video=%s library=%s renderer=%s version=%s\n", SDL_GetCurrentVideoDriver(),
           path, glGetString(GL_RENDERER), glGetString(GL_VERSION));
    printf("glArrayElement: SDL=%s GLX_ARB=%s ELF=%s\n", element ? "yes" : "no",
           glXGetProcAddressARB((const GLubyte *)"glArrayElement") ? "yes" : "no",
           dlsym(lib, "glArrayElement") ? "yes" : "no");

    /* Deliberately skip vertex 0. Its position and color must not be drawn. */
    static const struct vertex { GLfloat xy[2]; GLubyte rgba[4]; } verts[] = {
        {{0,0},{255,0,0,255}}, {{0,0.8f},{0,255,0,255}},
        {{-0.8f,-0.8f},{0,255,0,255}}, {{0.8f,-0.8f},{0,255,0,255}}
    };
    static const GLubyte indices[] = {2,3,1};
    glViewport(0, 0, 64, 64);
    glVertexPointer(2, GL_FLOAT, sizeof verts[0], verts[0].xy);
    glColorPointer(4, GL_UNSIGNED_BYTE, sizeof verts[0], verts[0].rgba);
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);
    glClearColor(0, 0, 1, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawElements(GL_TRIANGLE_STRIP, 3, GL_UNSIGNED_BYTE, indices);
    int rc = 2;
    if (check_pixels("glDrawElements control")) {
        rc = 1;
        if (!element) puts("FAIL SDL lookup; glArrayElement draw not run");
        else {
            glClear(GL_COLOR_BUFFER_BIT);
            glColor4f(1, 0, 0, 1);
            glBegin(GL_TRIANGLE_STRIP);
            for (int i = 0; i < 3; ++i) element(indices[i]);
            glEnd();
            rc = !check_pixels("glArrayElement strip");
            /* No pointer rebind: the immediate draw must leave client arrays intact. */
            void *vp = NULL, *cp = NULL;
            glGetPointerv(GL_VERTEX_ARRAY_POINTER, &vp);
            glGetPointerv(GL_COLOR_ARRAY_POINTER, &cp);
            if (vp != (const void *)verts[0].xy || cp != (const void *)verts[0].rgba) {
                puts("FAIL original client-array pointers changed");
                rc = 1;
            } else {
                glClear(GL_COLOR_BUFFER_BIT);
                glDrawElements(GL_TRIANGLE_STRIP, 3, GL_UNSIGNED_BYTE, indices);
                rc |= !check_pixels("glDrawElements after strip");
            }
            glClear(GL_COLOR_BUFFER_BIT);
            glBegin(GL_TRIANGLES);
            for (int i = 0; i < 3; ++i) element(indices[i]);
            glColor4f(1, 0, 0, 1);
            for (int i = 0; i < 3; ++i) glVertex2f(2, 2);
            glEnd();
            rc |= !check_pixels("array elements then immediate vertices");
            /* With no vertex array, the element sets attributes for subsequent vertices. */
            glDisableClientState(GL_VERTEX_ARRAY);
            glClear(GL_COLOR_BUFFER_BIT);
            glColor4f(1, 0, 0, 1);
            glBegin(GL_TRIANGLES);
            element(1);
            for (int i = 0; i < 3; ++i) glVertex2fv(verts[indices[i]].xy);
            glEnd();
            rc |= !check_pixels("color-only array with explicit vertices");
        }
    }
    /* Buffer-backed positions at offset zero are not null client pointers.
       Restore the element buffer before the application's next indexed draw. */
    if (rc == 0 && SDL_GL_GetProcAddress("glBindBuffer")) {
        GLuint bo[2]; GLint bound = 0;
        glGenBuffers(2, bo);
        glBindBuffer(GL_ARRAY_BUFFER, bo[0]);
        glBufferData(GL_ARRAY_BUFFER, sizeof verts, verts, GL_STATIC_DRAW);
        glVertexPointer(2, GL_FLOAT, sizeof verts[0], NULL);
        glEnableClientState(GL_VERTEX_ARRAY);
        glDisableClientState(GL_COLOR_ARRAY);
        glColor4f(0,1,0,1);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, bo[1]);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof indices, indices, GL_STATIC_DRAW);
        glClear(GL_COLOR_BUFFER_BIT);
        glBegin(GL_TRIANGLE_STRIP);
        for (int i = 0; i < 3; ++i) element(indices[i]);
        glEnd();
        rc |= !check_pixels("buffer-backed vertices at offset zero");
        glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &bound);
        if ((GLuint)bound != bo[1]) { puts("FAIL element-buffer binding changed"); rc = 1; }
        else {
            glClear(GL_COLOR_BUFFER_BIT);
            glDrawElements(GL_TRIANGLE_STRIP, 3, GL_UNSIGNED_BYTE, NULL);
            rc |= !check_pixels("bound element buffer after strip");
        }
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
        glDeleteBuffers(2, bo);
    }
    /* ioquake3 can use one coordinate array for the surface and another for
       the lightmap. Only the pair (left, right) produces green. */
    texture_unit_fn active = (texture_unit_fn)SDL_GL_GetProcAddress("glActiveTexture");
    texture_unit_fn client = (texture_unit_fn)SDL_GL_GetProcAddress("glClientActiveTexture");
    GLint units = 0;
    if (active && client) glGetIntegerv(GL_MAX_TEXTURE_UNITS, &units);
    if (rc == 0 && units >= 2) {
        static const GLubyte pixels[2][8] = {
            {0,255,0,255, 255,0,0,255}, {255,0,0,255, 255,255,255,255}
        };
        static const GLfloat st[2][8] = {
            {.75f,0, .25f,0, .25f,0, .25f,0}, {.25f,0, .75f,0, .75f,0, .75f,0}
        };
        GLuint textures[2];
        glGenTextures(2, textures);
        for (int i = 0; i < 2; ++i) {
            active(GL_TEXTURE0+i);
            glBindTexture(GL_TEXTURE_2D, textures[i]);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 2, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels[i]);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glEnable(GL_TEXTURE_2D);
            client(GL_TEXTURE0+i);
            glTexCoordPointer(2, GL_FLOAT, 0, st[i]);
            glEnableClientState(GL_TEXTURE_COORD_ARRAY);
        }
        glVertexPointer(2, GL_FLOAT, sizeof verts[0], verts[0].xy);
        glEnableClientState(GL_VERTEX_ARRAY);
        glDisableClientState(GL_COLOR_ARRAY);
        glColor4f(1,1,1,1);
        glClear(GL_COLOR_BUFFER_BIT);
        glDrawElements(GL_TRIANGLE_STRIP, 3, GL_UNSIGNED_BYTE, indices);
        rc |= !check_pixels("two texture units control");
        glClear(GL_COLOR_BUFFER_BIT);
        glBegin(GL_TRIANGLE_STRIP);
        for (int i = 0; i < 3; ++i) element(indices[i]);
        glEnd();
        rc |= !check_pixels("two texture units indexed strip");
        GLint selected = 0;
        glGetIntegerv(GL_CLIENT_ACTIVE_TEXTURE, &selected);
        if (selected != GL_TEXTURE1) { puts("FAIL client texture unit changed"); rc = 1; }
        glDeleteTextures(2, textures);
    } else if (rc == 0) puts("SKIP multitexture: no second texture unit");
    dlclose(lib);
    SDL_GL_DeleteContext(ctx);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return rc;
}
