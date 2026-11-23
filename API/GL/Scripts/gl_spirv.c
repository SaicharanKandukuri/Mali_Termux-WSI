/*
 * gl_spirv.c - included by gl_es_emul.c, AFTER the `static void cd_note(...)`
 * declaration and BEFORE `GLC void glShaderSource`.
 *
 * GL_ARB_gl_spirv emulated over Mali ES 3.2 (SPIRV-Cross C API):
 *   glShaderBinary(GL_SHADER_BINARY_FORMAT_SPIR_V)  -> stores the SPIR-V module
 *   glSpecializeShader / glSpecializeShaderARB      -> SPIR-V -> ESSL 3.20 + glCompileShader
 *   Other binary formats are forwarded to the native driver.
 * Debug: GLX_SHADER_DUMP=1 prints the generated ESSL on stderr.
 */

#include <spirv_cross/spirv_cross_c.h>

/* defined elsewhere in the shim; forward declaration so include order is irrelevant */
static void cd_note(unsigned sh, int n);

#define SPV_FMT_SPIRV 0x9551
#define SPV_MAGIC     0x07230203u

typedef void (*F_shbin)(int, const unsigned*, unsigned, const void*, int);
typedef void (*F_spv_gsiv)(unsigned, unsigned, int*);

/* ---------- Per-shader SPIR-V storage ---------- */
typedef struct { unsigned id; uint32_t* w; size_t n; } spv_ent;
static spv_ent*        spv_tab;
static int             spv_cnt, spv_cap;
static pthread_mutex_t spv_mu = PTHREAD_MUTEX_INITIALIZER;

static void spv_put(unsigned id, const void* bin, size_t bytes) {
    uint32_t* w = malloc(bytes);
    if (!w) return;
    memcpy(w, bin, bytes);
    pthread_mutex_lock(&spv_mu);
    int i;
    for (i = 0; i < spv_cnt; i++) if (spv_tab[i].id == id) break;
    if (i == spv_cnt) {
        if (spv_cnt == spv_cap) {
            int nc = spv_cap ? spv_cap * 2 : 64;
            spv_ent* t = realloc(spv_tab, (size_t)nc * sizeof *t);
            if (!t) { pthread_mutex_unlock(&spv_mu); free(w); return; }
            spv_tab = t; spv_cap = nc;
        }
        spv_cnt++;
    } else {
        free(spv_tab[i].w);
    }
    spv_tab[i].id = id; spv_tab[i].w = w; spv_tab[i].n = bytes / 4;
    pthread_mutex_unlock(&spv_mu);
}

/* Returns a malloc'd copy (or NULL). */
static uint32_t* spv_get(unsigned id, size_t* n) {
    uint32_t* r = NULL;
    pthread_mutex_lock(&spv_mu);
    for (int i = 0; i < spv_cnt; i++)
        if (spv_tab[i].id == id) {
            r = malloc(spv_tab[i].n * 4);
            if (r) { memcpy(r, spv_tab[i].w, spv_tab[i].n * 4); *n = spv_tab[i].n; }
            break;
        }
    pthread_mutex_unlock(&spv_mu);
    return r;
}

/* Optional: call from your glDeleteShader to free the stored module. */
__attribute__((unused))
static void spv_forget(unsigned id) {
    pthread_mutex_lock(&spv_mu);
    for (int i = 0; i < spv_cnt; i++)
        if (spv_tab[i].id == id) {
            free(spv_tab[i].w);
            spv_tab[i] = spv_tab[--spv_cnt];
            break;
        }
    pthread_mutex_unlock(&spv_mu);
}

/* ---------- SPIRV-Cross helpers ---------- */
typedef struct { char* buf; size_t size; } spv_err;

static void spv_err_cb(void* ud, const char* msg) {
    spv_err* e = (spv_err*)ud;
    if (e && e->buf && e->size) snprintf(e->buf, e->size, "%s", msg ? msg : "unknown error");
}

static void spv_strip_type(spvc_compiler c, spvc_resources r, spvc_resource_type t) {
    const spvc_reflected_resource* l = NULL; size_t n = 0;
    if (spvc_resources_get_resource_list_for_type(r, t, &l, &n) != SPVC_SUCCESS) return;
    for (size_t i = 0; i < n; i++)
        spvc_compiler_unset_decoration(c, l[i].id, SpvDecorationDescriptorSet);
}

static void spv_strip_sets(spvc_compiler c) {
    spvc_resources r = NULL;
    if (spvc_compiler_create_shader_resources(c, &r) != SPVC_SUCCESS) return;
    spv_strip_type(c, r, SPVC_RESOURCE_TYPE_UNIFORM_BUFFER);
    spv_strip_type(c, r, SPVC_RESOURCE_TYPE_STORAGE_BUFFER);
    spv_strip_type(c, r, SPVC_RESOURCE_TYPE_SAMPLED_IMAGE);
    spv_strip_type(c, r, SPVC_RESOURCE_TYPE_SEPARATE_IMAGE);
    spv_strip_type(c, r, SPVC_RESOURCE_TYPE_SEPARATE_SAMPLERS);
    spv_strip_type(c, r, SPVC_RESOURCE_TYPE_STORAGE_IMAGE);
}

/* GL shader type enum -> SPIR-V execution model (-1 = unknown) */
static int spv_model_for(int gltype) {
    switch (gltype) {
    case 0x8B31: return SpvExecutionModelVertex;
    case 0x8B30: return SpvExecutionModelFragment;
    case 0x91B9: return SpvExecutionModelGLCompute;
    case 0x8DD9: return SpvExecutionModelGeometry;
    case 0x8E88: return SpvExecutionModelTessellationControl;
    case 0x8E87: return SpvExecutionModelTessellationEvaluation;
    default:     return -1;
    }
}

/* SPIR-V -> ESSL 3.20. Returns malloc'd string or NULL (message in err). */
static char* spv_to_es(const uint32_t* w, size_t n, int model, const char* entry,
                       unsigned nsc, const unsigned* idx, const unsigned* val,
                       char* err, size_t es) {
    spvc_context          ctx = NULL;
    spvc_parsed_ir        ir  = NULL;
    spvc_compiler         comp = NULL;
    spvc_compiler_options opt = NULL;
    const char*           src = NULL;
    char*                 out = NULL;
    spv_err               ec  = { err, es };

    if (err && es) err[0] = 0;
    if (spvc_context_create(&ctx) != SPVC_SUCCESS) return NULL;
    spvc_context_set_error_callback(ctx, spv_err_cb, &ec);

    if (spvc_context_parse_spirv(ctx, (const SpvId*)w, n, &ir) != SPVC_SUCCESS) goto done;
    if (spvc_context_create_compiler(ctx, SPVC_BACKEND_GLSL, ir,
                                     SPVC_CAPTURE_MODE_TAKE_OWNERSHIP, &comp) != SPVC_SUCCESS)
        goto done;

    if (model < 0) model = (int)spvc_compiler_get_execution_model(comp);
    if (spvc_compiler_set_entry_point(comp, entry ? entry : "main", (SpvExecutionModel)model)
        != SPVC_SUCCESS)
        goto done;

    /* Specialization constants: raw 32-bit values (float/int/bool) */
    if (nsc && idx && val) {
        const spvc_specialization_constant* sc = NULL; size_t scn = 0;
        spvc_compiler_get_specialization_constants(comp, &sc, &scn);
        for (unsigned k = 0; k < nsc; k++)
            for (size_t j = 0; j < scn; j++)
                if (sc[j].constant_id == idx[k]) {
                    spvc_constant c = spvc_compiler_get_constant_handle(comp, sc[j].id);
                    if (c) spvc_constant_set_scalar_u32(c, 0, 0, val[k]);
                }
    }

    if (spvc_compiler_create_compiler_options(comp, &opt) != SPVC_SUCCESS) goto done;
    spvc_compiler_options_set_uint(opt, SPVC_COMPILER_OPTION_GLSL_VERSION, 320);
    spvc_compiler_options_set_bool(opt, SPVC_COMPILER_OPTION_GLSL_ES, SPVC_TRUE);
    spvc_compiler_options_set_bool(opt, SPVC_COMPILER_OPTION_GLSL_ES_DEFAULT_FLOAT_PRECISION_HIGHP, SPVC_TRUE);
    spvc_compiler_options_set_bool(opt, SPVC_COMPILER_OPTION_GLSL_ES_DEFAULT_INT_PRECISION_HIGHP, SPVC_TRUE);
    /* GL clip space already [-1,1]; clip-control is handled by the shim (xl_clip) */
    spvc_compiler_options_set_bool(opt, SPVC_COMPILER_OPTION_FLIP_VERTEX_Y, SPVC_FALSE);
    spvc_compiler_options_set_bool(opt, SPVC_COMPILER_OPTION_FIXUP_DEPTH_CONVENTION, SPVC_FALSE);
    spvc_compiler_options_set_bool(opt, SPVC_COMPILER_OPTION_GLSL_ENABLE_420PACK_EXTENSION, SPVC_FALSE);
    if (spvc_compiler_install_compiler_options(comp, opt) != SPVC_SUCCESS) goto done;

    if (spvc_compiler_build_combined_image_samplers(comp) != SPVC_SUCCESS) goto done;
    spv_strip_sets(comp);

    if (spvc_compiler_compile(comp, &src) != SPVC_SUCCESS) goto done;

    size_t l = strlen(src);
    out = malloc(l + 1);
    if (out) memcpy(out, src, l + 1);

done:
    spvc_context_destroy(ctx);
    return out;
}

/* ---------- GL entry points ---------- */
GLC void glShaderBinary(int count, const unsigned* shaders, unsigned fmt, const void* bin, int len) {
    if (fmt != SPV_FMT_SPIRV) {
        RP(glShaderBinary, F_shbin)(count, shaders, fmt, bin, len);
        return;
    }
    if (!bin || len < 20 || (len & 3) || *(const uint32_t*)bin != SPV_MAGIC) {
        if (gl_emul_debug_enabled())
            fprintf(stderr, "[spirv] glShaderBinary: invalid SPIR-V module (len=%d)\n", len);
        return;
    }
    for (int i = 0; i < count; i++) spv_put(shaders[i], bin, (size_t)len);
    if (gl_emul_debug_enabled())
        fprintf(stderr, "[spirv] stored %d byte module for %d shader(s)\n", len, count);
}

GLC void glSpecializeShader(unsigned sh, const char* entry, unsigned nsc,
                            const unsigned* idx, const unsigned* val) {
    size_t n = 0;
    uint32_t* w = spv_get(sh, &n);
    char err[1024];
    char* src = NULL;

    if (w) {
        int type = 0;
        RP(glGetShaderiv, F_spv_gsiv)(sh, 0x8B4F, &type);   /* GL_SHADER_TYPE */
        src = spv_to_es(w, n, spv_model_for(type), entry, nsc, idx, val, err, sizeof err);
        free(w);
        if (!src)
            fprintf(stderr, "[spirv] shader %u: SPIRV-Cross failed: %s\n", sh, err[0] ? err : "unknown");
    } else {
        fprintf(stderr, "[spirv] glSpecializeShader(%u): no SPIR-V module loaded\n", sh);
    }

    /* On failure the driver gets an #error source, so COMPILE_STATUS = false */
    const char* s = src ? src : "#version 320 es\n#error spirv_conversion_failed\n";

    if (getenv("GLX_SHADER_DUMP")) fprintf(stderr, "[glsl] spirv-cross\n%s\n", s);

    cd_note(sh, 0);
    dsb_note(sh, 0);
    sh_store(sh, NULL);
    sh_tr_store(sh, NULL);

    RP(glShaderSource, F_shsrc)(sh, 1, &s, NULL);
    RP(glCompileShader, F_ui)(sh);
    free(src);
}

GLC void glSpecializeShaderARB(unsigned sh, const char* entry, unsigned nsc,
                               const unsigned* idx, const unsigned* val) {
    glSpecializeShader(sh, entry, nsc, idx, val);
}
