/*
 * gl_dsa.c - ARB_direct_state_access (4.5) emulat peste GLES 3.2: bind -> apel -> restore.
 * Include in gl_es_emul.c INAINTE de gl_extmem.c (acesta foloseste dsa_in/dsa_out/dsa_note)
 * si dupa sectiunea N (tfo_cur) a fisierului. INLOCUIESTE blocul DSA din mesajul anterior
 * (glCreateTextures, glTextureStorage*, dsa_* ) - nu pastra ambele.
 *
 * Hook obligatoriu: in wrapper-ul tau glBindTexture adauga  dsa_note(tex, target);
 * (altfel texturile create cu glGenTextures nu au target cunoscut; implicit: 2D / 3D dupa functie)
 *
 * Nu acopera: glGetCompressedTextureImage, glTextureView, program pipelines, glTextureBarrier.
 */

#define DP(n, T)      ({ static T p_; if (!p_) p_ = (T)cs_gp(#n); p_; })
#define DP2(a, b, T)  ({ static T p_; if (!p_) { p_ = (T)cs_gp(#a); if (!p_) p_ = (T)cs_gp(#b); } p_; })

typedef void (*D_gen)(int, unsigned*);
typedef void (*D_ts2)(unsigned, int, unsigned, int, int);
typedef void (*D_ts3)(unsigned, int, unsigned, int, int, int);
typedef void (*D_cs3)(unsigned, int, int, int, int, int, int, int, int);
typedef void (*D_tbuf)(unsigned, unsigned, unsigned);
typedef void (*D_tbr)(unsigned, unsigned, unsigned, long, long);
typedef void (*D_bsx)(unsigned, long, const void*, unsigned);
typedef void (*D_bsd)(unsigned, long, long, const void*);
typedef void (*D_cbs)(unsigned, unsigned, long, long, long);
typedef void (*D_fmr)(unsigned, long, long);
typedef void (*D_gb64)(unsigned, unsigned, long long*);
typedef void (*D_gbpp)(unsigned, unsigned, void**);
typedef void (*D_fbr)(unsigned, unsigned, unsigned, unsigned);
typedef void (*D_fbt)(unsigned, unsigned, unsigned, int);
typedef void (*D_fbp)(unsigned, unsigned, int);
typedef void (*D_gfp)(unsigned, unsigned, int*);
typedef void (*D_dbs)(int, const unsigned*);
typedef void (*D_rb1)(unsigned);
typedef void (*D_inv)(unsigned, int, const unsigned*);
typedef void (*D_invs)(unsigned, int, const unsigned*, int, int, int, int);
typedef void (*D_cliv)(unsigned, int, const int*);
typedef void (*D_cluv)(unsigned, int, const unsigned*);
typedef void (*D_clfv)(unsigned, int, const float*);
typedef void (*D_clfi)(unsigned, int, float, int);
typedef void (*D_blit)(int, int, int, int, int, int, int, int, unsigned, unsigned);
typedef unsigned (*D_chk)(unsigned);
typedef void (*D_rbs)(unsigned, unsigned, int, int);
typedef void (*D_rbm)(unsigned, int, unsigned, int, int);
typedef void (*D_ve)(unsigned);
typedef void (*D_vaf)(unsigned, int, unsigned, unsigned char, unsigned);
typedef void (*D_vai)(unsigned, int, unsigned, unsigned);
typedef void (*D_vu2)(unsigned, unsigned);
typedef void (*D_bbb)(unsigned, unsigned, unsigned);
typedef void (*D_bbr)(unsigned, unsigned, unsigned, long, long);
typedef void (*D_bsm)(unsigned, unsigned);

/* ---------- Texturi: tabel nume -> target ---------- */
#define DSA_MAXTEX 65536
static unsigned short dsa_tt[DSA_MAXTEX];

static unsigned dsa_tgt(unsigned t) {
    t = t1d(t);
    if (t == 0x84F5) return 0x0DE1;               /* RECTANGLE -> 2D */
    if (t == 0x8C18) return 0x8C1A;               /* 1D_ARRAY -> 2D_ARRAY */
    return t;
}
static void dsa_note(unsigned tex, unsigned tgt) {
    if (tex && tex < DSA_MAXTEX && tgt) dsa_tt[tex] = (unsigned short)dsa_tgt(tgt);
}
static unsigned dsa_bq(unsigned t) {              /* target -> enum de binding */
    switch (t) {
    case 0x806F: return 0x806A;  case 0x8C1A: return 0x8C1D;  case 0x8513: return 0x8514;
    case 0x9009: return 0x900A;  case 0x9100: return 0x9104;  case 0x9102: return 0x9105;
    case 0x8C2A: return 0x8C2C;
    default: return 0x8069;
    }
}
typedef struct { unsigned tgt; int old; } dsa_b;
static dsa_b dsa_in(unsigned tex, unsigned def) {
    dsa_b b;
    b.tgt = (tex && tex < DSA_MAXTEX && dsa_tt[tex]) ? dsa_tt[tex] : def;
    b.old = 0;
    DP(glGetIntegerv, F_geti)(dsa_bq(b.tgt), &b.old);
    DP(glBindTexture, M_bt)(b.tgt, tex);
    return b;
}
static void dsa_out(dsa_b b) { DP(glBindTexture, M_bt)(b.tgt, (unsigned)b.old); }

GLC void glCreateTextures(unsigned target, int n, unsigned* t) {
    unsigned tg = dsa_tgt(target);
    DP(glGenTextures, D_gen)(n, t);
    int old = 0; DP(glGetIntegerv, F_geti)(dsa_bq(tg), &old);
    for (int i = 0; i < n; i++) { DP(glBindTexture, M_bt)(tg, t[i]); dsa_note(t[i], tg); }
    DP(glBindTexture, M_bt)(tg, (unsigned)old);
}

/* ---------- Texturi: storage ---------- */
GLC void glTextureStorage1D(unsigned tex, int lv, unsigned f, int w) {
    dsa_b b = dsa_in(tex, 0x0DE1); DP(glTexStorage2D, D_ts2)(0x0DE1, lv, f, w, 1); dsa_out(b);
}
GLC void glTextureStorage2D(unsigned tex, int lv, unsigned f, int w, int h) {
    dsa_b b = dsa_in(tex, 0x0DE1);
    if (b.tgt == 0x8C1A) DP(glTexStorage3D, D_ts3)(0x8C1A, lv, f, w, 1, h);   /* 1D array emulat */
    else                 DP(glTexStorage2D, D_ts2)(b.tgt, lv, f, w, h);
    dsa_out(b);
}
GLC void glTextureStorage3D(unsigned tex, int lv, unsigned f, int w, int h, int d) {
    dsa_b b = dsa_in(tex, 0x806F); DP(glTexStorage3D, D_ts3)(b.tgt, lv, f, w, h, d); dsa_out(b);
}
GLC void glTextureStorage2DMultisample(unsigned tex, int s, unsigned f, int w, int h, unsigned char fx) {
    dsa_b b = dsa_in(tex, 0x9100); DP(glTexStorage2DMultisample, L_tsm2)(b.tgt, s, f, w, h, fx); dsa_out(b);
}
GLC void glTextureStorage3DMultisample(unsigned tex, int s, unsigned f, int w, int h, int d, unsigned char fx) {
    dsa_b b = dsa_in(tex, 0x9102);
    DP2(glTexStorage3DMultisample, glTexStorage3DMultisampleOES, L_tsm3)(b.tgt, s, f, w, h, d, fx);
    dsa_out(b);
}
/* variantele EXT (cu parametru <target>) */
GLC void glTextureStorage1DEXT(unsigned tex, unsigned tgt, int lv, unsigned f, int w)
    { (void)tgt; dsa_note(tex, 0x0DE1); glTextureStorage1D(tex, lv, f, w); }
GLC void glTextureStorage2DEXT(unsigned tex, unsigned tgt, int lv, unsigned f, int w, int h)
    { dsa_note(tex, tgt); glTextureStorage2D(tex, lv, f, w, h); }
GLC void glTextureStorage3DEXT(unsigned tex, unsigned tgt, int lv, unsigned f, int w, int h, int d)
    { dsa_note(tex, tgt); glTextureStorage3D(tex, lv, f, w, h, d); }

/* ---------- Texturi: upload / copy / mipmap / parametri ---------- */
GLC void glTextureSubImage1D(unsigned tex, int lv, int xo, int w, unsigned f, unsigned ty, const void* px) {
    dsa_b b = dsa_in(tex, 0x0DE1);
    DP(glTexSubImage2D, F_teximg)(0x0DE1, lv, xo, 0, w, 1, f, ty, px);
    dsa_out(b);
}
GLC void glTextureSubImage2D(unsigned tex, int lv, int xo, int yo, int w, int h, unsigned f, unsigned ty, const void* px) {
    dsa_b b = dsa_in(tex, 0x0DE1);
    DP(glTexSubImage2D, F_teximg)(b.tgt, lv, xo, yo, w, h, f, ty, px);
    dsa_out(b);
}
GLC void glTextureSubImage3D(unsigned tex, int lv, int xo, int yo, int zo, int w, int h, int d,
                             unsigned f, unsigned ty, const void* px) {
    dsa_b b = dsa_in(tex, 0x806F);
    if (b.tgt == 0x8513) {                                   /* cubemap: zo = fata */
        if (d == 1) DP(glTexSubImage2D, F_teximg)(0x8515 + (unsigned)zo, lv, xo, yo, w, h, f, ty, px);
        else if (gl_emul_debug_enabled()) fprintf(stderr, "[dsa] glTextureSubImage3D cubemap cu depth>1 nesuportat\n");
    } else DP(glTexSubImage3D, F_tsi3)(b.tgt, lv, xo, yo, zo, w, h, d, f, ty, px);
    dsa_out(b);
}
GLC void glCompressedTextureSubImage1D(unsigned tex, int lv, int xo, int w, unsigned f, int sz, const void* d) {
    dsa_b b = dsa_in(tex, 0x0DE1); glCompressedTexSubImage2D(0x0DE1, lv, xo, 0, w, 1, f, sz, d); dsa_out(b);
}
GLC void glCompressedTextureSubImage2D(unsigned tex, int lv, int xo, int yo, int w, int h, unsigned f, int sz, const void* d) {
    dsa_b b = dsa_in(tex, 0x0DE1); glCompressedTexSubImage2D(b.tgt, lv, xo, yo, w, h, f, sz, d); dsa_out(b);
}
GLC void glCompressedTextureSubImage3D(unsigned tex, int lv, int xo, int yo, int zo, int w, int h, int dp,
                                       unsigned f, int sz, const void* d) {
    dsa_b b = dsa_in(tex, 0x806F);
    if (b.tgt == 0x8513) { if (dp == 1) glCompressedTexSubImage2D(0x8515 + (unsigned)zo, lv, xo, yo, w, h, f, sz, d); }
    else glCompressedTexSubImage3D(b.tgt, lv, xo, yo, zo, w, h, dp, f, sz, d);
    dsa_out(b);
}
GLC void glCopyTextureSubImage1D(unsigned tex, int lv, int xo, int x, int y, int w) {
    dsa_b b = dsa_in(tex, 0x0DE1);
    DP(glCopyTexSubImage2D, T1_ctsi2)(0x0DE1, lv, xo, 0, x, y, w, 1);
    dsa_out(b);
}
GLC void glCopyTextureSubImage2D(unsigned tex, int lv, int xo, int yo, int x, int y, int w, int h) {
    dsa_b b = dsa_in(tex, 0x0DE1);
    unsigned t = b.tgt;
    DP(glCopyTexSubImage2D, T1_ctsi2)(t, lv, xo, yo, x, y, w, h);
    dsa_out(b);
}
GLC void glCopyTextureSubImage3D(unsigned tex, int lv, int xo, int yo, int zo, int x, int y, int w, int h) {
    dsa_b b = dsa_in(tex, 0x806F);
    if (b.tgt == 0x8513) DP(glCopyTexSubImage2D, T1_ctsi2)(0x8515 + (unsigned)zo, lv, xo, yo, x, y, w, h);
    else DP(glCopyTexSubImage3D, D_cs3)(b.tgt, lv, xo, yo, zo, x, y, w, h);
    dsa_out(b);
}
GLC void glGenerateTextureMipmap(unsigned tex) { dsa_b b = dsa_in(tex, 0x0DE1); glGenerateMipmap(b.tgt); dsa_out(b); }

GLC void glTextureParameteri(unsigned t, unsigned p, int v)  { dsa_b b = dsa_in(t, 0x0DE1); glTexParameteri(b.tgt, p, v);  dsa_out(b); }
GLC void glTextureParameterf(unsigned t, unsigned p, float v){ dsa_b b = dsa_in(t, 0x0DE1); glTexParameterf(b.tgt, p, v);  dsa_out(b); }
GLC void glTextureParameteriv(unsigned t, unsigned p, const int* v)   { dsa_b b = dsa_in(t, 0x0DE1); glTexParameteriv(b.tgt, p, v); dsa_out(b); }
GLC void glTextureParameterfv(unsigned t, unsigned p, const float* v) { dsa_b b = dsa_in(t, 0x0DE1); glTexParameterfv(b.tgt, p, v); dsa_out(b); }
GLC void glTextureParameterIiv(unsigned t, unsigned p, const int* v) {
    dsa_b b = dsa_in(t, 0x0DE1); DP(glTexParameterIiv, T1_piv)(b.tgt, p, v); dsa_out(b);
}
GLC void glTextureParameterIuiv(unsigned t, unsigned p, const unsigned* v) {
    dsa_b b = dsa_in(t, 0x0DE1); DP(glTexParameterIuiv, T1_piv)(b.tgt, p, (const int*)v); dsa_out(b);
}
GLC void glGetTextureParameteriv(unsigned t, unsigned p, int* v)   { dsa_b b = dsa_in(t, 0x0DE1); glGetTexParameteriv(b.tgt, p, v); dsa_out(b); }
GLC void glGetTextureParameterfv(unsigned t, unsigned p, float* v) { dsa_b b = dsa_in(t, 0x0DE1); glGetTexParameterfv(b.tgt, p, v); dsa_out(b); }
GLC void glGetTextureParameterIiv(unsigned t, unsigned p, int* v) {
    dsa_b b = dsa_in(t, 0x0DE1); DP(glGetTexParameterIiv, T1_giv)(b.tgt, p, v); dsa_out(b);
}
GLC void glGetTextureParameterIuiv(unsigned t, unsigned p, unsigned* v) {
    dsa_b b = dsa_in(t, 0x0DE1); DP(glGetTexParameterIuiv, T1_giv)(b.tgt, p, (int*)v); dsa_out(b);
}
GLC void glGetTextureLevelParameteriv(unsigned t, int lv, unsigned p, int* v) {
    dsa_b b = dsa_in(t, 0x0DE1); glGetTexLevelParameteriv(b.tgt == 0x8513 ? 0x8515 : b.tgt, lv, p, v); dsa_out(b);
}
GLC void glGetTextureLevelParameterfv(unsigned t, int lv, unsigned p, float* v) {
    dsa_b b = dsa_in(t, 0x0DE1); glGetTexLevelParameterfv(b.tgt == 0x8513 ? 0x8515 : b.tgt, lv, p, v); dsa_out(b);
}
GLC void glGetTextureImage(unsigned tex, int lv, unsigned f, unsigned ty, int bufSize, void* px) {
    (void)bufSize;
    dsa_b b = dsa_in(tex, 0x0DE1); glGetTexImage(b.tgt, lv, f, ty, px); dsa_out(b);
}
GLC void glBindTextureUnit(unsigned unit, unsigned tex) {
    int act = 0; DP(glGetIntegerv, F_geti)(0x84E0, &act);
    DP(glActiveTexture, M_at)(0x84C0 + unit);
    if (tex) {
        unsigned t = (tex < DSA_MAXTEX && dsa_tt[tex]) ? dsa_tt[tex] : 0x0DE1;
        DP(glBindTexture, M_bt)(t, tex);
    } else {
        static const unsigned all[] = { 0x0DE1, 0x806F, 0x8C1A, 0x8513 };
        for (int i = 0; i < 4; i++) DP(glBindTexture, M_bt)(all[i], 0);
    }
    DP(glActiveTexture, M_at)((unsigned)act);
}
GLC void glTextureBuffer(unsigned tex, unsigned ifmt, unsigned buf) {
    dsa_b b = dsa_in(tex, 0x8C2A); dsa_note(tex, 0x8C2A);
    DP2(glTexBuffer, glTexBufferOES, D_tbuf)(0x8C2A, ifmt, buf); dsa_out(b);
}
GLC void glTextureBufferRange(unsigned tex, unsigned ifmt, unsigned buf, long off, long size) {
    dsa_b b = dsa_in(tex, 0x8C2A); dsa_note(tex, 0x8C2A);
    DP2(glTexBufferRange, glTexBufferRangeOES, D_tbr)(0x8C2A, ifmt, buf, off, size); dsa_out(b);
}

/* ---------- Buffere (se foloseste COPY_WRITE_BUFFER 0x8F37 / COPY_READ 0x8F36) ---------- */
static int  dsa_buf_in(unsigned buf) { int o = 0; DP(glGetIntegerv, F_geti)(0x8F37, &o); DP(glBindBuffer, F_uiui)(0x8F37, buf); return o; }
static void dsa_buf_out(int o)       { DP(glBindBuffer, F_uiui)(0x8F37, (unsigned)o); }

GLC void glCreateBuffers(int n, unsigned* ids) {
    DP(glGenBuffers, D_gen)(n, ids);
    int o = 0; DP(glGetIntegerv, F_geti)(0x8F37, &o);
    for (int i = 0; i < n; i++) DP(glBindBuffer, F_uiui)(0x8F37, ids[i]);   /* creeaza obiectul */
    DP(glBindBuffer, F_uiui)(0x8F37, (unsigned)o);
}
GLC void glNamedBufferStorage(unsigned buf, long size, const void* data, unsigned flags) {
    int o = dsa_buf_in(buf); DP2(glBufferStorage, glBufferStorageEXT, D_bsx)(0x8F37, size, data, flags); dsa_buf_out(o);
}
GLC void glNamedBufferData(unsigned buf, long size, const void* data, unsigned usage) {
    int o = dsa_buf_in(buf); DP(glBufferData, D_bsx)(0x8F37, size, data, usage); dsa_buf_out(o);
}
GLC void glNamedBufferSubData(unsigned buf, long off, long size, const void* data) {
    int o = dsa_buf_in(buf); DP(glBufferSubData, D_bsd)(0x8F37, off, size, data); dsa_buf_out(o);
}
GLC void glCopyNamedBufferSubData(unsigned rb, unsigned wb, long ro, long wo, long size) {
    int orr = 0, ow = 0;
    DP(glGetIntegerv, F_geti)(0x8F36, &orr); DP(glGetIntegerv, F_geti)(0x8F37, &ow);
    DP(glBindBuffer, F_uiui)(0x8F36, rb); DP(glBindBuffer, F_uiui)(0x8F37, wb);
    DP(glCopyBufferSubData, D_cbs)(0x8F36, 0x8F37, ro, wo, size);
    DP(glBindBuffer, F_uiui)(0x8F36, (unsigned)orr); DP(glBindBuffer, F_uiui)(0x8F37, (unsigned)ow);
}
GLC void glClearNamedBufferData(unsigned buf, unsigned ifmt, unsigned f, unsigned ty, const void* d) {
    int o = dsa_buf_in(buf); cb_fill(0x8F37, ifmt, 0, 0, f, ty, d, 1); dsa_buf_out(o);
}
GLC void glClearNamedBufferSubData(unsigned buf, unsigned ifmt, long off, long size, unsigned f, unsigned ty, const void* d) {
    int o = dsa_buf_in(buf); cb_fill(0x8F37, ifmt, off, size, f, ty, d, 0); dsa_buf_out(o);
}
GLC void* glMapNamedBufferRange(unsigned buf, long off, long len, unsigned acc) {
    int o = dsa_buf_in(buf); void* p = DP(glMapBufferRange, F_mapr)(0x8F37, off, len, acc); dsa_buf_out(o); return p;
}
GLC void* glMapNamedBuffer(unsigned buf, unsigned access) {
    int o = dsa_buf_in(buf), sz = 0;
    DP(glGetBufferParameteriv, F_gbpi)(0x8F37, 0x8764, &sz);
    unsigned fl = access == 0x88B8 ? 0x1 : access == 0x88B9 ? 0x2 : 0x3;     /* READ_ONLY / WRITE_ONLY / RW */
    void* p = DP(glMapBufferRange, F_mapr)(0x8F37, 0, sz, fl);
    dsa_buf_out(o); return p;
}
GLC unsigned char glUnmapNamedBuffer(unsigned buf) {
    int o = dsa_buf_in(buf); unsigned char r = DP(glUnmapBuffer, F_unmap)(0x8F37); dsa_buf_out(o); return r;
}
GLC void glFlushMappedNamedBufferRange(unsigned buf, long off, long len) {
    int o = dsa_buf_in(buf); DP(glFlushMappedBufferRange, D_fmr)(0x8F37, off, len); dsa_buf_out(o);
}
GLC void glGetNamedBufferParameteriv(unsigned buf, unsigned p, int* v) {
    int o = dsa_buf_in(buf); DP(glGetBufferParameteriv, F_gbpi)(0x8F37, p, v); dsa_buf_out(o);
}
GLC void glGetNamedBufferParameteri64v(unsigned buf, unsigned p, long long* v) {
    int o = dsa_buf_in(buf); DP(glGetBufferParameteri64v, D_gb64)(0x8F37, p, v); dsa_buf_out(o);
}
GLC void glGetNamedBufferPointerv(unsigned buf, unsigned p, void** v) {
    int o = dsa_buf_in(buf); DP(glGetBufferPointerv, D_gbpp)(0x8F37, p, v); dsa_buf_out(o);
}
GLC void glGetNamedBufferSubData(unsigned buf, long off, long size, void* data) {
    int o = dsa_buf_in(buf); glGetBufferSubData(0x8F37, off, size, data); dsa_buf_out(o);
}

/* ---------- Framebuffere (fb 0 = FBO intern al shim-ului, cand exista) ---------- */
static unsigned dsa_fb(unsigned fb) { return fb ? fb : shim_x11_default_fbo(); }
typedef struct { int d, r; } dsa_f;
static dsa_f dsa_fin(unsigned fb) {
    dsa_f s = { 0, 0 };
    DP(glGetIntegerv, F_geti)(0x8CA6, &s.d); DP(glGetIntegerv, F_geti)(0x8CAA, &s.r);
    DP(glBindFramebuffer, F_uiui)(0x8D40, dsa_fb(fb));
    return s;
}
static void dsa_fout(dsa_f s) {
    DP(glBindFramebuffer, F_uiui)(0x8CA9, (unsigned)s.d); DP(glBindFramebuffer, F_uiui)(0x8CA8, (unsigned)s.r);
}
static unsigned dsa_db(unsigned fb, unsigned b) {          /* GL_BACK etc. -> COLOR_ATTACHMENT0 pe FBO-ul shim */
    if (!fb && dsa_fb(0) && (b == 0x0405 || b == 0x0404 || b == 0x0400 || b == 0x0402)) return 0x8CE0;
    return b;
}

GLC void glCreateFramebuffers(int n, unsigned* ids) {
    DP(glGenFramebuffers, D_gen)(n, ids);
    int d = 0, r = 0; DP(glGetIntegerv, F_geti)(0x8CA6, &d); DP(glGetIntegerv, F_geti)(0x8CAA, &r);
    for (int i = 0; i < n; i++) DP(glBindFramebuffer, F_uiui)(0x8D40, ids[i]);
    DP(glBindFramebuffer, F_uiui)(0x8CA9, (unsigned)d); DP(glBindFramebuffer, F_uiui)(0x8CA8, (unsigned)r);
}
GLC void glNamedFramebufferRenderbuffer(unsigned fb, unsigned att, unsigned rbt, unsigned rb) {
    dsa_f s = dsa_fin(fb); DP(glFramebufferRenderbuffer, D_fbr)(0x8D40, att, rbt, rb); dsa_fout(s);
}
GLC void glNamedFramebufferParameteri(unsigned fb, unsigned p, int v) {
    dsa_f s = dsa_fin(fb); DP(glFramebufferParameteri, D_fbp)(0x8D40, p, v); dsa_fout(s);
}
GLC void glNamedFramebufferTexture(unsigned fb, unsigned att, unsigned tex, int lv) {
    dsa_f s = dsa_fin(fb);
    DP2(glFramebufferTexture, glFramebufferTextureEXT, D_fbt)(0x8D40, att, tex, lv);
    dsa_fout(s);
}
GLC void glNamedFramebufferTextureLayer(unsigned fb, unsigned att, unsigned tex, int lv, int layer) {
    dsa_f s = dsa_fin(fb); DP(glFramebufferTextureLayer, J_fbtl)(0x8D40, att, tex, lv, layer); dsa_fout(s);
}
GLC void glNamedFramebufferDrawBuffers(unsigned fb, int n, const unsigned* bufs) {
    unsigned t[16]; if (n > 16) n = 16;
    for (int i = 0; i < n; i++) t[i] = dsa_db(fb, bufs[i]);
    dsa_f s = dsa_fin(fb); DP(glDrawBuffers, D_dbs)(n, t); dsa_fout(s);
}
GLC void glNamedFramebufferDrawBuffer(unsigned fb, unsigned buf) { glNamedFramebufferDrawBuffers(fb, 1, &buf); }
GLC void glNamedFramebufferReadBuffer(unsigned fb, unsigned src) {
    dsa_f s = dsa_fin(fb); DP(glReadBuffer, D_rb1)(dsa_db(fb, src)); dsa_fout(s);
}
GLC void glInvalidateNamedFramebufferData(unsigned fb, int n, const unsigned* a) {
    dsa_f s = dsa_fin(fb); DP(glInvalidateFramebuffer, D_inv)(0x8D40, n, a); dsa_fout(s);
}
GLC void glInvalidateNamedFramebufferSubData(unsigned fb, int n, const unsigned* a, int x, int y, int w, int h) {
    dsa_f s = dsa_fin(fb); DP(glInvalidateSubFramebuffer, D_invs)(0x8D40, n, a, x, y, w, h); dsa_fout(s);
}
GLC void glClearNamedFramebufferiv(unsigned fb, unsigned buf, int db, const int* v) {
    dsa_f s = dsa_fin(fb); DP(glClearBufferiv, D_cliv)(buf, db, v); dsa_fout(s);
}
GLC void glClearNamedFramebufferuiv(unsigned fb, unsigned buf, int db, const unsigned* v) {
    dsa_f s = dsa_fin(fb); DP(glClearBufferuiv, D_cluv)(buf, db, v); dsa_fout(s);
}
GLC void glClearNamedFramebufferfv(unsigned fb, unsigned buf, int db, const float* v) {
    dsa_f s = dsa_fin(fb); DP(glClearBufferfv, D_clfv)(buf, db, v); dsa_fout(s);
}
GLC void glClearNamedFramebufferfi(unsigned fb, unsigned buf, int db, float d, int st) {
    dsa_f s = dsa_fin(fb); DP(glClearBufferfi, D_clfi)(buf, db, d, st); dsa_fout(s);
}
GLC void glBlitNamedFramebuffer(unsigned rfb, unsigned dfb, int sx0, int sy0, int sx1, int sy1,
                                int dx0, int dy0, int dx1, int dy1, unsigned mask, unsigned filter) {
    dsa_f s = { 0, 0 };
    DP(glGetIntegerv, F_geti)(0x8CA6, &s.d); DP(glGetIntegerv, F_geti)(0x8CAA, &s.r);
    DP(glBindFramebuffer, F_uiui)(0x8CA8, dsa_fb(rfb)); DP(glBindFramebuffer, F_uiui)(0x8CA9, dsa_fb(dfb));
    DP(glBlitFramebuffer, D_blit)(sx0, sy0, sx1, sy1, dx0, dy0, dx1, dy1, mask, filter);
    dsa_fout(s);
}
GLC unsigned glCheckNamedFramebufferStatus(unsigned fb, unsigned target) {
    (void)target; dsa_f s = dsa_fin(fb); unsigned r = DP(glCheckFramebufferStatus, D_chk)(0x8D40); dsa_fout(s); return r;
}
GLC void glGetNamedFramebufferParameteriv(unsigned fb, unsigned p, int* v) {
    dsa_f s = dsa_fin(fb); DP(glGetFramebufferParameteriv, D_gfp)(0x8D40, p, v); dsa_fout(s);
}
GLC void glGetNamedFramebufferAttachmentParameteriv(unsigned fb, unsigned att, unsigned p, int* v) {
    dsa_f s = dsa_fin(fb); glGetFramebufferAttachmentParameteriv(0x8D40, att, p, v); dsa_fout(s);
}

/* ---------- Renderbuffere ---------- */
GLC void glCreateRenderbuffers(int n, unsigned* ids) {
    DP(glGenRenderbuffers, D_gen)(n, ids);
    int o = 0; DP(glGetIntegerv, F_geti)(0x8CA7, &o);
    for (int i = 0; i < n; i++) DP(glBindRenderbuffer, F_uiui)(0x8D41, ids[i]);
    DP(glBindRenderbuffer, F_uiui)(0x8D41, (unsigned)o);
}
GLC void glNamedRenderbufferStorage(unsigned rb, unsigned f, int w, int h) {
    int o = 0; DP(glGetIntegerv, F_geti)(0x8CA7, &o);
    DP(glBindRenderbuffer, F_uiui)(0x8D41, rb); DP(glRenderbufferStorage, D_rbs)(0x8D41, f, w, h);
    DP(glBindRenderbuffer, F_uiui)(0x8D41, (unsigned)o);
}
GLC void glNamedRenderbufferStorageMultisample(unsigned rb, int s, unsigned f, int w, int h) {
    int o = 0; DP(glGetIntegerv, F_geti)(0x8CA7, &o);
    DP(glBindRenderbuffer, F_uiui)(0x8D41, rb); DP(glRenderbufferStorageMultisample, D_rbm)(0x8D41, s, f, w, h);
    DP(glBindRenderbuffer, F_uiui)(0x8D41, (unsigned)o);
}
GLC void glGetNamedRenderbufferParameteriv(unsigned rb, unsigned p, int* v) {
    int o = 0; DP(glGetIntegerv, F_geti)(0x8CA7, &o);
    DP(glBindRenderbuffer, F_uiui)(0x8D41, rb); DP(glGetRenderbufferParameteriv, D_gfp)(0x8D41, p, v);
    DP(glBindRenderbuffer, F_uiui)(0x8D41, (unsigned)o);
}

/* ---------- Vertex arrays ---------- */
static int  dsa_va_in(unsigned vao) { int o = 0; DP(glGetIntegerv, F_geti)(0x85B5, &o); DP(glBindVertexArray, D_ve)(vao); return o; }
static void dsa_va_out(int o)       { DP(glBindVertexArray, D_ve)((unsigned)o); }

GLC void glCreateVertexArrays(int n, unsigned* ids) {
    DP(glGenVertexArrays, D_gen)(n, ids);
    int o = 0; DP(glGetIntegerv, F_geti)(0x85B5, &o);
    for (int i = 0; i < n; i++) DP(glBindVertexArray, D_ve)(ids[i]);
    DP(glBindVertexArray, D_ve)((unsigned)o);
}
GLC void glVertexArrayElementBuffer(unsigned vao, unsigned buf) {
    int o = dsa_va_in(vao); DP(glBindBuffer, F_uiui)(0x8893, buf); dsa_va_out(o);
}
GLC void glVertexArrayVertexBuffer(unsigned vao, unsigned idx, unsigned buf, long off, int stride) {
    int o = dsa_va_in(vao); DP(glBindVertexBuffer, M_bvb)(idx, buf, off, stride); dsa_va_out(o);
}
GLC void glVertexArrayVertexBuffers(unsigned vao, unsigned first, int cnt, const unsigned* b, const long* off, const int* st) {
    int o = dsa_va_in(vao); glBindVertexBuffers(first, cnt, b, off, st); dsa_va_out(o);
}
GLC void glVertexArrayAttribFormat(unsigned vao, unsigned i, int sz, unsigned ty, unsigned char nm, unsigned ro) {
    int o = dsa_va_in(vao); DP(glVertexAttribFormat, D_vaf)(i, sz, ty, nm, ro); dsa_va_out(o);
}
GLC void glVertexArrayAttribIFormat(unsigned vao, unsigned i, int sz, unsigned ty, unsigned ro) {
    int o = dsa_va_in(vao); DP(glVertexAttribIFormat, D_vai)(i, sz, ty, ro); dsa_va_out(o);
}
GLC void glVertexArrayAttribLFormat(unsigned vao, unsigned i, int sz, unsigned ty, unsigned ro) {
    (void)vao; (void)i; (void)sz; (void)ty; (void)ro;
    if (gl_emul_debug_enabled()) fprintf(stderr, "[dsa] glVertexArrayAttribLFormat (double) nesuportat in ES\n");
}
GLC void glVertexArrayAttribBinding(unsigned vao, unsigned i, unsigned b) {
    int o = dsa_va_in(vao); DP(glVertexAttribBinding, D_vu2)(i, b); dsa_va_out(o);
}
GLC void glVertexArrayBindingDivisor(unsigned vao, unsigned b, unsigned d) {
    int o = dsa_va_in(vao); DP(glVertexBindingDivisor, D_vu2)(b, d); dsa_va_out(o);
}
GLC void glEnableVertexArrayAttrib(unsigned vao, unsigned i)  { int o = dsa_va_in(vao); DP(glEnableVertexAttribArray, D_ve)(i);  dsa_va_out(o); }
GLC void glDisableVertexArrayAttrib(unsigned vao, unsigned i) { int o = dsa_va_in(vao); DP(glDisableVertexAttribArray, D_ve)(i); dsa_va_out(o); }
GLC void glGetVertexArrayiv(unsigned vao, unsigned p, int* v) {
    int o = dsa_va_in(vao);
    if (p == 0x8895) DP(glGetIntegerv, F_geti)(0x8895, v);        /* ELEMENT_ARRAY_BUFFER_BINDING */
    dsa_va_out(o);
}

/* ---------- Samplers / queries / transform feedback ---------- */
GLC void glCreateSamplers(int n, unsigned* ids) {
    DP(glGenSamplers, D_gen)(n, ids);
    int o = 0; DP(glGetIntegerv, F_geti)(0x8919, &o);
    for (int i = 0; i < n; i++) DP(glBindSampler, M_bs)(0, ids[i]);
    DP(glBindSampler, M_bs)(0, (unsigned)o);
}
GLC void glCreateQueries(unsigned target, int n, unsigned* ids) { (void)target; DP(glGenQueries, D_gen)(n, ids); }

GLC void glCreateTransformFeedbacks(int n, unsigned* ids) {
    unsigned keep = tfo_cur;
    DP(glGenTransformFeedbacks, D_gen)(n, ids);
    for (int i = 0; i < n; i++) glBindTransformFeedback(0x8E22, ids[i]);
    glBindTransformFeedback(0x8E22, keep);
}
GLC void glTransformFeedbackBufferBase(unsigned xfb, unsigned idx, unsigned buf) {
    unsigned keep = tfo_cur; glBindTransformFeedback(0x8E22, xfb);
    DP(glBindBufferBase, D_bbb)(0x8C8E, idx, buf);
    glBindTransformFeedback(0x8E22, keep);
}
GLC void glTransformFeedbackBufferRange(unsigned xfb, unsigned idx, unsigned buf, long off, long size) {
    unsigned keep = tfo_cur; glBindTransformFeedback(0x8E22, xfb);
    DP(glBindBufferRange, D_bbr)(0x8C8E, idx, buf, off, size);
    glBindTransformFeedback(0x8E22, keep);
}
