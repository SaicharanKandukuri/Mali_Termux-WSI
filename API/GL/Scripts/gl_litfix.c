/*
 * gl_litfix.c - included by gl_es_emul.c (after glsl_to_es, before glShaderSource).
 *
 * Desktop GLSL implicitly converts int -> float ("float a = 1;", "max(x, 0)", "a * 2"); GLSL ES does not.
 * Mali reports the error with the line number ("0:3: S0001: Type mismatch, cannot convert from 'int' to 'float'").
 * On glCompileShader failure for a shader translated by us:
 *   1. read error lines from the log and classify error mode (float, uint, non-const variable);
 *   2. promote integer literals/macros/int-vars on THOSE lines (1 -> 1.0 or 1u, MAC -> float(MAC) or uint(MAC)),
 *      or strip 'const' from variables initialized with non-constant expressions;
 *   3. recompile; repeat up to 8 times (new errors may appear on other lines);
 *   4. if it still fails, restore the initial translated source: the application's view of the log remains original.
 * A shader that already compiles is never modified. GLX_NO_LITFIX=1 disables literal fixing.
 */

#define LF_FLOAT 1
#define LF_UINT  2
#define LF_CONST 4

typedef struct { char* p; size_t n, cap; } gl_sb;

static void sb_put(gl_sb* b, const char* s, size_t n) {
    if (b->n + n + 1 > b->cap) { b->cap = (b->n + n + 1) * 2; b->p = realloc(b->p, b->cap); }
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = 0;
}

static int lf_digit(char c) { return c >= '0' && c <= '9'; }
static int lf_alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
static int lf_idch(char c)  { return lf_alpha(c) || lf_digit(c); }

static const char* lf_last_line_dir(const char* t) {
    const char* d = NULL;
    for (const char* p = t; (p = strstr(p, "#line ")); p += 6)
        if (p == t || p[-1] == '\n') d = p;
    return d;
}

/* "#define NAME <integer>" in translated source; on error lines NAME -> float(NAME) / uint(NAME) */
static __thread char lf_mac[256][48];
static __thread int  lf_nmac;

static void lf_scan_macros(const char* t) {
    lf_nmac = 0;
    for (const char* p = t; *p && lf_nmac < 256; ) {
        const char* e = strchr(p, '\n');
        const char* q = p;
        while (*q == ' ' || *q == '\t') q++;
        if (!strncmp(q, "#define", 7) && (q[7] == ' ' || q[7] == '\t')) {
            q += 7;
            while (*q == ' ' || *q == '\t') q++;
            const char* nm = q;
            while (lf_idch(*q)) q++;
            size_t nl = (size_t)(q - nm);
            if (nl && nl < 48 && (*q == ' ' || *q == '\t')) {          /* object-like, not NAME( */
                while (*q == ' ' || *q == '\t') q++;
                const char* d = q;
                while (lf_digit(*q)) q++;
                if (q > d) {
                    while (*q == ' ' || *q == '\t' || *q == '\r') q++;
                    if (*q == 0 || *q == '\n' || (q[0] == '/' && (q[1] == '/' || q[1] == '*'))) {
                        memcpy(lf_mac[lf_nmac], nm, nl);
                        lf_mac[lf_nmac++][nl] = 0;
                    }
                }
            }
        }
        if (!e) break;
        p = e + 1;
    }
}

static int lf_is_intmacro(const char* s, size_t n) {
    for (int i = 0; i < lf_nmac; i++)
        if (strlen(lf_mac[i]) == n && !memcmp(lf_mac[i], s, n)) return 1;
    return 0;
}

/* int / uint variable declarations in translated source; on error lines wrap in float(...) / uint(...) near binops */
static __thread char lf_iv[128][48];
static __thread int  lf_niv;

static void lf_scan_intvars(const char* t) {
    lf_niv = 0;
    for (const char* p = t; *p && lf_niv < 128; p++) {
        if (p != t && lf_idch(p[-1])) continue;
        int ul = !strncmp(p, "uint", 4) ? 4 : !strncmp(p, "int", 3) ? 3 : 0;
        if (!ul) continue;
        const char* q = p + ul;
        if (lf_idch(*q)) continue;
        while (*q == ' ' || *q == '\t') q++;
        if (!lf_alpha(*q)) continue;                 /* int( / uint( */
        const char* nm = q;
        while (lf_idch(*q)) q++;
        size_t nl = (size_t)(q - nm);
        while (*q == ' ' || *q == '\t') q++;
        if (*q == '(' || *q == '[' || nl >= 48) continue;   /* function / array */
        memcpy(lf_iv[lf_niv], nm, nl); lf_iv[lf_niv++][nl] = 0;
    }
}

static int lf_is_intvar(const char* s, size_t n) {
    for (int i = 0; i < lf_niv; i++)
        if (strlen(lf_iv[i]) == n && !memcmp(lf_iv[i], s, n)) return 1;
    return 0;
}

static int lf_binop_adjacent(const char* s, size_t n, size_t i, size_t j) {
    size_t k = i;
    while (k > 0 && (s[k-1] == ' ' || s[k-1] == '\t')) k--;
    if (k > 0) {
        char c = s[k-1];
        if (strchr("<>+-*/%&|^", c)) return 1;      /* include <<, >> si bit ops */
        if (c == '=') return 1;                      /* =, ==, !=, <=, >= */
    }
    k = j;
    while (k < n && (s[k] == ' ' || s[k] == '\t')) k++;
    if (k < n) {
        char c = s[k];
        if (strchr("<>+-*/%&|^", c)) return 1;
        if (c == '=' && k + 1 < n && s[k+1] == '=') return 1;
        if (c == '!' && k + 1 < n && s[k+1] == '=') return 1;
    }
    return 0;
}

/* copies line s[0..n) into b, promoting integer literals, macros and int-vars to float or uint. Returns 1 if changed. */
static int gl_promote_line(gl_sb* b, const char* s, size_t n, int mode) {
    size_t i = 0;
    int changed = 0, br = 0;
    while (i < n && (s[i] == ' ' || s[i] == '\t')) i++;
    if (i < n && s[i] == '#') { sb_put(b, s, n); return 0; }                 /* preprocessor directive */
    i = 0;
    while (i < n) {
        char c = s[i];
        if (c == '/' && i + 1 < n && s[i + 1] == '/') { sb_put(b, s + i, n - i); break; }   /* comment */
        if (lf_alpha(c)) {                                                   /* identifier / keyword */
            size_t j = i;
            while (j < n && lf_idch(s[j])) j++;
            int is_layout = (j - i == 6 && !memcmp(s + i, "layout", 6));
            if (!is_layout && br == 0 && lf_is_intvar(s + i, j - i) && lf_binop_adjacent(s, n, i, j)) {
                sb_put(b, mode == LF_UINT ? "uint(" : "float(", mode == LF_UINT ? 5 : 6);
                sb_put(b, s + i, j - i);
                sb_put(b, ")", 1);
                changed = 1; i = j; continue;
            }
            if (!is_layout && (br == 0 || mode == LF_UINT) && lf_is_intmacro(s + i, j - i)) {
                sb_put(b, mode == LF_UINT ? "uint(" : "float(", mode == LF_UINT ? 5 : 6);
                sb_put(b, s + i, j - i);
                sb_put(b, ")", 1);
                changed = 1; i = j; continue;
            }
            sb_put(b, s + i, j - i);
            i = j;
            if (is_layout) {                                                 /* layout(...): values must remain int */
                size_t k = i;
                while (k < n && (s[k] == ' ' || s[k] == '\t')) k++;
                if (k < n && s[k] == '(') {
                    int depth = 0;
                    size_t m = k;
                    for (; m < n; m++) {
                        if (s[m] == '(') depth++;
                        else if (s[m] == ')' && --depth == 0) { m++; break; }
                    }
                    sb_put(b, s + i, m - i);
                    i = m;
                }
            }
            continue;
        }
        if (c == '[') br++;
        else if (c == ']' && br > 0) br--;
        if (lf_digit(c) || (c == '.' && i + 1 < n && lf_digit(s[i + 1]))) {
            size_t j = i;
            int is_float = 0, is_hex = 0, is_u = 0;
            if (c == '0' && i + 1 < n && (s[i + 1] == 'x' || s[i + 1] == 'X')) {
                is_hex = 1; j = i + 2;
                while (j < n && lf_idch(s[j])) j++;
            } else {
                while (j < n && lf_digit(s[j])) j++;
                if (j < n && s[j] == '.') { is_float = 1; j++; while (j < n && lf_digit(s[j])) j++; }
                if (j < n && (s[j] == 'e' || s[j] == 'E')) {
                    is_float = 1; j++;
                    if (j < n && (s[j] == '+' || s[j] == '-')) j++;
                    while (j < n && lf_digit(s[j])) j++;
                }
                if (j < n && (s[j] == 'f' || s[j] == 'F')) { is_float = 1; j++; }
                else if (j < n && (s[j] == 'u' || s[j] == 'U')) { is_u = 1; j++; }
            }
            sb_put(b, s + i, j - i);
            if (!is_float && !is_hex && !is_u && br == 0 &&
    ((mode == LF_FLOAT) || (mode == LF_UINT && lf_binop_adjacent(s, n, i, j)))) {
                if (mode == LF_UINT) sb_put(b, "u", 1);
                else sb_put(b, ".0", 2);
                changed = 1;
            }
            i = j;
            continue;
        }
        sb_put(b, &s[i], 1);
        i++;
    }
    return changed;
}


typedef struct { int line, mode; char name[48]; } lf_err;

static int lf_parse_log(const char* log, lf_err* er, int max) {
    int n = 0;
    for (const char* p = log; *p && n < max; p++) {
        if (!lf_digit(*p)) continue;
        if (p != log && p[-1] != '\n' && p[-1] != ' ') continue;
        const char* q = p;
        while (lf_digit(*q)) q++;
        if (*q != ':' || !lf_digit(q[1])) continue;
        int L = atoi(q + 1);
        if (L <= 0) continue;

        const char* e = strchr(p, '\n');
        size_t ml = e ? (size_t)(e - p) : strlen(p);
        char msg[256];
        if (ml >= sizeof msg) ml = sizeof msg - 1;
        memcpy(msg, p, ml); msg[ml] = 0;
        int mode = LF_FLOAT; char name[48] = "";
        const char* c = strstr(msg, "const variable '");
        if (c) {
            mode = LF_CONST; c += 16;
            size_t k = 0;
            while (c[k] && c[k] != '\'' && k < 47) { name[k] = c[k]; k++; }
            name[k] = 0;
        } else if ((strstr(msg, "'uint'") && strstr(msg, "'int'")) ||
           strstr(msg, "Cannot compare 'uint'") ||
           strstr(msg, "with 'uint'")) mode = LF_UINT;
        int dup = 0;
        for (int k = 0; k < n; k++)
            if (er[k].line == L && er[k].mode == mode && !strcmp(er[k].name, name)) dup = 1;
        if (!dup) { er[n].line = L; er[n].mode = mode; strcpy(er[n].name, name); n++; }
        p += ml ? ml - 1 : 0;
    }
    return n;
}

/* "const" dinaintea declaratiei lui `name` pe aceeasi linie */
static const char* lf_find_const(const char* s, size_t n, const char* name) {
    size_t nl = strlen(name);
    if (!nl) return NULL;
    for (size_t i = 0; i + nl <= n; i++) {
        if (memcmp(s + i, name, nl)) continue;
        if (i && lf_idch(s[i-1])) continue;
        if (i + nl < n && lf_idch(s[i + nl])) continue;
        for (size_t k = 0; k + 5 <= i; k++)
            if (!memcmp(s + k, "const", 5) && (k == 0 || !lf_idch(s[k-1])) && !lf_idch(s[k+5]))
                return s + k;
        return NULL;
    }
    return NULL;
}

static int lf_enum_member(const char* p, size_t L) {
    size_t i = 0;
    while (i < L && (p[i] == ' ' || p[i] == '\t')) i++;
    if (i >= L || !lf_alpha(p[i])) return 0;
    while (i < L && lf_idch(p[i])) i++;
    while (i < L && (p[i] == ' ' || p[i] == '\t')) i++;
    if (i >= L || p[i] != '=' || (i + 1 < L && p[i + 1] == '=')) return 0;
    for (size_t k = i; k < L; k++)
        if (p[k] == ';' || p[k] == '(') return 0;
    return 1;
}

static int lf_blank(const char* p, size_t L) {
    for (size_t i = 0; i < L; i++)
        if (p[i] != ' ' && p[i] != '\t' && p[i] != '\r') return 0;
    return 1;
}

static char lf_lastch(const char* p, size_t L) {
    size_t i = 0;
    while (i < L && (p[i] == ' ' || p[i] == '\t')) i++;
    if (i < L && p[i] == '#') return '#';
    while (L > i && (p[L-1] == ' ' || p[L-1] == '\t' || p[L-1] == '\r')) L--;
    return L > i ? p[L-1] : 0;
}

static char* gl_promote_errors(const char* text, const char* log) {
    lf_scan_macros(text);
    lf_scan_intvars(text);
    lf_err er[32];
    int ne = lf_parse_log(log, er, 32);
    if (!ne) return NULL;

    const char* d = lf_last_line_dir(text);
    int ln = 1;
    const char* body = text;
    if (d) {
        ln = atoi(d + 6);
        const char* e = strchr(d, '\n');
        body = e ? e + 1 : d + strlen(d);
    }
    int base = ln;
    int dbg = getenv("GLX_LITFIX_DEBUG") != NULL;

    /* index the lines */
    int nl = 0;
    for (const char* p = body; *p; nl++) {
        const char* e = strchr(p, '\n');
        p = e ? e + 1 : p + strlen(p);
    }
    const char** ls = malloc((nl + 1) * sizeof *ls);
    size_t* ll = malloc((nl + 1) * sizeof *ll);
    unsigned char* mk = calloc(nl + 1, 1);
    {
        int i = 0;
        for (const char* p = body; *p && i < nl; i++) {
            const char* e = strchr(p, '\n');
            ls[i] = p; ll[i] = e ? (size_t)(e - p) : strlen(p);
            p = e ? e + 1 : p + ll[i];
        }
    }

    /* mark the error line + preceding lines of the same statement */
    for (int k = 0; k < ne; k++) {
        if (!(er[k].mode & (LF_UINT | LF_FLOAT))) continue;
        int idx = er[k].line - base;
        if (idx < 0 || idx >= nl) continue;
        mk[idx] |= er[k].mode;
        for (int j = idx - 1; j >= 0 && j >= idx - 40; j--) {
            char c = lf_lastch(ls[j], ll[j]);
            if (c == 0 || c == '#') continue;
            if (c == ';' || c == '{' || c == '}') break;
            mk[j] |= er[k].mode;
        }
    }

    gl_sb b = { 0 };
    sb_put(&b, text, (size_t)(body - text));
    int changed = 0, carry = 0;

    for (int i = 0; i < nl; i++, ln++) {
        const char* p = ls[i];
        size_t L = ll[i];
        int mode = mk[i], ch = 0;
        const char* cs = NULL;
        for (int k = 0; k < ne; k++)
            if (!cs && er[k].mode == LF_CONST) cs = lf_find_const(p, L, er[k].name);

        if (mode & LF_UINT) carry = lf_enum_member(p, L);
        else if (carry) {
            if (lf_enum_member(p, L)) mode |= LF_UINT;
            else if (!lf_blank(p, L)) carry = 0;
        }
        size_t line_start = b.n;
        if (cs) {
            size_t a = (size_t)(cs - p), z = a + 5;
            while (z < L && (p[z] == ' ' || p[z] == '\t')) z++;
            sb_put(&b, p, a); sb_put(&b, p + z, L - z);
            ch = 1;
        } else if (mode & (LF_UINT | LF_FLOAT)) {
            ch = gl_promote_line(&b, p, L, (mode & LF_UINT) ? LF_UINT : LF_FLOAT);
        } else sb_put(&b, p, L);
        changed |= ch;
        if (dbg && mode) {
            if (ch) fprintf(stderr, "[litfix] line %d -> %s\n", ln, b.p + line_start);
            else    fprintf(stderr, "[litfix] line %d unchanged: %.*s\n", ln, (int)L, p);
        }
        if (p[L] == '\n') sb_put(&b, "\n", 1);
    }

    if (dbg && !changed) {
        for (int k = 0; k < ne; k++)
            for (int i = (er[k].line - base) - 15; i <= er[k].line - base; i++)
                if (i >= 0 && i < nl)
                    fprintf(stderr, "[ctx] %d: %.*s\n", base + i, (int)ll[i], ls[i]);
    }
    free(ls); free(ll); free(mk);
    if (!changed) { free(b.p); return NULL; }
    return b.p;
}


static void lf_dump_lines(const char* text, const char* log) {
    lf_err er[32];
    int ne = lf_parse_log(log, er, 32);
    const char* d = lf_last_line_dir(text);
    int ln = 1;
    const char* body = text;
    if (d) {
        ln = atoi(d + 6);
        const char* e = strchr(d, '\n');
        body = e ? e + 1 : d + strlen(d);
    }
    for (const char* p = body; *p; ln++) {
        const char* e = strchr(p, '\n');
        size_t L = e ? (size_t)(e - p) : strlen(p);
        for (int k = 0; k < ne; k++)
            if (er[k].line == ln) {
                fprintf(stderr, "[litfix] line %d: %.*s\n", ln, (int)L, p);
                break;
            }
        p = e ? e + 1 : p + L;
    }
}

/* ---- translated source kept per shader for recompilation ---- */
static char** sh_tr;
static unsigned sh_tr_cap;

static void sh_tr_store(unsigned id, char* t) {          /* takes ownership; NULL = deletes */
    if (id >= 1u << 20) { free(t); return; }
    pthread_mutex_lock(&sh_mu);
    if (id >= sh_tr_cap) {
        unsigned nc = sh_tr_cap ? sh_tr_cap : 64;
        while (nc <= id) nc *= 2;
        sh_tr = realloc(sh_tr, nc * sizeof *sh_tr);
        memset(sh_tr + sh_tr_cap, 0, (nc - sh_tr_cap) * sizeof *sh_tr);
        sh_tr_cap = nc;
    }
    free(sh_tr[id]);
    sh_tr[id] = t;
    pthread_mutex_unlock(&sh_mu);
}

static char* sh_tr_get_dup(unsigned id) {
    char* r = NULL;
    pthread_mutex_lock(&sh_mu);
    if (id < sh_tr_cap && sh_tr[id]) r = strdup(sh_tr[id]);
    pthread_mutex_unlock(&sh_mu);
    return r;
}

typedef void (*F_gsil)(unsigned, int, int*, char*);

/* ---- expandare manuala pentru #define NAME(args) ... ## ... (GLSL ES nu are token pasting) ---- */
#define PM_MAX 48
typedef struct { char name[48]; int np; char par[8][32]; char* body; } pm_t;

static const char* pm_lend(const char* p) {          /* sfarsitul liniei logice (cu '\' continuare) */
    for (;;) {
        const char* e = strchr(p, '\n');
        if (!e) return p + strlen(p);
        const char* b = e;
        if (b > p && b[-1] == '\r') b--;
        if (b > p && b[-1] == '\\') { p = e + 1; continue; }
        return e;
    }
}

static int pm_parse(pm_t* m, const char* q, const char* e) {   /* q dupa "#define" */
    while (*q == ' ' || *q == '\t') q++;
    const char* nm = q;
    while (lf_idch(*q)) q++;
    size_t nl = (size_t)(q - nm);
    if (!nl || nl >= 48 || *q != '(') return 0;
    memcpy(m->name, nm, nl); m->name[nl] = 0;
    q++; m->np = 0;
    for (;;) {
        while (q < e && (*q == ' ' || *q == '\t')) q++;
        const char* a = q;
        while (q < e && lf_idch(*q)) q++;
        size_t al = (size_t)(q - a);
        if (!al || al >= 32 || m->np >= 8) return 0;
        memcpy(m->par[m->np], a, al); m->par[m->np++][al] = 0;
        while (q < e && (*q == ' ' || *q == '\t')) q++;
        if (*q == ',') { q++; continue; }
        if (*q == ')') { q++; break; }
        return 0;
    }
    gl_sb b = { 0 };
    for (; q < e; q++) {                              /* corp, fara "\<newline>" */
        if (*q == '\\' && (q[1] == '\n' || (q[1] == '\r' && q[2] == '\n'))) {
            sb_put(&b, " ", 1);
            q += (q[1] == '\r') ? 2 : 1;
            continue;
        }
        sb_put(&b, q, 1);
    }
        if (!b.p || (!strstr(b.p, "##") && strncmp(m->name, "gpu_attr_load_", 14))) { free(b.p); return 0; }
    m->body = b.p;
    return 1;
}

static void pm_expand(gl_sb* o, const pm_t* m, const char (*arg)[256]) {
    gl_sb t = { 0 };
    for (const char* p = m->body; *p; ) {
        if (lf_alpha(*p)) {
            const char* j = p;
            while (lf_idch(*j)) j++;
            int k;
            for (k = 0; k < m->np; k++)
                if (strlen(m->par[k]) == (size_t)(j - p) && !memcmp(m->par[k], p, (size_t)(j - p))) break;
            if (k < m->np) sb_put(&t, arg[k], strlen(arg[k]));
            else sb_put(&t, p, (size_t)(j - p));
            p = j;
            continue;
        }
        sb_put(&t, p, 1); p++;
    }
    if (!t.p) return;
    for (const char* p = t.p; *p; ) {                 /* scoate "  ##  " */
        if (p[0] == '#' && p[1] == '#') {
            while (o->n && (o->p[o->n-1] == ' ' || o->p[o->n-1] == '\t')) o->p[--o->n] = 0;
            p += 2;
            while (*p == ' ' || *p == '\t') p++;
            continue;
        }
        sb_put(o, p, 1); p++;
    }
    free(t.p);
}

static int pm_pass(const char* s, gl_sb* o, const pm_t* pm, int npm) {
    int changed = 0, bol = 1;
    for (const char* p = s; *p; ) {
        if (bol) {
            const char* q = p;
            while (*q == ' ' || *q == '\t') q++;
            if (*q == '#') {                          /* directiva: copiata neschimbata */
                const char* e = pm_lend(p);
                sb_put(o, p, (size_t)(e - p));
                p = e;
                if (*p) { sb_put(o, "\n", 1); p++; }
                continue;
            }
            bol = 0;
        }
        if (*p == '\n') { sb_put(o, p, 1); p++; bol = 1; continue; }
        if (lf_alpha(*p) && (p == s || !lf_idch(p[-1]))) {
            const char* j = p;
            while (lf_idch(*j)) j++;
            int k;
            for (k = 0; k < npm; k++)
                if (strlen(pm[k].name) == (size_t)(j - p) && !memcmp(pm[k].name, p, (size_t)(j - p))) break;
            if (k < npm) {
                const char* a = j;
                while (*a == ' ' || *a == '\t' || *a == '\n' || *a == '\r') a++;
                if (*a == '(') {
                    char arg[8][256];
                    int na = 0, depth = 0, ok = 0;
                    const char* st = a + 1;
                    const char* c = a;
                    for (; *c; c++) {
                        if (*c == '(') depth++;
                        else if (*c == ')' || (*c == ',' && depth == 1)) {
                            if (*c == ')' && --depth == 0) ok = 1;
                            if (depth == 0 || *c == ',') {
                                size_t l = (size_t)(c - st);
                                const char* x = st;
                                while (l && (*x == ' ' || *x == '\t' || *x == '\n')) { x++; l--; }
                                while (l && (x[l-1] == ' ' || x[l-1] == '\t' || x[l-1] == '\n')) l--;
                                if (na < 8 && l < 256) { memcpy(arg[na], x, l); arg[na][l] = 0; na++; }
                                else { ok = 0; break; }
                                st = c + 1;
                            }
                            if (ok) break;
                        }
                    }
                    if (ok && na == pm[k].np) {
                        pm_expand(o, &pm[k], (const char (*)[256])arg);
                        for (const char* n = p; n <= c; n++)      /* pastreaza numarul de linii */
                            if (*n == '\n') sb_put(o, "\n", 1);
                        p = c + 1;
                        changed = 1;
                        continue;
                    }
                }
            }
            sb_put(o, p, (size_t)(j - p));
            p = j;
            continue;
        }
        sb_put(o, p, 1); p++;
    }
    return changed;
}

/* NULL = nimic de facut; altfel sursa noua (caller face free) */
static char* gl_expand_paste(const char* src) {
    pm_t pm[PM_MAX]; int npm = 0;
    gl_sb out = { 0 };
    for (const char* p = src; *p; ) {
        const char* e = pm_lend(p);
        const char* q = p;
        while (*q == ' ' || *q == '\t') q++;
        int done = 0;
        if (npm < PM_MAX && !strncmp(q, "#define", 7) && (q[7] == ' ' || q[7] == '\t')
            && pm_parse(&pm[npm], q + 7, e)) {
            npm++;
            for (const char* c = p; c < e; c++) if (*c == '\n') sb_put(&out, "\n", 1);
            done = 1;                                 /* definitia devine linii goale */
        }
        if (!done) sb_put(&out, p, (size_t)(e - p));
        if (*e) { sb_put(&out, "\n", 1); p = e + 1; } else p = e;
    }
    if (!npm) { free(out.p); return NULL; }
    for (int it = 0; it < 4; it++) {       
        gl_sb nx = { 0 };
        int ch = pm_pass(out.p, &nx, pm, npm);
        free(out.p); out.p = nx.p ? nx.p : strdup("");
        if (!ch) break;
    }
    for (int i = 0; i < npm; i++) free(pm[i].body);
    if (getenv("GLX_LITFIX_DEBUG") && strstr(out.p, "##"))
        fprintf(stderr, "[paste] atentie: au ramas '##' in sursa\n");
    return out.p;
}

GLC void glCompileShader(unsigned sh) {
    RP(glCompileShader, F_ui)(sh);
    char* t = sh_tr_get_dup(sh);
    if (!t) {
        int ok = 0;
        RP(glGetShaderiv, F_gsiv)(sh, 0x8B81, &ok);
        if (!ok) {
            char buf[2048]; int n = 0;
            RP(glGetShaderInfoLog, F_gsil)(sh, sizeof buf, &n, buf);
            fprintf(stderr, "[glsl-err] shader %u: %s\n", sh, buf);
        }
        return;                                           /* not a shader translated by us */
    }
    int ok = 0;
    RP(glGetShaderiv, F_gsiv)(sh, 0x8B81, &ok);          /* COMPILE_STATUS */
    if (ok) { free(t); return; }

    char* cur = t;
    int fixed = 0;
    if (!getenv("GLX_NO_LITFIX")) {
        for (int it = 0; it < 32 && !fixed; it++) {
            char log[4096];
            log[0] = 0;
            RP(glGetShaderInfoLog, F_gsil)(sh, (int)sizeof log, NULL, log);
            char* nw = gl_promote_errors(cur, log);
            if (!nw) break;
            if (cur != t) free(cur);
            cur = nw;
            const char* src = cur;
            RP(glShaderSource, F_shsrc)(sh, 1, &src, NULL);
            RP(glCompileShader, F_ui)(sh);
RP(glGetShaderiv, F_gsiv)(sh, 0x8B81, &ok);
if (getenv("GLX_LITFIX_DEBUG")) {
    char l2[2048]; l2[0] = 0;
    RP(glGetShaderInfoLog, F_gsil)(sh, sizeof l2, NULL, l2);
    fprintf(stderr, "[litfix] iter %d ok=%d log: %.400s\n", it, ok, l2);
}
if (ok) fixed = 1;
        }
    }
    if (fixed) {
        fprintf(stderr, "[glsl] shader %u: promoted int->float literals/macros on error lines (GLSL ES does not convert implicitly)\n", sh);
        sh_tr_store(sh, cur);                             /* cur: taken by store */
        free(t);
    } else {
        if (cur != t) free(cur);
        const char* src = t;
        RP(glShaderSource, F_shsrc)(sh, 1, &src, NULL);   /* initial source: app log = original log */
        RP(glCompileShader, F_ui)(sh);
        char buf[2048]; int n = 0;
        RP(glGetShaderInfoLog, F_gsil)(sh, sizeof buf, &n, buf);
        fprintf(stderr, "[glsl-err] shader %u: %s\n", sh, buf);
        lf_dump_lines(t, buf);
        free(t);
    }
}
