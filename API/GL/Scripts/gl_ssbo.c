/*
 * gl_ssbo.c - included by gl_es_emul.c AFTER gl_litfix.c (uses gl_sb/sb_put, gl_has, gl_idch, s1d_ws).
 *
 * Mali G52 reports MAX_VERTEX_SHADER_STORAGE_BLOCKS = 0, so vertex shaders cannot declare SSBOs.
 * Emulation for "readonly" scalar-array blocks (float[] / uint[] / int[]) in VERTEX shaders:
 *
 *   layout(binding = N, std430) restrict readonly buffer _pos { float pos[]; };
 *       -> layout(binding = U) uniform highp usampler2D xs_pos_t;  + accessor xs_pos(int|uint)
 *   pos[expr]  ->  xs_pos(expr)
 *
 * The SSBO content is copied (GPU side, via PIXEL_UNPACK_BUFFER) into an R32UI texture, 4096 texels wide,
 * on every glBindBufferBase/Range(GL_SHADER_STORAGE_BUFFER, N, buf). Float/int are bit-reinterpreted.
 * Texture unit U = (min(MAX_VERTEX_TEXTURE_IMAGE_UNITS,16) - 1) - N, i.e. 15..8, fixed via layout(binding).
 * Compute / fragment shaders keep using the native SSBO.
 *
 * Not handled yet: struct element arrays (e.g. GlyphQuad glyphs[]), writable blocks, arr.length().
 */

#define XS_W    4096
#define XS_SSBO 0x90D2
#define SSBO_MAX_SLOTS 16

static unsigned xs_mask;                 /* slots rewritten in at least one vertex shader */

#ifndef SSBO_MAX_SLOTS
#define SSBO_MAX_SLOTS 8
#endif

static unsigned xs_tex[SSBO_MAX_SLOTS];
static int      xs_rows[SSBO_MAX_SLOTS];


typedef void (*XS_bbb)(unsigned, unsigned, unsigned);
typedef void (*XS_bbr)(unsigned, unsigned, unsigned, long, long);
typedef void (*XS_gbp)(unsigned, unsigned, int*);

static int xs_base(void) {
    static int b;
    if (!b) {
        int n = 16;
        RP(glGetIntegerv, F_geti)(0x8B4C, &n);          /* real MAX_VERTEX_TEXTURE_IMAGE_UNITS */
        if (n > 16) n = 16;
        if (n < 9) n = 9;
        b = n - 1;
    }
    return b;
}
static int xs_unit(int bind) { return xs_base() - bind; }

static size_t xs_ws(const char* s, size_t n, size_t i) { while (i < n && s1d_ws(s[i])) i++; return i; }
static size_t xs_id(const char* s, size_t n, size_t i) { while (i < n && gl_idch(s[i])) i++; return i; }

typedef struct {
    int n, stride;
    char name[48];
    char ty[16][12], nm[16][32], bs[16];
    int c[16], off[16];
} xs_st;

static int xs_mtype(const char* t, size_t l, char* base, int* cnt) {
    if (l == 8 && !memcmp(t, "float4x4", 8)) { *base = 'f'; *cnt = 16; return 1; }
    if (l == 4 && !memcmp(t, "mat4", 4))     { *base = 'f'; *cnt = 16; return 1; }
    if (l == 6 && !memcmp(t, "float", 5) && t[5] >= '2' && t[5] <= '4') { *base='f'; *cnt=t[5]-'0'; return 1; }
    if (l == 4 && !memcmp(t, "int", 3)   && t[3] >= '2' && t[3] <= '4') { *base='i'; *cnt=t[3]-'0'; return 1; }
    if (l == 5 && !memcmp(t, "uint", 4)  && t[4] >= '2' && t[4] <= '4') { *base='u'; *cnt=t[4]-'0'; return 1; }
    size_t p = 0; char b = 'f';
    if (l >= 1 && t[0] == 'i') { b = 'i'; p = 1; }
    else if (l >= 1 && t[0] == 'u') { b = 'u'; p = 1; }
    if (l == p + 4 && !memcmp(t + p, "vec", 3) && t[p + 3] >= '2' && t[p + 3] <= '4') {
        *base = b; *cnt = t[p + 3] - '0'; return 1;
    }
    return 0;
}

/* skips a preprocessor directive at s[j]=='#'; for #if.. skips up to the matching #endif */
static size_t xs_skip_pp(const char* s, size_t n, size_t j) {
    int depth = 0;
    while (j < n) {
        size_t k = j;
        while (k < n && (s[k] == ' ' || s[k] == '\t')) k++;
        if (k < n && s[k] == '#') {
            size_t d = k + 1;
            while (d < n && (s[d] == ' ' || s[d] == '\t')) d++;
            if (d + 2 <= n && !memcmp(s + d, "if", 2)) depth++;
            else if (d + 5 <= n && !memcmp(s + d, "endif", 5)) depth--;
        }
        while (j < n && s[j] != '\n') j++;          /* end of line (handles '\' continuations poorly, fine here) */
        if (j < n) j++;
        if (depth <= 0) break;
    }
    return j;
}

/* finds "struct NAME { members };" in s[0..n) and computes std430 offsets (in 4-byte words) */
static int xs_struct(const char* s, size_t n, const char* tn, size_t tl, xs_st* st) {
    for (size_t i = 0; i + 6 < n; i++) {
        if (memcmp(s + i, "struct", 6) || (i && gl_idch(s[i - 1])) || gl_idch(s[i + 6])) continue;
        size_t j = xs_ws(s, n, i + 6), a = j;
        j = xs_id(s, n, j);
        if (j - a != tl || memcmp(s + a, tn, tl)) continue;
        j = xs_ws(s, n, j);
        if (j >= n || s[j] != '{') continue;
        j++;
        st->n = 0;
        int w = 0, al = 1;
        for (;;) {
            j = xs_ws(s, n, j);
            if (j >= n) return 0;
            if (s[j] == '#') { j = xs_skip_pp(s, n, j); continue; }
            if (s[j] == '}') break;
            size_t t0 = j; j = xs_id(s, n, j); size_t t1 = j;
            char bs; int c;
            if (t1 == t0 || t1 - t0 >= 12  || st->n >= 16 || !xs_mtype(s + t0, t1 - t0, &bs, &c)) {
                if (gl_emul_debug_enabled())
                    fprintf(stderr, "[ssbo-struct] %.*s: member type '%.*s' rejected\n",
                            (int)tl, tn, (int)(t1 - t0), s + t0);
                return 0;
            }
            j = xs_ws(s, n, j);
            size_t n0 = j; j = xs_id(s, n, j); size_t n1 = j;
            if (n1 == n0 || n1 - n0 >= 32) return 0;
            j = xs_ws(s, n, j);
            if (j >= n || s[j] != ';') return 0;      /* array / multiple declarators: not handled */
            j++;
            int ma = c == 1 ? 1 : c == 2 ? 2 : 4;
            w = (w + ma - 1) / ma * ma;
            int k = st->n++;
            memcpy(st->ty[k], s + t0, t1 - t0); st->ty[k][t1 - t0] = 0;
            memcpy(st->nm[k], s + n0, n1 - n0); st->nm[k][n1 - n0] = 0;
            st->bs[k] = bs; st->c[k] = c; st->off[k] = w;
            w += c;
            if (ma > al) al = ma;
        }
        if (!st->n) return 0;
        st->stride = (w + al - 1) / al * al;
        return 1;
    }
    return 0;
}

#define XS_SKIP() do { if (gl_emul_debug_enabled()) \
    fprintf(stderr, "[ssbo-skip] gl_ssbo.c:%d | decl: %.*s | buffer %.80s\n", \
            __LINE__, (int)(dl > 160 ? 160 : dl), dc, s + i + 6); \
    return 2; } while (0)

/* 0 = no more blocks, 1 = handled block [*ds,*de), 2 = skipped (continue from *de) */
static int xs_find(const char* s, size_t n, size_t from, size_t* ds, size_t* de,
                   int* bind, char* nm, int* kind, xs_st* st) {
    for (size_t i = from; i + 6 <= n; i++) {
        if (memcmp(s + i, "buffer", 6) || (i && gl_idch(s[i - 1])) || (i + 6 < n && gl_idch(s[i + 6]))) continue;
        size_t d = i;
        while (d && s[d - 1] != ';' && s[d - 1] != '}' && s[d - 1] != '\n') d--;
        const char* dc = s + d; size_t dl = i - d, k;
        *de = i + 6;
        for (k = 0; k + 7 <= dl && memcmp(dc + k, "binding", 7); k++) {}
        if (k + 7 > dl || !gl_has(dc, dl, "readonly")) XS_SKIP();
        size_t p = xs_ws(dc, dl, k + 7);
        if (p >= dl || dc[p] != '=') XS_SKIP();
        p = xs_ws(dc, dl, p + 1);
        int b = atoi(dc + p);

        size_t j = xs_ws(s, n, i + 6);
        j = xs_ws(s, n, xs_id(s, n, j));                          /* block name */
        if (j >= n || s[j] != '{') XS_SKIP();
        j = xs_ws(s, n, j + 1);
        size_t t0 = j; j = xs_id(s, n, j); size_t t1 = j;         /* element type */
        j = xs_ws(s, n, j);
        size_t n0 = j; j = xs_id(s, n, j); size_t n1 = j;         /* array name */
        j = xs_ws(s, n, j);
        if (t1 == t0 || n1 == n0 || n1 - n0 >= 48 || j >= n || s[j] != '[') XS_SKIP();
        j = xs_ws(s, n, j + 1);
        if (j >= n || s[j] != ']') XS_SKIP();
        j = xs_ws(s, n, j + 1);
        if (j >= n || s[j] != ';') XS_SKIP();
        j = xs_ws(s, n, j + 1);
        if (j >= n || s[j] != '}') XS_SKIP();
        j = xs_ws(s, n, j + 1);
        if (j >= n || s[j] != ';') XS_SKIP();                      /* instance name -> not handled */

        size_t tl = t1 - t0;
        *kind = (tl == 5 && !memcmp(s + t0, "float", 5)) ? 1 :
                (tl == 4 && !memcmp(s + t0, "uint", 4))  ? 2 :
                (tl == 3 && !memcmp(s + t0, "int", 3))   ? 3 : 0;
                if (!*kind) {
            if (tl >= 48 || !xs_struct(s, d, s + t0, tl, st)) XS_SKIP();
            memcpy(st->name, s + t0, tl); st->name[tl] = 0;
            *kind = 4;
        }
        if (b < 0 || b >= SSBO_MAX_SLOTS) XS_SKIP();
        memcpy(nm, s + n0, n1 - n0); nm[n1 - n0] = 0;
        *ds = d; *de = j + 1; *bind = b;
        return 1;
    }
    return 0;
}

static int xs_decl(char* o, size_t cap, int bind, const char* nm, int kind) {
    const char* ty = kind == 1 ? "float" : kind == 2 ? "uint" : "int";
    const char* cv = kind == 1 ? "uintBitsToFloat" : kind == 2 ? "uint" : "int";
    return snprintf(o, cap,
        "layout(binding = %d) uniform highp usampler2D xs_%s_t; "
        "uint xs_%s_r(int i){ return texelFetch(xs_%s_t, ivec2(i & 4095, i >> 12), 0).r; } "
        "%s xs_%s(int i){ return %s(xs_%s_r(i)); } "
        "%s xs_%s(uint i){ return xs_%s(int(i)); }",
        xs_unit(bind), nm, nm, nm, ty, nm, cv, nm, ty, nm, nm);
}

static void xs_decl_struct(gl_sb* b, int bind, const char* nm, const xs_st* st) {
    char t[640];
    int l = snprintf(t, sizeof t,
        "layout(binding = %d) uniform highp usampler2D xs_%s_t; "
        "uint xs_%s_r(int i){ return texelFetch(xs_%s_t, ivec2(i & 4095, i >> 12), 0).r; } "
        "%s xs_%s(int xs_i){ int xs_b = xs_i * %d; %s xs_s; ",
        xs_unit(bind), nm, nm, nm, st->name, nm, st->stride, st->name);
    sb_put(b, t, (size_t)l);
    for (int k = 0; k < st->n; k++) {
        l = snprintf(t, sizeof t, "xs_s.%s = ", st->nm[k]);
        sb_put(b, t, (size_t)l);
        if (st->c[k] > 1) { l = snprintf(t, sizeof t, "%s(", st->ty[k]); sb_put(b, t, (size_t)l); }
        for (int q = 0; q < st->c[k]; q++) {
            const char* f = st->bs[k] == 'f' ? "uintBitsToFloat(" : st->bs[k] == 'i' ? "int(" : "(";
            l = snprintf(t, sizeof t, "%s%s%s xs_%s_r(xs_b + %d))", q ? ", " : "", f, "", nm, st->off[k] + q);
            sb_put(b, t, (size_t)l);
        }
        sb_put(b, st->c[k] > 1 ? ");" : ";", st->c[k] > 1 ? 2 : 1);
        sb_put(b, " ", 1);
    }
    l = snprintf(t, sizeof t, "return xs_s; } %s xs_%s(uint xs_i){ return xs_%s(int(xs_i)); }",
                 st->name, nm, nm);
    sb_put(b, t, (size_t)l);
}

/* nm[expr] -> xs_nm(expr), with bracket matching */
static void xs_accesses(gl_sb* b, const char* s, size_t n, const char* nm, const char* out) {
    size_t nl = strlen(nm), ol = strlen(out), i = 0;
    while (i < n) {
        if (i + nl <= n && !memcmp(s + i, nm, nl) && (i == 0 || !gl_idch(s[i - 1])) &&
            (i + nl == n || !gl_idch(s[i + nl]))) {
            size_t j = xs_ws(s, n, i + nl);
            if (j < n && s[j] == '[') {
                int depth = 0; size_t k = j;
                for (; k < n; k++) {
                    if (s[k] == '[') depth++;
                    else if (s[k] == ']' && --depth == 0) break;
                }
                if (k < n) {
                    sb_put(b, "xs_", 3); sb_put(b, out, ol); sb_put(b, "(", 1);
                    sb_put(b, s + j + 1, k - j - 1);
                    sb_put(b, ")", 1);
                    i = k + 1;
                    continue;
                }
            }
        }
        sb_put(b, s + i, 1);
        i++;
    }
}

/* returns a new malloc'd source or NULL when nothing was rewritten. Keeps the line count unchanged. */
static char* xs_rewrite(const char* src) {
    char* cur = NULL;
    const char* s = src;
    size_t n = strlen(src), from = 0;
    for (int guard = 0; guard < 64; guard++) {
        size_t ds = 0, de = 0; int bind = 0, kind = 0; char nm[48];
        xs_st st;
        int r = xs_find(s, n, from, &ds, &de, &bind, nm, &kind, &st);
        if (!r) break;
        if (r == 2) { from = de; continue; }

        gl_sb b = { 0 };
        sb_put(&b, s, ds);
        if (kind == 4) xs_decl_struct(&b, bind, nm, &st);
        else {
            char decl[768];
            int dl = xs_decl(decl, sizeof decl, bind, nm, kind);
            sb_put(&b, decl, (size_t)dl);
        }
        from = b.n;

        gl_sb t1 = { 0 };
        xs_accesses(&t1, s + de, n - de, nm, nm);
        if (nm[0] == '_') {
            gl_sb t2 = { 0 };
            xs_accesses(&t2, t1.p ? t1.p : "", t1.n, nm + 1, nm);
            free(t1.p); t1 = t2;
        }
        sb_put(&b, t1.p ? t1.p : "", t1.n);
        free(t1.p);

        free(cur);
        cur = b.p; s = cur; n = b.n;
        xs_mask |= 1u << bind;
        if (gl_emul_debug_enabled())
            fprintf(stderr, "[ssbo] vertex block '%s' (binding %d) -> usampler2D, unit %d\n", nm, bind, xs_unit(bind));
    }
    return cur;
}

/* ---- runtime: copy the SSBO range into the slot's texture ---- */
static void xs_sync(unsigned idx, unsigned buf, long off, long size) {
    if (!buf || idx >= SSBO_MAX_SLOTS || !((xs_mask >> idx) & 1)) return;
    static const unsigned pn[4] = { 0x0CF2, 0x0CF3, 0x0CF4, 0x0CF5 };
    int ub = 0, at = 0, tx = 0, bsz = 0, mx = 4096, sv[4], i;

    RP(glGetIntegerv, F_geti)(0x88EF, &ub);                      /* PIXEL_UNPACK_BUFFER_BINDING */
    RP(glBindBuffer, F_uiui)(0x88EC, buf);
    RP(glGetBufferParameteriv, XS_gbp)(0x88EC, 0x8764, &bsz);    /* BUFFER_SIZE */
    if (size < 0) size = bsz - off;
    long cnt = size / 4;
    if (off < 0 || (off & 3) || cnt <= 0) { RP(glBindBuffer, F_uiui)(0x88EC, (unsigned)ub); return; }
    RP(glGetIntegerv, F_geti)(0x0D33, &mx);                      /* MAX_TEXTURE_SIZE */
    int rows = (int)((cnt + XS_W - 1) / XS_W);
    if (rows > mx) { rows = mx; cnt = (long)rows * XS_W; }

    RP(glMemoryBarrier, F_ui)(0x2000 | 0x0200 | 0x0080);         /* SSBO | BUFFER_UPDATE | PIXEL_BUFFER */
    RP(glGetIntegerv, F_geti)(0x84E0, &at);                      /* ACTIVE_TEXTURE */
    RP(glGetIntegerv, F_geti)(0x8069, &tx);                      /* TEXTURE_BINDING_2D (on the target unit, below) */
    for (i = 0; i < 4; i++) { RP(glGetIntegerv, F_geti)(pn[i], &sv[i]); RP(glPixelStorei, F_pstore)(pn[i], i == 3 ? 4 : 0); }

    RP(glActiveTexture, F_ui)(0x84C0 + (unsigned)xs_unit((int)idx));
    RP(glGetIntegerv, F_geti)(0x8069, &tx);
    if (!xs_tex[idx]) RP(glGenTextures, F_gentex)(1, &xs_tex[idx]);
    RP(glBindTexture, F_uiui)(0x0DE1, xs_tex[idx]);
    if (rows > xs_rows[idx]) {
        RP(glBindBuffer, F_uiui)(0x88EC, 0);                     /* NULL data, not a PBO offset */
        RP(glTexParameteri, F_da)(0x0DE1, 0x2801, 0x2600);
        RP(glTexParameteri, F_da)(0x0DE1, 0x2800, 0x2600);
        RP(glTexParameteri, F_da)(0x0DE1, 0x2802, 0x812F);
        RP(glTexParameteri, F_da)(0x0DE1, 0x2803, 0x812F);
        RP(glTexImage2D, F_teximg)(0x0DE1, 0, 0x8236, XS_W, rows, 0, 0x8D94, 0x1405, NULL);   /* R32UI */
        RP(glBindBuffer, F_uiui)(0x88EC, buf);
        xs_rows[idx] = rows;
    }
    long fr = cnt / XS_W, rem = cnt - fr * XS_W;
    if (fr > 0)
        RP(glTexSubImage2D, F_teximg)(0x0DE1, 0, 0, 0, XS_W, (int)fr, 0x8D94, 0x1405, (const void*)(uintptr_t)off);
    if (rem > 0)
        RP(glTexSubImage2D, F_teximg)(0x0DE1, 0, 0, (int)fr, (int)rem, 1, 0x8D94, 0x1405,
                                      (const void*)(uintptr_t)(off + fr * XS_W * 4));

    RP(glBindTexture, F_uiui)(0x0DE1, (unsigned)tx);
    RP(glActiveTexture, F_ui)((unsigned)at);
    for (i = 0; i < 4; i++) RP(glPixelStorei, F_pstore)(pn[i], sv[i]);
    RP(glBindBuffer, F_uiui)(0x88EC, (unsigned)ub);
}

GLC void glBindBufferBase(unsigned target, unsigned idx, unsigned buf) {
    RP(glBindBufferBase, XS_bbb)(target, idx, buf);
    if (target == XS_SSBO) xs_sync(idx, buf, 0, -1);
}

GLC void glBindBufferRange(unsigned target, unsigned idx, unsigned buf, long off, long size) {
    RP(glBindBufferRange, XS_bbr)(target, idx, buf, off, size);
    if (target == XS_SSBO) xs_sync(idx, buf, off, size);
}
