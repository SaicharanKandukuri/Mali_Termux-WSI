/* OpenArena/ioquake3 checks desktop GL 1.1 functions via SDL before loading a map. */
#define GL_GLEXT_PROTOTYPES
#include <SDL.h>
#include <GL/gl.h>
#include <GL/glx.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef GL_ARRAY_ELEMENT_LOCK_FIRST_EXT
#define GL_ARRAY_ELEMENT_LOCK_FIRST_EXT 0x81A8
#define GL_ARRAY_ELEMENT_LOCK_COUNT_EXT 0x81A9
#endif

typedef void (APIENTRY *array_element_fn)(GLint);
typedef void (APIENTRY *texture_unit_fn)(GLenum);
typedef void (APIENTRY *lock_arrays_fn)(GLint, GLsizei);
typedef void (APIENTRY *unlock_arrays_fn)(void);

static int has_extension(const char *extensions, const char *name)
{
    size_t len = strlen(name);
    const char *found = extensions;
    while (found && (found = strstr(found, name))) {
        if ((found == extensions || found[-1] == ' ') &&
            (found[len] == '\0' || found[len] == ' ')) return 1;
        found += len;
    }
    return 0;
}

static int clear_errors(const char *label)
{
    GLenum err = glGetError();
    if (err == GL_NO_ERROR) return 1;
    printf("FAIL %s inherited glError=0x%x\n", label, err);
    while (glGetError() != GL_NO_ERROR) {}
    return 0;
}

static int expect_error(const char *label, GLenum expected)
{
    GLenum got = glGetError(), extra = glGetError();
    int ok = got == expected && extra == GL_NO_ERROR;
    printf("%s %s glError=0x%x expected=0x%x extra=0x%x\n",
           ok ? "PASS" : "FAIL", label, got, expected, extra);
    return ok;
}

static int call_lock(lock_arrays_fn lock_arrays, GLint first, GLsizei count,
                     const char *label, GLenum expected)
{
    int clean = clear_errors(label);
    lock_arrays(first, count);
    return expect_error(label, expected) && clean;
}

static int call_unlock(unlock_arrays_fn unlock_arrays, const char *label,
                       GLenum expected)
{
    int clean = clear_errors(label);
    unlock_arrays();
    return expect_error(label, expected) && clean;
}

static int check_lock_range(const char *label, GLint expected_first,
                            GLint expected_count)
{
    GLint first = -1, count = -1;
    int clean = clear_errors(label);
    glGetIntegerv(GL_ARRAY_ELEMENT_LOCK_FIRST_EXT, &first);
    glGetIntegerv(GL_ARRAY_ELEMENT_LOCK_COUNT_EXT, &count);
    GLboolean b = GL_FALSE;
    GLfloat f = -1;
    GLdouble d = -1;
    glGetBooleanv(GL_ARRAY_ELEMENT_LOCK_COUNT_EXT, &b);
    glGetFloatv(GL_ARRAY_ELEMENT_LOCK_FIRST_EXT, &f);
    glGetDoublev(GL_ARRAY_ELEMENT_LOCK_COUNT_EXT, &d);
    GLenum err = glGetError(), extra = glGetError();
    int ok = clean && err == GL_NO_ERROR && extra == GL_NO_ERROR &&
             first == expected_first && count == expected_count &&
             b == (expected_count != 0) && f == expected_first && d == expected_count;
    printf("%s %s first=%d count=%d bool=%u float=%g double=%g glError=0x%x extra=0x%x\n",
           ok ? "PASS" : "FAIL", label, first, count, (unsigned)b, (double)f, d, err, extra);
    return ok;
}

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
    const char *extensions = (const char *)glGetString(GL_EXTENSIONS);
    printf("video=%s library=%s renderer=%s version=%s\n", SDL_GetCurrentVideoDriver(),
           path, glGetString(GL_RENDERER), glGetString(GL_VERSION));
    printf("GL_EXTENSIONS=%s\n", extensions ? extensions : "(null)");
    if (!extensions) {
        fputs("GL_EXTENSIONS query failed with a current context\n", stderr);
        return 2;
    }
    array_element_fn element = (array_element_fn)SDL_GL_GetProcAddress("glArrayElement");
    int cva_gl = has_extension(extensions, "GL_EXT_compiled_vertex_array");
    int cva_sdl = SDL_GL_ExtensionSupported("GL_EXT_compiled_vertex_array") == SDL_TRUE;
    lock_arrays_fn lock_arrays =
        (lock_arrays_fn)SDL_GL_GetProcAddress("glLockArraysEXT");
    unlock_arrays_fn unlock_arrays =
        (unlock_arrays_fn)SDL_GL_GetProcAddress("glUnlockArraysEXT");
    printf("glArrayElement: SDL=%s GLX_ARB=%s ELF=%s\n", element ? "yes" : "no",
           glXGetProcAddressARB((const GLubyte *)"glArrayElement") ? "yes" : "no",
           dlsym(lib, "glArrayElement") ? "yes" : "no");
    printf("GL_EXT_compiled_vertex_array: GL=%s SDL=%s\n",
           cva_gl ? "yes" : "no", cva_sdl ? "yes" : "no");
    printf("glLockArraysEXT: SDL=%s\n", lock_arrays ? "yes" : "no");
    printf("glUnlockArraysEXT: SDL=%s\n", unlock_arrays ? "yes" : "no");

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
    int control_ok = check_pixels("glDrawElements control");
    int cva_rc = 0;
    if (cva_gl != cva_sdl) {
        puts("FAIL GL_EXT_compiled_vertex_array app discovery mismatch between GL and SDL");
        cva_rc = 1;
    }
    if (!cva_gl) {
        puts("SKIP GL_EXT_compiled_vertex_array: not advertised by GL");
    } else if (!lock_arrays || !unlock_arrays) {
        puts("FAIL GL_EXT_compiled_vertex_array advertised but SDL proc lookup is incomplete");
        cva_rc = 1;
    } else {
        cva_rc |= !check_lock_range("CVA initial range", 0, 0);

        cva_rc |= !call_lock(lock_arrays, 1, 3, "CVA valid lock", GL_NO_ERROR);
        cva_rc |= !check_lock_range("CVA locked range", 1, 3);
        /* An unshared context on the same thread must not inherit this lock. */
        SDL_GLContext other = SDL_GL_CreateContext(win);
        if (!other || SDL_GL_MakeCurrent(win, other)) {
            fprintf(stderr, "CVA context switch: %s\n", SDL_GetError()); return 2;
        }
        cva_rc |= !check_lock_range("CVA independent context", 0, 0);
        if (SDL_GL_MakeCurrent(win, ctx)) {
            fprintf(stderr, "CVA restore context: %s\n", SDL_GetError()); return 2;
        }
        SDL_GL_DeleteContext(other);
        cva_rc |= !check_lock_range("CVA restored context", 1, 3);
        cva_rc |= !clear_errors("CVA locked draw");
        glClear(GL_COLOR_BUFFER_BIT);
        glDrawElements(GL_TRIANGLE_STRIP, 3, GL_UNSIGNED_BYTE, indices);
        cva_rc |= !check_pixels("CVA locked draw");
        cva_rc |= !call_unlock(unlock_arrays, "CVA valid unlock", GL_NO_ERROR);

        cva_rc |= !call_lock(lock_arrays, -1, 3, "CVA negative first", GL_INVALID_VALUE);
        cva_rc |= !call_lock(lock_arrays, 1, 0, "CVA zero count", GL_INVALID_VALUE);
        cva_rc |= !call_lock(lock_arrays, 1, -1, "CVA negative count", GL_INVALID_VALUE);
        cva_rc |= !call_unlock(unlock_arrays, "CVA unmatched unlock", GL_INVALID_OPERATION);

        cva_rc |= !call_lock(lock_arrays, 1, 3, "CVA nested setup", GL_NO_ERROR);
        cva_rc |= !call_lock(lock_arrays, 0, 1, "CVA nested lock", GL_INVALID_OPERATION);
        cva_rc |= !check_lock_range("CVA range after nested lock", 1, 3);
        cva_rc |= !call_unlock(unlock_arrays, "CVA nested cleanup", GL_NO_ERROR);

        cva_rc |= !clear_errors("CVA lock in begin/end");
        glBegin(GL_POINTS);
        lock_arrays(1, 3);
        glEnd();
        cva_rc |= !expect_error("CVA lock in begin/end", GL_INVALID_OPERATION);
        cva_rc |= !call_lock(lock_arrays, 1, 3, "CVA unlock in begin/end setup",
                             GL_NO_ERROR);
        cva_rc |= !clear_errors("CVA unlock in begin/end");
        glBegin(GL_POINTS);
        unlock_arrays();
        glEnd();
        cva_rc |= !expect_error("CVA unlock in begin/end", GL_INVALID_OPERATION);
        cva_rc |= !check_lock_range("CVA range after begin/end unlock", 1, 3);
        cva_rc |= !call_unlock(unlock_arrays, "CVA begin/end cleanup", GL_NO_ERROR);

        /* Both operations execute during list compilation and are not replayed. */
        GLuint list = glGenLists(2);
        if (!list) { fputs("CVA display-list allocation failed\n", stderr); return 2; }
        glNewList(list, GL_COMPILE);
        lock_arrays(1, 3);
        glEndList();
        cva_rc |= !check_lock_range("CVA lock during list compile", 1, 3);
        glNewList(list + 1, GL_COMPILE);
        unlock_arrays();
        glEndList();
        glCallList(list);
        cva_rc |= !call_lock(lock_arrays, 1, 3, "CVA lock not replayed", GL_NO_ERROR);
        glCallList(list + 1);
        cva_rc |= !call_unlock(unlock_arrays, "CVA unlock not replayed", GL_NO_ERROR);
        glDeleteLists(list, 2);
    }
    int rc = 2;
    if (control_ok) {
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
    typedef void (APIENTRY *coord2f_fn)(GLenum, GLfloat, GLfloat);
    texture_unit_fn active_arb = (texture_unit_fn)SDL_GL_GetProcAddress("glActiveTextureARB");
    texture_unit_fn client_arb = (texture_unit_fn)SDL_GL_GetProcAddress("glClientActiveTextureARB");
    coord2f_fn coord_arb = (coord2f_fn)SDL_GL_GetProcAddress("glMultiTexCoord2fARB");
    int arb = SDL_GL_ExtensionSupported("GL_ARB_multitexture");
    if (arb && (!active_arb || !client_arb || !coord_arb)) {
        puts("FAIL GL_ARB_multitexture advertised but OpenArena entry points missing");
        rc = 1;
    }
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
        if (arb) {
            /* Replace the two coordinate arrays with different per-unit constants. */
            for (int i = 0; i < 2; i++) {
                active_arb(GL_TEXTURE0 + i);
                glGetIntegerv(GL_ACTIVE_TEXTURE, &selected);
                if (selected != (GLint)(GL_TEXTURE0 + i)) { puts("FAIL ARB active unit"); rc = 1; }
                client_arb(GL_TEXTURE0 + i);
                glGetIntegerv(GL_CLIENT_ACTIVE_TEXTURE, &selected);
                if (selected != (GLint)(GL_TEXTURE0 + i)) { puts("FAIL ARB client unit"); rc = 1; }
                glDisableClientState(GL_TEXTURE_COORD_ARRAY);
                coord_arb(GL_TEXTURE0 + i, i ? .75f : .25f, 0);
            }
            glClear(GL_COLOR_BUFFER_BIT);
            glDrawElements(GL_TRIANGLE_STRIP, 3, GL_UNSIGNED_BYTE, indices);
            rc |= !check_pixels("OpenArena ARB multitexture draw");
        }
        glDeleteTextures(2, textures);
    } else if (rc == 0) puts("SKIP multitexture: no second texture unit");
    if (rc != 2) rc |= cva_rc;
    dlclose(lib);
    SDL_GL_DeleteContext(ctx);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return rc;
}
