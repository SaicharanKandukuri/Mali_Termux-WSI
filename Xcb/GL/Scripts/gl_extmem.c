/*
 * gl_extmem.c - GL_EXT_memory_object + GL_EXT_semaphore for OpenGL Core Profile (via AHardwareBuffer / DMABUF / EGL sync).
 * Include in gl_es_emul.c right before: #include "gl_es_alias.c"
 *
 * Import & Export:
 *     void glImportMemoryDmaBufEXT(GLuint mem, int fd, int pitch, GLuint64 mod);
 *     void glImportMemoryAHardwareBufferEXT(GLuint mem, void* ahb);
 *     void glImportMemoryFdEXT(GLuint mem, GLuint64 size, GLenum handleType, int fd);
 *     int  glExportMemoryFdEXT(GLuint mem);
 *     void glImportSemaphoreFdEXT(GLuint sem, GLenum handleType, int fd);  // handleType 0x9586, fd = sync_file
 *     int  glExportSemaphoreFdEXT(GLuint sem);
 */
#include <dlfcn.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>

typedef void*    (*X_cimg)(void*, void*, unsigned, void*, const int*);
typedef unsigned (*X_dimg)(void*, void*);
typedef void     (*X_tgt)(unsigned, void*);
typedef void*    (*X_csync)(void*, unsigned, const int*);
typedef int      (*X_wsync)(void*, void*, int);
typedef unsigned (*X_dsync)(void*, void*);
typedef void     (*X_flush)(void);
typedef void*    (*X_a2w)(void*);
typedef void     (*X_ahbop)(void*);
typedef void     (*X_bse)(unsigned, long, long, void*, unsigned);

#define XF(n, T) ((T)eglGetProcAddress(n))

static void* xm_nw(const char* s) {
    static void* h;
    if (!h) h = dlopen("libnativewindow.so", RTLD_NOW);
    if (!h) h = dlopen("/system/lib64/libnativewindow.so", RTLD_NOW);
    return h ? dlsym(h, s) : NULL;
}

/* AHardwareBuffer* -> ANativeWindowBuffer* (same object, no separate refcount) */
static void* xm_anwb(void* ahb) {
    static X_a2w fn; static int tried;
    if (!tried) {
        tried = 1;
        fn = (X_a2w)xm_nw("_ZN7android38AHardwareBuffer_to_ANativeWindowBufferEP15AHardwareBuffer");
        if (!fn) fn = (X_a2w)xm_nw("_ZN7android38AHardwareBuffer_to_ANativeWindowBufferEPK15AHardwareBuffer");
        if (gl_emul_debug_enabled()) fprintf(stderr, "[gl-extmem] AHB->ANWB: %s\n", fn ? "ok" : "NOT FOUND");
    }
    return fn ? fn(ahb) : NULL;
}

/* ---------- Memory objects ---------- */
#define XM_MAX 1024
typedef struct { 
    int used, dedicated, prot; 
    int fd; 
    int pitch; 
    unsigned long long size;
    unsigned long long mod; 
    void* ahb; 
    void* img; 
} xm_t;

static xm_t xm[XM_MAX];
static pthread_mutex_t xm_mu = PTHREAD_MUTEX_INITIALIZER;
static xm_t* xm_get(unsigned n) { return (n && n < XM_MAX && xm[n].used) ? &xm[n] : NULL; }

GLC void glCreateMemoryObjectsEXT(int n, unsigned* out) {
    pthread_mutex_lock(&xm_mu);
    for (int i = 0, k = 1; i < n; i++) {
        while (k < XM_MAX && xm[k].used) k++;
        if (k >= XM_MAX) { out[i] = 0; continue; }
        memset(&xm[k], 0, sizeof xm[k]); xm[k].fd = -1; xm[k].used = 1; out[i] = (unsigned)k;
    }
    pthread_mutex_unlock(&xm_mu);
}

GLC void glDeleteMemoryObjectsEXT(int n, const unsigned* ids) {
    pthread_mutex_lock(&xm_mu);
    for (int i = 0; i < n; i++) {
        xm_t* o = xm_get(ids[i]); if (!o) continue;
        if (o->img) XF("eglDestroyImageKHR", X_dimg)(eglGetCurrentDisplay(), o->img);
        if (o->fd >= 0) close(o->fd);
        if (o->ahb) {
            X_ahbop r = (X_ahbop)xm_nw("AHardwareBuffer_release");
            if (r) r(o->ahb);
        }
        memset(o, 0, sizeof *o);
    }
    pthread_mutex_unlock(&xm_mu);
}

GLC unsigned char glIsMemoryObjectEXT(unsigned m) { return xm_get(m) != NULL; }

GLC void glMemoryObjectParameterivEXT(unsigned m, unsigned pname, const int* p) {
    xm_t* o = xm_get(m); if (!o || o->fd >= 0 || o->ahb) return;      /* immutable after import */
    if (pname == 0x9581) o->dedicated = *p != 0;
    else if (pname == 0x959B) o->prot = *p != 0;
}

GLC void glGetMemoryObjectParameterivEXT(unsigned m, unsigned pname, int* p) {
    xm_t* o = xm_get(m); if (!o) return;
    if (pname == 0x9581) *p = o->dedicated;
    else if (pname == 0x959B) *p = o->prot;
}

/* mem = object, fd = dma-buf (duped), pitch = bytes/row, mod = DRM modifier (0 = LINEAR) */
GLC void glImportMemoryDmaBufEXT(unsigned m, int fd, int pitch, unsigned long long mod) {
    xm_t* o = xm_get(m); if (!o || o->fd >= 0 || o->ahb || fd < 0) return;
    o->fd = dup(fd); o->pitch = pitch; o->mod = mod;
}

GLC void glImportMemoryAHardwareBufferEXT(unsigned m, void* ahb) {
    xm_t* o = xm_get(m); if (!o || !ahb || o->ahb || o->fd >= 0) return;
    X_ahbop acq = (X_ahbop)xm_nw("AHardwareBuffer_acquire");
    if (acq) acq(ahb);
    o->ahb = ahb;
}

GLC void glImportMemoryFdEXT(unsigned m, unsigned long long size, unsigned handleType, int fd) {
    xm_t* o = xm_get(m);
    if (!o || handleType != 0x9586 || o->fd >= 0 || o->ahb || fd < 0) return;
    o->fd = fd;                              /* ownership transferred, no dup (as per specification) */
    o->size = size; o->pitch = 0;
    o->mod = 0x00ffffffffffffffULL;          /* DRM_FORMAT_MOD_INVALID: do not send modifier */
}

typedef struct { int version, numFds, numInts; int data[]; } xm_nh;
typedef const xm_nh* (*X_gnh)(const void*);

GLC int glExportMemoryFdEXT(unsigned m) {          /* returns a dup'd dma-buf fd, or -1 */
    xm_t* o = xm_get(m); if (!o) return -1;
    if (o->fd >= 0) return dup(o->fd);
    if (!o->ahb) return -1;
    X_gnh g = (X_gnh)xm_nw("AHardwareBuffer_getNativeHandle");
    const xm_nh* nh = g ? g(o->ahb) : NULL; if (!nh) return -1;
    int best = -1; off_t bs = 0;
    for (int i = 0; i < nh->numFds; i++) {
        off_t sz = lseek(nh->data[i], 0, SEEK_END); lseek(nh->data[i], 0, SEEK_SET);
        if (sz > bs) { bs = sz; best = nh->data[i]; }
    }
    return best >= 0 ? dup(best) : -1;
}

static unsigned xm_fourcc(unsigned f) {
    switch (f) {
    case 0x8058: case 0x8C43: return 0x34324241;  /* GL_RGBA8 / GL_SRGB8_ALPHA8 -> ABGR8888 */
    case 0x881A:              return 0x48344241;  /* GL_RGBA16F -> ABGR16161616F */
    case 0x8D62:              return 0x36314752;  /* GL_RGB565 */
    default: return 0;
    }
}

static void xm_tex(unsigned target, unsigned ifmt, int w, int h, unsigned mem, unsigned long long off) {
    xm_t* o = xm_get(mem);
    unsigned fc = xm_fourcc(ifmt);

    if (o && o->ahb) {
        void* cb = xm_anwb(o->ahb);
        static const int at[] = { 0x30D2 /*EGL_IMAGE_PRESERVED_KHR*/, 1, 0x3038 };
        void* dpy = eglGetCurrentDisplay();
        void* img = cb ? XF("eglCreateImageKHR", X_cimg)(dpy, NULL, 0x3140 /*EGL_NATIVE_BUFFER_ANDROID*/, cb, at) : NULL;
        if (!img) { if (gl_emul_debug_enabled()) fprintf(stderr, "[gl-extmem] native-buffer eglCreateImage failed\n"); return; }
        XF("glEGLImageTargetTexture2DOES", X_tgt)(target, img);
        XF("eglDestroyImageKHR", X_dimg)(dpy, img);
        return;
    }

    if (!o || o->fd < 0 || !fc) { if (gl_emul_debug_enabled()) fprintf(stderr, "[gl-extmem] bad mem/format 0x%x\n", ifmt); return; }
    
    int pitch = o->pitch;
    if (!pitch) {
        long long sz = o->size ? (long long)o->size : (long long)lseek(o->fd, 0, SEEK_END);
        if (sz > (long long)off && h > 0) pitch = (int)((sz - (long long)off) / h);
    }
    int at_dmabuf[24], k = 0;
    at_dmabuf[k++] = 0x3057; at_dmabuf[k++] = w;
    at_dmabuf[k++] = 0x3056; at_dmabuf[k++] = h;
    at_dmabuf[k++] = 0x3271; at_dmabuf[k++] = (int)fc;
    at_dmabuf[k++] = 0x3272; at_dmabuf[k++] = o->fd;
    at_dmabuf[k++] = 0x3273; at_dmabuf[k++] = (int)off;
    at_dmabuf[k++] = 0x3274; at_dmabuf[k++] = pitch;
    if (o->mod != 0x00ffffffffffffffULL) {
        at_dmabuf[k++] = 0x3443; at_dmabuf[k++] = (int)(o->mod & 0xffffffffu);
        at_dmabuf[k++] = 0x3444; at_dmabuf[k++] = (int)(o->mod >> 32);
    }
    at_dmabuf[k] = 0x3038;

    void* dpy = eglGetCurrentDisplay();
    void* img = XF("eglCreateImageKHR", X_cimg)(dpy, NULL, 0x3270 /*LINUX_DMA_BUF_EXT*/, NULL, at_dmabuf);
    if (!img) { if (gl_emul_debug_enabled()) fprintf(stderr, "[gl-extmem] dma-buf eglCreateImage failed\n"); return; }
    XF("glEGLImageTargetTexture2DOES", X_tgt)(target, img);
    XF("eglDestroyImageKHR", X_dimg)(dpy, img);   /* GL texture keeps the reference */
}

/* ---------- OpenGL Textures (Traditional & DSA) ---------- */
GLC void glTexStorageMem2DEXT(unsigned t, int lv, unsigned f, int w, int h, unsigned m, unsigned long long off) {
    (void)lv; xm_tex(t, f, w, h, m, off);
}

GLC void glTexStorageMem2DMultisampleEXT(unsigned t, int s, unsigned f, int w, int h, unsigned char fx, unsigned m, unsigned long long off) {
    (void)t; (void)s; (void)f; (void)w; (void)h; (void)fx; (void)m; (void)off;
    if (gl_emul_debug_enabled()) fprintf(stderr, "[gl-extmem] MS memory textures unsupported\n");
}

GLC void glTexStorageMem3DEXT(unsigned t, int lv, unsigned f, int w, int h, int d, unsigned m, unsigned long long off) {
    (void)t; (void)lv; (void)f; (void)w; (void)h; (void)d; (void)m; (void)off;
    if (gl_emul_debug_enabled()) fprintf(stderr, "[gl-extmem] 3D memory textures unsupported\n");
}

GLC void glTexStorageMem1DEXT(unsigned t, int lv, unsigned f, int w, unsigned m, unsigned long long off) {
    (void)lv; xm_tex(t1d(t), f, w, 1, m, off);
}

GLC void glTextureStorageMem1DEXT(unsigned tex, int lv, unsigned f, int w, unsigned m, unsigned long long off) {
    (void)lv;
    int old = 0; RP(glGetIntegerv, F_geti)(0x8069 /*GL_TEXTURE_BINDING_2D*/, &old);
    RP(glBindTexture, M_bt)(0x0DE1 /*GL_TEXTURE_2D*/, tex);
    xm_tex(0x0DE1, f, w, 1, m, off);
    RP(glBindTexture, M_bt)(0x0DE1, (unsigned)old);
}

GLC void glTextureStorageMem2DEXT(unsigned tex, int lv, unsigned f, int w, int h, unsigned m, unsigned long long off) {
    (void)lv;
    int old = 0; RP(glGetIntegerv, F_geti)(0x8069 /*GL_TEXTURE_BINDING_2D*/, &old);
    RP(glBindTexture, M_bt)(0x0DE1 /*GL_TEXTURE_2D*/, tex);
    xm_tex(0x0DE1, f, w, h, m, off);
    RP(glBindTexture, M_bt)(0x0DE1, (unsigned)old);
}

/* ---------- OpenGL Buffers (Traditional & DSA) ---------- */
GLC void glBufferStorageMemEXT(unsigned target, long size, unsigned mem, unsigned long long off) {
    xm_t* o = xm_get(mem);
    X_bse f = XF("glBufferStorageExternalEXT", X_bse);
    void* cb = (o && o->ahb) ? xm_anwb(o->ahb) : NULL;
    if (!cb || !f) { if (gl_emul_debug_enabled()) fprintf(stderr, "[gl-extmem] external buffer unavailable\n"); return; }
    f(target, (long)off, size, cb, 0);
}

GLC void glNamedBufferStorageMemEXT(unsigned buf, long size, unsigned mem, unsigned long long off) {
    int old = 0; RP(glGetIntegerv, F_geti)(0x8F36 /*GL_COPY_READ_BUFFER_BINDING*/, &old);
    RP(glBindBuffer, F_uiui)(0x8F36 /*GL_COPY_READ_BUFFER*/, buf);
    glBufferStorageMemEXT(0x8F36, size, mem, off);
    RP(glBindBuffer, F_uiui)(0x8F36, (unsigned)old);
}

/* ---------- Semaphores (GL Server Sync) ---------- */
#define XS_MAX 1024
typedef struct { int used; void* sync; int fd; } xs_t;
static xs_t xs[XS_MAX];
static pthread_mutex_t xs_mu = PTHREAD_MUTEX_INITIALIZER;
static xs_t* xs_get(unsigned n) { return (n && n < XS_MAX && xs[n].used) ? &xs[n] : NULL; }

GLC void glGenSemaphoresEXT(int n, unsigned* out) {
    pthread_mutex_lock(&xs_mu);
    for (int i = 0, k = 1; i < n; i++) {
        while (k < XS_MAX && xs[k].used) k++;
        if (k >= XS_MAX) { out[i] = 0; continue; }
        memset(&xs[k], 0, sizeof xs[k]); xs[k].fd = -1; xs[k].used = 1; out[i] = (unsigned)k;
    }
    pthread_mutex_unlock(&xs_mu);
}

GLC void glDeleteSemaphoresEXT(int n, const unsigned* ids) {
    pthread_mutex_lock(&xs_mu);
    for (int i = 0; i < n; i++) {
        xs_t* s = xs_get(ids[i]); if (!s) continue;
        if (s->sync) XF("eglDestroySyncKHR", X_dsync)(eglGetCurrentDisplay(), s->sync);
        if (s->fd >= 0) close(s->fd);
        memset(s, 0, sizeof *s);
    }
    pthread_mutex_unlock(&xs_mu);
}

GLC unsigned char glIsSemaphoreEXT(unsigned s) { return xs_get(s) != NULL; }
GLC void glSemaphoreParameterui64vEXT(unsigned s, unsigned p, const unsigned long long* v) { (void)s; (void)p; (void)v; }
GLC void glGetSemaphoreParameterui64vEXT(unsigned s, unsigned p, unsigned long long* v) { (void)s; (void)p; if (v) *v = 0; }

GLC void glImportSemaphoreFdEXT(unsigned sem, unsigned handleType, int fd) {
    xs_t* s = xs_get(sem); if (!s || handleType != 0x9586) return;
    if (s->fd >= 0) close(s->fd);
    s->fd = fd;
}

GLC int glExportSemaphoreFdEXT(unsigned sem) {      /* sync_file for commands issued up to this point */
    if (!xs_get(sem)) return -1;
    void* dpy = eglGetCurrentDisplay();
    X_csync cs = XF("eglCreateSyncKHR", X_csync);
    typedef int (*X_dupfd)(void*, void*);
    X_dupfd df = XF("eglDupNativeFenceFDANDROID", X_dupfd);
    X_dsync ds = XF("eglDestroySyncKHR", X_dsync);
    if (!cs || !df || !ds) return -1;
    const int at[] = { 0x3145 /*NATIVE_FENCE_FD_ANDROID*/, -1, 0x3038 };
    void* sy = cs(dpy, 0x3144, at);
    if (!sy) return -1;
    RP(glFlush, X_flush)();
    int fd = df(dpy, sy);                          /* -1 = already signaled */
    ds(dpy, sy);
    return fd;
}

GLC void glSignalSemaphoreEXT(unsigned sem, unsigned nb, const unsigned* bufs,
                              unsigned nt, const unsigned* texs, const unsigned* dstLayouts) {
    (void)nb; (void)bufs; (void)nt; (void)texs; (void)dstLayouts;
    xs_t* s = xs_get(sem); if (!s) return;
    void* dpy = eglGetCurrentDisplay();
    X_csync cs = XF("eglCreateSyncKHR", X_csync);
    if (!cs) { RP(glFlush, X_flush)(); return; }
    if (s->sync) XF("eglDestroySyncKHR", X_dsync)(dpy, s->sync);

    const int at[] = { 0x30F8 /*EGL_SYNC_CONDITION_KHR*/, 0x328A /*IMPLICIT_EXTERNAL_ARM*/, 0x3038 };
    const char* how = "implicit-external (0x328A)";
    s->sync = cs(dpy, 0x30F9 /*EGL_SYNC_FENCE_KHR*/, at);
    if (!s->sync) { how = "plain fence"; s->sync = cs(dpy, 0x30F9, NULL); }
    if (!s->sync) how = "NO sync";
    XF("eglGetError", unsigned (*)(void))();
    if (gl_emul_debug_enabled()) fprintf(stderr, "[extmem] signal: %s\n", how);
    RP(glFlush, X_flush)();
}

GLC void glWaitSemaphoreEXT(unsigned sem, unsigned nb, const unsigned* bufs,
                            unsigned nt, const unsigned* texs, const unsigned* srcLayouts) {
    (void)nb; (void)bufs; (void)nt; (void)texs; (void)srcLayouts;
    xs_t* s = xs_get(sem); if (!s) return;
    void* dpy = eglGetCurrentDisplay();
    X_csync cs = XF("eglCreateSyncKHR", X_csync);
    X_wsync ws = XF("eglWaitSyncKHR", X_wsync);
    X_dsync ds = XF("eglDestroySyncKHR", X_dsync);
    if (!ws || !ds) return;
    void* sy = s->sync; s->sync = NULL;
    if (!sy && s->fd >= 0 && cs) {
        const int at[] = { 0x3145 /*NATIVE_FENCE_FD_ANDROID*/, s->fd, 0x3038 };
        sy = cs(dpy, 0x3144 /*EGL_SYNC_NATIVE_FENCE_ANDROID*/, at);
        if (sy) s->fd = -1;
    }
    if (gl_emul_debug_enabled())
        fprintf(stderr, "[extmem] wait: %s\n",
                sy ? (s->fd < 0 && !s->sync ? "stored/native fence" : "stored fence") : "no sync (implicit dma-buf only)");
    if (sy) { ws(dpy, sy, 0); ds(dpy, sy); }
}

/* ---------- Device / driver UUID ---------- */
typedef unsigned (*X_qda)(void*, int, intptr_t*);
typedef unsigned (*X_qdb)(void*, int, int, void*, int*);

static void xm_uuid(unsigned char* d, int drv) {
    static unsigned char cache[2][16]; static int have[2];
    int k = drv ? 1 : 0;
    if (!have[k]) {
        memset(cache[k], 0, 16);
        const char* e = getenv(drv ? "GLX_DRIVER_UUID" : "GLX_DEVICE_UUID");   /* manual override */
        if (e && strlen(e) >= 32) {
            for (int i = 0; i < 16; i++) { unsigned v = 0; sscanf(e + 2 * i, "%2x", &v); cache[k][i] = (unsigned char)v; }
            have[k] = 1;
        } else {
            void* dpy = eglGetCurrentDisplay();
            X_qda qa = XF("eglQueryDisplayAttribEXT", X_qda);
            X_qdb qb = XF("eglQueryDeviceBinaryEXT", X_qdb);
            intptr_t dev = 0; int n = 0;
            if (dpy && qa && qb && qa(dpy, 0x322C /*EGL_DEVICE_EXT*/, &dev) && dev) {
                if (!qb((void*)dev, drv ? 0x335D : 0x335C /*DRIVER/DEVICE_UUID_EXT*/, 16, cache[k], &n) || n != 16)
                    memset(cache[k], 0, 16);
                have[k] = 1;
            }
        }
    }
    memcpy(d, cache[k], 16);
}

GLC void glGetUnsignedBytevEXT(unsigned pname, unsigned char* d) {
    if (pname == 0x9598) xm_uuid(d, 1); else if (pname == 0x9597) xm_uuid(d, 0);
}

GLC void glGetUnsignedBytei_vEXT(unsigned target, unsigned idx, unsigned char* d) {
    if (target == 0x9597 && idx == 0) xm_uuid(d, 0);
}

/* ---------- Tiling query ---------- */
typedef void (*X_giv)(unsigned, unsigned, unsigned, int, int*);
GLC void glGetInternalformativ(unsigned target, unsigned ifmt, unsigned pname, int n, int* p) {
    if (pname == 0x9582) { *p = 1; return; }                      /* GL_NUM_TILING_TYPES_EXT */
    if (pname == 0x9583) { if (n > 0) p[0] = 0x9584; return; }    /* GL_OPTIMAL_TILING_EXT */
    RP(glGetInternalformativ, X_giv)(target, ifmt, pname, n, p);
}

GLC void glTexStorageMem3DMultisampleEXT(unsigned t, int s, unsigned f, int w, int h, int d,
                                         unsigned char fx, unsigned m, unsigned long long off) {
    (void)t; (void)s; (void)f; (void)w; (void)h; (void)d; (void)fx; (void)m; (void)off;
    if (gl_emul_debug_enabled()) fprintf(stderr, "[extmem] 3D MS memory textures unsupported\n");
}
GLC void glTextureStorageMem2DMultisampleEXT(unsigned t, int s, unsigned f, int w, int h,
                                             unsigned char fx, unsigned m, unsigned long long off) {
    (void)t; (void)s; (void)f; (void)w; (void)h; (void)fx; (void)m; (void)off;
    if (gl_emul_debug_enabled()) fprintf(stderr, "[extmem] MS memory textures unsupported\n");
}
GLC void glTextureStorageMem3DEXT(unsigned t, int lv, unsigned f, int w, int h, int d,
                                  unsigned m, unsigned long long off) {
    (void)t; (void)lv; (void)f; (void)w; (void)h; (void)d; (void)m; (void)off;
    if (gl_emul_debug_enabled()) fprintf(stderr, "[extmem] 3D memory textures unsupported\n");
}
GLC void glTextureStorageMem3DMultisampleEXT(unsigned t, int s, unsigned f, int w, int h, int d,
                                             unsigned char fx, unsigned m, unsigned long long off) {
    (void)t; (void)s; (void)f; (void)w; (void)h; (void)d; (void)fx; (void)m; (void)off;
    if (gl_emul_debug_enabled()) fprintf(stderr, "[extmem] 3D MS memory textures unsupported\n");
}