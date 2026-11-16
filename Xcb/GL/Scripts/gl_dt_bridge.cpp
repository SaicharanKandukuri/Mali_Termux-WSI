#define DXT1_GLES
#include "gl_desktop_texture.h"   

static DesktopTexCodec* g_codec;
typedef void (*geti_t)(GLenum, GLint*);
typedef void (*getii_t)(GLenum, GLuint, GLint*);
typedef void (*getiii_t)(GLenum, GLuint, GLint64*);
typedef void (*bbr_t)(GLenum, GLuint, GLuint, GLintptr, GLsizeiptr);
static geti_t p_geti; static getii_t p_getii; static getiii_t p_getiii; static bbr_t p_bbr;

extern "C" __attribute__((visibility("hidden")))
int dtb_init(void* (*gp)(const char*)) {
    g_codec = new DesktopTexCodec();          
    p_geti = (geti_t)gp("glGetIntegerv");
    p_getii = (getii_t)gp("glGetIntegeri_v");
    p_getiii = (getiii_t)gp("glGetInteger64i_v");
    p_bbr = (bbr_t)gp("glBindBufferRange");
    if (!p_geti || !p_getii || !p_getiii || !p_bbr || !g_codec->init(gp)) { delete g_codec; g_codec = nullptr; return 0; }
    return 1;
}

// f = DtFormat. Returns an RGBA8/RGBA16F texture (dimensions rounded up to a multiple of 4) or 0.
extern "C" __attribute__((visibility("hidden")))
unsigned dtb_decode(int f, const void* blocks, int w, int h) {
    if (!g_codec) return 0;
    DtFormat fmt = (DtFormat)f;
    if (!g_codec->ensure(fmt)) return 0;
    DtGL& gl = g_codec->gl;

    GLint prog = 0, t2d = 0, ssbo = 0, ssbo0 = 0; GLint64 off = 0, sz = 0;
    p_geti(GL_CURRENT_PROGRAM, &prog);
    p_geti(GL_TEXTURE_BINDING_2D, &t2d);
    p_geti(GL_SHADER_STORAGE_BUFFER_BINDING, &ssbo);
    p_getii(GL_SHADER_STORAGE_BUFFER_BINDING, 0, &ssbo0);
    p_getiii(GL_SHADER_STORAGE_BUFFER_START, 0, &off);
    p_getiii(GL_SHADER_STORAGE_BUFFER_SIZE, 0, &sz);

    GLuint buf = 0;
    gl.GenBuffers(1, &buf);
    gl.BindBuffer(GL_SHADER_STORAGE_BUFFER, buf);
    gl.BufferData(GL_SHADER_STORAGE_BUFFER, (GLsizeiptr)DesktopTexCodec::compressedSize(fmt, w, h), blocks, GL_STREAM_DRAW);
    GLuint dst = g_codec->createTexture2D((w + 3) & ~3, (h + 3) & ~3, dtImageFormat(fmt));
    g_codec->decode(fmt, buf, w, h, dst);

    gl.DeleteBuffers(1, &buf);
    gl.BindTexture(GL_TEXTURE_2D, (GLuint)t2d);
    gl.BindBuffer(GL_SHADER_STORAGE_BUFFER, (GLuint)ssbo);
    if (ssbo0 && sz > 0) p_bbr(GL_SHADER_STORAGE_BUFFER, 0, ssbo0, (GLintptr)off, (GLsizeiptr)sz);
    else gl.BindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, (GLuint)ssbo0);
    gl.UseProgram((GLuint)prog);
    return dst;
}

extern "C" __attribute__((visibility("hidden")))
void dtb_release(unsigned t) { if (g_codec && t) g_codec->gl.DeleteTextures(1, &t); }
