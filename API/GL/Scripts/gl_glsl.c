/*
 * gl_glsl.c - included by gl_es_emul.c (after glGetStringi, before section F).
 * Desktop GLSL -> GLSL ES translation, shader source store, glShaderSource,
 * glDeleteProgram/glUseProgram. Not a separate TU: uses static helpers and
 * RP()/T_glx from gl_es_emul.c.
 * Needs defined before: cc_prog, cc_forget, cc_apply, F_shsrc, F_ui, emu_ver,
 *                       gl_emul_debug_enabled.
 * Includes: gl_dsb.c, gl_litfix.c, gl_ssbo.c
 */
typedef void (*F_gsiv)(unsigned, unsigned, int*);
typedef void (*F_gssrc)(unsigned, int, int*, char*);
static char** sh_orig;
static unsigned sh_cap;
static pthread_mutex_t sh_mu = PTHREAD_MUTEX_INITIALIZER;

static void sh_store(unsigned id, char* orig) {
    if (id >= 1u << 20) { free(orig); return; }
    pthread_mutex_lock(&sh_mu);
    if (id >= sh_cap) {
        unsigned nc = sh_cap ? sh_cap : 64;
        while (nc <= id) nc *= 2;
        sh_orig = realloc(sh_orig, nc * sizeof *sh_orig);
        memset(sh_orig + sh_cap, 0, (nc - sh_cap) * sizeof *sh_orig);
        sh_cap = nc;
    }
    free(sh_orig[id]);
    sh_orig[id] = orig;
    pthread_mutex_unlock(&sh_mu);
}

static char* sh_get_dup(unsigned id) {
    char* r = NULL;
    pthread_mutex_lock(&sh_mu);
    if (id < sh_cap && sh_orig[id]) r = strdup(sh_orig[id]);
    pthread_mutex_unlock(&sh_mu);
    return r;
}

GLC void glGetShaderiv(unsigned sh, unsigned pname, int* v) {
    if (pname == 0x8B88) {
        char* o = sh_get_dup(sh);
        if (o) { *v = (int)strlen(o) + 1; free(o); return; }
    }
    RP(glGetShaderiv, F_gsiv)(sh, pname, v);
}

GLC void glGetShaderSource(unsigned sh, int bufsize, int* length, char* out) {
    char* o = sh_get_dup(sh);
    if (!o) { RP(glGetShaderSource, F_gssrc)(sh, bufsize, length, out); return; }
    int n = 0;
    if (bufsize > 0 && out) {
        n = (int)strlen(o);
        if (n > bufsize - 1) n = bufsize - 1;
        memcpy(out, o, (size_t)n); out[n] = 0;
    }
    if (length) *length = n;
    free(o);
}

static int gl_has(const char* h, size_t hl, const char* n) {
    size_t nl = strlen(n);
    for (size_t i = 0; i + nl <= hl; i++) if (!memcmp(h + i, n, nl)) return 1;
    return 0;
}

static int gl_idch(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; }

static void gl_blank_word(char* s, size_t n, const char* w) {
    size_t wl = strlen(w);
    for (size_t i = 0; i + wl <= n; i++)
        if (!memcmp(s + i, w, wl) && (i == 0 || !gl_idch(s[i - 1])) && (i + wl == n || !gl_idch(s[i + wl])))
            memset(s + i, ' ', wl);
}

static void gl_rep_word(char* s, size_t n, const char* from, const char* to) {
    size_t l = strlen(from);
    for (size_t i = 0; i + l <= n; i++)
        if (!memcmp(s + i, from, l) && (i == 0 || !gl_idch(s[i - 1])) && (i + l == n || !gl_idch(s[i + l])))
            memcpy(s + i, to, l);
}

