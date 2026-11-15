/* mali_ahb.h : single shared AHardwareBuffer layer for vulkan / gbm / drm shims.
 * Everything is resolved with dlsym (works on bionic and on libhybris via MALI_DLOPEN/MALI_DLSYM).
 * Env: HYBRIS_AHB_LIBRARY=<path>   MALI_AHB_FD_INDEX=<n> (force native-handle fd index) */
#ifndef MALI_AHB_H
#define MALI_AHB_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <android/hardware_buffer.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifndef MALI_DLOPEN
#define MALI_DLOPEN dlopen
#endif
#ifndef MALI_DLSYM
#define MALI_DLSYM dlsym
#endif
#define MALI_AHB_FN static inline __attribute__((unused))

/* native_handle_t layout (not exposed by the NDK) */
typedef struct { int version, numFds, numInts; int data[]; } mali_nh_t;

typedef struct {
    int   (*allocate)(const AHardwareBuffer_Desc*, AHardwareBuffer**);
    void  (*acquire)(AHardwareBuffer*);
    void  (*release)(AHardwareBuffer*);
    void  (*describe)(const AHardwareBuffer*, AHardwareBuffer_Desc*);
    int   (*lock)(AHardwareBuffer*, uint64_t, int32_t, const void*, void**);
    int   (*unlock)(AHardwareBuffer*, int32_t*);
    const mali_nh_t* (*native_handle)(const AHardwareBuffer*);
    void* (*to_anwb)(AHardwareBuffer*);                 /* private symbol, may be NULL */
    int   (*create_from_handle)(const AHardwareBuffer_Desc*, const mali_nh_t*, int32_t,
                                AHardwareBuffer**);     /* may be NULL */
} mali_ahb_t;

static mali_ahb_t mali_ahb;
static int mali_ahb_state;                 /* 0 untried, 1 ok, -1 failed */
static pthread_mutex_t mali_ahb_mu = PTHREAD_MUTEX_INITIALIZER;

/* Returns 0 when the mandatory functions are available. Thread-safe, idempotent. */
MALI_AHB_FN int mali_ahb_init(void) {
    pthread_mutex_lock(&mali_ahb_mu);
    if (!mali_ahb_state) {
        const char* p = getenv("HYBRIS_AHB_LIBRARY");
        void* l = MALI_DLOPEN(p && *p ? p : "libnativewindow.so", RTLD_NOW);
        if (!l) l = MALI_DLOPEN("/system/lib64/libnativewindow.so", RTLD_NOW);
        if (l) {
            mali_ahb.allocate      = MALI_DLSYM(l, "AHardwareBuffer_allocate");
            mali_ahb.acquire       = MALI_DLSYM(l, "AHardwareBuffer_acquire");
            mali_ahb.release       = MALI_DLSYM(l, "AHardwareBuffer_release");
            mali_ahb.describe      = MALI_DLSYM(l, "AHardwareBuffer_describe");
            mali_ahb.lock          = MALI_DLSYM(l, "AHardwareBuffer_lock");
            mali_ahb.unlock        = MALI_DLSYM(l, "AHardwareBuffer_unlock");
            mali_ahb.native_handle = MALI_DLSYM(l, "AHardwareBuffer_getNativeHandle");
            mali_ahb.create_from_handle = MALI_DLSYM(l, "AHardwareBuffer_createFromHandle");
            static const char* m =
                "_ZN7android38AHardwareBuffer_to_ANativeWindowBufferEP15AHardwareBuffer";
            mali_ahb.to_anwb = dlvsym(l, m, "LIBNATIVEWINDOW_PLATFORM");
            if (!mali_ahb.to_anwb) mali_ahb.to_anwb = MALI_DLSYM(l, m);
        }
        mali_ahb_state = (mali_ahb.allocate && mali_ahb.acquire && mali_ahb.release &&
                          mali_ahb.describe && mali_ahb.lock && mali_ahb.unlock &&
                          mali_ahb.native_handle) ? 1 : -1;
        if (mali_ahb_state < 0)
            fprintf(stderr, "mali_ahb: libnativewindow symbols missing\n");
    }
    int r = mali_ahb_state == 1 ? 0 : -1;
    pthread_mutex_unlock(&mali_ahb_mu);
    return r;
}

/* dup() that bypasses any LD_PRELOAD hook on dup/fcntl (libdrm shim tracks them) */
MALI_AHB_FN int mali_dup_raw(int fd) {
    return (int)syscall(SYS_fcntl, fd, F_DUPFD_CLOEXEC, 0);
}

MALI_AHB_FN int mali_ahb_alloc(uint32_t w, uint32_t h, uint32_t fmt, uint64_t usage,
                               AHardwareBuffer** out) {
    if (mali_ahb_init()) return -ENODEV;
    AHardwareBuffer_Desc d = { .width = w, .height = h, .layers = 1, .format = fmt, .usage = usage };
    *out = NULL;
    int r = mali_ahb.allocate(&d, out);
    return (r || !*out) ? (r ? r : -ENOMEM) : 0;
}

/* dma-buf fd of the pixel data: gralloc puts metadata at data[0], pixels at data[1].
 * Returns a NEW fd (caller closes) or -1. */
MALI_AHB_FN int mali_ahb_dmabuf_fd(AHardwareBuffer* ahb) {
    if (mali_ahb_init() || !ahb) return -1;
    const mali_nh_t* h = mali_ahb.native_handle(ahb);
    if (!h || h->numFds < 1) return -1;
    int idx = h->numFds > 1 ? 1 : 0;
    const char* e = getenv("MALI_AHB_FD_INDEX");
    if (e && *e) idx = atoi(e);
    if (idx < 0 || idx >= h->numFds) return -1;
    return mali_dup_raw(h->data[idx]);
}

/* size of the pixel fd (for DRI3 PixmapFromBuffer); -1 on error */
MALI_AHB_FN long mali_ahb_dmabuf_size(AHardwareBuffer* ahb) {
    int fd = mali_ahb_dmabuf_fd(ahb);
    if (fd < 0) return -1;
    off_t n = lseek(fd, 0, SEEK_END);
    close(fd);
    return (long)n;
}

MALI_AHB_FN int mali_ahb_lock_rw(AHardwareBuffer* ahb, void** out) {
    if (mali_ahb_init()) return -ENODEV;
    return mali_ahb.lock(ahb, AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN |
                              AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN, -1, NULL, out);
}

MALI_AHB_FN void* mali_ahb_anwb(AHardwareBuffer* ahb) {
    if (mali_ahb_init() || !mali_ahb.to_anwb) return NULL;
    return mali_ahb.to_anwb(ahb);
}

#endif
