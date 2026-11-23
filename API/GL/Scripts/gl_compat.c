/*
 * gl_compat.c - subset of desktop OpenGL 1.x over GLES 1.1 (included by glx_shim.c).
 *
 *   glBegin/glEnd/glVertex*  -> client-side vertex arrays (QUADS -> triangles, QUAD_STRIP -> TRIANGLE_STRIP,
 *                               POLYGON -> TRIANGLE_FAN)
 *   glArrayElement           -> one indexed ES draw for array-only blocks; client-memory decode for mixed blocks
 *   glNewList/glCallList     -> recorded and replayed commands (Begin..End blocks bake into vertex arrays)
 *   glMaterialfv(GL_FRONT)   -> GL_FRONT_AND_BACK (the only one accepted by ES 1.1)
 *   glFrustum/glOrtho/...d   -> float variants from Mali
 *
 * Works only in a GLES 1.x context (GLX_ES=1). ES2/3 lacks fixed-functionality.
 * Recorded in lists: Begin/End, Normal, Color, TexCoord, Material, Light, ShadeModel, FrontFace,
 * Enable/Disable, Push/PopMatrix, Translate/Rotate/Scale, MultMatrix, LoadIdentity, BindTexture, CallList.
 * Any other call made between glNewList and glEndList executes immediately (unrecorded).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <limits.h>

#define GLC __attribute__((visibility("default")))
#define RP(n, T) ((T)(p_##n ? p_##n : gl_rp_missing(#n)))

/* Convert TEXTURE_1D (0x0DE0) -> TEXTURE_2D (0x0DE1) */
#ifndef T1D_DEFINED
#define T1D_DEFINED
static inline unsigned t1d(unsigned t) { 
    return (t == 0x0DE0 || t == 0x8C18 || t == 0x84F5) ? 0x0DE1 : t; 
}
#endif

typedef void (*F_v)(void);
typedef void (*F_ui)(unsigned);
typedef void (*F_uiui)(unsigned, unsigned);
typedef void (*F_fff)(float, float, float);
typedef void (*F_ffff)(float, float, float, float);
typedef void (*F_utex)(unsigned, float, float, float, float);
typedef void (*F_mat)(unsigned, unsigned, const float*);
typedef void (*F_pf)(const float*);
typedef unsigned char (*F_isen)(unsigned);
typedef void (*F_vptr)(int, unsigned, int, const void*);
typedef void (*F_nptr)(unsigned, int, const void*);
typedef void (*F_da)(unsigned, int, int);
typedef void (*F_de)(unsigned, int, unsigned, const void*);
typedef void (*F_6f)(float, float, float, float, float, float);
typedef void (*F_f)(float);
typedef void (*F_ff)(float, float);
typedef void (*F_uif)(unsigned, float);
typedef void (*F_clip)(unsigned, const float*);
typedef void (*F_getf)(unsigned, float*);
typedef void (*F_geti)(unsigned, int*);
typedef const unsigned char* (*F_gstri)(unsigned, unsigned);
typedef void (*F_gentex)(int, unsigned*);
typedef void (*F_teximg)(unsigned, int, int, int, int, int, unsigned, unsigned, const void*);   /* and TexSubImage2D */
typedef void (*F_texenv)(unsigned, unsigned, float);
typedef void (*F_texgeti)(unsigned, unsigned, int*);
typedef void (*F_getp)(unsigned, void**);

static void cc_note_front(unsigned m);
static void cc_setfront(void);

enum { OP_DRAW = 1, OP_NORMAL, OP_COLOR, OP_TEXCOORD, OP_MATERIAL, OP_SHADE, OP_FRONTFACE, OP_ENABLE,
       OP_DISABLE, OP_PUSH, OP_POP, OP_TRANSLATE, OP_ROTATE, OP_SCALE, OP_MULT, OP_IDENT, OP_LIGHT,
       OP_BINDTEX, OP_CALL, OP_WINDOWPOS, OP_RASTERPOS };

typedef struct { int op; unsigned u[2]; float f[16]; void* p; } gl_cmd;
typedef struct { int mode, n, preserve_arrays; float *pos, *nrm, *col, *tex; } gl_block;
typedef struct { gl_cmd* c; int n, cap, used; } gl_list;
typedef struct {
    int enabled, size, type, stride, buffer;
    const unsigned char* ptr;
} im_array;
typedef struct {
    im_array vertex, normal, color, tex;
    int array_buffer, element_buffer, client_active, mixed_multitex;
} im_arrays;

static gl_list* gl_lists;
static unsigned gl_lists_cap, gl_next_list = 1;
static pthread_mutex_t gl_lmu = PTHREAD_MUTEX_INITIALIZER;

static __thread struct {
    int init, in_begin, mode, n, cap, has_n, has_c, has_t, decode_elements, used_elements, elements_failed;
    float *pos, *nrm, *col, *tex;
    float cn[3], cc[4], ct[2];
    int *elements, elements_n, elements_cap;
    unsigned rec_id;
    int rec_mode;                      /* 0x1300 GL_COMPILE, 0x1301 GL_COMPILE_AND_EXECUTE */
} IM;

enum {
    CVA_LOCK_FIRST = 0x81A8,
    CVA_LOCK_COUNT = 0x81A9,
    CVA_INVALID_VALUE_BIT = 1,
    CVA_INVALID_OPERATION_BIT = 2
};

static glx_ctx* cva_context(void) {
    glx_ctx* c = T_glx.ctx;
    return c && c->ver == 1 && !c->es_native ? c : NULL;
}

static void cva_set_error(unsigned error) {
    glx_ctx* c = cva_context();
    if (!c) return;
    if (error == 0x0501) c->cva_errors |= CVA_INVALID_VALUE_BIT;
    else if (error == 0x0502) c->cva_errors |= CVA_INVALID_OPERATION_BIT;
}

static int cva_get(unsigned pname, int* value) {
    glx_ctx* c = cva_context();
    if (!c || (pname != CVA_LOCK_FIRST && pname != CVA_LOCK_COUNT)) return 0;
    if (IM.in_begin) { cva_set_error(0x0502); return 1; }
    *value = pname == CVA_LOCK_FIRST ? c->cva_first : c->cva_count;
    return 1;
}

/* raster position (window coordinates) + zoom, per thread/context */
static __thread float g_rasterx, g_rastery, g_zoomx = 1.0f, g_zoomy = 1.0f;
static __thread int   g_rvalid = 1;

/* persistent texture for glDrawPixels */
static __thread unsigned g_dp_tex, g_dp_pw, g_dp_ph, g_dp_ifmt, g_dp_type;

static inline int gl_compat_debug_enabled(void) {
    const char* env_gl = getenv("GL_DEBUG");
    const char* env_mali = getenv("MALI_DEBUG");
    const char* env_egl = getenv("EGL_DEBUG");
    return (env_gl && env_gl[0] != '0') || 
           (env_mali && env_mali[0] != '0') || 
           (env_egl && env_egl[0] != '0');
}

static void im_init(void) {
    if (IM.init) return;
    IM.init = 1;
    IM.cn[0] = 0.0f; IM.cn[1] = 0.0f; IM.cn[2] = 1.0f;
    IM.cc[0] = IM.cc[1] = IM.cc[2] = IM.cc[3] = 1.0f;
    IM.ct[0] = IM.ct[1] = 0.0f;
}

static void gl_warn_es(const char* fn) {
    static int warned;
    if (!warned && T_glx.ctx && T_glx.ctx->ver != 1) {
        warned = 1;
        fprintf(stderr, "libGL.so: warning: %s called in an ES%d context: fixed-function pipeline only exists in ES1. "
                        "Run with GLX_ES=1\n", fn, T_glx.ctx->ver);
    }
}

GLC void glWindowPos3f(float x, float y, float z);
GLC void glRasterPos4f(float x, float y, float z, float w);

/* ---------- Display Lists ---------- */
static void lists_ensure(unsigned n) {
    if (n <= gl_lists_cap) return;
    unsigned nc = gl_lists_cap ? gl_lists_cap : 64;
    while (nc < n) nc *= 2;
    gl_lists = realloc(gl_lists, nc * sizeof *gl_lists);
    memset(gl_lists + gl_lists_cap, 0, (nc - gl_lists_cap) * sizeof *gl_lists);
    gl_lists_cap = nc;
}

static void list_clear(gl_list* L) {
    for (int i = 0; i < L->n; i++)
        if (L->c[i].op == OP_DRAW && L->c[i].p) {
            gl_block* b = L->c[i].p;
            free(b->pos); free(b->nrm); free(b->col); free(b->tex); free(b);
        }
    free(L->c);
    memset(L, 0, sizeof *L);
}

static gl_cmd* rec_cmd(int op) {
    gl_list* L = &gl_lists[IM.rec_id];
    if (L->n == L->cap) {
        L->cap = L->cap ? L->cap * 2 : 64;
        L->c = realloc(L->c, (size_t)L->cap * sizeof(gl_cmd));
    }
    gl_cmd* c = &L->c[L->n++];
    memset(c, 0, sizeof *c);
    c->op = op;
    return c;
}
#define REC_SKIP() (IM.rec_mode == 0x1300)      /* GL_COMPILE: do not execute */

/* ---------- Drawing (Immediate Mode -> Arrays) ---------- */
static void im_get_array(im_array* a, unsigned cap, int size, unsigned size_pname,
                         unsigned type_pname, unsigned stride_pname,
                         unsigned pointer_pname, unsigned buffer_pname) {
    memset(a, 0, sizeof *a);
    a->enabled = RP(glIsEnabled, F_isen)(cap);
    a->size = size;
    if (size_pname) RP(glGetIntegerv, F_geti)(size_pname, &a->size);
    RP(glGetIntegerv, F_geti)(type_pname, &a->type);
    RP(glGetIntegerv, F_geti)(stride_pname, &a->stride);
    RP(glGetIntegerv, F_geti)(buffer_pname, &a->buffer);
    RP(glGetPointerv, F_getp)(pointer_pname, (void**)&a->ptr);
}

static void im_get_arrays(im_arrays* a) {
    memset(a, 0, sizeof *a);
    RP(glGetIntegerv, F_geti)(0x8894, &a->array_buffer);         /* ARRAY_BUFFER_BINDING */
    RP(glGetIntegerv, F_geti)(0x8895, &a->element_buffer);       /* ELEMENT_ARRAY_BUFFER_BINDING */
    RP(glGetIntegerv, F_geti)(0x84E1, &a->client_active);        /* CLIENT_ACTIVE_TEXTURE */
    im_get_array(&a->vertex, 0x8074, 4, 0x807A, 0x807B, 0x807C, 0x808E, 0x8896);
    im_get_array(&a->normal, 0x8075, 3, 0,      0x807E, 0x807F, 0x808F, 0x8897);
    im_get_array(&a->color,  0x8076, 4, 0x8081, 0x8082, 0x8083, 0x8090, 0x8898);
    if (a->client_active != 0x84C0) RP(glClientActiveTexture, F_ui)(0x84C0);
    im_get_array(&a->tex,    0x8078, 4, 0x8088, 0x8089, 0x808A, 0x8092, 0x889A);
    if (a->client_active != 0x84C0) RP(glClientActiveTexture, F_ui)((unsigned)a->client_active);
}

static void im_get_mixed_arrays(im_arrays* a) {
    /* shortcut: mixed blocks support texture unit 0 only, compact per-unit arrays before accepting multitexture */
    im_get_arrays(a);
    int units = 1;
    RP(glGetIntegerv, F_geti)(0x84E2, &units);                    /* MAX_TEXTURE_UNITS */
    for (int i = 1; i < units; i++) {
        RP(glClientActiveTexture, F_ui)(0x84C0u + (unsigned)i);
        if (RP(glIsEnabled, F_isen)(0x8078)) a->mixed_multitex = 1;
    }
    if (units > 1) RP(glClientActiveTexture, F_ui)((unsigned)a->client_active);
}

static void im_restore_pointer(const im_array* a, int kind) {
    RP(glBindBuffer, F_uiui)(0x8892, (unsigned)a->buffer);
    if (kind == 1) RP(glNormalPointer, F_nptr)((unsigned)a->type, a->stride, a->ptr);
    else if (kind == 2) RP(glColorPointer, F_vptr)(a->size, (unsigned)a->type, a->stride, a->ptr);
    else if (kind == 3) RP(glTexCoordPointer, F_vptr)(a->size, (unsigned)a->type, a->stride, a->ptr);
    else RP(glVertexPointer, F_vptr)(a->size, (unsigned)a->type, a->stride, a->ptr);
}

static void im_draw(int mode, int n, const float* pos, const float* nrm, const float* col, const float* tex,
                    int preserve_arrays) {
    unsigned gm = 0;
    unsigned short* ix = NULL;
    int cnt = n;
    switch (mode) {
    case 0: case 1: case 2: case 3: case 4: case 5: case 6: gm = (unsigned)mode; break;
    case 7: {                                                   /* GL_QUADS -> 2 triangles / quad */
        if (n > 65536) n = 65536;
        int q = n / 4;
        if (q <= 0) return;
        ix = malloc((size_t)q * 6 * sizeof *ix);
        if (!ix) return;
        for (int i = 0; i < q; i++) {
            unsigned short b = (unsigned short)(i * 4);
            ix[i * 6 + 0] = b;     ix[i * 6 + 1] = b + 1; ix[i * 6 + 2] = b + 2;
            ix[i * 6 + 3] = b;     ix[i * 6 + 4] = b + 2; ix[i * 6 + 5] = b + 3;
        }
        cnt = q * 6; gm = 4;
        break; }
    case 8: gm = 5; break;                                      /* GL_QUAD_STRIP -> TRIANGLE_STRIP */
    case 9: gm = 6; break;                                      /* GL_POLYGON -> TRIANGLE_FAN */
    default: return;
    }
    static const unsigned caps[4] = { 0x8074, 0x8075, 0x8076, 0x8078 };
    int want[4] = { 1, nrm != NULL, col != NULL, tex != NULL };
    im_arrays saved;
    int prev[4];
    if (preserve_arrays) {
        im_get_arrays(&saved);
        prev[0] = saved.vertex.enabled; prev[1] = saved.normal.enabled;
        prev[2] = saved.color.enabled; prev[3] = saved.tex.enabled;
        if (saved.client_active != 0x84C0) RP(glClientActiveTexture, F_ui)(0x84C0);
        RP(glBindBuffer, F_uiui)(0x8892, 0);                     /* immediate arrays are host pointers */
    } else {
        for (int i = 0; i < 4; i++) prev[i] = RP(glIsEnabled, F_isen)(caps[i]);
    }
    for (int i = 0; i < 4; i++)
        if (want[i] != prev[i])
            (want[i] ? RP(glEnableClientState, F_ui) : RP(glDisableClientState, F_ui))(caps[i]);
    RP(glVertexPointer, F_vptr)(3, 0x1406, 0, pos);
    if (nrm) RP(glNormalPointer, F_nptr)(0x1406, 0, nrm);
    if (col) RP(glColorPointer, F_vptr)(4, 0x1406, 0, col);
    if (tex) RP(glTexCoordPointer, F_vptr)(2, 0x1406, 0, tex);
    if (ix && preserve_arrays && saved.element_buffer) RP(glBindBuffer, F_uiui)(0x8893, 0);
    if (ix) RP(glDrawElements, F_de)(gm, cnt, 0x1403, ix);
    else    RP(glDrawArrays, F_da)(gm, 0, cnt);
    if (ix && preserve_arrays && saved.element_buffer)
        RP(glBindBuffer, F_uiui)(0x8893, (unsigned)saved.element_buffer);
    if (preserve_arrays) {
        im_restore_pointer(&saved.vertex, 0);
        im_restore_pointer(&saved.normal, 1);
        im_restore_pointer(&saved.color, 2);
        im_restore_pointer(&saved.tex, 3);
    }
    for (int i = 0; i < 4; i++)
        if (want[i] != prev[i])
            (want[i] ? RP(glDisableClientState, F_ui) : RP(glEnableClientState, F_ui))(caps[i]);
    if (preserve_arrays) {
        RP(glBindBuffer, F_uiui)(0x8892, (unsigned)saved.array_buffer);
        if (saved.client_active != 0x84C0) RP(glClientActiveTexture, F_ui)((unsigned)saved.client_active);
    }
    free(ix);
    /* After block, "current" values are those of the last vertex */
    int l = n - 1;
    if (nrm) RP(glNormal3f, F_fff)(nrm[3 * l], nrm[3 * l + 1], nrm[3 * l + 2]);
    if (col) RP(glColor4f, F_ffff)(col[4 * l], col[4 * l + 1], col[4 * l + 2], col[4 * l + 3]);
    if (tex) RP(glMultiTexCoord4f, F_utex)(0x84C0, tex[2 * l], tex[2 * l + 1], 0.0f, 1.0f);
}

static float* fdup(const float* s, size_t n) {
    float* d = malloc(n * sizeof(float));
    if (d) memcpy(d, s, n * sizeof(float));
    return d;
}

static void im_emit_vertex(float x, float y, float z) {
    if (!IM.in_begin) return;
    if (IM.n == IM.cap) {
        IM.cap = IM.cap ? IM.cap * 2 : 256;
        IM.pos = realloc(IM.pos, (size_t)IM.cap * 3 * sizeof(float));
        IM.nrm = realloc(IM.nrm, (size_t)IM.cap * 3 * sizeof(float));
        IM.col = realloc(IM.col, (size_t)IM.cap * 4 * sizeof(float));
        IM.tex = realloc(IM.tex, (size_t)IM.cap * 2 * sizeof(float));
    }
    float* p = IM.pos + 3 * IM.n; p[0] = x; p[1] = y; p[2] = z;
    memcpy(IM.nrm + 3 * IM.n, IM.cn, 3 * sizeof(float));
    memcpy(IM.col + 4 * IM.n, IM.cc, 4 * sizeof(float));
    memcpy(IM.tex + 2 * IM.n, IM.ct, 2 * sizeof(float));
    IM.n++;
}

static int im_type_size(unsigned type) {
    switch (type) {
    case 0x1400: case 0x1401: return 1;                            /* BYTE / UNSIGNED_BYTE */
    case 0x1402: case 0x1403: return 2;                            /* SHORT / UNSIGNED_SHORT */
    case 0x1404: case 0x1405: case 0x1406: return 4;               /* INT / UNSIGNED_INT / FLOAT */
    case 0x140A: return 8;                                         /* DOUBLE */
    default: return 0;
    }
}

static float im_component(const unsigned char* p, unsigned type, int normalized) {
    signed char b; unsigned char ub; short s; unsigned short us;
    int i; unsigned ui; float f; double d;
    switch (type) {
    case 0x1400: memcpy(&b, p, 1);  return normalized ? (b == SCHAR_MIN ? -1.0f : b / 127.0f) : (float)b;
    case 0x1401: memcpy(&ub, p, 1); return normalized ? ub / 255.0f : (float)ub;
    case 0x1402: memcpy(&s, p, 2);  return normalized ? (s == SHRT_MIN ? -1.0f : s / 32767.0f) : (float)s;
    case 0x1403: memcpy(&us, p, 2); return normalized ? us / 65535.0f : (float)us;
    case 0x1404: memcpy(&i, p, 4);  return normalized ? (i == INT_MIN ? -1.0f : i / 2147483647.0f) : (float)i;
    case 0x1405: memcpy(&ui, p, 4); return normalized ? ui / 4294967295.0f : (float)ui;
    case 0x1406: memcpy(&f, p, 4);  return f;
    case 0x140A: memcpy(&d, p, 8);  return (float)d;
    default: return 0.0f;
    }
}

static int im_read(const im_array* a, int index, float* out, int normalized) {
    int bytes = im_type_size((unsigned)a->type);
    if (!a->enabled) return 1;
    if (a->buffer || !a->ptr || bytes <= 0 || a->size < 1 || a->size > 4 || a->stride < 0) return 0;
    size_t stride = a->stride ? (size_t)a->stride : (size_t)a->size * (size_t)bytes;
    if ((size_t)index > SIZE_MAX / stride) return 0;
    const unsigned char* p = a->ptr + (size_t)index * stride;
    for (int c = 0; c < a->size; c++) out[c] = im_component(p + (size_t)c * bytes, (unsigned)a->type, normalized);
    return 1;
}

static int im_decode_element(const im_arrays* a, int index, int emit) {
    float v[4] = { 0, 0, 0, 1 }, x[4];
    if (index < 0 || (a->normal.enabled && !im_read(&a->normal, index, x, 1))) return 0;
    if (a->normal.enabled) { memcpy(IM.cn, x, 3 * sizeof(float)); IM.has_n = 1; }
    if (a->color.enabled && !im_read(&a->color, index, x, 1)) return 0;
    if (a->color.enabled) {
        IM.cc[0] = x[0]; IM.cc[1] = a->color.size > 1 ? x[1] : 1.0f;
        IM.cc[2] = a->color.size > 2 ? x[2] : 1.0f; IM.cc[3] = a->color.size > 3 ? x[3] : 1.0f;
        IM.has_c = 1;
    }
    if (a->tex.enabled && !im_read(&a->tex, index, x, 0)) return 0;
    if (a->tex.enabled) {
        IM.ct[0] = x[0]; IM.ct[1] = a->tex.size > 1 ? x[1] : 0.0f; IM.has_t = 1;
    }
    if (emit && a->vertex.enabled) {
        if (!im_read(&a->vertex, index, v, 0)) return 0;
        im_emit_vertex(v[0], a->vertex.size > 1 ? v[1] : 0.0f, a->vertex.size > 2 ? v[2] : 0.0f);
    }
    return 1;
}

static void im_decode_warning(void) {
    static int warned;
    if (!warned) {
        warned = 1;
        fprintf(stderr, "libGL.so: glArrayElement block rejected (unsupported mixed arrays or allocation failure)\n");
    }
}

static int im_flush_elements(void) {
    if (!IM.elements_n) return 1;
    im_arrays arrays;
    im_get_mixed_arrays(&arrays);
    if (arrays.mixed_multitex) {
        IM.elements_n = 0;
        IM.decode_elements = 1;
        IM.used_elements = 1;
        IM.elements_failed = 1;
        im_decode_warning();
        return 0;
    }
    for (int i = 0; i < IM.elements_n; i++)
        if (!im_decode_element(&arrays, IM.elements[i], 1)) {
            IM.elements_n = 0;
            IM.decode_elements = 1;
            IM.elements_failed = 1;
            im_decode_warning();
            return 0;
        }
    IM.elements_n = 0;
    IM.decode_elements = 1;
    IM.used_elements = 1;
    return 1;
}

static void im_vertex(float x, float y, float z) {
    if (!IM.in_begin) return;
    im_flush_elements();
    IM.decode_elements = 1;
    im_emit_vertex(x, y, z);
}

static int im_draw_elements(int mode, const int* elements, int n) {
    unsigned gm = (unsigned)mode;
    int count = n, max = 0;
    if (mode == 8) gm = 5;                                      /* QUAD_STRIP */
    else if (mode == 9) gm = 6;                                 /* POLYGON */
    else if (mode < 0 || mode > 7) return 0;
    for (int i = 0; i < n; i++) {
        if (elements[i] < 0) return 0;
        if (elements[i] > max) max = elements[i];
    }
    if (mode == 7) {
        int quads = n / 4;
        if (quads <= 0 || quads > INT_MAX / 6 || (size_t)quads > SIZE_MAX / (6 * sizeof(int))) return 0;
        int* q = malloc((size_t)quads * 6 * sizeof *q);
        if (!q) return 0;
        for (int i = 0; i < quads; i++) {
            int b = i * 4, o = i * 6;
            q[o] = elements[b]; q[o + 1] = elements[b + 1]; q[o + 2] = elements[b + 2];
            q[o + 3] = elements[b]; q[o + 4] = elements[b + 2]; q[o + 5] = elements[b + 3];
        }
        elements = q; count = quads * 6; gm = 4;
    }
    unsigned short* short_elements = NULL;
    const void* indices = elements;
    unsigned type = 0x1405;                                     /* UNSIGNED_INT (OES_element_index_uint) */
    if (max <= 65535) {
        if ((size_t)count > SIZE_MAX / sizeof *short_elements) {
            if (mode == 7) free((void*)elements);
            return 0;
        }
        short_elements = malloc((size_t)count * sizeof *short_elements);
        if (!short_elements) { if (mode == 7) free((void*)elements); return 0; }
        for (int i = 0; i < count; i++) short_elements[i] = (unsigned short)elements[i];
        indices = short_elements; type = 0x1403;
    }
    int ebo = 0;
    RP(glGetIntegerv, F_geti)(0x8895, &ebo);                      /* ELEMENT_ARRAY_BUFFER_BINDING */
    if (ebo) RP(glBindBuffer, F_uiui)(0x8893, 0);
    RP(glDrawElements, F_de)(gm, count, type, indices);
    if (ebo) RP(glBindBuffer, F_uiui)(0x8893, (unsigned)ebo);
    free(short_elements);
    if (mode == 7) free((void*)elements);
    return 1;
}

GLC void glArrayElement(int index) {
    if (!T_glx.ctx || T_glx.ctx->ver != 1 || T_glx.ctx->es_native || !IM.in_begin || index < 0) return;
    if (IM.decode_elements) {
        im_arrays arrays;
        im_get_mixed_arrays(&arrays);
        IM.used_elements = 1;
        if (arrays.mixed_multitex || !im_decode_element(&arrays, index, 1)) {
            IM.elements_failed = 1;
            im_decode_warning();
        }
        return;
    }
    if (IM.elements_n == IM.elements_cap) {
        if (IM.elements_cap > INT_MAX / 2) {
            IM.elements_n = 0; IM.decode_elements = IM.elements_failed = 1;
            im_decode_warning(); return;
        }
        int cap = IM.elements_cap ? IM.elements_cap * 2 : 256;
        if ((size_t)cap > SIZE_MAX / sizeof *IM.elements) {
            IM.elements_n = 0; IM.decode_elements = IM.elements_failed = 1;
            im_decode_warning(); return;
        }
        int* elements = realloc(IM.elements, (size_t)cap * sizeof *elements);
        if (!elements) {
            IM.elements_n = 0; IM.decode_elements = IM.elements_failed = 1;
            im_decode_warning(); return;
        }
        IM.elements = elements; IM.elements_cap = cap;
    }
    IM.elements[IM.elements_n++] = index;
}

GLC void glBegin(unsigned mode) {
    im_init();
    gl_warn_es("glBegin");
    IM.in_begin = 1; IM.mode = (int)mode; IM.n = 0; IM.elements_n = 0;
    IM.decode_elements = IM.rec_id != 0; IM.used_elements = IM.elements_failed = 0;
    IM.has_n = IM.has_c = IM.has_t = 0;
}

GLC void glEnd(void) {
    if (!IM.in_begin) return;
    if (IM.elements_n && !IM.decode_elements && IM.n == 0) {
        im_arrays arrays;
        im_get_arrays(&arrays);
        if (arrays.vertex.enabled && !im_draw_elements(IM.mode, IM.elements, IM.elements_n))
            im_decode_warning();
        if (im_decode_element(&arrays, IM.elements[IM.elements_n - 1], 0)) {
            if (arrays.normal.enabled) RP(glNormal3f, F_fff)(IM.cn[0], IM.cn[1], IM.cn[2]);
            if (arrays.color.enabled) RP(glColor4f, F_ffff)(IM.cc[0], IM.cc[1], IM.cc[2], IM.cc[3]);
            if (arrays.tex.enabled) RP(glMultiTexCoord4f, F_utex)(0x84C0, IM.ct[0], IM.ct[1], 0.0f, 1.0f);
        } else if (!arrays.vertex.enabled) im_decode_warning();
        IM.elements_n = 0;
        IM.in_begin = 0;
        return;
    }
    im_flush_elements();
    if (IM.elements_failed) {
        IM.in_begin = 0;
        IM.n = 0;
        return;
    }
    IM.in_begin = 0;
    int n = IM.n;
    if (n <= 0) return;
    const float *nr = IM.has_n ? IM.nrm : NULL, *co = IM.has_c ? IM.col : NULL, *te = IM.has_t ? IM.tex : NULL;
    if (IM.rec_id) {
        gl_block* b = calloc(1, sizeof *b);
        if (b) {
            b->mode = IM.mode; b->n = n; b->preserve_arrays = IM.used_elements;
            b->pos = fdup(IM.pos, (size_t)n * 3);
            if (nr) b->nrm = fdup(nr, (size_t)n * 3);
            if (co) b->col = fdup(co, (size_t)n * 4);
            if (te) b->tex = fdup(te, (size_t)n * 2);
            rec_cmd(OP_DRAW)->p = b;
        }
        if (REC_SKIP()) return;
    }
    im_draw(IM.mode, n, IM.pos, nr, co, te, IM.used_elements);
}

/* EXT_compiled_vertex_array is a stateful hint; no copy or cache is required. */
GLC void glLockArraysEXT(int first, int count) {
    glx_ctx* c = cva_context();
    if (!c) return;
    if (IM.in_begin) { cva_set_error(0x0502); return; }
    if (first < 0 || count <= 0) { cva_set_error(0x0501); return; }
    if (c->cva_count) { cva_set_error(0x0502); return; }
    c->cva_first = first;
    c->cva_count = count;
}

GLC void glUnlockArraysEXT(void) {
    glx_ctx* c = cva_context();
    if (!c) return;
    if (IM.in_begin || !c->cva_count) { cva_set_error(0x0502); return; }
    c->cva_first = 0;
    c->cva_count = 0;
}

GLC unsigned glGetError(void) {
    glx_ctx* c = cva_context();
    if (c && (c->cva_errors & CVA_INVALID_VALUE_BIT)) {
        c->cva_errors &= ~CVA_INVALID_VALUE_BIT;
        return 0x0501;
    }
    if (c && (c->cva_errors & CVA_INVALID_OPERATION_BIT)) {
        c->cva_errors &= ~CVA_INVALID_OPERATION_BIT;
        return 0x0502;
    }
    return RP(glGetError, unsigned (*)(void))();
}

GLC void glGetBooleanv(unsigned pname, unsigned char* out) {
    int value;
    if (cva_get(pname, &value)) {
        if (!IM.in_begin) *out = value != 0;
        return;
    }
    RP(glGetBooleanv, void (*)(unsigned, unsigned char*))(pname, out);
}

GLC void glGetFloatv(unsigned pname, float* out) {
    int value;
    if (cva_get(pname, &value)) {
        if (!IM.in_begin) *out = (float)value;
        return;
    }
    RP(glGetFloatv, F_getf)(pname, out);
}

GLC void glVertex2f(float x, float y)                { im_vertex(x, y, 0.0f); }
GLC void glVertex3f(float x, float y, float z)       { im_vertex(x, y, z); }
GLC void glVertex4f(float x, float y, float z, float w) { (void)w; im_vertex(x, y, z); }
GLC void glVertex2fv(const float* v)                 { im_vertex(v[0], v[1], 0.0f); }
GLC void glVertex3fv(const float* v)                 { im_vertex(v[0], v[1], v[2]); }
GLC void glVertex4fv(const float* v)                 { im_vertex(v[0], v[1], v[2]); }
GLC void glVertex2i(int x, int y)                    { im_vertex((float)x, (float)y, 0.0f); }
GLC void glVertex3i(int x, int y, int z)             { im_vertex((float)x, (float)y, (float)z); }
GLC void glVertex4i(int x, int y, int z, int w)      { (void)w; im_vertex((float)x, (float)y, (float)z); }
GLC void glVertex2iv(const int* v)                   { im_vertex((float)v[0], (float)v[1], 0.0f); }
GLC void glVertex3iv(const int* v)                   { im_vertex((float)v[0], (float)v[1], (float)v[2]); }
GLC void glVertex4iv(const int* v)                   { im_vertex((float)v[0], (float)v[1], (float)v[2]); }
GLC void glVertex2s(short x, short y)                { im_vertex((float)x, (float)y, 0.0f); }
GLC void glVertex3s(short x, short y, short z)       { im_vertex((float)x, (float)y, (float)z); }
GLC void glVertex4s(short x, short y, short z, short w) { (void)w; im_vertex((float)x, (float)y, (float)z); }
GLC void glVertex2sv(const short* v)                 { im_vertex((float)v[0], (float)v[1], 0.0f); }
GLC void glVertex3sv(const short* v)                 { im_vertex((float)v[0], (float)v[1], (float)v[2]); }
GLC void glVertex4sv(const short* v)                 { im_vertex((float)v[0], (float)v[1], (float)v[2]); }
GLC void glVertex2d(double x, double y)              { im_vertex((float)x, (float)y, 0.0f); }
GLC void glVertex3d(double x, double y, double z)    { im_vertex((float)x, (float)y, (float)z); }
GLC void glVertex4d(double x, double y, double z, double w) { (void)w; im_vertex((float)x, (float)y, (float)z); }
GLC void glVertex2dv(const double* v)                { im_vertex((float)v[0], (float)v[1], 0.0f); }
GLC void glVertex3dv(const double* v)                { im_vertex((float)v[0], (float)v[1], (float)v[2]); }
GLC void glVertex4dv(const double* v)                { im_vertex((float)v[0], (float)v[1], (float)v[2]); }

/* Rectangles (desktop): a quad in the z=0 plane */
GLC void glRectf(float x1, float y1, float x2, float y2) {
    glBegin(7 /*GL_QUADS*/);
    im_vertex(x1, y1, 0.0f); im_vertex(x2, y1, 0.0f); im_vertex(x2, y2, 0.0f); im_vertex(x1, y2, 0.0f);
    glEnd();
}
GLC void glRectd(double x1, double y1, double x2, double y2) { glRectf((float)x1, (float)y1, (float)x2, (float)y2); }
GLC void glRecti(int x1, int y1, int x2, int y2)             { glRectf((float)x1, (float)y1, (float)x2, (float)y2); }
GLC void glRects(short x1, short y1, short x2, short y2)     { glRectf((float)x1, (float)y1, (float)x2, (float)y2); }
GLC void glRectfv(const float* a, const float* b)            { glRectf(a[0], a[1], b[0], b[1]); }
GLC void glRectdv(const double* a, const double* b)          { glRectf((float)a[0], (float)a[1], (float)b[0], (float)b[1]); }
GLC void glRectiv(const int* a, const int* b)                { glRectf((float)a[0], (float)a[1], (float)b[0], (float)b[1]); }
GLC void glRectsv(const short* a, const short* b)            { glRectf((float)a[0], (float)a[1], (float)b[0], (float)b[1]); }

/* ---------- Current Attributes (recordable) ---------- */
GLC void glNormal3f(float x, float y, float z) {
    im_init();
    if (IM.in_begin && IM.elements_n) im_flush_elements();
    IM.cn[0] = x; IM.cn[1] = y; IM.cn[2] = z;
    if (IM.in_begin) { IM.has_n = 1; return; }
    if (IM.rec_id) {
        gl_cmd* c = rec_cmd(OP_NORMAL); c->f[0] = x; c->f[1] = y; c->f[2] = z;
        if (REC_SKIP()) return;
    }
    RP(glNormal3f, F_fff)(x, y, z);
}
GLC void glNormal3fv(const float* v) { glNormal3f(v[0], v[1], v[2]); }
GLC void glNormal3d(double x, double y, double z) { glNormal3f((float)x, (float)y, (float)z); }
GLC void glNormal3dv(const double* v) { glNormal3f((float)v[0], (float)v[1], (float)v[2]); }
GLC void glNormal3i(int x, int y, int z) { glNormal3f((float)x, (float)y, (float)z); }
GLC void glNormal3iv(const int* v) { glNormal3f((float)v[0], (float)v[1], (float)v[2]); }
GLC void glNormal3s(short x, short y, short z) { glNormal3f((float)x, (float)y, (float)z); }
GLC void glNormal3sv(const short* v) { glNormal3f((float)v[0], (float)v[1], (float)v[2]); }
GLC void glNormal3b(signed char x, signed char y, signed char z) { glNormal3f(x / 127.0f, y / 127.0f, z / 127.0f); }
GLC void glNormal3bv(const signed char* v) { glNormal3f(v[0] / 127.0f, v[1] / 127.0f, v[2] / 127.0f); }

GLC void glColor4f(float r, float g, float b, float a) {
    im_init();
    if (IM.in_begin && IM.elements_n) im_flush_elements();
    IM.cc[0] = r; IM.cc[1] = g; IM.cc[2] = b; IM.cc[3] = a;
    if (IM.in_begin) { IM.has_c = 1; return; }
    if (IM.rec_id) {
        gl_cmd* c = rec_cmd(OP_COLOR); c->f[0] = r; c->f[1] = g; c->f[2] = b; c->f[3] = a;
        if (REC_SKIP()) return;
    }
    RP(glColor4f, F_ffff)(r, g, b, a);
}
GLC void glColor3f(float r, float g, float b)  { glColor4f(r, g, b, 1.0f); }
GLC void glColor3fv(const float* v)            { glColor4f(v[0], v[1], v[2], 1.0f); }
GLC void glColor4fv(const float* v)            { glColor4f(v[0], v[1], v[2], v[3]); }
GLC void glColor3d(double r, double g, double b) { glColor4f((float)r, (float)g, (float)b, 1.0f); }
GLC void glColor4d(double r, double g, double b, double a) { glColor4f((float)r, (float)g, (float)b, (float)a); }
GLC void glColor3dv(const double* v)           { glColor4f((float)v[0], (float)v[1], (float)v[2], 1.0f); }
GLC void glColor4dv(const double* v)           { glColor4f((float)v[0], (float)v[1], (float)v[2], (float)v[3]); }
GLC void glColor3ub(unsigned char r, unsigned char g, unsigned char b) { glColor4f(r / 255.0f, g / 255.0f, b / 255.0f, 1.0f); }
GLC void glColor4ub(unsigned char r, unsigned char g, unsigned char b, unsigned char a) { glColor4f(r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f); }
GLC void glColor3ubv(const unsigned char* v)   { glColor4f(v[0] / 255.0f, v[1] / 255.0f, v[2] / 255.0f, 1.0f); }
GLC void glColor4ubv(const unsigned char* v)   { glColor4f(v[0] / 255.0f, v[1] / 255.0f, v[2] / 255.0f, v[3] / 255.0f); }
GLC void glColor3ui(unsigned r, unsigned g, unsigned b) { glColor4f(r / 4294967295.0f, g / 4294967295.0f, b / 4294967295.0f, 1.0f); }
GLC void glColor4ui(unsigned r, unsigned g, unsigned b, unsigned a) { glColor4f(r / 4294967295.0f, g / 4294967295.0f, b / 4294967295.0f, a / 4294967295.0f); }
GLC void glColor3uiv(const unsigned* v)         { glColor3ui(v[0], v[1], v[2]); }
GLC void glColor4uiv(const unsigned* v)         { glColor4ui(v[0], v[1], v[2], v[3]); }
GLC void glColor3us(unsigned short r, unsigned short g, unsigned short b) { glColor4f(r / 65535.0f, g / 65535.0f, b / 65535.0f, 1.0f); }
GLC void glColor4us(unsigned short r, unsigned short g, unsigned short b, unsigned short a) { glColor4f(r / 65535.0f, g / 65535.0f, b / 65535.0f, a / 65535.0f); }
GLC void glColor3usv(const unsigned short* v)   { glColor3us(v[0], v[1], v[2]); }
GLC void glColor4usv(const unsigned short* v)   { glColor4us(v[0], v[1], v[2], v[3]); }

/* ---------- TexCoord ---------- */
GLC void glTexCoord2f(float s, float t) {
    im_init();
    if (IM.in_begin && IM.elements_n) im_flush_elements();
    IM.ct[0] = s; IM.ct[1] = t;
    if (IM.in_begin) { IM.has_t = 1; return; }
    if (IM.rec_id) {
        gl_cmd* c = rec_cmd(OP_TEXCOORD); c->f[0] = s; c->f[1] = t;
        if (REC_SKIP()) return;
    }
    RP(glMultiTexCoord4f, F_utex)(0x84C0, s, t, 0.0f, 1.0f);
}
GLC void glTexCoord2fv(const float* v)          { glTexCoord2f(v[0], v[1]); }
GLC void glTexCoord1f(float s)                   { glTexCoord2f(s, 0.0f); }
GLC void glTexCoord1fv(const float* v)          { glTexCoord2f(v[0], 0.0f); }
GLC void glTexCoord3f(float s, float t, float r) { (void)r; glTexCoord2f(s, t); }
GLC void glTexCoord3fv(const float* v)          { glTexCoord2f(v[0], v[1]); }
GLC void glTexCoord4f(float s, float t, float r, float q) { (void)r; (void)q; glTexCoord2f(s, t); }
GLC void glTexCoord4fv(const float* v)          { glTexCoord2f(v[0], v[1]); }

GLC void glTexCoord1d(double s)                 { glTexCoord2f((float)s, 0.0f); }
GLC void glTexCoord2d(double s, double t)       { glTexCoord2f((float)s, (float)t); }
GLC void glTexCoord3d(double s, double t, double r) { (void)r; glTexCoord2f((float)s, (float)t); }
GLC void glTexCoord4d(double s, double t, double r, double q) { (void)r; (void)q; glTexCoord2f((float)s, (float)t); }
GLC void glTexCoord1dv(const double* v)         { glTexCoord2f((float)v[0], 0.0f); }
GLC void glTexCoord2dv(const double* v)         { glTexCoord2f((float)v[0], (float)v[1]); }
GLC void glTexCoord3dv(const double* v)         { glTexCoord2f((float)v[0], (float)v[1]); }
GLC void glTexCoord4dv(const double* v)         { glTexCoord2f((float)v[0], (float)v[1]); }

GLC void glTexCoord1i(int s)                    { glTexCoord2f((float)s, 0.0f); }
GLC void glTexCoord2i(int s, int t)            { glTexCoord2f((float)s, (float)t); }
GLC void glTexCoord3i(int s, int t, int r)      { (void)r; glTexCoord2f((float)s, (float)t); }
GLC void glTexCoord4i(int s, int t, int r, int q) { (void)r; (void)q; glTexCoord2f((float)s, (float)t); }
GLC void glTexCoord1iv(const int* v)            { glTexCoord2f((float)v[0], 0.0f); }
GLC void glTexCoord2iv(const int* v)            { glTexCoord2f((float)v[0], (float)v[1]); }
GLC void glTexCoord3iv(const int* v)            { glTexCoord2f((float)v[0], (float)v[1]); }
GLC void glTexCoord4iv(const int* v)            { glTexCoord2f((float)v[0], (float)v[1]); }

GLC void glTexCoord1s(short s)                  { glTexCoord2f((float)s, 0.0f); }
GLC void glTexCoord2s(short s, short t)          { glTexCoord2f((float)s, (float)t); }
GLC void glTexCoord3s(short s, short t, short r) { (void)r; glTexCoord2f((float)s, (float)t); }
GLC void glTexCoord4s(short s, short t, short r, short q) { (void)r; (void)q; glTexCoord2f((float)s, (float)t); }
GLC void glTexCoord1sv(const short* v)          { glTexCoord2f((float)v[0], 0.0f); }
GLC void glTexCoord2sv(const short* v)          { glTexCoord2f((float)v[0], (float)v[1]); }
GLC void glTexCoord3sv(const short* v)          { glTexCoord2f((float)v[0], (float)v[1]); }
GLC void glTexCoord4sv(const short* v)          { glTexCoord2f((float)v[0], (float)v[1]); }

/* ---------- Fixed-function Recordable Calls ---------- */
static int light_count(unsigned pname) {
    if (pname >= 0x1205 && pname <= 0x1209) return 1;      /* SPOT_EXPONENT .. QUADRATIC_ATTENUATION */
    if (pname == 0x1204) return 3;                          /* SPOT_DIRECTION */
    return 4;
}

GLC void glMaterialfv(unsigned face, unsigned pname, const float* v) {
    (void)face;                                             /* ES 1.1 accepts GL_FRONT_AND_BACK only */
    if (IM.rec_id) {
        gl_cmd* c = rec_cmd(OP_MATERIAL);
        c->u[0] = 0x0408; c->u[1] = pname;
        memcpy(c->f, v, (pname == 0x1601 ? 1 : 4) * sizeof(float));
        if (REC_SKIP()) return;
    }
    RP(glMaterialfv, F_mat)(0x0408, pname, v);
}

GLC void glMaterialf(unsigned face, unsigned pname, float v) {
    glMaterialfv(face, pname, &v);
}

GLC void glLightfv(unsigned light, unsigned pname, const float* v) {
    if (IM.rec_id) {
        gl_cmd* c = rec_cmd(OP_LIGHT);
        c->u[0] = light; c->u[1] = pname;
        memcpy(c->f, v, (size_t)light_count(pname) * sizeof(float));
        if (REC_SKIP()) return;
    }
    RP(glLightfv, F_mat)(light, pname, v);
}

GLC void glLightf(unsigned light, unsigned pname, float v) {
    glLightfv(light, pname, &v);
}

#define REC1(OPC, FN, T, VAL)                                    \
    if (IM.rec_id) { gl_cmd* c = rec_cmd(OPC); c->u[0] = (VAL); if (REC_SKIP()) return; } \
    RP(FN, T)(VAL);

GLC void glShadeModel(unsigned m) { REC1(OP_SHADE, glShadeModel, F_ui, m) }
GLC void glFrontFace(unsigned m) {
    if (IM.rec_id) { gl_cmd* c = rec_cmd(OP_FRONTFACE); c->u[0] = m; if (REC_SKIP()) return; }
    cc_note_front(m);
    cc_setfront();
}
/* GL_TEXTURE_GEN_S/T/R (0x0C60..0x0C62) -> GL_TEXTURE_GEN_STR_OES (ES1 only; OES binds them together) */
static unsigned gl_cap_xlat(unsigned cap) {
    return (cap >= 0x0C60 && cap <= 0x0C62 && T_glx.ctx && T_glx.ctx->ver == 1) ? 0x8D60 : cap;
}

/* Capabilitati desktop inexistente in ES2/3: ignorate (altfel Mali da GL_INVALID_ENUM).
   Doar in contexte emulate (nu ES1, nu ES nativ), unde unele dintre ele sunt valide. */
static int gl_cap_ignored(unsigned cap) {
    if (cap == 0x0DE0) return 1;                                   /* TEXTURE_1D (mereu) */
    if (!T_glx.ctx || T_glx.ctx->ver < 2 || T_glx.ctx->es_native) return 0;
    switch (cap) {
    case 0x8F9D: case 0x8D69:                                      /* PRIMITIVE_RESTART (ES3: index fix, mereu activ) */
    case 0x8861: case 0x8642: case 0x864F: case 0x884F:            /* POINT_SPRITE, PROGRAM_POINT_SIZE, DEPTH_CLAMP, CUBE_MAP_SEAMLESS */
    case 0x809D: case 0x0B20: case 0x0B41: case 0x0BF2:            /* MULTISAMPLE, LINE_SMOOTH, POLYGON_SMOOTH, COLOR_LOGIC_OP */
    case 0x0DE1: case 0x8513:                                      /* TEXTURE_2D, TEXTURE_CUBE_MAP (legacy compat) */
    case 0x0B50: case 0x0BC0: case 0x0B60: case 0x0B57: case 0x0BA1: /* LIGHTING, ALPHA_TEST, FOG, COLOR_MATERIAL, NORMALIZE */
    case 0x0C60: case 0x0C61: case 0x0C62:                         /* TEXTURE_GEN_S/T/R */
    case 0x8DB9:                                                   /* FRAMEBUFFER_SRGB */
    case 0x0B10:                                                   /* POINT_SMOOTH */
        return 1;
    }
    if ((cap >= 0x3000 && cap <= 0x3007) ||                        /* CLIP_DISTANCE0..7 */
        (cap >= 0x4000 && cap <= 0x4007)) return 1;                /* LIGHT0..7 */
    return 0;
}

/* Forward declarations */
static void dsb_on_enable(unsigned cap, int on);
static int cm_cap(unsigned cap, int on);
static int ev_cap(unsigned cap, int on);

GLC void glEnable(unsigned cap) {
    if (gl_cap_ignored(cap)) return;
    cap = gl_cap_xlat(cap);
    if (IM.rec_id) { gl_cmd* c = rec_cmd(OP_ENABLE); c->u[0] = cap; if (REC_SKIP()) return; }
    if (cm_cap(cap, 1)) return;
    RP(glEnable, F_ui)(cap);
    dsb_on_enable(cap, 1);
}

GLC void glDisable(unsigned cap) {
    if (gl_cap_ignored(cap)) return;
    cap = gl_cap_xlat(cap);
    if (IM.rec_id) { gl_cmd* c = rec_cmd(OP_DISABLE); c->u[0] = cap; if (REC_SKIP()) return; }
    if (cm_cap(cap, 0)) return;
    RP(glDisable, F_ui)(cap);
    dsb_on_enable(cap, 0);
}

GLC void glBindTexture(unsigned target, unsigned tex) {
    target = t1d(target);                                  /* Convert GL_TEXTURE_1D -> GL_TEXTURE_2D */
    if (IM.rec_id) {
        gl_cmd* c = rec_cmd(OP_BINDTEX); c->u[0] = target; c->u[1] = tex;
        if (REC_SKIP()) return;
    }
    RP(glBindTexture, F_uiui)(target, tex);
}

GLC void glPushMatrix(void) {
    if (IM.rec_id) { rec_cmd(OP_PUSH); if (REC_SKIP()) return; }
    RP(glPushMatrix, F_v)();
}
GLC void glPopMatrix(void) {
    if (IM.rec_id) { rec_cmd(OP_POP); if (REC_SKIP()) return; }
    RP(glPopMatrix, F_v)();
}
GLC void glLoadIdentity(void) {
    if (IM.rec_id) { rec_cmd(OP_IDENT); if (REC_SKIP()) return; }
    RP(glLoadIdentity, F_v)();
}
GLC void glTranslatef(float x, float y, float z) {
    if (IM.rec_id) { gl_cmd* c = rec_cmd(OP_TRANSLATE); c->f[0] = x; c->f[1] = y; c->f[2] = z; if (REC_SKIP()) return; }
    RP(glTranslatef, F_fff)(x, y, z);
}
GLC void glScalef(float x, float y, float z) {
    if (IM.rec_id) { gl_cmd* c = rec_cmd(OP_SCALE); c->f[0] = x; c->f[1] = y; c->f[2] = z; if (REC_SKIP()) return; }
    RP(glScalef, F_fff)(x, y, z);
}
GLC void glRotatef(float a, float x, float y, float z) {
    if (IM.rec_id) {
        gl_cmd* c = rec_cmd(OP_ROTATE); c->f[0] = a; c->f[1] = x; c->f[2] = y; c->f[3] = z;
        if (REC_SKIP()) return;
    }
    RP(glRotatef, F_ffff)(a, x, y, z);
}
GLC void glMultMatrixf(const float* m) {
    if (IM.rec_id) { gl_cmd* c = rec_cmd(OP_MULT); memcpy(c->f, m, 16 * sizeof(float)); if (REC_SKIP()) return; }
    RP(glMultMatrixf, F_pf)(m);
}

/* ---------- Raster Position: ARB_window_pos + glRasterPos ---------- */
/* GLES lacks a raster position; we manage it manually (in window coordinates) and glDrawPixels consumes it. */
GLC void glWindowPos3f(float x, float y, float z) {
    im_init();
    gl_warn_es("glWindowPos3f");
    if (IM.rec_id) {
        gl_cmd* c = rec_cmd(OP_WINDOWPOS);
        c->f[0] = x; c->f[1] = y; c->f[2] = z;
        if (REC_SKIP()) return;
    }
    g_rasterx = x; g_rastery = y; g_rvalid = 1;
}

GLC void glWindowPos2f(float x, float y)             { glWindowPos3f(x, y, 0.0f); }
GLC void glWindowPos2i(int x, int y)                 { glWindowPos3f((float)x, (float)y, 0.0f); }
GLC void glWindowPos3i(int x, int y, int z)          { glWindowPos3f((float)x, (float)y, (float)z); }
GLC void glWindowPos2d(double x, double y)           { glWindowPos3f((float)x, (float)y, 0.0f); }
GLC void glWindowPos3d(double x, double y, double z) { glWindowPos3f((float)x, (float)y, (float)z); }
GLC void glWindowPos2s(short x, short y)             { glWindowPos3f((float)x, (float)y, 0.0f); }
GLC void glWindowPos3s(short x, short y, short z)    { glWindowPos3f((float)x, (float)y, (float)z); }

GLC void glWindowPos2fv(const float* v)              { glWindowPos3f(v[0], v[1], 0.0f); }
GLC void glWindowPos3fv(const float* v)              { glWindowPos3f(v[0], v[1], v[2]); }
GLC void glWindowPos2iv(const int* v)                { glWindowPos3f((float)v[0], (float)v[1], 0.0f); }
GLC void glWindowPos3iv(const int* v)                { glWindowPos3f((float)v[0], (float)v[1], (float)v[2]); }
GLC void glWindowPos2dv(const double* v)             { glWindowPos3f((float)v[0], (float)v[1], 0.0f); }
GLC void glWindowPos3dv(const double* v)             { glWindowPos3f((float)v[0], (float)v[1], (float)v[2]); }
GLC void glWindowPos2sv(const short* v)              { glWindowPos3f((float)v[0], (float)v[1], 0.0f); }
GLC void glWindowPos3sv(const short* v)              { glWindowPos3f((float)v[0], (float)v[1], (float)v[2]); }

GLC void glWindowPos2dARB(double x, double y)           { glWindowPos2d(x, y); }
GLC void glWindowPos2fARB(float x, float y)            { glWindowPos2f(x, y); }
GLC void glWindowPos2iARB(int x, int y)                { glWindowPos2i(x, y); }
GLC void glWindowPos2sARB(short x, short y)            { glWindowPos2s(x, y); }
GLC void glWindowPos2dvARB(const double *p)            { glWindowPos2dv(p); }
GLC void glWindowPos2fvARB(const float *p)             { glWindowPos2fv(p); }
GLC void glWindowPos2ivARB(const int *p)               { glWindowPos2iv(p); }
GLC void glWindowPos2svARB(const short *p)             { glWindowPos2sv(p); }
GLC void glWindowPos3dARB(double x, double y, double z) { glWindowPos3d(x, y, z); }
GLC void glWindowPos3fARB(float x, float y, float z)   { glWindowPos3f(x, y, z); }
GLC void glWindowPos3iARB(int x, int y, int z)         { glWindowPos3i(x, y, z); }
GLC void glWindowPos3sARB(short x, short y, short z)   { glWindowPos3s(x, y, z); }
GLC void glWindowPos3dvARB(const double *p)            { glWindowPos3dv(p); }
GLC void glWindowPos3fvARB(const float *p)             { glWindowPos3fv(p); }
GLC void glWindowPos3ivARB(const int *p)               { glWindowPos3iv(p); }
GLC void glWindowPos3svARB(const short *p)             { glWindowPos3sv(p); }

/* glRasterPos: CPU projection using current modelview/projection matrices (column-major) */
GLC void glRasterPos4f(float x, float y, float z, float w) {
    im_init();
    gl_warn_es("glRasterPos");
    if (IM.rec_id) {
        gl_cmd* c = rec_cmd(OP_RASTERPOS);
        c->f[0] = x; c->f[1] = y; c->f[2] = z; c->f[3] = w;
        if (REC_SKIP()) return;
    }
    float mv[16], pr[16];
    int vp[4];
    RP(glGetFloatv, F_getf)(0x0BA6, mv);        /* GL_MODELVIEW_MATRIX */
    RP(glGetFloatv, F_getf)(0x0BA7, pr);        /* GL_PROJECTION_MATRIX */
    RP(glGetIntegerv, F_geti)(0x0BA2, vp);      /* GL_VIEWPORT */
    float e[4] = { x, y, z, w }, t[4], c[4];
    for (int r = 0; r < 4; r++) t[r] = mv[r] * e[0] + mv[4 + r] * e[1] + mv[8 + r] * e[2] + mv[12 + r] * e[3];
    for (int r = 0; r < 4; r++) c[r] = pr[r] * t[0] + pr[4 + r] * t[1] + pr[8 + r] * t[2] + pr[12 + r] * t[3];
    if (c[3] <= 0.0f) { g_rvalid = 0; return; } /* Behind camera: invalid raster position */
    g_rasterx = vp[0] + (c[0] / c[3] * 0.5f + 0.5f) * vp[2];
    g_rastery = vp[1] + (c[1] / c[3] * 0.5f + 0.5f) * vp[3];
    g_rvalid = 1;
}
GLC void glRasterPos3f(float x, float y, float z)     { glRasterPos4f(x, y, z, 1.0f); }
GLC void glRasterPos2f(float x, float y)              { glRasterPos4f(x, y, 0.0f, 1.0f); }
GLC void glRasterPos2i(int x, int y)                  { glRasterPos4f((float)x, (float)y, 0.0f, 1.0f); }
GLC void glRasterPos3i(int x, int y, int z)           { glRasterPos4f((float)x, (float)y, (float)z, 1.0f); }
GLC void glRasterPos4i(int x, int y, int z, int w)    { glRasterPos4f((float)x, (float)y, (float)z, (float)w); }
GLC void glRasterPos2s(short x, short y)              { glRasterPos4f((float)x, (float)y, 0.0f, 1.0f); }
GLC void glRasterPos3s(short x, short y, short z)     { glRasterPos4f((float)x, (float)y, (float)z, 1.0f); }
GLC void glRasterPos4s(short x, short y, short z, short w) { glRasterPos4f((float)x, (float)y, (float)z, (float)w); }
GLC void glRasterPos2d(double x, double y)            { glRasterPos4f((float)x, (float)y, 0.0f, 1.0f); }
GLC void glRasterPos3d(double x, double y, double z)  { glRasterPos4f((float)x, (float)y, (float)z, 1.0f); }
GLC void glRasterPos4d(double x, double y, double z, double w) { glRasterPos4f((float)x, (float)y, (float)z, (float)w); }
GLC void glRasterPos2fv(const float* v)               { glRasterPos4f(v[0], v[1], 0.0f, 1.0f); }
GLC void glRasterPos3fv(const float* v)               { glRasterPos4f(v[0], v[1], v[2], 1.0f); }
GLC void glRasterPos4fv(const float* v)               { glRasterPos4f(v[0], v[1], v[2], v[3]); }
GLC void glRasterPos2iv(const int* v)                 { glRasterPos4f((float)v[0], (float)v[1], 0.0f, 1.0f); }
GLC void glRasterPos3iv(const int* v)                 { glRasterPos4f((float)v[0], (float)v[1], (float)v[2], 1.0f); }
GLC void glRasterPos4iv(const int* v)                 { glRasterPos4f((float)v[0], (float)v[1], (float)v[2], (float)v[3]); }
GLC void glRasterPos2sv(const short* v)               { glRasterPos4f((float)v[0], (float)v[1], 0.0f, 1.0f); }
GLC void glRasterPos3sv(const short* v)               { glRasterPos4f((float)v[0], (float)v[1], (float)v[2], 1.0f); }
GLC void glRasterPos4sv(const short* v)               { glRasterPos4f((float)v[0], (float)v[1], (float)v[2], (float)v[3]); }
GLC void glRasterPos2dv(const double* v)              { glRasterPos4f((float)v[0], (float)v[1], 0.0f, 1.0f); }
GLC void glRasterPos3dv(const double* v)              { glRasterPos4f((float)v[0], (float)v[1], (float)v[2], 1.0f); }
GLC void glRasterPos4dv(const double* v)              { glRasterPos4f((float)v[0], (float)v[1], (float)v[2], (float)v[3]); }

/* ---------- glDrawPixels (Emulation) ---------- */
GLC void glPixelZoom(float xfactor, float yfactor) { g_zoomx = xfactor; g_zoomy = yfactor; }

static unsigned dp_pot(unsigned v) { unsigned p = 1; while (p < v) p <<= 1; return p; }

/*
 * glDrawPixels emulation on GLES 1.1 (Mali):
 *  - Persistent POT texture (no NPOT, no per-frame gen/delete), loaded via glTexSubImage2D
 *  - internalformat == format, UNSIGNED_BYTE only (+ RGB 5_6_5); BGRA converted on CPU to RGBA
 *  - Ortho quad on viewport at raster position (glRasterPos / glWindowPos), respecting glPixelZoom
 *  - Preserves application's scissor/blend (matters on desktop); depth/lighting/fog/cull are disabled
 *  - All modified state is restored; uses direct RP pointers (bypasses display lists)
 * Limitations: not recorded in display lists, no pixel transfer (scale/bias/map), no
 *              GL_UNPACK_ROW_LENGTH (unavailable in ES 1.x).
 */
GLC void glDrawPixels(int w, int h, unsigned fmt, unsigned type, const void *px) {
    im_init();
    gl_warn_es("glDrawPixels");
    if (IM.in_begin) return;
    if (w <= 0 || h <= 0 || !px) return;
    if (IM.rec_id) {                                   /* Unsupported in display lists */
        if (gl_compat_debug_enabled()) {
            static int warned;
            if (!warned) { warned = 1; fprintf(stderr, "[gl_compat] glDrawPixels in display list: ignored\n"); }
        }
        return;
    }
    if (!g_rvalid) return;                             /* Invalid raster position -> draw nothing */

    int is565 = (type == 0x8363 && fmt == 0x1907);     /* UNSIGNED_SHORT_5_6_5 with RGB */
    if (type != 0x1401 && !is565) {
        if (gl_compat_debug_enabled()) {
            fprintf(stderr, "[gl_compat] glDrawPixels: Unsupported type 0x%x (UNSIGNED_BYTE only)\n", type);
        }
        return;
    }
    if (fmt != 0x1906 && fmt != 0x1907 && fmt != 0x1908 && fmt != 0x1909 && fmt != 0x190A && fmt != 0x80E1) {
        if (gl_compat_debug_enabled()) {
            fprintf(stderr, "[gl_compat] glDrawPixels: Unsupported format 0x%x\n", fmt);
        }
        return;
    }

    /* BGRA -> RGBA on CPU (safe across all Mali drivers) */
    unsigned char* conv = NULL;
    unsigned ifmt = fmt;
    if (fmt == 0x80E1) {
        const unsigned char* s = px;
        conv = malloc((size_t)w * h * 4);
        if (!conv) return;
        for (size_t i = 0, n = (size_t)w * h; i < n; i++) {
            conv[4 * i + 0] = s[4 * i + 2];
            conv[4 * i + 1] = s[4 * i + 1];
            conv[4 * i + 2] = s[4 * i + 0];
            conv[4 * i + 3] = s[4 * i + 3];
        }
        px = conv; ifmt = 0x1908;
    }

    /* --- Save State --- */
    int old_tex = 0, old_mm = 0x1700, old_env = 0x2100, vp[4];
    float old_col[4] = { 1, 1, 1, 1 }, old_tc[4] = { 0, 0, 0, 1 };
    RP(glGetIntegerv, F_geti)(0x8069, &old_tex);                 /* TEXTURE_BINDING_2D */
    RP(glGetIntegerv, F_geti)(0x0BA0, &old_mm);                  /* MATRIX_MODE */
    RP(glGetIntegerv, F_geti)(0x0BA2, vp);                       /* VIEWPORT */
    RP(glGetFloatv,   F_getf)(0x0B00, old_col);                  /* GL_CURRENT_COLOR */
    RP(glGetFloatv,   F_getf)(0x0B03, old_tc);                   /* GL_CURRENT_TEXTURE_COORDS */
    RP(glGetTexEnviv, F_texgeti)(0x2300, 0x2200, &old_env);      /* TEXTURE_ENV_MODE */
    int was_tex   = RP(glIsEnabled, F_isen)(0x0DE1);
    int was_depth = RP(glIsEnabled, F_isen)(0x0B71);
    int was_light = RP(glIsEnabled, F_isen)(0x0B50);
    int was_cull  = RP(glIsEnabled, F_isen)(0x0B44);
    int was_fog   = RP(glIsEnabled, F_isen)(0x0B60);

    if (was_depth) RP(glDisable, F_ui)(0x0B71);
    if (was_light) RP(glDisable, F_ui)(0x0B50);
    if (was_cull)  RP(glDisable, F_ui)(0x0B44);
    if (was_fog)   RP(glDisable, F_ui)(0x0B60);

    /* --- Persistent POT Texture --- */
    if (!g_dp_tex) RP(glGenTextures, F_gentex)(1, &g_dp_tex);
    RP(glBindTexture, F_uiui)(0x0DE1, g_dp_tex);

    unsigned pw = dp_pot((unsigned)w), ph = dp_pot((unsigned)h);
    if (pw < g_dp_pw) pw = g_dp_pw;
    if (ph < g_dp_ph) ph = g_dp_ph;
    if (pw != g_dp_pw || ph != g_dp_ph || ifmt != g_dp_ifmt || type != g_dp_type) {
        g_dp_pw = pw; g_dp_ph = ph; g_dp_ifmt = ifmt; g_dp_type = type;
        RP(glTexParameteri, F_da)(0x0DE1, 0x2801, 0x2600);       /* MIN_FILTER NEAREST */
        RP(glTexParameteri, F_da)(0x0DE1, 0x2800, 0x2600);       /* MAG_FILTER NEAREST */
        RP(glTexParameteri, F_da)(0x0DE1, 0x2802, 0x812F);       /* WRAP_S CLAMP_TO_EDGE */
        RP(glTexParameteri, F_da)(0x0DE1, 0x2803, 0x812F);       /* WRAP_T CLAMP_TO_EDGE */
        RP(glTexImage2D, F_teximg)(0x0DE1, 0, (int)ifmt, (int)pw, (int)ph, 0, ifmt, type, NULL);
    }
    /* Unpack alignment remains as set by application (default 4) */
    RP(glTexSubImage2D, F_teximg)(0x0DE1, 0, 0, 0, w, h, ifmt, type, px);
    free(conv);

    RP(glTexEnvf, F_texenv)(0x2300, 0x2200, (float)0x1E01);      /* GL_REPLACE: ignore current color */
    RP(glEnable, F_ui)(0x0DE1);
    RP(glColor4f, F_ffff)(1.0f, 1.0f, 1.0f, 1.0f);

    /* --- Ortho projection over viewport (texture matrix = identity) --- */
    RP(glMatrixMode, F_ui)(0x1702);                              /* GL_TEXTURE */
    RP(glPushMatrix, F_v)(); RP(glLoadIdentity, F_v)();
    RP(glMatrixMode, F_ui)(0x1701);                              /* GL_PROJECTION */
    RP(glPushMatrix, F_v)(); RP(glLoadIdentity, F_v)();
    RP(glOrthof, F_6f)((float)vp[0], (float)(vp[0] + vp[2]), (float)vp[1], (float)(vp[1] + vp[3]), -1.0f, 1.0f);
    RP(glMatrixMode, F_ui)(0x1700);                              /* GL_MODELVIEW */
    RP(glPushMatrix, F_v)(); RP(glLoadIdentity, F_v)();

    /* Rows in glDrawPixels are bottom-to-top: t=0 at y (raster), increasing upwards.
       Negative PixelZoom on Y naturally flips the image (coordinates become decreasing). */
    float x0 = g_rasterx, y0 = g_rastery;
    float x1 = x0 + w * g_zoomx, y1 = y0 + h * g_zoomy;
    float s = (float)w / (float)g_dp_pw, t = (float)h / (float)g_dp_ph;
    float v[12] = { x0, y0, 0,   x1, y0, 0,   x0, y1, 0,   x1, y1, 0 };
    float c[8]  = { 0, 0,        s, 0,         0, t,        s, t };
    im_draw(8, 4, v, NULL, NULL, c, 0);                          /* QUAD_STRIP -> TRIANGLE_STRIP */

    /* --- Restore State --- */
    RP(glPopMatrix, F_v)();                                      /* Modelview */
    RP(glMatrixMode, F_ui)(0x1701); RP(glPopMatrix, F_v)();      /* Projection */
    RP(glMatrixMode, F_ui)(0x1702); RP(glPopMatrix, F_v)();      /* Texture */
    RP(glMatrixMode, F_ui)((unsigned)old_mm);

    RP(glTexEnvf, F_texenv)(0x2300, 0x2200, (float)old_env);
    RP(glBindTexture, F_uiui)(0x0DE1, (unsigned)old_tex);
    RP(glColor4f, F_ffff)(old_col[0], old_col[1], old_col[2], old_col[3]);
    RP(glMultiTexCoord4f, F_utex)(0x84C0, old_tc[0], old_tc[1], old_tc[2], old_tc[3]);
    if (!was_tex)  RP(glDisable, F_ui)(0x0DE1);
    if (was_depth) RP(glEnable, F_ui)(0x0B71);
    if (was_light) RP(glEnable, F_ui)(0x0B50);
    if (was_cull)  RP(glEnable, F_ui)(0x0B44);
    if (was_fog)   RP(glEnable, F_ui)(0x0B60);
}

/* ---------- glBitmap (emulat): cate un quad per serie orizontala de pixeli setati ---------- */
static float bm_floor(float f) { int i = (int)f; return (float)(f < (float)i ? i - 1 : i); }

GLC void glBitmap(int w, int h, float xorig, float yorig, float xmove, float ymove, const unsigned char* bm) {
    im_init();
    gl_warn_es("glBitmap");
    if (IM.in_begin || IM.rec_id) return;              /* nu se inregistreaza in display lists */
    if (!g_rvalid) return;

    if (w > 0 && h > 0 && bm) {
        int al = 4; RP(glGetIntegerv, F_geti)(0x0CF5, &al);          /* GL_UNPACK_ALIGNMENT */
        if (al < 1) al = 4;
        int stride = ((((w + 7) / 8) + al - 1) / al) * al;
        size_t maxq = (size_t)h * (size_t)((w + 1) / 2);
        if (maxq > 16384) maxq = 16384;                               /* limita im_draw: 65536 varfuri */
        float* v = malloc(maxq * 12 * sizeof(float));
        size_t nq = 0;
        float bx = bm_floor(g_rasterx - xorig), by = bm_floor(g_rastery - yorig);

        for (int r = 0; r < h && v; r++) {
            const unsigned char* row = bm + (size_t)r * stride;       /* randurile sunt de jos in sus */
            int x = 0;
            while (x < w) {
                if (!((row[x >> 3] >> (7 - (x & 7))) & 1)) { x++; continue; }
                int x0 = x;
                while (x < w && ((row[x >> 3] >> (7 - (x & 7))) & 1)) x++;
                if (nq >= maxq) break;
                float* q = v + nq * 12;
                float xa = bx + x0, xb = bx + x, ya = by + r, yb = ya + 1.0f;
                q[0] = xa; q[1] = ya; q[2] = 0;   q[3] = xb; q[4] = ya; q[5] = 0;
                q[6] = xb; q[7] = yb; q[8] = 0;   q[9] = xa; q[10] = yb; q[11] = 0;
                nq++;
            }
        }

        if (v && nq) {
            int old_mm = 0x1700, vp[4];
            RP(glGetIntegerv, F_geti)(0x0BA0, &old_mm);               /* MATRIX_MODE */
            RP(glGetIntegerv, F_geti)(0x0BA2, vp);                    /* VIEWPORT */
            int was_tex   = RP(glIsEnabled, F_isen)(0x0DE1);
            int was_depth = RP(glIsEnabled, F_isen)(0x0B71);
            int was_light = RP(glIsEnabled, F_isen)(0x0B50);
            int was_cull  = RP(glIsEnabled, F_isen)(0x0B44);
            int was_fog   = RP(glIsEnabled, F_isen)(0x0B60);
            if (was_tex)   RP(glDisable, F_ui)(0x0DE1);
            if (was_depth) RP(glDisable, F_ui)(0x0B71);
            if (was_light) RP(glDisable, F_ui)(0x0B50);
            if (was_cull)  RP(glDisable, F_ui)(0x0B44);
            if (was_fog)   RP(glDisable, F_ui)(0x0B60);

            RP(glMatrixMode, F_ui)(0x1701);                           /* PROJECTION */
            RP(glPushMatrix, F_v)(); RP(glLoadIdentity, F_v)();
            RP(glOrthof, F_6f)((float)vp[0], (float)(vp[0] + vp[2]), (float)vp[1], (float)(vp[1] + vp[3]), -1.0f, 1.0f);
            RP(glMatrixMode, F_ui)(0x1700);                           /* MODELVIEW */
            RP(glPushMatrix, F_v)(); RP(glLoadIdentity, F_v)();

            im_draw(7, (int)(nq * 4), v, NULL, NULL, NULL, 0);        /* GL_QUADS -> triunghiuri */

            RP(glPopMatrix, F_v)();
            RP(glMatrixMode, F_ui)(0x1701); RP(glPopMatrix, F_v)();
            RP(glMatrixMode, F_ui)((unsigned)old_mm);
            if (was_tex)   RP(glEnable, F_ui)(0x0DE1);
            if (was_depth) RP(glEnable, F_ui)(0x0B71);
            if (was_light) RP(glEnable, F_ui)(0x0B50);
            if (was_cull)  RP(glEnable, F_ui)(0x0B44);
            if (was_fog)   RP(glEnable, F_ui)(0x0B60);
        }
        free(v);
    }
    g_rasterx += xmove; g_rastery += ymove;                           /* avans raster, ca in GL */
}

/* ---------- Display Lists Execution ---------- */
static void list_exec(unsigned id, int depth) {
    if (id == 0 || id >= gl_lists_cap || depth > 32) return;
    gl_list* L = &gl_lists[id];
    for (int i = 0; i < L->n; i++) {
        gl_cmd* c = &L->c[i];
        switch (c->op) {
        case OP_DRAW: { gl_block* b = c->p; im_draw(b->mode, b->n, b->pos, b->nrm, b->col, b->tex, b->preserve_arrays); break; }
        case OP_NORMAL:    RP(glNormal3f, F_fff)(c->f[0], c->f[1], c->f[2]); break;
        case OP_COLOR:     RP(glColor4f, F_ffff)(c->f[0], c->f[1], c->f[2], c->f[3]); break;
        case OP_TEXCOORD:  RP(glMultiTexCoord4f, F_utex)(0x84C0, c->f[0], c->f[1], 0.0f, 1.0f); break;
        case OP_MATERIAL:  RP(glMaterialfv, F_mat)(c->u[0], c->u[1], c->f); break;
        case OP_LIGHT:     RP(glLightfv, F_mat)(c->u[0], c->u[1], c->f); break;
        case OP_SHADE:     RP(glShadeModel, F_ui)(c->u[0]); break;
        case OP_FRONTFACE: cc_note_front(c->u[0]); cc_setfront(); break;
        case OP_ENABLE:  if (!cm_cap(c->u[0], 1)) { RP(glEnable, F_ui)(c->u[0]);  dsb_on_enable(c->u[0], 1); } break;
        case OP_DISABLE: if (!cm_cap(c->u[0], 0)) { RP(glDisable, F_ui)(c->u[0]); dsb_on_enable(c->u[0], 0); } break;
        case OP_BINDTEX:   RP(glBindTexture, F_uiui)(c->u[0], c->u[1]); break;
        case OP_PUSH:      RP(glPushMatrix, F_v)(); break;
        case OP_POP:       RP(glPopMatrix, F_v)(); break;
        case OP_IDENT:     RP(glLoadIdentity, F_v)(); break;
        case OP_TRANSLATE: RP(glTranslatef, F_fff)(c->f[0], c->f[1], c->f[2]); break;
        case OP_SCALE:     RP(glScalef, F_fff)(c->f[0], c->f[1], c->f[2]); break;
        case OP_ROTATE:    RP(glRotatef, F_ffff)(c->f[0], c->f[1], c->f[2], c->f[3]); break;
        case OP_MULT:      RP(glMultMatrixf, F_pf)(c->f); break;
        case OP_WINDOWPOS: glWindowPos3f(c->f[0], c->f[1], c->f[2]); break;
        case OP_RASTERPOS: glRasterPos4f(c->f[0], c->f[1], c->f[2], c->f[3]); break;
        case OP_CALL:      list_exec(c->u[0], depth + 1); break;
        default: break;
        }
    }
}

GLC unsigned glGenLists(int range) {
    if (range <= 0) return 0;
    pthread_mutex_lock(&gl_lmu);
    unsigned first = gl_next_list;
    gl_next_list += (unsigned)range;
    lists_ensure(gl_next_list);
    pthread_mutex_unlock(&gl_lmu);
    return first;
}

GLC void glDeleteLists(unsigned first, int range) {
    pthread_mutex_lock(&gl_lmu);
    for (unsigned i = first; range > 0 && i < first + (unsigned)range && i < gl_lists_cap; i++) list_clear(&gl_lists[i]);
    pthread_mutex_unlock(&gl_lmu);
}

GLC unsigned char glIsList(unsigned id) { return id && id < gl_lists_cap && gl_lists[id].used; }

GLC void glNewList(unsigned id, unsigned mode) {
    im_init();
    gl_warn_es("glNewList");
    if (!id) return;
    pthread_mutex_lock(&gl_lmu);
    if (id >= gl_next_list) gl_next_list = id + 1;
    lists_ensure(gl_next_list);
    list_clear(&gl_lists[id]);
    gl_lists[id].used = 1;
    pthread_mutex_unlock(&gl_lmu);
    IM.rec_id = id; IM.rec_mode = (int)mode;
}

GLC void glEndList(void) { IM.rec_id = 0; IM.rec_mode = 0; }

GLC void glCallList(unsigned id) {
    if (IM.rec_id) { rec_cmd(OP_CALL)->u[0] = id; if (REC_SKIP()) return; }
    list_exec(id, 0);
}

GLC void glCallLists(int n, unsigned type, const void* lists) {
    for (int i = 0; i < n; i++) {
        unsigned id = 0;
        switch (type) {
        case 0x1401: id = ((const unsigned char*)lists)[i]; break;     /* UNSIGNED_BYTE */
        case 0x1400: id = ((const signed char*)lists)[i]; break;       /* BYTE */
        case 0x1403: id = ((const unsigned short*)lists)[i]; break;    /* UNSIGNED_SHORT */
        case 0x1402: id = ((const short*)lists)[i]; break;             /* SHORT */
        case 0x1404: case 0x1405: id = ((const unsigned*)lists)[i]; break;   /* INT / UNSIGNED_INT */
        case 0x1406: id = (unsigned)((const float*)lists)[i]; break;   /* FLOAT */
        default: return;
        }
        glCallList(id);
    }
}

/* ---------- Double Variants / Desktop Enums ---------- */
GLC void glFrustum(double l, double r, double b, double t, double n, double f) {
    RP(glFrustumf, F_6f)((float)l, (float)r, (float)b, (float)t, (float)n, (float)f);
}
GLC void glOrtho(double l, double r, double b, double t, double n, double f) {
    RP(glOrthof, F_6f)((float)l, (float)r, (float)b, (float)t, (float)n, (float)f);
}
GLC void glTranslated(double x, double y, double z) { glTranslatef((float)x, (float)y, (float)z); }
GLC void glScaled(double x, double y, double z)     { glScalef((float)x, (float)y, (float)z); }
GLC void glRotated(double a, double x, double y, double z) { glRotatef((float)a, (float)x, (float)y, (float)z); }
GLC void glLoadMatrixd(const double* m) {
    float f[16];
    for (int i = 0; i < 16; i++) f[i] = (float)m[i];
    RP(glLoadMatrixf, F_pf)(f);
}
GLC void glMultMatrixd(const double* m) {
    float f[16];
    for (int i = 0; i < 16; i++) f[i] = (float)m[i];
    glMultMatrixf(f);
}
GLC void glClearDepth(double d)               { RP(glClearDepthf, F_f)((float)d); }
GLC void glDepthRange(double n, double f)     { RP(glDepthRangef, F_ff)((float)n, (float)f); }
GLC void glClipPlane(unsigned plane, const double* e) {
    float f[4] = { (float)e[0], (float)e[1], (float)e[2], (float)e[3] };
    RP(glClipPlanef, F_clip)(plane, f);
}
GLC void glLightModeli(unsigned pname, int v) { RP(glLightModelf, F_uif)(pname, (float)v); }
GLC void glGetDoublev(unsigned pname, double* out) {
    int value;
    if (cva_get(pname, &value)) {
        if (!IM.in_begin) *out = (double)value;
        return;
    }
    if (pname == 0x8068) pname = 0x8069;                           /* GL_TEXTURE_BINDING_1D -> GL_TEXTURE_BINDING_2D */
    float f[16] = { 0 };
    int n = 1;
    if (pname == 0x0BA6 || pname == 0x0BA7 || pname == 0x0BA8) n = 16;     /* MODELVIEW/PROJECTION/TEXTURE matrix */
    else if (pname == 0x0BA2 || pname == 0x0C22) n = 4;                    /* VIEWPORT / CURRENT_COLOR-like */
    RP(glGetFloatv, F_getf)(pname, f);
    for (int i = 0; i < n; i++) out[i] = f[i];
}

/* ===== GL 1.3 / 1.4 fixed-function gaps ===== */
/* MultiTexCoord: doar unitatea 0 se înregistrează în glBegin/End; restul merg direct la ES1 */
GLC void glMultiTexCoord2f(unsigned t, float s, float v) {
    if (t == 0x84C0) { glTexCoord2f(s, v); return; }
    RP(glMultiTexCoord4f, F_utex)(t, s, v, 0.0f, 1.0f);
}
GLC void glMultiTexCoord1f(unsigned t, float s)                      { glMultiTexCoord2f(t, s, 0.0f); }
GLC void glMultiTexCoord3f(unsigned t, float s, float v, float r)    { (void)r; glMultiTexCoord2f(t, s, v); }
GLC void glMultiTexCoord4fv(unsigned t, const float* v) {
    if (t == 0x84C0) { glTexCoord2f(v[0], v[1]); return; }
    RP(glMultiTexCoord4f, F_utex)(t, v[0], v[1], v[2], v[3]);
}
GLC void glMultiTexCoord1fv(unsigned t, const float* v) { glMultiTexCoord2f(t, v[0], 0.0f); }
GLC void glMultiTexCoord2fv(unsigned t, const float* v) { glMultiTexCoord2f(t, v[0], v[1]); }
GLC void glMultiTexCoord3fv(unsigned t, const float* v) { glMultiTexCoord2f(t, v[0], v[1]); }
GLC void glMultiTexCoord1d(unsigned t, double s)                    { glMultiTexCoord2f(t, (float)s, 0.0f); }
GLC void glMultiTexCoord2d(unsigned t, double s, double v)          { glMultiTexCoord2f(t, (float)s, (float)v); }
GLC void glMultiTexCoord3d(unsigned t, double s, double v, double r){ (void)r; glMultiTexCoord2f(t, (float)s, (float)v); }
GLC void glMultiTexCoord4d(unsigned t, double s, double v, double r, double q){ (void)r;(void)q; glMultiTexCoord2f(t, (float)s, (float)v); }
GLC void glMultiTexCoord1dv(unsigned t, const double* v) { glMultiTexCoord2f(t, (float)v[0], 0.0f); }
GLC void glMultiTexCoord2dv(unsigned t, const double* v) { glMultiTexCoord2f(t, (float)v[0], (float)v[1]); }
GLC void glMultiTexCoord3dv(unsigned t, const double* v) { glMultiTexCoord2f(t, (float)v[0], (float)v[1]); }
GLC void glMultiTexCoord4dv(unsigned t, const double* v) { glMultiTexCoord2f(t, (float)v[0], (float)v[1]); }
GLC void glMultiTexCoord1i(unsigned t, int s)                 { glMultiTexCoord2f(t, (float)s, 0.0f); }
GLC void glMultiTexCoord2i(unsigned t, int s, int v)          { glMultiTexCoord2f(t, (float)s, (float)v); }
GLC void glMultiTexCoord3i(unsigned t, int s, int v, int r)   { (void)r; glMultiTexCoord2f(t, (float)s, (float)v); }
GLC void glMultiTexCoord4i(unsigned t, int s, int v, int r, int q){ (void)r;(void)q; glMultiTexCoord2f(t, (float)s, (float)v); }
GLC void glMultiTexCoord1iv(unsigned t, const int* v) { glMultiTexCoord2f(t, (float)v[0], 0.0f); }
GLC void glMultiTexCoord2iv(unsigned t, const int* v) { glMultiTexCoord2f(t, (float)v[0], (float)v[1]); }
GLC void glMultiTexCoord3iv(unsigned t, const int* v) { glMultiTexCoord2f(t, (float)v[0], (float)v[1]); }
GLC void glMultiTexCoord4iv(unsigned t, const int* v) { glMultiTexCoord2f(t, (float)v[0], (float)v[1]); }
GLC void glMultiTexCoord1s(unsigned t, short s)                 { glMultiTexCoord2f(t, s, 0.0f); }
GLC void glMultiTexCoord2s(unsigned t, short s, short v)        { glMultiTexCoord2f(t, s, v); }
GLC void glMultiTexCoord3s(unsigned t, short s, short v, short r){ (void)r; glMultiTexCoord2f(t, s, v); }
GLC void glMultiTexCoord4s(unsigned t, short s, short v, short r, short q){ (void)r;(void)q; glMultiTexCoord2f(t, s, v); }
GLC void glMultiTexCoord1sv(unsigned t, const short* v) { glMultiTexCoord2f(t, v[0], 0.0f); }
GLC void glMultiTexCoord2sv(unsigned t, const short* v) { glMultiTexCoord2f(t, v[0], v[1]); }
GLC void glMultiTexCoord3sv(unsigned t, const short* v) { glMultiTexCoord2f(t, v[0], v[1]); }
GLC void glMultiTexCoord4sv(unsigned t, const short* v) { glMultiTexCoord2f(t, v[0], v[1]); }

/* FogCoord / SecondaryColor: ES1 nu le are -> ignorate (rar folosite) */
GLC void glFogCoordf(float c)                  { (void)c; }
GLC void glFogCoordd(double c)                 { (void)c; }
GLC void glFogCoordfv(const float* c)          { (void)c; }
GLC void glFogCoorddv(const double* c)         { (void)c; }
GLC void glFogCoordPointer(unsigned t, int s, const void* p) { (void)t; (void)s; (void)p; }
GLC void glSecondaryColor3b(signed char r, signed char g, signed char b)      { (void)r;(void)g;(void)b; }
GLC void glSecondaryColor3d(double r, double g, double b)                     { (void)r;(void)g;(void)b; }
GLC void glSecondaryColor3f(float r, float g, float b)                        { (void)r;(void)g;(void)b; }
GLC void glSecondaryColor3i(int r, int g, int b)                              { (void)r;(void)g;(void)b; }
GLC void glSecondaryColor3s(short r, short g, short b)                        { (void)r;(void)g;(void)b; }
GLC void glSecondaryColor3ub(unsigned char r, unsigned char g, unsigned char b){ (void)r;(void)g;(void)b; }
GLC void glSecondaryColor3ui(unsigned r, unsigned g, unsigned b)              { (void)r;(void)g;(void)b; }
GLC void glSecondaryColor3us(unsigned short r, unsigned short g, unsigned short b){ (void)r;(void)g;(void)b; }
GLC void glSecondaryColor3bv(const signed char* v)   { (void)v; }
GLC void glSecondaryColor3dv(const double* v)        { (void)v; }
GLC void glSecondaryColor3fv(const float* v)         { (void)v; }
GLC void glSecondaryColor3iv(const int* v)           { (void)v; }
GLC void glSecondaryColor3sv(const short* v)         { (void)v; }
GLC void glSecondaryColor3ubv(const unsigned char* v){ (void)v; }
GLC void glSecondaryColor3uiv(const unsigned* v)     { (void)v; }
GLC void glSecondaryColor3usv(const unsigned short* v){ (void)v; }
GLC void glSecondaryColorPointer(int s, unsigned t, int st, const void* p) { (void)s;(void)t;(void)st;(void)p; }

/* PointParameter int: ES1 are doar float */
GLC void glPointParameteri(unsigned p, int v) {
    RP(glPointParameterf, F_uif)(p, (float)v);
}
GLC void glPointParameteriv(unsigned p, const int* v) {
    float f = (float)v[0];
    RP(glPointParameterfv, F_clip)(p, &f);
}

/* Transpose matrices: transpune pe CPU, apoi Load/Mult */
static void tr16(const double* s, float* d) { for (int i=0;i<4;i++) for (int j=0;j<4;j++) d[i*4+j]=(float)s[j*4+i]; }
static void tr16f(const float* s, float* d) { for (int i=0;i<4;i++) for (int j=0;j<4;j++) d[i*4+j]=s[j*4+i]; }
GLC void glLoadTransposeMatrixf(const float* m)  { float t[16]; tr16f(m, t); RP(glLoadMatrixf, F_pf)(t); }
GLC void glMultTransposeMatrixf(const float* m)  { float t[16]; tr16f(m, t); glMultMatrixf(t); }
GLC void glLoadTransposeMatrixd(const double* m) { float t[16]; tr16(m, t);  RP(glLoadMatrixf, F_pf)(t); }
GLC void glMultTransposeMatrixd(const double* m) { float t[16]; tr16(m, t);  glMultMatrixf(t); }

GLC void glCompressedTexImage2D(unsigned, int, unsigned, int, int, int, int, const void*);
GLC void glCompressedTexSubImage2D(unsigned, int, int, int, int, int, unsigned, int, const void*);

/* ===== glColorMaterial emulation (ES1.1: only AMBIENT_AND_DIFFUSE is native) ===== */
typedef void (*CM_mfv)(unsigned, unsigned, const float*);
typedef void (*CM_en)(unsigned);

static unsigned cm_mode = 0x1602;   /* GL_AMBIENT_AND_DIFFUSE */
static int      cm_on;              /* app's GL_COLOR_MATERIAL state */
static float    cm_col[4] = { 1, 1, 1, 1 };

static int cm_emul(void) { return cm_on && cm_mode != 0x1602; }

static void cm_native(void) {       /* sync the native enable with the state above */
    if (cm_on && cm_mode == 0x1602) RP(glEnable, CM_en)(0x0B57);
    else                            RP(glDisable, CM_en)(0x0B57);
}

static void cm_apply(void) {        /* push current color into the tracked material */
    if (!cm_emul()) return;
    RP(glMaterialfv, CM_mfv)(0x0408, cm_mode, cm_col);
}

GLC void glColorMaterial(unsigned face, unsigned mode) {
    (void)face;
    if (mode != 0x1200 && mode != 0x1201 && mode != 0x1202 &&
        mode != 0x1600 && mode != 0x1602) return;   /* GL_INVALID_ENUM */
    cm_mode = mode;
    cm_native();
    cm_apply();
}

/* call from your existing glEnable/glDisable when cap == 0x0B57; return 1 = handled */
static int cm_cap(unsigned cap, int on) {
    if (ev_cap(cap, on)) return 1;
    if (cap != 0x0B57) return 0;
    cm_on = on;
    cm_native();
    cm_apply();
    return 1;
}

/* ---------- glFog int variants (ES1 has only f/fv/x) ---------- */
typedef void (*F_uif_)(unsigned, float);
typedef void (*F_fogfv)(unsigned, const float*);

GLC void glFogi(unsigned pname, int v) {
    /* GL_FOG_MODE (0x0B65) takes an enum: pass as-is; others convert */
    RP(glFogf, F_uif_)(pname, (float)v);
}
GLC void glFogiv(unsigned pname, const int* v) {
    if (pname == 0x0B66) {                       /* GL_FOG_COLOR: ints map [-MAXINT,MAXINT] -> [-1,1] */
        float c[4];
        for (int i = 0; i < 4; i++) c[i] = (float)((double)v[i] / 2147483647.0);
        RP(glFogfv, F_fogfv)(pname, c);
    } else {
        RP(glFogf, F_uif_)(pname, (float)v[0]);
    }
}

typedef void (*X_b)(unsigned char);
typedef void (*X_4b)(unsigned char, unsigned char, unsigned char, unsigned char);
typedef void (*X_4i)(int, int, int, int);
typedef void (*X_get3)(unsigned, unsigned, float*);
typedef void (*X_rp)(int, int, int, int, unsigned, unsigned, void*);

GLC void glTexGenf(unsigned coord, unsigned pname, float v);
GLC void glTexGenfv(unsigned coord, unsigned pname, const float* v);

static int   x_es1(void) { return T_glx.ctx && T_glx.ctx->ver == 1; }
static int   x_gi(unsigned p) { int v = 0;   RP(glGetIntegerv, F_geti)(p, &v); return v; }
static float x_gf(unsigned p) { float v = 0; RP(glGetFloatv, F_getf)(p, &v);   return v; }

/* ======================= glPushAttrib / glPopAttrib ======================= */
#define XA_CURRENT     0x00000001u
#define XA_POINT       0x00000002u
#define XA_LINE        0x00000004u
#define XA_POLYGON     0x00000008u
#define XA_LIGHTING    0x00000040u
#define XA_FOG         0x00000080u
#define XA_DEPTH       0x00000100u
#define XA_STENCIL     0x00000400u
#define XA_VIEWPORT    0x00000800u
#define XA_TRANSFORM   0x00001000u
#define XA_ENABLE      0x00002000u
#define XA_COLOR       0x00004000u
#define XA_TEXTURE     0x00040000u
#define XA_SCISSOR     0x00080000u
#define XA_MULTISAMPLE 0x20000000u

static const struct { unsigned cap, grp; } x_caps[] = {
    /* generic (also valid in ES2/3) */
    { 0x0B71, XA_ENABLE | XA_DEPTH },   { 0x0BE2, XA_ENABLE | XA_COLOR },   { 0x0B44, XA_ENABLE | XA_POLYGON },
    { 0x0C11, XA_ENABLE | XA_SCISSOR }, { 0x0B90, XA_ENABLE | XA_STENCIL }, { 0x0BD0, XA_ENABLE | XA_COLOR },
    { 0x8037, XA_ENABLE | XA_POLYGON },
    /* fixed-function: ES1 only */
    { 0x0BC0, XA_ENABLE | XA_COLOR },   { 0x0BF2, XA_ENABLE | XA_COLOR },
    { 0x0B50, XA_ENABLE | XA_LIGHTING },{ 0x0B57, XA_ENABLE | XA_LIGHTING },
    { 0x4000, XA_ENABLE | XA_LIGHTING },{ 0x4001, XA_ENABLE | XA_LIGHTING },{ 0x4002, XA_ENABLE | XA_LIGHTING },
    { 0x4003, XA_ENABLE | XA_LIGHTING },{ 0x4004, XA_ENABLE | XA_LIGHTING },{ 0x4005, XA_ENABLE | XA_LIGHTING },
    { 0x4006, XA_ENABLE | XA_LIGHTING },{ 0x4007, XA_ENABLE | XA_LIGHTING },
    { 0x0B60, XA_ENABLE | XA_FOG },     { 0x0B20, XA_ENABLE | XA_LINE },    { 0x0B10, XA_ENABLE | XA_POINT },
    { 0x0DE1, XA_ENABLE | XA_TEXTURE },
    { 0x0BA1, XA_ENABLE | XA_TRANSFORM },{ 0x803A, XA_ENABLE | XA_TRANSFORM },
    { 0x3000, XA_ENABLE | XA_TRANSFORM },{ 0x3001, XA_ENABLE | XA_TRANSFORM },{ 0x3002, XA_ENABLE | XA_TRANSFORM },
    { 0x3003, XA_ENABLE | XA_TRANSFORM },{ 0x3004, XA_ENABLE | XA_TRANSFORM },{ 0x3005, XA_ENABLE | XA_TRANSFORM },
    { 0x809D, XA_ENABLE | XA_MULTISAMPLE },
};
#define XA_NGEN 7
#define XA_NCAP 32
_Static_assert(sizeof x_caps / sizeof x_caps[0] == XA_NCAP, "x_caps size");

static const struct { unsigned pn; int off; } x_lp[] = {
    { 0x1200, 0 }, { 0x1201, 4 }, { 0x1202, 8 }, { 0x1203, 12 }, { 0x1204, 16 },
    { 0x1205, 19 }, { 0x1206, 20 }, { 0x1207, 21 }, { 0x1208, 22 }, { 0x1209, 23 },
};
static const unsigned x_mp[5] = { 0x1200, 0x1201, 0x1202, 0x1600, 0x1601 };

typedef struct {
    unsigned mask;
    unsigned char en[XA_NCAP];
    float col[4], nrm[3], tc[2];
    int dfunc, dmask; float dclear;
    int bsrc, bdst, afunc; float aref, clr[4]; int cmask[4];
    int cull; float pofs[2], lw, ps;
    int vp[4], sc[4]; float dr[2];
    int mm, shade, tex;
    float fogd, fogs, foge, fogc[4]; int fogm;
    float lma[4]; int lm2;
    float lt[8][24], mat[5][4];
} x_attr;

static __thread x_attr* x_as[32];
static __thread int x_an;

static int x_isen(unsigned cap) {
    if (cap == 0x0B57) return cm_on;                 /* tracked by glColorMaterial emulation */
    return RP(glIsEnabled, F_isen)(cap) ? 1 : 0;
}

GLC void glPushAttrib(unsigned mask) {
    im_init();
    if (IM.rec_id || IM.in_begin || x_an >= 32) return;   /* not recordable in display lists */
    x_attr* a = calloc(1, sizeof *a);
    if (!a) return;
    int es1 = x_es1(), nc = es1 ? XA_NCAP : XA_NGEN;
    a->mask = mask;
    for (int i = 0; i < nc; i++)
        if (x_caps[i].grp & mask) a->en[i] = (unsigned char)x_isen(x_caps[i].cap);

    if (mask & XA_CURRENT) { memcpy(a->col, IM.cc, sizeof a->col); memcpy(a->nrm, IM.cn, sizeof a->nrm); memcpy(a->tc, IM.ct, sizeof a->tc); }
    if (mask & XA_DEPTH) { a->dfunc = x_gi(0x0B74); a->dmask = x_gi(0x0B72); a->dclear = x_gf(0x0B73); }
    if (mask & XA_COLOR) {
        RP(glGetFloatv, F_getf)(0x0C22, a->clr);
        RP(glGetIntegerv, F_geti)(0x0C23, a->cmask);
        if (es1) { a->bsrc = x_gi(0x0BE1); a->bdst = x_gi(0x0BE0); a->afunc = x_gi(0x0BC1); a->aref = x_gf(0x0BC2); }
    }
    if (mask & XA_POLYGON) { a->cull = x_gi(0x0B45); a->pofs[0] = x_gf(0x8038); a->pofs[1] = x_gf(0x2A00); }
    if (mask & XA_LINE)    a->lw = x_gf(0x0B21);
    if ((mask & XA_POINT) && es1) a->ps = x_gf(0x0B11);
    if (mask & XA_VIEWPORT) { RP(glGetIntegerv, F_geti)(0x0BA2, a->vp); RP(glGetFloatv, F_getf)(0x0B70, a->dr); }
    if (mask & XA_SCISSOR)  RP(glGetIntegerv, F_geti)(0x0C10, a->sc);
    if (mask & XA_TEXTURE)  a->tex = x_gi(0x8069);
    if (es1) {
        if (mask & XA_TRANSFORM) a->mm = x_gi(0x0BA0);
        if (mask & XA_FOG) {
            a->fogd = x_gf(0x0B62); a->fogs = x_gf(0x0B63); a->foge = x_gf(0x0B64);
            a->fogm = x_gi(0x0B65); RP(glGetFloatv, F_getf)(0x0B66, a->fogc);
        }
        if (mask & XA_LIGHTING) {
            a->shade = x_gi(0x0B54); a->lm2 = x_gi(0x0B52);
            RP(glGetFloatv, F_getf)(0x0B53, a->lma);
            for (int i = 0; i < 8; i++)
                for (int k = 0; k < 10; k++)
                    RP(glGetLightfv, X_get3)(0x4000 + i, x_lp[k].pn, a->lt[i] + x_lp[k].off);
            for (int k = 0; k < 5; k++) RP(glGetMaterialfv, X_get3)(0x0408, x_mp[k], a->mat[k]);
        }
    }
    x_as[x_an++] = a;
}

GLC void glPopAttrib(void) {
    if (IM.rec_id || x_an <= 0) return;
    x_attr* a = x_as[--x_an];
    unsigned m = a->mask;
    int es1 = x_es1(), nc = es1 ? XA_NCAP : XA_NGEN;

    for (int i = 0; i < nc; i++)                      /* our glEnable/glDisable handle xlat/color-material/dsb */
        if (x_caps[i].grp & m) { if (a->en[i]) glEnable(x_caps[i].cap); else glDisable(x_caps[i].cap); }

    if (m & XA_CURRENT) { glColor4f(a->col[0], a->col[1], a->col[2], a->col[3]); glNormal3f(a->nrm[0], a->nrm[1], a->nrm[2]); glTexCoord2f(a->tc[0], a->tc[1]); }
    if (m & XA_DEPTH) { RP(glDepthFunc, F_ui)((unsigned)a->dfunc); RP(glDepthMask, X_b)((unsigned char)a->dmask); RP(glClearDepthf, F_f)(a->dclear); }
    if (m & XA_COLOR) {
        RP(glClearColor, F_ffff)(a->clr[0], a->clr[1], a->clr[2], a->clr[3]);
        RP(glColorMask, X_4b)((unsigned char)a->cmask[0], (unsigned char)a->cmask[1], (unsigned char)a->cmask[2], (unsigned char)a->cmask[3]);
        if (es1) {
            RP(glBlendFunc, F_uiui)((unsigned)a->bsrc, (unsigned)a->bdst);
            RP(glAlphaFunc, F_uif)((unsigned)a->afunc, a->aref);
        }
    }
    if (m & XA_TEXTURE)  RP(glBindTexture, F_uiui)(0x0DE1, (unsigned)a->tex);

    if (es1) {
        if (m & XA_FOG) {
            float fm = (float)a->fogm;
            RP(glFogfv, F_clip)(0x0B62, &a->fogd); RP(glFogfv, F_clip)(0x0B63, &a->fogs);
            RP(glFogfv, F_clip)(0x0B64, &a->foge); RP(glFogfv, F_clip)(0x0B65, &fm);
            RP(glFogfv, F_clip)(0x0B66, a->fogc);
        }
        if (m & XA_LIGHTING) {
            float two = (float)a->lm2;
            int mm0 = x_gi(0x0BA0);
            RP(glShadeModel, F_ui)((unsigned)a->shade);
            RP(glLightModelfv, F_clip)(0x0B53, a->lma); RP(glLightModelfv, F_clip)(0x0B52, &two);
            for (int k = 0; k < 5; k++) RP(glMaterialfv, F_mat)(0x0408, x_mp[k], a->mat[k]);
            /* saved position/direction are in eye space: set them under an identity modelview */
            RP(glMatrixMode, F_ui)(0x1700); RP(glPushMatrix, F_v)(); RP(glLoadIdentity, F_v)();
            for (int i = 0; i < 8; i++)
                for (int k = 0; k < 10; k++)
                    RP(glLightfv, F_mat)(0x4000 + i, x_lp[k].pn, a->lt[i] + x_lp[k].off);
            RP(glPopMatrix, F_v)(); RP(glMatrixMode, F_ui)((unsigned)mm0);
        }
        if (m & XA_TRANSFORM) RP(glMatrixMode, F_ui)((unsigned)a->mm);
    }
    free(a);
}

/* ---------- Client attribs ---------- */
typedef struct { unsigned mask; unsigned char en[4]; int ua, pa; } x_cattr;
static __thread x_cattr x_cs[16];
static __thread int x_cn;
static const unsigned x_ccaps[4] = { 0x8074, 0x8075, 0x8076, 0x8078 };

GLC void glPushClientAttrib(unsigned mask) {
    if (x_cn >= 16) return;
    x_cattr* c = &x_cs[x_cn++];
    c->mask = mask;
    if (mask & 0x2) for (int i = 0; i < 4; i++) c->en[i] = RP(glIsEnabled, F_isen)(x_ccaps[i]) ? 1 : 0;
    if (mask & 0x1) { c->ua = x_gi(0x0CF5); c->pa = x_gi(0x0D05); }
}
GLC void glPopClientAttrib(void) {
    if (x_cn <= 0) return;
    x_cattr* c = &x_cs[--x_cn];
    if (c->mask & 0x2)
        for (int i = 0; i < 4; i++)
            (c->en[i] ? RP(glEnableClientState, F_ui) : RP(glDisableClientState, F_ui))(x_ccaps[i]);
    if (c->mask & 0x1) {
        RP(glPixelStorei, F_uiui)(0x0CF5, (unsigned)c->ua);
        RP(glPixelStorei, F_uiui)(0x0D05, (unsigned)c->pa);
    }
}

/* ======================= Vertex array tracking + glArrayElement ======================= */
typedef struct { int size, stride, vbo; unsigned type; const char* p; } x_ap;
static __thread x_ap x_av, x_an_, x_ac, x_at;

GLC void glVertexPointer(int s, unsigned t, int st, const void* p) {
    x_av = (x_ap){ s, st, x_gi(0x8894), t, p };
    RP(glVertexPointer, F_vptr)(s, t, st, p);
}
GLC void glColorPointer(int s, unsigned t, int st, const void* p) {
    x_ac = (x_ap){ s, st, x_gi(0x8894), t, p };
    RP(glColorPointer, F_vptr)(s, t, st, p);
}
GLC void glTexCoordPointer(int s, unsigned t, int st, const void* p) {
    x_at = (x_ap){ s, st, x_gi(0x8894), t, p };
    RP(glTexCoordPointer, F_vptr)(s, t, st, p);
}
GLC void glNormalPointer(unsigned t, int st, const void* p) {
    x_an_ = (x_ap){ 3, st, x_gi(0x8894), t, p };
    RP(glNormalPointer, F_nptr)(t, st, p);
}

static int x_tsz(unsigned t) {
    switch (t) {
    case 0x1400: case 0x1401: return 1;
    case 0x1402: case 0x1403: return 2;
    case 0x140A: return 8;
    default: return 4;                                   /* INT, UINT, FLOAT, FIXED */
    }
}
/* read component k; norm=1 maps integer types to [0,1] / [-1,1] like glColor/glNormal */
static float x_rd(unsigned t, const char* q, int k, int norm) {
    float f; double d; int i; unsigned u; short s; unsigned short us; signed char sb; unsigned char ub;
    q += (size_t)k * x_tsz(t);
    switch (t) {
    case 0x1406: memcpy(&f, q, 4);  return f;
    case 0x140A: memcpy(&d, q, 8);  return (float)d;
    case 0x140C: memcpy(&i, q, 4);  return i / 65536.0f;
    case 0x1401: ub = *(const unsigned char*)q; return norm ? ub / 255.0f : (float)ub;
    case 0x1400: sb = *(const signed char*)q;   return norm ? sb / 127.0f : (float)sb;
    case 0x1403: memcpy(&us, q, 2); return norm ? us / 65535.0f : (float)us;
    case 0x1402: memcpy(&s, q, 2);  return norm ? s / 32767.0f : (float)s;
    case 0x1405: memcpy(&u, q, 4);  return norm ? (float)(u / 4294967295.0) : (float)u;
    case 0x1404: memcpy(&i, q, 4);  return norm ? (float)(i / 2147483647.0) : (float)i;
    }
    return 0.0f;
}
static const char* x_el(const x_ap* a, int i) {
    int st = a->stride ? a->stride : a->size * x_tsz(a->type);
    return a->p + (size_t)i * (size_t)st;
}

static void x_arr(unsigned cap, int on) {
    (on ? RP(glEnableClientState, F_ui) : RP(glDisableClientState, F_ui))(cap);
}

GLC void glInterleavedArrays(unsigned fmt, int stride, const void* p) {
    int tc = 0, cs = 0, csz = 0, nrm = 0, vs = 3;
    unsigned ct = 0x1406;
    switch (fmt) {
    case 0x2A20: vs = 2; break;                                              /* V2F */
    case 0x2A21: break;                                                      /* V3F */
    case 0x2A22: cs = 4; ct = 0x1401; csz = 4; vs = 2; break;                /* C4UB_V2F */
    case 0x2A23: cs = 4; ct = 0x1401; csz = 4; break;                        /* C4UB_V3F */
    case 0x2A25: nrm = 1; break;                                             /* N3F_V3F */
    case 0x2A26: cs = 4; csz = 16; nrm = 1; break;                           /* C4F_N3F_V3F */
    case 0x2A27: tc = 2; break;                                              /* T2F_V3F */
    case 0x2A28: tc = 4; vs = 4; break;                                      /* T4F_V4F */
    case 0x2A29: tc = 2; cs = 4; ct = 0x1401; csz = 4; break;                /* T2F_C4UB_V3F */
    case 0x2A2B: tc = 2; nrm = 1; break;                                     /* T2F_N3F_V3F */
    case 0x2A2C: tc = 2; cs = 4; csz = 16; nrm = 1; break;                   /* T2F_C4F_N3F_V3F */
    case 0x2A2D: tc = 4; cs = 4; csz = 16; nrm = 1; vs = 4; break;           /* T4F_C4F_N3F_V4F */
    default: return;                                                         /* C3F_*: unsupported in ES1 */
    }
    int tcb = tc * 4, nb = nrm * 12, total = tcb + csz + nb + vs * 4;
    if (stride <= 0) stride = total;
    const char* b = p;
    x_arr(0x8078, tc); x_arr(0x8076, cs); x_arr(0x8075, nrm); x_arr(0x8074, 1);
    if (tc)  glTexCoordPointer(tc, 0x1406, stride, b);
    if (cs)  glColorPointer(4, ct, stride, b + tcb);
    if (nrm) glNormalPointer(0x1406, stride, b + tcb + csz);
    glVertexPointer(vs, 0x1406, stride, b + tcb + csz + nb);
}

/* ======================= TexGen double ======================= */
GLC void glTexGend(unsigned c, unsigned p, double v) { glTexGenf(c, p, (float)v); }
GLC void glTexGendv(unsigned c, unsigned p, const double* v) {
    float f[4] = { 0, 0, 0, 0 };
    int n = (p == 0x2500) ? 1 : 4;                    /* TEXTURE_GEN_MODE : OBJECT/EYE_PLANE */
    for (int i = 0; i < n; i++) f[i] = (float)v[i];
    glTexGenfv(c, p, f);
}

/* ======================= Getters ======================= */
static int x_i32(float f) { if (f > 1.0f) f = 1.0f; if (f < -1.0f) f = -1.0f; return (int)((double)f * 2147483647.0); }

GLC void glGetMaterialiv(unsigned face, unsigned p, int* out) {
    (void)face;
    float f[4] = { 0, 0, 0, 0 };
    RP(glGetMaterialfv, X_get3)(0x0408, p, f);
    if (p == 0x1601) out[0] = (int)f[0];
    else for (int i = 0; i < 4; i++) out[i] = x_i32(f[i]);
}
GLC void glGetLightiv(unsigned l, unsigned p, int* out) {
    float f[4] = { 0, 0, 0, 0 };
    RP(glGetLightfv, X_get3)(l, p, f);
    if (p >= 0x1200 && p <= 0x1202) for (int i = 0; i < 4; i++) out[i] = x_i32(f[i]);   /* colors: scaled */
    else if (p == 0x1203) for (int i = 0; i < 4; i++) out[i] = (int)f[i];               /* position */
    else if (p == 0x1204) for (int i = 0; i < 3; i++) out[i] = (int)f[i];               /* spot direction */
    else out[0] = (int)f[0];
}
GLC void glGetClipPlane(unsigned plane, double* eq) {
    float f[4] = { 0, 0, 0, 0 };
    RP(glGetClipPlanef, F_getf)(plane, f);            /* ES 1.1 */
    for (int i = 0; i < 4; i++) eq[i] = f[i];
}
GLC void glGetPolygonStipple(unsigned char* m) { memset(m, 0xFF, 128); }

/* ======================= glCopyPixels (GL_COLOR only) ======================= */
GLC void glCopyPixels(int x, int y, int w, int h, unsigned type) {
    if (type != 0x1800 || w <= 0 || h <= 0) return;           /* GL_COLOR */
    unsigned char* b = malloc((size_t)w * h * 4);
    if (!b) return;
    RP(glReadPixels, X_rp)(x, y, w, h, 0x1908, 0x1401, b);
    glDrawPixels(w, h, 0x1908, 0x1401, b);
    free(b);
}

/* ======================= Stubs ======================= */
GLC void glIndexi(int c) { (void)c; }
GLC void glIndexf(float c) { (void)c; }
GLC void glIndexd(double c) { (void)c; }
GLC void glIndexs(short c) { (void)c; }
GLC void glIndexub(unsigned char c) { (void)c; }
GLC void glIndexiv(const int* c) { (void)c; }
GLC void glIndexfv(const float* c) { (void)c; }
GLC void glIndexdv(const double* c) { (void)c; }
GLC void glIndexsv(const short* c) { (void)c; }
GLC void glIndexubv(const unsigned char* c) { (void)c; }
GLC void glClearIndex(float c) { (void)c; }
GLC void glIndexMask(unsigned m) { (void)m; }
GLC void glIndexPointer(unsigned t, int s, const void* p) { (void)t; (void)s; (void)p; }
GLC void glEdgeFlagPointer(int s, const void* p) { (void)s; (void)p; }

GLC int  glRenderMode(unsigned mode) { (void)mode; return 0; }       /* select/feedback: no hits */
GLC void glSelectBuffer(int n, unsigned* b) { (void)n; (void)b; }
GLC void glFeedbackBuffer(int n, unsigned t, float* b) { (void)n; (void)t; (void)b; }
GLC void glPassThrough(float t) { (void)t; }
GLC void glInitNames(void) {}
GLC void glLoadName(unsigned n) { (void)n; }
GLC void glPushName(unsigned n) { (void)n; }
GLC void glPopName(void) {}

GLC void glPixelTransferf(unsigned p, float v) { (void)p; (void)v; }
GLC void glPixelTransferi(unsigned p, int v) { (void)p; (void)v; }
GLC void glPixelMapfv(unsigned m, int n, const float* v) { (void)m; (void)n; (void)v; }
GLC void glPixelMapuiv(unsigned m, int n, const unsigned* v) { (void)m; (void)n; (void)v; }
GLC void glPixelMapusv(unsigned m, int n, const unsigned short* v) { (void)m; (void)n; (void)v; }

GLC void glPrioritizeTextures(int n, const unsigned* t, const float* p) { (void)n; (void)t; (void)p; }
GLC unsigned char glAreTexturesResident(int n, const unsigned* t, unsigned char* r) {
    (void)t;
    for (int i = 0; i < n; i++) r[i] = 1;
    return 1;
}

/* ---------- glLineStipple: ES1 has no line stipple -> state is stored, effect is ignored ---------- */
static __thread int      ev_ls_factor = 1;
static __thread unsigned ev_ls_pattern = 0xFFFF;

GLC void glLineStipple(int factor, unsigned short pattern) {
    ev_ls_factor  = factor < 1 ? 1 : (factor > 256 ? 256 : factor);
    ev_ls_pattern = pattern;
}

/* ---------- Evaluator state ---------- */
#define EV_MAXO 16
typedef struct { int order, k; float a, b; float* d; } ev_m1;
typedef struct { int uo, vo, k; float u1, u2, v1, v2; float* d; } ev_m2;

/* index: 0 COLOR_4, 1 INDEX, 2 NORMAL, 3 TEX_1, 4 TEX_2, 5 TEX_3, 6 TEX_4, 7 VERTEX_3, 8 VERTEX_4
   MAP1: 0x0D90 + idx      MAP2: 0x0DB0 + idx      GL_AUTO_NORMAL: 0x0D80 */
static const unsigned char ev_k[9] = { 4, 1, 3, 1, 2, 3, 4, 3, 4 };

static __thread struct {
    ev_m1 m1[9];
    ev_m2 m2[9];
    unsigned char en1[9], en2[9], autonorm;
    int un, vn;
    float gu1, gu2, gv1, gv2;
    int init;
} EV;

static void ev_init(void) {
    if (EV.init) return;
    EV.init = 1;
    EV.un = EV.vn = 1;
    EV.gu1 = EV.gv1 = 0.0f;
    EV.gu2 = EV.gv2 = 1.0f;
}

/* called from cm_cap(): return 1 = cap handled here (do not forward to ES) */
static int ev_cap(unsigned cap, int on) {
    if (cap >= 0x0D90 && cap <= 0x0D98) { EV.en1[cap - 0x0D90] = (unsigned char)(on != 0); return 1; }
    if (cap >= 0x0DB0 && cap <= 0x0DB8) { EV.en2[cap - 0x0DB0] = (unsigned char)(on != 0); return 1; }
    if (cap == 0x0D80)                  { EV.autonorm = (unsigned char)(on != 0);           return 1; }
    return 0;
}

/* ---------- Bernstein basis ---------- */
static void ev_bern(int order, float t, float* B) {
    B[0] = 1.0f;
    for (int j = 1; j < order; j++) {
        float saved = 0.0f;
        for (int k = 0; k < j; k++) {
            float tmp = B[k];
            B[k] = saved + (1.0f - t) * tmp;
            saved = t * tmp;
        }
        B[j] = saved;
    }
}

static void ev_bernd(int order, float t, float* B, float* D) {
    ev_bern(order, t, B);
    if (order < 2) { D[0] = 0.0f; return; }
    float L[EV_MAXO];
    int n = order - 1;
    ev_bern(order - 1, t, L);
    for (int i = 0; i < order; i++)
        D[i] = (float)n * ((i > 0 ? L[i - 1] : 0.0f) - (i < n ? L[i] : 0.0f));
}

/* ---------- glMap1 / glMap2 ---------- */
static void ev_map1(unsigned target, float u1, float u2, int stride, int order, const void* p, int dbl) {
    ev_init();
    unsigned i = target - 0x0D90;
    if (i > 8 || order < 1 || order > EV_MAXO || u1 == u2 || stride < 0 || !p) return;
    int k = ev_k[i];
    float* d = malloc((size_t)order * k * sizeof(float));
    if (!d) return;
    for (int o = 0; o < order; o++)
        for (int c = 0; c < k; c++)
            d[o * k + c] = dbl ? (float)((const double*)p)[(size_t)o * stride + c]
                               :        ((const float*) p)[(size_t)o * stride + c];
    free(EV.m1[i].d);
    EV.m1[i] = (ev_m1){ order, k, u1, u2, d };
}

static void ev_map2(unsigned target, float u1, float u2, int us, int uo,
                    float v1, float v2, int vs, int vo, const void* p, int dbl) {
    ev_init();
    unsigned i = target - 0x0DB0;
    if (i > 8 || uo < 1 || vo < 1 || uo > EV_MAXO || vo > EV_MAXO || u1 == u2 || v1 == v2 ||
        us < 0 || vs < 0 || !p) return;
    int k = ev_k[i];
    float* d = malloc((size_t)uo * vo * k * sizeof(float));
    if (!d) return;
    for (int a = 0; a < uo; a++)
        for (int b = 0; b < vo; b++)
            for (int c = 0; c < k; c++) {
                size_t src = (size_t)a * us + (size_t)b * vs + c;
                d[((size_t)a * vo + b) * k + c] = dbl ? (float)((const double*)p)[src]
                                                       :        ((const float*) p)[src];
            }
    free(EV.m2[i].d);
    EV.m2[i] = (ev_m2){ uo, vo, k, u1, u2, v1, v2, d };
}

GLC void glMap1f(unsigned t, float u1, float u2, int stride, int order, const float* p)
    { ev_map1(t, u1, u2, stride, order, p, 0); }
GLC void glMap1d(unsigned t, double u1, double u2, int stride, int order, const double* p)
    { ev_map1(t, (float)u1, (float)u2, stride, order, p, 1); }
GLC void glMap2f(unsigned t, float u1, float u2, int us, int uo, float v1, float v2, int vs, int vo, const float* p)
    { ev_map2(t, u1, u2, us, uo, v1, v2, vs, vo, p, 0); }
GLC void glMap2d(unsigned t, double u1, double u2, int us, int uo, double v1, double v2, int vs, int vo, const double* p)
    { ev_map2(t, (float)u1, (float)u2, us, uo, (float)v1, (float)v2, vs, vo, p, 1); }

/* ---------- Evaluation ---------- */
static void ev_eval1(int i, float u, float* out) {
    const ev_m1* m = &EV.m1[i];
    float B[EV_MAXO], t = (u - m->a) / (m->b - m->a);
    ev_bern(m->order, t, B);
    for (int c = 0; c < 4; c++) out[c] = 0.0f;
    for (int o = 0; o < m->order; o++)
        for (int c = 0; c < m->k; c++) out[c] += B[o] * m->d[o * m->k + c];
}

/* out = value; du/dv = partial derivatives wrt normalized params (may be NULL) */
static void ev_eval2(int i, float u, float v, float* out, float* du, float* dv) {
    const ev_m2* m = &EV.m2[i];
    float Bu[EV_MAXO], Bv[EV_MAXO], Du[EV_MAXO], Dv[EV_MAXO];
    float tu = (u - m->u1) / (m->u2 - m->u1), tv = (v - m->v1) / (m->v2 - m->v1);
    if (du && dv) { ev_bernd(m->uo, tu, Bu, Du); ev_bernd(m->vo, tv, Bv, Dv); }
    else          { ev_bern(m->uo, tu, Bu);      ev_bern(m->vo, tv, Bv); }
    for (int c = 0; c < 4; c++) { out[c] = 0.0f; if (du) du[c] = dv[c] = 0.0f; }
    for (int a = 0; a < m->uo; a++)
        for (int b = 0; b < m->vo; b++) {
            const float* P = m->d + ((size_t)a * m->vo + b) * m->k;
            float w = Bu[a] * Bv[b];
            for (int c = 0; c < m->k; c++) {
                out[c] += w * P[c];
                if (du) { du[c] += Du[a] * Bv[b] * P[c]; dv[c] += Bu[a] * Dv[b] * P[c]; }
            }
        }
}

static void ev_texcoord(int i, const float* o) { glTexCoord2f(o[0], ev_k[i] > 1 ? o[1] : 0.0f); }

GLC void glEvalCoord1f(float u) {
    ev_init();
    float o[4];
    if (EV.en1[0] && EV.m1[0].d) { ev_eval1(0, u, o); glColor4f(o[0], o[1], o[2], o[3]); }
    if (EV.en1[2] && EV.m1[2].d) { ev_eval1(2, u, o); glNormal3f(o[0], o[1], o[2]); }
    for (int i = 6; i >= 3; i--)
        if (EV.en1[i] && EV.m1[i].d) { ev_eval1(i, u, o); ev_texcoord(i, o); break; }
    if (EV.en1[8] && EV.m1[8].d) {
        ev_eval1(8, u, o);
        float w = o[3] != 0.0f ? o[3] : 1.0f;
        glVertex3f(o[0] / w, o[1] / w, o[2] / w);
    } else if (EV.en1[7] && EV.m1[7].d) {
        ev_eval1(7, u, o);
        glVertex3f(o[0], o[1], o[2]);
    }
}

GLC void glEvalCoord2f(float u, float v) {
    ev_init();
    float o[4];
    if (EV.en2[0] && EV.m2[0].d) { ev_eval2(0, u, v, o, NULL, NULL); glColor4f(o[0], o[1], o[2], o[3]); }
    if (EV.en2[2] && EV.m2[2].d) { ev_eval2(2, u, v, o, NULL, NULL); glNormal3f(o[0], o[1], o[2]); }
    for (int i = 6; i >= 3; i--)
        if (EV.en2[i] && EV.m2[i].d) { ev_eval2(i, u, v, o, NULL, NULL); ev_texcoord(i, o); break; }

    int vi = (EV.en2[8] && EV.m2[8].d) ? 8 : (EV.en2[7] && EV.m2[7].d) ? 7 : -1;
    if (vi < 0) return;
    int an = EV.autonorm && !(EV.en2[2] && EV.m2[2].d);
    float du[4], dv[4];
    ev_eval2(vi, u, v, o, an ? du : NULL, an ? dv : NULL);
    float x = o[0], y = o[1], z = o[2];
    if (vi == 8) {
        float w = o[3] != 0.0f ? o[3] : 1.0f;
        x /= w; y /= w; z /= w;
        if (an) {                                   /* quotient rule: d(P/w) = (dP - P*dw) / w */
            for (int c = 0; c < 3; c++) {
                du[c] = (du[c] - (o[c] / w) * du[3]) / w;
                dv[c] = (dv[c] - (o[c] / w) * dv[3]) / w;
            }
        }
    }
    if (an) {
        float nx = du[1] * dv[2] - du[2] * dv[1];
        float ny = du[2] * dv[0] - du[0] * dv[2];
        float nz = du[0] * dv[1] - du[1] * dv[0];
        float l2 = nx * nx + ny * ny + nz * nz;
        if (l2 > 1e-24f) {
            float il = 1.0f / sqrtf(l2);
            glNormal3f(nx * il, ny * il, nz * il);
        }
    }
    glVertex3f(x, y, z);
}

GLC void glEvalCoord1d(double u)           { glEvalCoord1f((float)u); }
GLC void glEvalCoord1fv(const float* u)    { glEvalCoord1f(u[0]); }
GLC void glEvalCoord1dv(const double* u)   { glEvalCoord1f((float)u[0]); }
GLC void glEvalCoord2d(double u, double v) { glEvalCoord2f((float)u, (float)v); }
GLC void glEvalCoord2fv(const float* u)    { glEvalCoord2f(u[0], u[1]); }
GLC void glEvalCoord2dv(const double* u)   { glEvalCoord2f((float)u[0], (float)u[1]); }

/* ---------- Grids + meshes ---------- */
GLC void glMapGrid1f(int un, float u1, float u2) {
    ev_init();
    if (un < 1) return;
    EV.un = un; EV.gu1 = u1; EV.gu2 = u2;
}
GLC void glMapGrid1d(int un, double u1, double u2) { glMapGrid1f(un, (float)u1, (float)u2); }
GLC void glMapGrid2f(int un, float u1, float u2, int vn, float v1, float v2) {
    ev_init();
    if (un < 1 || vn < 1) return;
    EV.un = un; EV.gu1 = u1; EV.gu2 = u2;
    EV.vn = vn; EV.gv1 = v1; EV.gv2 = v2;
}
GLC void glMapGrid2d(int un, double u1, double u2, int vn, double v1, double v2)
    { glMapGrid2f(un, (float)u1, (float)u2, vn, (float)v1, (float)v2); }

static float ev_gu(int i) { return i == EV.un ? EV.gu2 : EV.gu1 + (float)i * (EV.gu2 - EV.gu1) / (float)EV.un; }
static float ev_gv(int j) { return j == EV.vn ? EV.gv2 : EV.gv1 + (float)j * (EV.gv2 - EV.gv1) / (float)EV.vn; }

GLC void glEvalPoint1(int i)        { ev_init(); glEvalCoord1f(ev_gu(i)); }
GLC void glEvalPoint2(int i, int j) { ev_init(); glEvalCoord2f(ev_gu(i), ev_gv(j)); }

GLC void glEvalMesh1(unsigned mode, int i1, int i2) {
    ev_init();
    if (mode != 0x1B00 && mode != 0x1B01) return;           /* GL_POINT / GL_LINE */
    glBegin(mode == 0x1B00 ? 0 /*POINTS*/ : 3 /*LINE_STRIP*/);
    for (int i = i1; i <= i2; i++) glEvalCoord1f(ev_gu(i));
    glEnd();
}

GLC void glEvalMesh2(unsigned mode, int i1, int i2, int j1, int j2) {
    ev_init();
    if (mode == 0x1B00) {                                    /* GL_POINT */
        glBegin(0);
        for (int j = j1; j <= j2; j++)
            for (int i = i1; i <= i2; i++) glEvalCoord2f(ev_gu(i), ev_gv(j));
        glEnd();
    } else if (mode == 0x1B01) {                             /* GL_LINE */
        for (int j = j1; j <= j2; j++) {
            glBegin(3);
            for (int i = i1; i <= i2; i++) glEvalCoord2f(ev_gu(i), ev_gv(j));
            glEnd();
        }
        for (int i = i1; i <= i2; i++) {
            glBegin(3);
            for (int j = j1; j <= j2; j++) glEvalCoord2f(ev_gu(i), ev_gv(j));
            glEnd();
        }
    } else if (mode == 0x1B02) {                             /* GL_FILL */
        for (int j = j1; j < j2; j++) {
            glBegin(8);                                      /* QUAD_STRIP */
            for (int i = i1; i <= i2; i++) {
                glEvalCoord2f(ev_gu(i), ev_gv(j));
                glEvalCoord2f(ev_gu(i), ev_gv(j + 1));
            }
            glEnd();
        }
    }
}
