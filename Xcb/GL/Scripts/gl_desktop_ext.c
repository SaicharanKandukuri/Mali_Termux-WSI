/* gl_desktop_ext.c - missing desktop functions in ES (included by glx_shim.c after gl_es_emul.c) */
#include <stdio.h>

typedef void (*F_mda)(unsigned, const int*, const int*, int);
static void ad_dump(unsigned, int, int, int, int, int, int, unsigned);
static void ad_install(void);

static void dx_warn(const char* m) {
    if (gl_compat_debug_enabled()) fprintf(stderr, "[gl_desktop] %s\n", m);
}

GLC void glMultiDrawArrays(unsigned mode, const int* first, const int* count, int n) {
    for (int i = 0; i < n; i++) if (count[i] > 0) RP(glDrawArrays, F_da)(mode, first[i], count[i]);
}

GLC void glMultiDrawElements(unsigned mode, const int* count, unsigned type, const void* const* idx, int n) {
    for (int i = 0; i < n; i++) if (count[i] > 0) RP(glDrawElements, F_de)(mode, count[i], type, idx[i]);
}

/* ES3 only supports primitive restart on the maximum index of the type (0xFF/0xFFFF/0xFFFFFFFF) */
GLC void glPrimitiveRestartIndex(unsigned idx) {
    if (idx != 0xFFu && idx != 0xFFFFu && idx != 0xFFFFFFFFu)
        dx_warn("glPrimitiveRestartIndex: only the maximum index of the type is supported");
}

GLC void glProvokingVertex(unsigned mode) {
    if (mode == 0x8E4D) dx_warn("glProvokingVertex(FIRST) is not supported in ES (remains LAST)");
}

GLC void glClampColor(unsigned target, unsigned clamp) { (void)target; (void)clamp; }

static int dx_emulated(void) { return T_glx.ctx && T_glx.ctx->ver >= 2 && !T_glx.ctx->es_native; }

extern void* G_mali;   /* from libGLESv2_shim.c */

GLC void glLogicOp(unsigned op) {
    static void (*real)(unsigned);
    if (!real && G_mali)
        real = (void (*)(unsigned))dlsym(G_mali, "glLogicOp");
    if (real) { real(op); return; }

    if (dx_emulated()) {
        if (op != 0x1503) dx_warn("glLogicOp != GL_COPY ignored");
        return;
    }
    RP(glLogicOp, F_ui)(op);
}

GLC unsigned char glIsEnabled(unsigned cap) {
    if (gl_cap_ignored(cap)) return 0;
    return RP(glIsEnabled, F_isen)(gl_cap_xlat(cap));
}

/* ---------- glPolygonMode: LINE/POINT emulated via custom index buffer ---------- */
typedef void (*F_bdata)(unsigned, long, const void*, unsigned);

static unsigned pm_mode = 0x1B02;       /* GL_FILL */
static unsigned pm_ebo;

GLC void glPolygonMode(unsigned face, unsigned mode) {
    (void)face;
    if (mode == 0x1B00 || mode == 0x1B01 || mode == 0x1B02) pm_mode = mode;
    if (!dx_emulated() && mode != 0x1B02) dx_warn("glPolygonMode(LINE/POINT) unimplemented in native ES/ES1 context");
}

/* v[n] = vertex indices for TRIANGLES(4)/STRIP(5)/FAN(6) -> lines or points */
static unsigned* pm_expand(unsigned mode, const unsigned* v, int n, int* outn, unsigned* outmode) {
    int tris = mode == 4 ? n / 3 : n - 2, pts = (pm_mode == 0x1B00);
    if (tris <= 0) return NULL;
    unsigned* o = malloc((size_t)tris * (pts ? 3 : 6) * sizeof *o);
    if (!o) return NULL;
    int k = 0;
    for (int t = 0; t < tris; t++) {
        unsigned a, b, c;
        if (mode == 4)      { a = v[3 * t]; b = v[3 * t + 1]; c = v[3 * t + 2]; }
        else if (mode == 5) { a = v[t];     b = v[t + 1];     c = v[t + 2]; }
        else                { a = v[0];     b = v[t + 1];     c = v[t + 2]; }
        if (pts) { o[k++] = a; o[k++] = b; o[k++] = c; }
        else { o[k++] = a; o[k++] = b; o[k++] = b; o[k++] = c; o[k++] = c; o[k++] = a; }
    }
    *outn = k; *outmode = pts ? 0 : 1;
    return o;
}

static void pm_draw_idx(unsigned mode, const unsigned* v, int n) {
    int cnt = 0, prev = 0; unsigned gm = 0;
    unsigned* o = pm_expand(mode, v, n, &cnt, &gm);
    if (!o) return;
    RP(glGetIntegerv, F_geti)(0x8895, &prev);                    /* ELEMENT_ARRAY_BUFFER_BINDING (part of VAO) */
    if (!pm_ebo) RP(glGenBuffers, F_gentex)(1, &pm_ebo);
    RP(glBindBuffer, F_uiui)(0x8893, pm_ebo);
    RP(glBufferData, F_bdata)(0x8893, (long)cnt * 4, o, 0x88E0); /* STREAM_DRAW */
    RP(glDrawElements, F_de)(gm, cnt, 0x1405, (const void*)0);
    RP(glBindBuffer, F_uiui)(0x8893, (unsigned)prev);            /* restores VAO's EBO */
    free(o);
}

GLC void glDrawArrays(unsigned mode, int first, int count) {
    if (pm_mode != 0x1B02 && mode >= 4 && mode <= 6 && count >= 3 && dx_emulated()) {
        unsigned* v = malloc((size_t)count * sizeof *v);
        if (v) {
            for (int i = 0; i < count; i++) v[i] = (unsigned)(first + i);
            pm_draw_idx(mode, v, count); free(v); return;
        }
    }
    RP(glDrawArrays, F_da)(mode, first, count);
}

GLC void glDrawElements(unsigned mode, int count, unsigned type, const void* ind) {
    int tb = type == 0x1401 ? 1 : type == 0x1403 ? 2 : type == 0x1405 ? 4 : 0;
    if (pm_mode == 0x1B02 || mode < 4 || mode > 6 || count < 3 || !tb || !dx_emulated()) {
        RP(glDrawElements, F_de)(mode, count, type, ind); return;
    }
    int eb = 0;
    RP(glGetIntegerv, F_geti)(0x8895, &eb);
    const uint8_t* src = ind;
    if (eb) {                                                    /* "ind" is an offset in buffer: read via mapping */
        src = RP(glMapBufferRange, F_mapr)(0x8893, (long)(uintptr_t)ind, (long)count * tb, 0x0001);
        if (!src) { RP(glDrawElements, F_de)(mode, count, type, ind); return; }
    }
    unsigned* v = malloc((size_t)count * sizeof *v);
    if (v) {
        for (int i = 0; i < count; i++)
            v[i] = tb == 1 ? src[i] : tb == 2 ? ((const uint16_t*)src)[i] : ((const uint32_t*)src)[i];
    }
    if (eb) RP(glUnmapBuffer, F_unmap)(0x8893);
    if (!v) { RP(glDrawElements, F_de)(mode, count, type, ind); return; }
    pm_draw_idx(mode, v, count); free(v);
}

typedef struct { int align, rowlen, skiprows, skippixels, subimage; } dx_unpack;

static int dx_driver_ext(const char* name) {
    const char* ext = (const char*)RP(glGetString, F_gstr)(0x1F03);
    size_t n = strlen(name);
    for (const char* p = ext; p && (p = strstr(p, name)); p += n)
        if ((p == ext || p[-1] == ' ') && (!p[n] || p[n] == ' ')) return 1;
    return 0;
}

/* Return false rather than treating a PBO offset or invalid layout as a host pointer. */
static int dx_rgba_to_rgb(const void* pixels, int w, int h, dx_unpack* u, unsigned char** out) {
    int pbo = 0;
    *out = NULL;
    if (dx_driver_ext("GL_NV_pixel_buffer_object")) {
        RP(glGetIntegerv, F_geti)(0x88EF, &pbo);          /* PIXEL_UNPACK_BUFFER_BINDING_NV */
        if (pbo) return 0;
    }
    if (!pixels || !w || !h) return 1;

    u->rowlen = u->skiprows = u->skippixels = 0;
    u->subimage = dx_driver_ext("GL_EXT_unpack_subimage");
    RP(glGetIntegerv, F_geti)(0x0CF5, &u->align);
    if (u->subimage) {
        RP(glGetIntegerv, F_geti)(0x0CF2, &u->rowlen);
        RP(glGetIntegerv, F_geti)(0x0CF3, &u->skiprows);
        RP(glGetIntegerv, F_geti)(0x0CF4, &u->skippixels);
    }
    if ((u->align != 1 && u->align != 2 && u->align != 4 && u->align != 8) ||
        u->rowlen < 0 || u->skiprows < 0 || u->skippixels < 0) return 0;

    size_t top = (size_t)-1, rowsize = (size_t)(u->rowlen ? u->rowlen : w);
    if (rowsize > top / 4 || (size_t)w > top / 4 || (size_t)w > top / 3 ||
        (size_t)u->skippixels > top / 4) return 0;
    size_t rowbytes = rowsize * 4, readbytes = (size_t)w * 4, writebytes = (size_t)w * 3;
    if (rowbytes > top - (size_t)(u->align - 1)) return 0;
    size_t stride = (rowbytes + (size_t)(u->align - 1)) & ~(size_t)(u->align - 1);
    size_t offset = (size_t)u->skippixels * 4;
    if ((size_t)u->skiprows > (top - offset) / stride) return 0;
    offset += (size_t)u->skiprows * stride;
    if ((size_t)(h - 1) > (top - offset) / stride ||
        readbytes > top - offset - (size_t)(h - 1) * stride ||
        (size_t)h > top / writebytes) return 0;

    unsigned char* rgb = malloc(writebytes * (size_t)h);
    if (!rgb) return 0;
    const unsigned char* src = (const unsigned char*)pixels + offset;
    for (int y = 0; y < h; y++) {
        const unsigned char* s = src + (size_t)y * stride;
        unsigned char* d = rgb + (size_t)y * writebytes;
        for (int x = 0; x < w; x++) {
            d[(size_t)x * 3 + 0] = s[(size_t)x * 4 + 0];
            d[(size_t)x * 3 + 1] = s[(size_t)x * 4 + 1];
            d[(size_t)x * 3 + 2] = s[(size_t)x * 4 + 2];
        }
    }
    *out = rgb;
    return 1;
}

static void dx_unpack_set(const dx_unpack* u, int tight) {
    RP(glPixelStorei, F_pstore)(0x0CF5, tight ? 1 : u->align);
    if (u->subimage) {
        RP(glPixelStorei, F_pstore)(0x0CF2, tight ? 0 : u->rowlen);
        RP(glPixelStorei, F_pstore)(0x0CF3, tight ? 0 : u->skiprows);
        RP(glPixelStorei, F_pstore)(0x0CF4, tight ? 0 : u->skippixels);
    }
}

GLC void glTexImage2D(unsigned t, int l, int i, int w, int h, int b, unsigned f, unsigned ty, const void* p) {
    unsigned char* rgb = NULL;
    dx_unpack unpack = { 0 };

    /* Both legacy 3 and GL_RGB request RGB storage; GLES1 needs matching source bytes. */
    if ((i == 3 || i == 0x1907) && f == 0x1908 && ty == 0x1401 && emu_ver() == 1 &&
        t == 0x0DE1 && l >= 0 && l < 31 && b == 0 && w >= 0 && h >= 0) {
        int max = 0;
        RP(glGetIntegerv, F_geti)(0x0D33, &max);          /* GL_MAX_TEXTURE_SIZE */
        int limit = max > 0 ? max >> l : 0;
        if (limit > 0 && w <= limit && h <= limit && dx_rgba_to_rgb(p, w, h, &unpack, &rgb)) {
            i = f = 0x1907;                              /* GL_RGB */
            p = rgb;
            if (rgb) dx_unpack_set(&unpack, 1);
        }
    }
    /* GL 1.0 component counts; ES1 needs the named base format. Only alias identical layouts. */
    static const unsigned base[] = {0x1909, 0x190A, 0x1907, 0x1908}; /* L, LA, RGB, RGBA */
    if (i >= 1 && i <= 4 && f == base[i-1] && emu_ver() == 1)
        i = (int)f;
    if (f == 0x80E1) {                                   /* GL_BGRA */
        if (ty == 0x8367) ty = 0x1401;                   /* UNSIGNED_INT_8_8_8_8_REV -> UNSIGNED_BYTE */
        if (i == 0x1908 || i == 0x8058) i = 0x80E1;      /* internal = BGRA_EXT (ES requires format == internal) */
    }
    RP(glTexImage2D, F_teximg)(t1d(t), l, i, w, h, b, f, ty, p);
    if (rgb) {
        dx_unpack_set(&unpack, 0);
        free(rgb);
    }
    if (t == 0x84F5 && l == 0 && dx_emulated()) {
        RP(glTexParameteri, F_da)(0x0DE1, 0x2801, 0x2601);
        RP(glTexParameteri, F_da)(0x0DE1, 0x2802, 0x812F);
        RP(glTexParameteri, F_da)(0x0DE1, 0x2803, 0x812F);
    }
}

GLC void glTexSubImage2D(unsigned t, int l, int xo, int yo, int w, int h, unsigned f, unsigned ty, const void* p) {
    if (f == 0x80E1 && ty == 0x8367) ty = 0x1401;
    RP(glTexSubImage2D, F_teximg)(t1d(t), l, xo, yo, w, h, f, ty, p);
    if (t == 0x0DE1) ad_dump(0x0DE1, l, xo, yo, 0, w, h, f);
}
