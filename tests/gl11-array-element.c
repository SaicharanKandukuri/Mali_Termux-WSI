/* OpenArena/ioquake3 checks desktop GL 1.1 functions via SDL before loading a map. */
#include <SDL.h>
#include <GL/gl.h>
#include <GL/glx.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>

typedef void (APIENTRY *array_element_fn)(GLint);

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
    static const GLfloat xy[][2] = {{0,0}, {0,0.8f}, {-0.8f,-0.8f}, {0.8f,-0.8f}};
    static const GLubyte rgba[][4] = {{255,0,0,255}, {0,255,0,255}, {0,255,0,255}, {0,255,0,255}};
    static const GLubyte indices[] = {2,3,1};
    glViewport(0, 0, 64, 64);
    glVertexPointer(2, GL_FLOAT, 0, xy);
    glColorPointer(4, GL_UNSIGNED_BYTE, 0, rgba);
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
        }
    }
    dlclose(lib);
    SDL_GL_DeleteContext(ctx);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return rc;
}
