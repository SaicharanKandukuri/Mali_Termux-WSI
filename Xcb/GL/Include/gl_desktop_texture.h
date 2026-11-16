// gl_desktop_texture.h
// GPU-based encoder/decoder (compute shaders) for "desktop" texture formats:
//   BC1 (DXT1), BC2 (DXT3), BC3 (DXT5), BC4 (RGTC1 / 1 channel), BC5 (RGTC2 / 2 channels),
//   BC7 (BPTC; full decoder supporting all 8 modes, encoder mode 6 + mode 1),
//   BC6H (BPTC float, HDR): DT_BC6H_UF16 and DT_BC6H_SF16; full decoder supporting all 14 modes,
//        encoder for single-region modes (3, 7, 11, 15). Inputs/outputs are RGBA16F textures.
// Works on GLES 3.1+ and desktop OpenGL 4.3+.
// Single header: includes BC7 partition tables (formerly bc7_tables.h), BC6H tables/shared,
// and respective shaders.
//
// Configuration (before including):
//   #define DXT1_GLES                -> uses <GLES3/gl31.h> and "#version 310 es"
//   #define DXT1_GL_HEADER "x.h"     -> custom GL header (e.g. glad), "#version 430 core"
//   #define BC7_TOPN 4               -> how many candidate partitions to fully evaluate for
//                                       mode 1 (1..8); smaller = faster
//
// GL functions are NOT called directly (no linking against libGLES): they are loaded
// through a loader (dlsym / eglGetProcAddress). This works seamlessly with the Mali driver
// loaded via mali_sphal.h.
//
// Usage:
//   DesktopTexCodec codec;
//   codec.init(getProc);                         // getProc(const char*) -> void*
//   GLuint buf = codec.createCompressedBuffer(DT_BC7, w, h);
//   codec.encode(DT_BC7, srcRGBA8Tex, w, h, buf);
//   codec.decode(DT_BC7, buf, w, h, dstRGBA8Tex);
//   codec.uploadToCompressedTexture(DT_BC7, buf, hwTex, w, h);  // requires S3TC/RGTC/BPTC
//
// RGBA8 textures for encode/decode must be immutable (glTexStorage2D);
// createTexture2D() handles this. For BC6H, textures are RGBA16F: createTexture2D(w, h, GL_RGBA16F),
// use uploadRGBA16F() / readRGBA16F() for float data.
//
// BC7 Encoder (mode 6 / mode 1):
//   - mode 6 (RGBA, 1 subset, 7 bits + p-bit / endpoint, 4-bit indices)
//   - mode 1 (RGB, 2 subsets, 6 bits + shared p-bit, 3-bit indices), used when beneficial
//   - endpoints: PCA + 2 least-squares iterations; mode 1 partition: 1D k-means + closest
//     BC7_TOPN partitions (Hamming distance)

#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#if defined(DXT1_GL_HEADER)
  #include DXT1_GL_HEADER
#elif defined(DXT1_GLES)
  #include <GLES3/gl31.h>
  #include <GLES2/gl2ext.h>
#endif

#if defined(DXT1_GLES)
  #define DXT1_GLSL_VERSION "#version 310 es\n"
#else
  #define DXT1_GLSL_VERSION "#version 430 core\n"
#endif

#if defined(GL_APIENTRY)
  #define DT_APIENTRY GL_APIENTRY
#elif defined(APIENTRY)
  #define DT_APIENTRY APIENTRY
#else
  #define DT_APIENTRY
#endif

#ifndef BC7_TOPN
#define BC7_TOPN 4   // number of candidate partitions fully evaluated for mode 1 (1..8)
#endif

// Custom constants (prevents conflicts between EXT/ARB/core variants)
#define DT_GL_COMPRESSED_RGB_S3TC_DXT1   0x83F0
#define DT_GL_COMPRESSED_RGBA_S3TC_DXT3  0x83F2
#define DT_GL_COMPRESSED_RGBA_S3TC_DXT5  0x83F3
#define DT_GL_COMPRESSED_RED_RGTC1       0x8DBB
#define DT_GL_COMPRESSED_RG_RGTC2        0x8DBD
#define DT_GL_COMPRESSED_RGBA_BPTC_UNORM 0x8E8C
#define DT_GL_COMPRESSED_RGB_BPTC_SIGNED_FLOAT   0x8E8E
#define DT_GL_COMPRESSED_RGB_BPTC_UNSIGNED_FLOAT 0x8E8F

// ----------------------------------------------------------------------------
// GL function table loaded via loader
// ----------------------------------------------------------------------------
#define DT_GL_FUNCS(X) \
    X(GLuint,   CreateShader,        (GLenum)) \
    X(void,     ShaderSource,        (GLuint, GLsizei, const GLchar* const*, const GLint*)) \
    X(void,     CompileShader,       (GLuint)) \
    X(void,     GetShaderiv,         (GLuint, GLenum, GLint*)) \
    X(void,      GetShaderInfoLog,   (GLuint, GLsizei, GLsizei*, GLchar*)) \
    X(void,     DeleteShader,        (GLuint)) \
    X(GLuint,   CreateProgram,       (void)) \
    X(void,     AttachShader,        (GLuint, GLuint)) \
    X(void,     LinkProgram,         (GLuint)) \
    X(void,     GetProgramiv,        (GLuint, GLenum, GLint*)) \
    X(void,     GetProgramInfoLog,   (GLuint, GLsizei, GLsizei*, GLchar*)) \
    X(void,     DeleteProgram,       (GLuint)) \
    X(void,     UseProgram,          (GLuint)) \
    X(void,     Uniform2i,           (GLint, GLint, GLint)) \
    X(void,     BindImageTexture,    (GLuint, GLuint, GLint, GLboolean, GLint, GLenum, GLenum)) \
    X(void,     BindBufferBase,      (GLenum, GLuint, GLuint)) \
    X(void,     DispatchCompute,     (GLuint, GLuint, GLuint)) \
    X(void,     MemoryBarrier,       (GLbitfield)) \
    X(void,     GenBuffers,          (GLsizei, GLuint*)) \
    X(void,     DeleteBuffers,       (GLsizei, const GLuint*)) \
    X(void,     BindBuffer,          (GLenum, GLuint)) \
    X(void,     BufferData,          (GLenum, GLsizeiptr, const void*, GLenum)) \
    X(void*,    MapBufferRange,      (GLenum, GLintptr, GLsizeiptr, GLbitfield)) \
    X(GLboolean,UnmapBuffer,         (GLenum)) \
    X(void,     GenTextures,         (GLsizei, GLuint*)) \
    X(void,     DeleteTextures,      (GLsizei, const GLuint*)) \
    X(void,     BindTexture,         (GLenum, GLuint)) \
    X(void,     ActiveTexture,       (GLenum)) \
    X(void,     TexStorage2D,        (GLenum, GLsizei, GLenum, GLsizei, GLsizei)) \
    X(void,     TexSubImage2D,       (GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void*)) \
    X(void,     CompressedTexSubImage2D, (GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLsizei, const void*)) \
    X(void,     TexParameteri,       (GLenum, GLenum, GLint)) \
    X(void,     GenFramebuffers,     (GLsizei, GLuint*)) \
    X(void,     DeleteFramebuffers,  (GLsizei, const GLuint*)) \
    X(void,     BindFramebuffer,     (GLenum, GLuint)) \
    X(void,     FramebufferTexture2D,(GLenum, GLenum, GLenum, GLuint, GLint)) \
    X(GLenum,   CheckFramebufferStatus, (GLenum)) \
    X(void,     ReadPixels,          (GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void*)) \
    X(GLenum,   GetError,            (void)) \
    X(void,     Finish,              (void)) \
    X(const GLubyte*, GetString,     (GLenum))

struct DtGL {
#define DT_DECL(ret, name, args) ret (DT_APIENTRY *name) args = nullptr;
    DT_GL_FUNCS(DT_DECL)
#undef DT_DECL

    bool load(void* (*getProc)(const char*)) {
        bool ok = true;
#define DT_LOAD(ret, name, args)                                              \
        name = reinterpret_cast<ret (DT_APIENTRY *) args>(getProc("gl" #name)); \
        if (!name) { std::fprintf(stderr, "[dt] missing gl%s\n", #name); ok = false; }
        DT_GL_FUNCS(DT_LOAD)
#undef DT_LOAD
        return ok;
    }
};

// ----------------------------------------------------------------------------
// Formats
// ----------------------------------------------------------------------------
enum DtFormat { DT_BC1 = 0, DT_BC2, DT_BC3, DT_BC4, DT_BC5, DT_BC7, DT_BC6H_UF16, DT_BC6H_SF16, DT_FORMAT_COUNT };

inline const char* dtFormatName(DtFormat f) {
    static const char* n[] = { "BC1/DXT1", "BC2/DXT3", "BC3/DXT5", "BC4/RGTC1", "BC5/RGTC2", "BC7/BPTC", "BC6H/UF16", "BC6H/SF16" };
    return n[f];
}
inline int dtBlockBytes(DtFormat f) { return (f == DT_BC1 || f == DT_BC4) ? 8 : 16; }
inline GLenum dtGlInternalFormat(DtFormat f) {
    switch (f) {
        case DT_BC1: return DT_GL_COMPRESSED_RGB_S3TC_DXT1;
        case DT_BC2: return DT_GL_COMPRESSED_RGBA_S3TC_DXT3;
        case DT_BC3: return DT_GL_COMPRESSED_RGBA_S3TC_DXT5;
        case DT_BC4: return DT_GL_COMPRESSED_RED_RGTC1;
        case DT_BC5: return DT_GL_COMPRESSED_RG_RGTC2;
        case DT_BC6H_UF16: return DT_GL_COMPRESSED_RGB_BPTC_UNSIGNED_FLOAT;
        case DT_BC6H_SF16: return DT_GL_COMPRESSED_RGB_BPTC_SIGNED_FLOAT;
        default:     return DT_GL_COMPRESSED_RGBA_BPTC_UNORM;
    }
}
// BC6H works with float images (RGBA16F), other formats use RGBA8
inline bool dtIsFloatFormat(DtFormat f) { return f == DT_BC6H_UF16 || f == DT_BC6H_SF16; }
inline GLenum dtImageFormat(DtFormat f) { return dtIsFloatFormat(f) ? GL_RGBA16F : GL_RGBA8; }

namespace dt_detail {

// ----------------------------------------------------------------------------
// BC7 Partition Tables
// ----------------------------------------------------------------------------

static const uint32_t kBc7Part2[64] = {
    0xCCCCu, 0x8888u, 0xEEEEu, 0xECC8u, 0xC880u, 0xFEECu, 0xFEC8u, 0xEC80u,
    0xC800u, 0xFFECu, 0xFE80u, 0xE800u, 0xFFE8u, 0xFF00u, 0xFFF0u, 0xF000u,
    0xF710u, 0x008Eu, 0x7100u, 0x08CEu, 0x008Cu, 0x7310u, 0x3100u, 0x8CCEu,
    0x088Cu, 0x3110u, 0x6666u, 0x366Cu, 0x17E8u, 0x0FF0u, 0x718Eu, 0x399Cu,
    0xAAAAu, 0xF0F0u, 0x5A5Au, 0x33CCu, 0x3C3Cu, 0x55AAu, 0x9696u, 0xA55Au,
    0x73CEu, 0x13C8u, 0x324Cu, 0x3BDCu, 0x6996u, 0xC33Cu, 0x9966u, 0x0660u,
    0x0272u, 0x04E4u, 0x4E40u, 0x2720u, 0xC936u, 0x936Cu, 0x39C6u, 0x639Cu,
    0x9336u, 0x9CC6u, 0x817Eu, 0xE718u, 0xCCF0u, 0x0FCCu, 0x7744u, 0xEE22u,
};

static const uint32_t kBc7Part3[64] = {
    0xAA685050u, 0x6A5A5040u, 0x5A5A4200u, 0x5450A0A8u, 0xA5A50000u, 0xA0A05050u, 0x5555A0A0u, 0x5A5A5050u,
    0xAA550000u, 0xAA555500u, 0xAAAA5500u, 0x90909090u, 0x94949494u, 0xA4A4A4A4u, 0xA9A59450u, 0x2A0A4250u,
    0xA5945040u, 0x0A425054u, 0xA5A5A500u, 0x55A0A0A0u, 0xA8A85454u, 0x6A6A4040u, 0xA4A45000u, 0x1A1A0500u,
    0x0050A4A4u, 0xAAA59090u, 0x14696914u, 0x69691400u, 0xA08585A0u, 0xAA821414u, 0x50A4A450u, 0x6A5A0200u,
    0xA9A58000u, 0x5090A0A8u, 0xA8A09050u, 0x24242424u, 0x00AA5500u, 0x24924924u, 0x24499224u, 0x50A50A50u,
    0x500AA550u, 0xAAAA4444u, 0x66660000u, 0xA5A0A5A0u, 0x50A050A0u, 0x69286928u, 0x44AAAA44u, 0x66666600u,
    0xAA444444u, 0x54A854A8u, 0x95809580u, 0x96969600u, 0xA85454A8u, 0x80959580u, 0xAA141414u, 0x96960000u,
    0xAAAA1414u, 0xA05050A0u, 0xA0A5A5A0u, 0x96000000u, 0x40804080u, 0xA9A8A9A8u, 0xAAAAAA44u, 0x2A4A5254u,
};

static const uint8_t kBc7Anchor2[64] = {
    15, 15, 15, 15, 15, 15, 15, 15,
    15, 15, 15, 15, 15, 15, 15, 15,
    15, 2, 8, 2, 2, 8, 8, 15,
    2, 8, 2, 2, 8, 8, 2, 2,
    15, 15, 6, 8, 2, 8, 15, 15,
    2, 8, 2, 2, 2, 15, 15, 6,
    6, 2, 6, 8, 15, 15, 2, 2,
    15, 15, 15, 15, 15, 2, 2, 15,
};

static const uint8_t kBc7Anchor3a[64] = {
    3, 3, 15, 15, 8, 3, 15, 15,
    8, 8, 6, 6, 6, 5, 3, 3,
    3, 3, 8, 15, 3, 3, 6, 10,
    5, 8, 8, 6, 8, 5, 15, 15,
    8, 15, 3, 5, 6, 10, 8, 15,
    15, 3, 15, 5, 15, 15, 15, 15,
    3, 15, 5, 5, 5, 8, 5, 10,
    5, 10, 8, 13, 15, 12, 3, 3,
};

static const uint8_t kBc7Anchor3b[64] = {
    15, 8, 8, 3, 15, 15, 3, 8,
    15, 15, 15, 15, 15, 15, 15, 8,
    15, 8, 15, 3, 15, 8, 15, 8,
    3, 15, 6, 10, 15, 15, 10, 8,
    15, 3, 15, 10, 10, 8, 9, 10,
    6, 15, 8, 15, 3, 6, 6, 8,
    15, 3, 15, 15, 15, 15, 15, 15,
    15, 15, 15, 15, 3, 15, 15, 8,
};

inline std::string bc7GlslTables() {
    std::string s;
    char buf[32];
    auto emitU = [&](const char* name, const uint32_t* t) {
        s += "const uint "; s += name; s += "[64] = uint[64](";
        for (int i = 0; i < 64; i++) { std::snprintf(buf, sizeof(buf), "%uu%s", t[i], i < 63 ? "," : ""); s += buf; }
        s += ");\n";
    };
    auto emitI = [&](const char* name, const uint8_t* t) {
        s += "const int "; s += name; s += "[64] = int[64](";
        for (int i = 0; i < 64; i++) { std::snprintf(buf, sizeof(buf), "%d%s", int(t[i]), i < 63 ? "," : ""); s += buf; }
        s += ");\n";
    };
    emitU("kPart2", kBc7Part2);
    emitU("kPart3", kBc7Part3);
    emitI("kAnchor2", kBc7Anchor2);
    emitI("kAnchor3a", kBc7Anchor3a);
    emitI("kAnchor3b", kBc7Anchor3b);
    return s;
}

// ----------------------------------------------------------------------------
// BC1-BC5 Shaders
// ----------------------------------------------------------------------------

static const char* kCommon = R"GLSL(
precision highp float;
precision highp int;
layout(local_size_x = 8, local_size_y = 8) in;
layout(location = 0) uniform ivec2 u_imageSizeInBlocks;

uint pack565(vec3 c) {
    uint r = uint(clamp(c.r, 0.0, 1.0) * 31.0 + 0.5);
    uint g = uint(clamp(c.g, 0.0, 1.0) * 63.0 + 0.5);
    uint b = uint(clamp(c.b, 0.0, 1.0) * 31.0 + 0.5);
    return (r << 11) | (g << 5) | b;
}
vec3 unpack565(uint c) {
    return vec3(float((c >> 11) & 31u) / 31.0,
                float((c >> 5)  & 63u) / 63.0,
                float( c        & 31u) / 31.0);
}
)GLSL";

static const char* kEncoder = R"GLSL(
layout(rgba8, binding = 0) readonly uniform highp image2D u_inputImage;
layout(std430, binding = 0) writeonly buffer CompressedBuffer { uint blocks[]; };

vec3  g_rgb[16];
float g_a[16];
float g_ch[16];

uvec2 encodeColor() {
    vec3 mean = vec3(0.0);
    vec3 mn = vec3(1.0);
    vec3 mx = vec3(0.0);
    for (int i = 0; i < 16; i++) { mean += g_rgb[i]; mn = min(mn, g_rgb[i]); mx = max(mx, g_rgb[i]); }
    mean /= 16.0;

    mat3 cov = mat3(0.0);
    for (int i = 0; i < 16; i++) { vec3 d = g_rgb[i] - mean; cov += outerProduct(d, d); }
    vec3 axis = mx - mn;
    for (int it = 0; it < 4; it++) {
        vec3 r = cov * axis;
        float len = length(r);
        if (len < 1e-8) break;
        axis = r / len;
    }

    vec3 e0 = mx;
    vec3 e1 = mn;
    float alen = length(axis);
    if (alen > 1e-6) {
        axis /= alen;
        float tmin = 1e9;
        float tmax = -1e9;
        for (int i = 0; i < 16; i++) {
            float t = dot(g_rgb[i] - mean, axis);
            tmin = min(tmin, t);
            tmax = max(tmax, t);
        }
        e0 = clamp(mean + axis * tmax, 0.0, 1.0);
        e1 = clamp(mean + axis * tmin, 0.0, 1.0);
    }

    const float kWt[4] = float[4](0.0, 1.0, 1.0 / 3.0, 2.0 / 3.0);

    uint c0 = 0u;
    uint c1 = 0u;
    uint bits = 0u;
    for (int pass = 0; pass < 3; pass++) {
        c0 = pack565(e0);
        c1 = pack565(e1);
        if (c0 < c1) { uint t = c0; c0 = c1; c1 = t; }

        vec3 p[4];
        p[0] = unpack565(c0);
        p[1] = unpack565(c1);
        p[2] = (2.0 * p[0] + p[1]) / 3.0;
        p[3] = (p[0] + 2.0 * p[1]) / 3.0;

        bits = 0u;
        float A = 0.0;
        float B = 0.0;
        float G = 0.0;
        vec3 s0 = vec3(0.0);
        vec3 s1 = vec3(0.0);
        for (int i = 0; i < 16; i++) {
            float best = 1e9;
            uint bi = 0u;
            for (uint j = 0u; j < 4u; j++) {
                vec3 d = g_rgb[i] - p[j];
                float dd = dot(d, d);
                if (dd < best) { best = dd; bi = j; }
            }
            bits |= bi << uint(i * 2);
            float w = kWt[bi];
            float a = 1.0 - w;
            A += a * a; B += a * w; G += w * w;
            s0 += a * g_rgb[i];
            s1 += w * g_rgb[i];
        }
        if (pass == 2) break;
        float det = A * G - B * B;
        if (abs(det) < 1e-6) break;
        e0 = clamp((G * s0 - B * s1) / det, 0.0, 1.0);
        e1 = clamp((A * s1 - B * s0) / det, 0.0, 1.0);
    }
    return uvec2(c0 | (c1 << 16), bits);
}

uvec2 encodeBC4() {
    float mn = 1.0;
    float mx = 0.0;
    for (int i = 0; i < 16; i++) { mn = min(mn, g_ch[i]); mx = max(mx, g_ch[i]); }
    uint a0 = uint(clamp(mx, 0.0, 1.0) * 255.0 + 0.5);
    uint a1 = uint(clamp(mn, 0.0, 1.0) * 255.0 + 0.5);
    float f0 = float(a0);
    float f1 = float(a1);

    float pal[8];
    pal[0] = f0;
    pal[1] = f1;
    for (int k = 2; k < 8; k++) pal[k] = (float(8 - k) * f0 + float(k - 1) * f1) / 7.0;

    uint lo = 0u;
    uint hi = 0u;
    for (int i = 0; i < 16; i++) {
        float v = g_ch[i] * 255.0;
        float best = 1e9;
        uint bi = 0u;
        for (int k = 0; k < 8; k++) {
            float d = abs(v - pal[k]);
            if (d < best) { best = d; bi = uint(k); }
        }
        int pos = 3 * i;
        if (pos < 32) {
            lo |= bi << uint(pos);
            if (pos > 29) hi |= bi >> uint(32 - pos);
        } else {
            hi |= bi << uint(pos - 32);
        }
    }
    return uvec2(a0 | (a1 << 8) | (lo << 16), (lo >> 16) | (hi << 16));
}

void main() {
    ivec2 bc = ivec2(gl_GlobalInvocationID.xy);
    if (bc.x >= u_imageSizeInBlocks.x || bc.y >= u_imageSizeInBlocks.y) return;

    ivec2 imgMax = imageSize(u_inputImage) - ivec2(1);
    ivec2 bp = bc * 4;
    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
            vec4 c = imageLoad(u_inputImage, min(bp + ivec2(x, y), imgMax));
            g_rgb[y * 4 + x] = c.rgb;
            g_a[y * 4 + x] = c.a;
        }
    }

    int base = (bc.y * u_imageSizeInBlocks.x + bc.x) * BLOCK_WORDS;

#if defined(FMT_BC1)
    uvec2 col = encodeColor();
    blocks[base + 0] = col.x;
    blocks[base + 1] = col.y;
#elif defined(FMT_BC2)
    uint aw0 = 0u;
    uint aw1 = 0u;
    for (int i = 0; i < 8; i++) {
        aw0 |= uint(clamp(g_a[i],     0.0, 1.0) * 15.0 + 0.5) << uint(4 * i);
        aw1 |= uint(clamp(g_a[i + 8], 0.0, 1.0) * 15.0 + 0.5) << uint(4 * i);
    }
    uvec2 col = encodeColor();
    blocks[base + 0] = aw0;
    blocks[base + 1] = aw1;
    blocks[base + 2] = col.x;
    blocks[base + 3] = col.y;
#elif defined(FMT_BC3)
    for (int i = 0; i < 16; i++) g_ch[i] = g_a[i];
    uvec2 al = encodeBC4();
    uvec2 col = encodeColor();
    blocks[base + 0] = al.x;
    blocks[base + 1] = al.y;
    blocks[base + 2] = col.x;
    blocks[base + 3] = col.y;
#elif defined(FMT_BC4)
    for (int i = 0; i < 16; i++) g_ch[i] = g_rgb[i].r;
    uvec2 r = encodeBC4();
    blocks[base + 0] = r.x;
    blocks[base + 1] = r.y;
#elif defined(FMT_BC5)
    for (int i = 0; i < 16; i++) g_ch[i] = g_rgb[i].r;
    uvec2 r = encodeBC4();
    for (int i = 0; i < 16; i++) g_ch[i] = g_rgb[i].g;
    uvec2 g = encodeBC4();
    blocks[base + 0] = r.x;
    blocks[base + 1] = r.y;
    blocks[base + 2] = g.x;
    blocks[base + 3] = g.y;
#endif
}
)GLSL";

static const char* kDecoder = R"GLSL(
layout(std430, binding = 0) readonly buffer CompressedBuffer { uint blocks[]; };
layout(rgba8, binding = 0) writeonly uniform highp image2D u_outputImage;

vec3 g_pal[4];

void buildPalette(uint c0, uint c1, bool force4) {
    vec3 a = unpack565(c0);
    vec3 b = unpack565(c1);
    g_pal[0] = a;
    g_pal[1] = b;
    if (force4 || c0 > c1) {
        g_pal[2] = (2.0 * a + b) / 3.0;
        g_pal[3] = (a + 2.0 * b) / 3.0;
    } else {
        g_pal[2] = (a + b) * 0.5;
        g_pal[3] = vec3(0.0);
    }
}

float decodeBC4(uint w0, uint w1, int px) {
    float a0 = float(w0 & 255u);
    float a1 = float((w0 >> 8) & 255u);
    uint lo = (w0 >> 16) | (w1 << 16);
    uint hi = w1 >> 16;
    int pos = 3 * px;
    uint idx;
    if (pos + 3 <= 32)  idx = (lo >> uint(pos)) & 7u;
    else if (pos >= 32) idx = (hi >> uint(pos - 32)) & 7u;
    else                idx = ((lo >> 30u) | (hi << 2u)) & 7u;

    float v;
    if (idx == 0u)      v = a0;
    else if (idx == 1u) v = a1;
    else if (a0 > a1)   v = (float(8u - idx) * a0 + float(idx - 1u) * a1) / 7.0;
    else if (idx == 6u) v = 0.0;
    else if (idx == 7u) v = 255.0;
    else                v = (float(6u - idx) * a0 + float(idx - 1u) * a1) / 5.0;
    return v / 255.0;
}

void main() {
    ivec2 bc = ivec2(gl_GlobalInvocationID.xy);
    if (bc.x >= u_imageSizeInBlocks.x || bc.y >= u_imageSizeInBlocks.y) return;

    int base = (bc.y * u_imageSizeInBlocks.x + bc.x) * BLOCK_WORDS;

#if defined(FMT_BC1) || defined(FMT_BC2) || defined(FMT_BC3)
  #if defined(FMT_BC1)
    int co = 0;
  #else
    int co = 2;
  #endif
    uint cw0 = blocks[base + co];
    uint cw1 = blocks[base + co + 1];
    uint c0 = cw0 & 0xFFFFu;
    uint c1 = cw0 >> 16;
  #if defined(FMT_BC1)
    buildPalette(c0, c1, false);
    bool punch = (c0 <= c1);
  #else
    buildPalette(c0, c1, true);
  #endif
#endif
#if defined(FMT_BC2)
    uint aw0 = blocks[base + 0];
    uint aw1 = blocks[base + 1];
#endif
#if defined(FMT_BC3) || defined(FMT_BC4) || defined(FMT_BC5)
    uint ra0 = blocks[base + 0];
    uint ra1 = blocks[base + 1];
#endif
#if defined(FMT_BC5)
    uint ga0 = blocks[base + 2];
    uint ga1 = blocks[base + 3];
#endif

    ivec2 bp = bc * 4;
    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
            int px = y * 4 + x;
            vec4 o;
#if defined(FMT_BC1)
            uint code = (cw1 >> uint(2 * px)) & 3u;
            o = vec4(g_pal[code], (punch && code == 3u) ? 0.0 : 1.0);
#elif defined(FMT_BC2)
            uint code = (cw1 >> uint(2 * px)) & 3u;
            uint aw = (px < 8) ? aw0 : aw1;
            float al = float((aw >> uint(4 * (px & 7))) & 15u) / 15.0;
            o = vec4(g_pal[code], al);
#elif defined(FMT_BC3)
            uint code = (cw1 >> uint(2 * px)) & 3u;
            o = vec4(g_pal[code], decodeBC4(ra0, ra1, px));
#elif defined(FMT_BC4)
            o = vec4(decodeBC4(ra0, ra1, px), 0.0, 0.0, 1.0);
#elif defined(FMT_BC5)
            o = vec4(decodeBC4(ra0, ra1, px), decodeBC4(ga0, ga1, px), 0.0, 1.0);
#endif
            imageStore(u_outputImage, bp + ivec2(x, y), o);
        }
    }
}
)GLSL";

// ----------------------------------------------------------------------------
// BC7 Decoder (all 8 modes)
// ----------------------------------------------------------------------------
static const char* kBc7Decoder = R"GLSL(
layout(std430, binding = 0) readonly buffer CompressedBuffer { uint blocks[]; };
layout(rgba8, binding = 0) writeonly uniform highp image2D u_outputImage;

const int kW2[4]  = int[4](0, 21, 43, 64);
const int kW3[8]  = int[8](0, 9, 18, 27, 37, 46, 55, 64);
const int kW4[16] = int[16](0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64);

const int kNS[8]  = int[8](3, 2, 3, 2, 1, 1, 1, 2);
const int kPB[8]  = int[8](4, 6, 6, 6, 0, 0, 0, 6);
const int kRB[8]  = int[8](0, 0, 0, 0, 2, 2, 0, 0);
const int kISB[8] = int[8](0, 0, 0, 0, 1, 0, 0, 0);
const int kCB[8]  = int[8](4, 6, 5, 7, 5, 7, 7, 5);
const int kAB[8]  = int[8](0, 0, 0, 0, 6, 8, 7, 5);
const int kEPB[8] = int[8](1, 0, 0, 1, 0, 0, 1, 1);
const int kSPB[8] = int[8](0, 1, 0, 0, 0, 0, 0, 0);
const int kIB[8]  = int[8](3, 3, 2, 2, 2, 2, 4, 2);
const int kIB2[8] = int[8](0, 0, 0, 0, 3, 2, 0, 0);

uint g_b[4];
int  g_pos;

uint rd(int n) {
    int w = g_pos >> 5;
    int o = g_pos & 31;
    uint v = g_b[w] >> uint(o);
    if (o + n > 32) v |= g_b[w + 1] << uint(32 - o);
    g_pos += n;
    return v & ((1u << uint(n)) - 1u);
}

int weightOf(int bits, int idx) {
    return bits == 2 ? kW2[idx] : (bits == 3 ? kW3[idx] : kW4[idx]);
}

int exp8(int v, int n) {
    return n >= 8 ? v : ((v << (8 - n)) | (v >> (2 * n - 8)));
}

void main() {
    ivec2 bc = ivec2(gl_GlobalInvocationID.xy);
    if (bc.x >= u_imageSizeInBlocks.x || bc.y >= u_imageSizeInBlocks.y) return;

    int base = (bc.y * u_imageSizeInBlocks.x + bc.x) * 4;
    for (int k = 0; k < 4; k++) g_b[k] = blocks[base + k];
    g_pos = 0;
    ivec2 bp = bc * 4;

    int mode = 0;
    while (mode < 8 && rd(1) == 0u) mode++;
    if (mode >= 8) {
        for (int i = 0; i < 16; i++) imageStore(u_outputImage, bp + ivec2(i & 3, i >> 2), vec4(0.0));
        return;
    }

    int ns = kNS[mode];
    int cb = kCB[mode];
    int ab = kAB[mode];
    int ib = kIB[mode];
    int ib2 = kIB2[mode];
    int part = int(rd(kPB[mode]));
    int rot  = int(rd(kRB[mode]));
    int isel = int(rd(kISB[mode]));
    int ne = ns * 2;

    ivec4 ep[6];
    for (int e = 0; e < 6; e++) ep[e] = ivec4(0, 0, 0, 255);
    for (int c = 0; c < 3; c++) for (int e = 0; e < ne; e++) ep[e][c] = int(rd(cb));
    if (ab > 0) for (int e = 0; e < ne; e++) ep[e].a = int(rd(ab));

    int pbit[6];
    for (int e = 0; e < 6; e++) pbit[e] = 0;
    if (kEPB[mode] == 1) for (int e = 0; e < ne; e++) pbit[e] = int(rd(1));
    if (kSPB[mode] == 1) for (int s = 0; s < ns; s++) { int b = int(rd(1)); pbit[2 * s] = b; pbit[2 * s + 1] = b; }

    int cbits = cb;
    int abits = ab;
    if (kEPB[mode] == 1 || kSPB[mode] == 1) {
        for (int e = 0; e < ne; e++) {
            for (int c = 0; c < 3; c++) ep[e][c] = (ep[e][c] << 1) | pbit[e];
            if (ab > 0) ep[e].a = (ep[e].a << 1) | pbit[e];
        }
        cbits++;
        if (ab > 0) abits++;
    }
    for (int e = 0; e < ne; e++) {
        for (int c = 0; c < 3; c++) ep[e][c] = exp8(ep[e][c], cbits);
        if (ab > 0) ep[e].a = exp8(ep[e].a, abits);
    }

    int anc1 = -1;
    int anc2 = -1;
    if (ns == 2) anc1 = kAnchor2[part];
    if (ns == 3) { anc1 = kAnchor3a[part]; anc2 = kAnchor3b[part]; }

    int idx[16];
    int idx2[16];
    for (int i = 0; i < 16; i++) {
        bool isA = (i == 0) || (i == anc1) || (i == anc2);
        idx[i] = int(rd(isA ? ib - 1 : ib));
    }
    for (int i = 0; i < 16; i++) idx2[i] = 0;
    if (ib2 > 0) for (int i = 0; i < 16; i++) idx2[i] = int(rd(i == 0 ? ib2 - 1 : ib2));

    for (int i = 0; i < 16; i++) {
        int s = 0;
        if (ns == 2) s = int((kPart2[part] >> uint(i)) & 1u);
        else if (ns == 3) s = int((kPart3[part] >> uint(2 * i)) & 3u);
        ivec4 e0 = ep[2 * s];
        ivec4 e1 = ep[2 * s + 1];
        ivec4 px;
        if (ib2 > 0) {
            int ci  = (isel == 1) ? idx2[i] : idx[i];
            int cbb = (isel == 1) ? ib2 : ib;
            int ai  = (isel == 1) ? idx[i] : idx2[i];
            int abb = (isel == 1) ? ib : ib2;
            int wc = weightOf(cbb, ci);
            int wa = weightOf(abb, ai);
            px.rgb = ((64 - wc) * e0.rgb + wc * e1.rgb + 32) >> 6;
            px.a   = ((64 - wa) * e0.a + wa * e1.a + 32) >> 6;
        } else {
            int w = weightOf(ib, idx[i]);
            px = ((64 - w) * e0 + w * e1 + 32) >> 6;
        }
        if (rot > 0) { int t = px.a; px.a = px[rot - 1]; px[rot - 1] = t; }
        imageStore(u_outputImage, bp + ivec2(i & 3, i >> 2), vec4(px) / 255.0);
    }
}
)GLSL";

// ----------------------------------------------------------------------------
// BC7 Encoder (mode 6 + mode 1)
// ----------------------------------------------------------------------------
static const char* kBc7Encoder = R"GLSL(
layout(rgba8, binding = 0) readonly uniform highp image2D u_inputImage;
layout(std430, binding = 0) writeonly buffer CompressedBuffer { uint blocks[]; };

const int kW2[4]  = int[4](0, 21, 43, 64);
const int kW3[8]  = int[8](0, 9, 18, 27, 37, 46, 55, 64);
const int kW4[16] = int[16](0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64);

int weightOf(int bits, int idx) {
    return bits == 2 ? kW2[idx] : (bits == 3 ? kW3[idx] : kW4[idx]);
}

vec4 g_px[16];
uint g_o[4];
int  g_op;

void putBits(uint v, int n) {
    v &= (1u << uint(n)) - 1u;
    int w = g_op >> 5;
    int o = g_op & 31;
    g_o[w] |= v << uint(o);
    if (o + n > 32) g_o[w + 1] |= v >> uint(32 - o);
    g_op += n;
}

void fitEndpoints(uint mask, bool useA, int bitsIdx, out vec4 e0, out vec4 e1) {
    vec4 chm = useA ? vec4(1.0) : vec4(1.0, 1.0, 1.0, 0.0);
    vec4 mean = vec4(0.0);
    vec4 mn = vec4(255.0);
    vec4 mx = vec4(0.0);
    float cnt = 0.0;
    for (int i = 0; i < 16; i++) {
        if (((mask >> uint(i)) & 1u) != 0u) {
            vec4 p = g_px[i] * chm;
            cnt += 1.0;
            mean += p;
            mn = min(mn, p);
            mx = max(mx, p);
        }
    }
    mean /= cnt;

    mat4 cov = mat4(0.0);
    for (int i = 0; i < 16; i++) {
        if (((mask >> uint(i)) & 1u) != 0u) {
            vec4 d = g_px[i] * chm - mean;
            cov += outerProduct(d, d);
        }
    }
    vec4 axis = mx - mn;
    for (int it = 0; it < 4; it++) {
        vec4 r = cov * axis;
        float len = length(r);
        if (len < 1e-6) break;
        axis = r / len;
    }
    float alen = length(axis);
    if (alen < 1e-6) { e0 = mean; e1 = mean; return; }
    axis /= alen;

    float tmin = 1e9;
    float tmax = -1e9;
    for (int i = 0; i < 16; i++) {
        if (((mask >> uint(i)) & 1u) != 0u) {
            float t = dot(g_px[i] * chm - mean, axis);
            tmin = min(tmin, t);
            tmax = max(tmax, t);
        }
    }
    e0 = clamp(mean + axis * tmin, 0.0, 255.0);
    e1 = clamp(mean + axis * tmax, 0.0, 255.0);

    float maxIdx = float((1 << bitsIdx) - 1);
    for (int it = 0; it < 2; it++) {
        vec4 d = e1 - e0;
        float len2 = dot(d, d);
        if (len2 < 1e-6) break;
        float A = 0.0;
        float B = 0.0;
        float G = 0.0;
        vec4 s0 = vec4(0.0);
        vec4 s1 = vec4(0.0);
        for (int i = 0; i < 16; i++) {
            if (((mask >> uint(i)) & 1u) != 0u) {
                vec4 p = g_px[i] * chm;
                float t = dot(p - e0, d);
                float w = floor(clamp(t / len2, 0.0, 1.0) * maxIdx + 0.5) / maxIdx;
                float a = 1.0 - w;
                A += a * a; B += a * w; G += w * w;
                s0 += a * p;
                s1 += w * p;
            }
        }
        float det = A * G - B * B;
        if (abs(det) < 1e-6) break;
        e0 = clamp((G * s0 - B * s1) / det, 0.0, 255.0);
        e1 = clamp((A * s1 - B * s0) / det, 0.0, 255.0);
    }
}

int deq8(int f, int tb) {
    return tb >= 8 ? f : ((f << (8 - tb)) | (f >> (2 * tb - 8)));
}

int quantField(float t, int bits, int p, out int deqOut) {
    int tb = bits + 1;
    int maxF = (1 << bits) - 1;
    float x = t / 255.0 * float((1 << tb) - 1);
    int f = int(floor((x - float(p)) * 0.5 + 0.5));
    int best = 0;
    float bestErr = 1e9;
    deqOut = 0;
    for (int k = -1; k <= 1; k++) {
        int ff = min(max(f + k, 0), maxF);
        int d = deq8((ff << 1) | p, tb);
        float err = abs(float(d) - t);
        if (err < bestErr) { bestErr = err; best = ff; deqOut = d; }
    }
    return best;
}

struct Fit {
    float err;
    ivec4 f0;
    ivec4 f1;
    ivec4 d0;
    ivec4 d1;
    ivec2 p;
    int idx[16];
};

int bestIndex(vec4 pxv, bool useA, ivec4 e0, ivec4 e1, int bitsIdx, out float errOut) {
    int levels = 1 << bitsIdx;
    int best = 0;
    float be = 1e30;
    for (int k = 0; k < levels; k++) {
        int w = weightOf(bitsIdx, k);
        ivec4 q = ((64 - w) * e0 + w * e1 + 32) >> 6;
        vec4 d = pxv - vec4(q);
        if (!useA) d.a = 0.0;
        float e = dot(d, d);
        if (e < be) { be = e; best = k; }
    }
    errOut = be;
    return best;
}

Fit fitMode6() {
    vec4 e0;
    vec4 e1;
    fitEndpoints(0xFFFFu, true, 4, e0, e1);
    Fit f;
    f.err = 0.0;
    f.p = ivec2(0);
    f.f0 = ivec4(0); f.f1 = ivec4(0); f.d0 = ivec4(0); f.d1 = ivec4(0);
    for (int e = 0; e < 2; e++) {
        vec4 t = (e == 0) ? e0 : e1;
        float bestE = 1e30;
        for (int p = 0; p < 2; p++) {
            ivec4 fld = ivec4(0);
            ivec4 dq = ivec4(0);
            float E = 0.0;
            for (int c = 0; c < 4; c++) {
                int d8;
                fld[c] = quantField(t[c], 7, p, d8);
                dq[c] = d8;
                float d = float(d8) - t[c];
                E += d * d;
            }
            if (E < bestE) {
                bestE = E;
                if (e == 0) { f.f0 = fld; f.d0 = dq; f.p.x = p; }
                else        { f.f1 = fld; f.d1 = dq; f.p.y = p; }
            }
        }
    }
    for (int i = 0; i < 16; i++) {
        float e;
        f.idx[i] = bestIndex(g_px[i], true, f.d0, f.d1, 4, e);
        f.err += e;
    }
    if (f.idx[0] >= 8) {
        ivec4 t = f.f0; f.f0 = f.f1; f.f1 = t;
        t = f.d0; f.d0 = f.d1; f.d1 = t;
        f.p = f.p.yx;
        for (int i = 0; i < 16; i++) f.idx[i] = 15 - f.idx[i];
    }
    return f;
}

Fit fitMode1Subset(uint mask, int anchor) {
    vec4 e0;
    vec4 e1;
    fitEndpoints(mask, false, 3, e0, e1);
    Fit f;
    f.err = 0.0;
    f.p = ivec2(0);
    f.f0 = ivec4(0); f.f1 = ivec4(0); f.d0 = ivec4(0, 0, 0, 255); f.d1 = ivec4(0, 0, 0, 255);
    for (int i = 0; i < 16; i++) f.idx[i] = 0;
    float bestE = 1e30;
    for (int p = 0; p < 2; p++) {
        ivec4 fl0 = ivec4(0);
        ivec4 fl1 = ivec4(0);
        ivec4 dq0 = ivec4(0, 0, 0, 255);
        ivec4 dq1 = ivec4(0, 0, 0, 255);
        float E = 0.0;
        for (int c = 0; c < 3; c++) {
            int d8;
            fl0[c] = quantField(e0[c], 6, p, d8);
            dq0[c] = d8;
            float d = float(d8) - e0[c];
            E += d * d;
            fl1[c] = quantField(e1[c], 6, p, d8);
            dq1[c] = d8;
            d = float(d8) - e1[c];
            E += d * d;
        }
        if (E < bestE) { bestE = E; f.f0 = fl0; f.f1 = fl1; f.d0 = dq0; f.d1 = dq1; f.p = ivec2(p); }
    }
    for (int i = 0; i < 16; i++) {
        if (((mask >> uint(i)) & 1u) != 0u) {
            float e;
            f.idx[i] = bestIndex(g_px[i], false, f.d0, f.d1, 3, e);
            f.err += e;
        }
    }
    if (f.idx[anchor] >= 4) {
        ivec4 t = f.f0; f.f0 = f.f1; f.f1 = t;
        t = f.d0; f.d0 = f.d1; f.d1 = t;
        for (int i = 0; i < 16; i++) if (((mask >> uint(i)) & 1u) != 0u) f.idx[i] = 7 - f.idx[i];
    }
    return f;
}

uint clusterMask() {
    vec4 chm = vec4(1.0, 1.0, 1.0, 0.0);
    vec4 mean = vec4(0.0);
    for (int i = 0; i < 16; i++) mean += g_px[i] * chm / 16.0;
    mat4 cov = mat4(0.0);
    vec4 mn = vec4(255.0);
    vec4 mx = vec4(0.0);
    for (int i = 0; i < 16; i++) {
        vec4 p = g_px[i] * chm;
        vec4 d = p - mean;
        mn = min(mn, p);
        mx = max(mx, p);
        cov += outerProduct(d, d);
    }
    vec4 axis = mx - mn;
    for (int it = 0; it < 4; it++) {
        vec4 r = cov * axis;
        float len = length(r);
        if (len < 1e-6) break;
        axis = r / len;
    }
    float proj[16];
    float pmin = 1e9;
    float pmax = -1e9;
    for (int i = 0; i < 16; i++) {
        proj[i] = dot(g_px[i] * chm - mean, axis);
        pmin = min(pmin, proj[i]);
        pmax = max(pmax, proj[i]);
    }
    float thr = 0.5 * (pmin + pmax);
    for (int it = 0; it < 3; it++) {
        float s0 = 0.0;
        float s1 = 0.0;
        int n0 = 0;
        int n1 = 0;
        for (int i = 0; i < 16; i++) {
            if (proj[i] > thr) { s1 += proj[i]; n1++; } else { s0 += proj[i]; n0++; }
        }
        if (n0 == 0 || n1 == 0) break;
        thr = 0.5 * (s0 / float(n0) + s1 / float(n1));
    }
    uint m = 0u;
    for (int i = 0; i < 16; i++) if (proj[i] > thr) m |= 1u << uint(i);
    return m;
}

void putBlockMode6(Fit f) {
    for (int k = 0; k < 4; k++) g_o[k] = 0u;
    g_op = 0;
    putBits(64u, 7);
    for (int c = 0; c < 4; c++) { putBits(uint(f.f0[c]), 7); putBits(uint(f.f1[c]), 7); }
    putBits(uint(f.p.x), 1);
    putBits(uint(f.p.y), 1);
    for (int i = 0; i < 16; i++) putBits(uint(f.idx[i]), i == 0 ? 3 : 4);
}

void putBlockMode1(int part, Fit s0, Fit s1) {
    for (int k = 0; k < 4; k++) g_o[k] = 0u;
    g_op = 0;
    putBits(2u, 2);
    putBits(uint(part), 6);
    for (int c = 0; c < 3; c++) {
        putBits(uint(s0.f0[c]), 6); putBits(uint(s0.f1[c]), 6);
        putBits(uint(s1.f0[c]), 6); putBits(uint(s1.f1[c]), 6);
    }
    putBits(uint(s0.p.x), 1);
    putBits(uint(s1.p.x), 1);
    int a1 = kAnchor2[part];
    uint pm = kPart2[part];
    for (int i = 0; i < 16; i++) {
        int sub = int((pm >> uint(i)) & 1u);
        int v = (sub == 0) ? s0.idx[i] : s1.idx[i];
        putBits(uint(v), (i == 0 || i == a1) ? 2 : 3);
    }
}

void main() {
    ivec2 bc = ivec2(gl_GlobalInvocationID.xy);
    if (bc.x >= u_imageSizeInBlocks.x || bc.y >= u_imageSizeInBlocks.y) return;

    ivec2 imgMax = imageSize(u_inputImage) - ivec2(1);
    ivec2 bp = bc * 4;
    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
            vec4 c = imageLoad(u_inputImage, min(bp + ivec2(x, y), imgMax));
            g_px[y * 4 + x] = floor(c * 255.0 + 0.5);
        }
    }

    Fit f6 = fitMode6();
    float bestErr = f6.err;
    putBlockMode6(f6);
    uvec4 best = uvec4(g_o[0], g_o[1], g_o[2], g_o[3]);

    float aErr = 0.0;
    for (int i = 0; i < 16; i++) { float d = g_px[i].a - 255.0; aErr += d * d; }

    if (aErr < bestErr) {
        uint m = clusterMask();
        uint usedLo = 0u;
        uint usedHi = 0u;
        for (int n = 0; n < BC7_TOPN; n++) {
            int part = -1;
            int bh = 99;
            for (int p = 0; p < 64; p++) {
                bool used = (p < 32) ? (((usedLo >> uint(p)) & 1u) != 0u) : (((usedHi >> uint(p - 32)) & 1u) != 0u);
                if (used) continue;
                int h = int(bitCount(m ^ kPart2[p]));
                h = min(h, 16 - h);
                if (h < bh) { bh = h; part = p; }
            }
            if (part < 0) break;
            if (part < 32) usedLo |= 1u << uint(part); else usedHi |= 1u << uint(part - 32);

            uint pm = kPart2[part];
            Fit s0 = fitMode1Subset(~pm & 0xFFFFu, 0);
            Fit s1 = fitMode1Subset(pm, kAnchor2[part]);
            float E = s0.err + s1.err + aErr;
            if (E < bestErr) {
                bestErr = E;
                putBlockMode1(part, s0, s1);
                best = uvec4(g_o[0], g_o[1], g_o[2], g_o[3]);
            }
        }
    }

    int base = (bc.y * u_imageSizeInBlocks.x + bc.x) * 4;
    blocks[base + 0] = best.x;
    blocks[base + 1] = best.y;
    blocks[base + 2] = best.z;
    blocks[base + 3] = best.w;
}
)GLSL";

// ----------------------------------------------------------------------------
// BC6H (HDR, UF16 / SF16): Mode Tables & Shared Code
// ----------------------------------------------------------------------------
struct Bc6Mode {
    int code;
    int two;
    int tr;
    int prec;
    int dr, dg, db;
    const char* fields;
};
static const Bc6Mode kBc6Modes[14] = {
    {  0, 1, 1, 10, 5, 5, 5, "G2.4 B2.4 B3.4 R0.9.0 G0.9.0 B0.9.0 R1.4.0 G3.4 G2.3.0 G1.4.0 B3.0 G3.3.0 B1.4.0 B3.1 B2.3.0 R2.4.0 B3.2 R3.4.0 B3.3 P.4.0" },
    {  1, 1, 1,  7, 6, 6, 6, "G2.5 G3.4 G3.5 R0.6.0 B3.0 B3.1 B2.4 G0.6.0 B2.5 B3.2 G2.4 B0.6.0 B3.3 B3.5 B3.4 R1.5.0 G2.3.0 G1.5.0 G3.3.0 B1.5.0 B2.3.0 R2.5.0 R3.5.0 P.4.0" },
    {  2, 1, 1, 11, 5, 4, 4, "R0.9.0 G0.9.0 B0.9.0 R1.4.0 R0.10 G2.3.0 G1.3.0 G0.10 B3.0 G3.3.0 B1.3.0 B0.10 B3.1 B2.3.0 R2.4.0 B3.2 R3.4.0 B3.3 P.4.0" },
    {  6, 1, 1, 11, 4, 5, 4, "R0.9.0 G0.9.0 B0.9.0 R1.3.0 R0.10 G3.4 G2.3.0 G1.4.0 G0.10 G3.3.0 B1.3.0 B0.10 B3.1 B2.3.0 R2.3.0 B3.0 B3.2 R3.3.0 G2.4 B3.3 P.4.0" },
    { 10, 1, 1, 11, 4, 4, 5, "R0.9.0 G0.9.0 B0.9.0 R1.3.0 R0.10 B2.4 G2.3.0 G1.3.0 G0.10 B3.0 G3.3.0 B1.4.0 B0.10 B2.3.0 R2.3.0 B3.1 B3.2 R3.3.0 B3.4 B3.3 P.4.0" },
    { 14, 1, 1,  9, 5, 5, 5, "R0.8.0 B2.4 G0.8.0 G2.4 B0.8.0 B3.4 R1.4.0 G3.4 G2.3.0 G1.4.0 B3.0 G3.3.0 B1.4.0 B3.1 B2.3.0 R2.4.0 B3.2 R3.4.0 B3.3 P.4.0" },
    { 18, 1, 1,  8, 6, 5, 5, "R0.7.0 G3.4 B2.4 G0.7.0 B3.2 G2.4 B0.7.0 B3.3 B3.4 R1.5.0 G2.3.0 G1.4.0 B3.0 G3.3.0 B1.4.0 B3.1 B2.3.0 R2.5.0 R3.5.0 P.4.0" },
    { 22, 1, 1,  8, 5, 6, 5, "R0.7.0 B3.0 B2.4 G0.7.0 G2.5 G2.4 B0.7.0 G3.5 B3.4 R1.4.0 G3.4 G2.3.0 G1.5.0 G3.3.0 B1.4.0 B3.1 B2.3.0 R2.4.0 B3.2 R3.4.0 B3.3 P.4.0" },
    { 26, 1, 1,  8, 5, 5, 6, "R0.7.0 B3.1 B2.4 G0.7.0 B2.5 G2.4 B0.7.0 B3.5 B3.4 R1.4.0 G3.4 G2.3.0 G1.4.0 B3.0 G3.3.0 B1.5.0 B2.3.0 R2.4.0 B3.2 R3.4.0 B3.3 P.4.0" },
    { 30, 1, 0,  6, 6, 6, 6, "R0.5.0 G3.4 B3.0 B3.1 B2.4 G0.5.0 G2.5 B2.5 B3.2 G2.4 B0.5.0 G3.5 B3.3 B3.5 B3.4 R1.5.0 G2.3.0 G1.5.0 G3.3.0 B1.5.0 B2.3.0 R2.5.0 R3.5.0 P.4.0" },
    {  3, 0, 0, 10,10,10,10, "R0.9.0 G0.9.0 B0.9.0 R1.9.0 G1.9.0 B1.9.0" },
    {  7, 0, 1, 11, 9, 9, 9, "R0.9.0 G0.9.0 B0.9.0 R1.8.0 R0.10 G1.8.0 G0.10 B1.8.0 B0.10" },
    { 11, 0, 1, 12, 8, 8, 8, "R0.9.0 G0.9.0 B0.9.0 R1.7.0 R0.10.11 G1.7.0 G0.10.11 B1.7.0 B0.10.11" },
    { 15, 0, 1, 16, 4, 4, 4, "R0.9.0 G0.9.0 B0.9.0 R1.3.0 R0.10.15 G1.3.0 G0.10.15 B1.3.0 B0.10.15" },
};

inline std::string bc6hGlslTables() {
    const int kStride = 24;
    int fields[14 * kStride];
    int cnt[14], code[14], two[14], tr[14], prec[14], dlt[14 * 3], map[32];
    std::memset(fields, 0, sizeof(fields));
    for (int i = 0; i < 32; i++) map[i] = -1;
    for (int m = 0; m < 14; m++) {
        const Bc6Mode& md = kBc6Modes[m];
        int n = 0;
        const char* p = md.fields;
        while (*p) {
            while (*p == ' ') p++;
            if (!*p) break;
            int var;
            if (*p == 'P') { var = 12; p++; }
            else {
                int ch = (*p == 'R') ? 0 : (*p == 'G') ? 1 : 2;
                p++;
                var = ch * 4 + (*p - '0');
                p++;
            }
            p++;
            char* e = nullptr;
            int a = int(std::strtol(p, &e, 10)); p = e;
            int b = a;
            if (*p == '.') { p++; b = int(std::strtol(p, &e, 10)); p = e; }
            if (n < kStride) fields[m * kStride + n] = (var << 8) | (a << 4) | b;
            n++;
        }
        cnt[m] = n; code[m] = md.code; two[m] = md.two; tr[m] = md.tr; prec[m] = md.prec;
        dlt[m * 3 + 0] = md.dr; dlt[m * 3 + 1] = md.dg; dlt[m * 3 + 2] = md.db;
        if (md.code >= 2) map[md.code] = m;
    }

    std::string s;
    char buf[16];
    auto emit = [&](const char* name, const int* t, int n) {
        s += "const int "; s += name; s += "["; s += std::to_string(n); s += "] = int["; s += std::to_string(n); s += "](";
        for (int i = 0; i < n; i++) { std::snprintf(buf, sizeof(buf), "%d%s", t[i], i < n - 1 ? "," : ""); s += buf; }
        s += ");\n";
    };
    emit("kBc6Fields", fields, 14 * kStride);
    emit("kBc6Cnt", cnt, 14);
    emit("kBc6Code", code, 14);
    emit("kBc6Two", two, 14);
    emit("kBc6Tr", tr, 14);
    emit("kBc6Prec", prec, 14);
    emit("kBc6Dlt", dlt, 14 * 3);
    emit("kBc6Map", map, 32);
    return s;
}

static const char* kBc6Shared = R"GLSL(
#ifdef BC6H_SIGNED
const bool kSigned = true;
#else
const bool kSigned = false;
#endif
const int kW3[8]  = int[8](0, 9, 18, 27, 37, 46, 55, 64);
const int kW4[16] = int[16](0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64);

int asr(int x, int n) { return x >= 0 ? (x >> n) : -((-x + ((1 << n) - 1)) >> n); }

int signExt(int v, int bits) {
    v &= (1 << bits) - 1;
    int m = 1 << (bits - 1);
    return (v ^ m) - m;
}

int unq(int comp, int prec) {
    if (kSigned) {
        if (prec >= 16) return comp;
        int neg = 0;
        if (comp < 0) { neg = 1; comp = -comp; }
        int u;
        if (comp == 0) u = 0;
        else if (comp >= ((1 << (prec - 1)) - 1)) u = 0x7FFF;
        else u = ((comp << 15) + 0x4000) >> (prec - 1);
        return neg != 0 ? -u : u;
    }
    if (prec >= 15) return comp;
    if (comp == 0) return 0;
    if (comp == ((1 << prec) - 1)) return 0xFFFF;
    return ((comp << 16) + 0x8000) >> prec;
}

int finI(int c) {
    if (kSigned) { int m = (abs(c) * 31) >> 5; return c < 0 ? -m : m; }
    return (c * 31) >> 6;
}

uint finBits(int c) {
    int v = finI(c);
    if (kSigned && v < 0) return 0x8000u | uint(-v);
    return uint(v);
}
)GLSL";

// ----------------------------------------------------------------------------
// BC6H Decoder (all 14 modes, UF16 and SF16)
// ----------------------------------------------------------------------------
static const char* kBc6Decoder = R"GLSL(
layout(std430, binding = 0) readonly buffer CompressedBuffer { uint blocks[]; };
layout(rgba16f, binding = 0) writeonly uniform highp image2D u_outputImage;

uint g_b[4];

uint rdAt(int pos, int n) {
    int w = pos >> 5;
    int o = pos & 31;
    uint x = g_b[w] >> uint(o);
    if (o + n > 32 && w < 3) x |= g_b[w + 1] << uint(32 - o);
    return x & ((1u << uint(n)) - 1u);
}

float h2f(uint h) { return unpackHalf2x16(h).x; }

void main() {
    ivec2 bc = ivec2(gl_GlobalInvocationID.xy);
    if (bc.x >= u_imageSizeInBlocks.x || bc.y >= u_imageSizeInBlocks.y) return;

    int base = (bc.y * u_imageSizeInBlocks.x + bc.x) * 4;
    for (int k = 0; k < 4; k++) g_b[k] = blocks[base + k];
    ivec2 bp = bc * 4;

    int m2 = int(g_b[0] & 3u);
    int mode = (m2 < 2) ? m2 : kBc6Map[int(g_b[0] & 31u)];
    if (mode < 0) {
        for (int i = 0; i < 16; i++) imageStore(u_outputImage, bp + ivec2(i & 3, i >> 2), vec4(0.0, 0.0, 0.0, 1.0));
        return;
    }

    int v[13];
    for (int i = 0; i < 13; i++) v[i] = 0;
    int pos = (m2 < 2) ? 2 : 5;
    int cnt = kBc6Cnt[mode];
    for (int f = 0; f < cnt; f++) {
        int d = kBc6Fields[mode * 24 + f];
        int var = d >> 8;
        int a = (d >> 4) & 15;
        int b = d & 15;
        int n = abs(a - b) + 1;
        int bits = int(rdAt(pos, n));
        if (a >= b) v[var] |= bits << b;
        else for (int k = 0; k < n; k++) v[var] |= ((bits >> k) & 1) << (b - k);
        pos += n;
    }

    int prec = kBc6Prec[mode];
    bool two = kBc6Two[mode] == 1;
    bool tr = kBc6Tr[mode] == 1;
    int ne = two ? 4 : 2;

    ivec3 e[4];
    for (int k = 0; k < 4; k++) for (int c = 0; c < 3; c++) e[k][c] = v[c * 4 + k];

    for (int c = 0; c < 3; c++) {
        if (kSigned) e[0][c] = signExt(e[0][c], prec);
        if (tr || kSigned) {
            int dl = kBc6Dlt[mode * 3 + c];
            for (int k = 1; k < ne; k++) e[k][c] = signExt(e[k][c], tr ? dl : prec);
        }
    }
    if (tr) {
        int mask = (1 << prec) - 1;
        for (int c = 0; c < 3; c++) {
            for (int k = 1; k < ne; k++) {
                int s = (e[k][c] + e[0][c]) & mask;
                if (kSigned) s = signExt(s, prec);
                e[k][c] = s;
            }
        }
    }

    ivec3 q[4];
    for (int k = 0; k < 4; k++) for (int c = 0; c < 3; c++) q[k][c] = (k < ne) ? unq(e[k][c], prec) : 0;

    int part = two ? v[12] : 0;
    int anchor = two ? kAnchor2[part] : -1;
    int nb = two ? 3 : 4;
    int ip = two ? 82 : 65;
    uint pm = two ? kPart2[part] : 0u;

    for (int i = 0; i < 16; i++) {
        int ib = (i == 0 || i == anchor) ? nb - 1 : nb;
        int idx = int(rdAt(ip, ib));
        ip += ib;
        int s = two ? int((pm >> uint(i)) & 1u) : 0;
        int w = (nb == 3) ? kW3[idx] : kW4[idx];
        ivec3 ea = q[2 * s];
        ivec3 eb = q[2 * s + 1];
        ivec3 c3;
        for (int c = 0; c < 3; c++) c3[c] = asr(ea[c] * (64 - w) + eb[c] * w + 32, 6);
        vec4 o = vec4(h2f(finBits(c3.x)), h2f(finBits(c3.y)), h2f(finBits(c3.z)), 1.0);
        imageStore(u_outputImage, bp + ivec2(i & 3, i >> 2), o);
    }
}
)GLSL";

// ----------------------------------------------------------------------------
// BC6H Encoder (single-region modes only: 3, 7, 11, 15; UF16 and SF16)
// ----------------------------------------------------------------------------
static const char* kBc6Encoder = R"GLSL(
layout(rgba16f, binding = 0) readonly uniform highp image2D u_inputImage;
layout(std430, binding = 0) writeonly buffer CompressedBuffer { uint blocks[]; };

vec3 g_h[16];
vec3 g_u[16];
int  g_idx[16];
uint g_o[4];
int  g_op;

void putBits(uint v, int n) {
    v &= (1u << uint(n)) - 1u;
    int w = g_op >> 5;
    int o = g_op & 31;
    g_o[w] |= v << uint(o);
    if (o + n > 32) g_o[w + 1] |= v >> uint(32 - o);
    g_op += n;
}

float halfBits(float x) {
    if (isnan(x)) x = 0.0;
    x = clamp(x, -65504.0, 65504.0);
    uint h = packHalf2x16(vec2(x, 0.0)) & 0xFFFFu;
    uint mag = h & 0x7FFFu;
    if (mag > 0x7C00u) mag = 0u;
    mag = min(mag, 0x7BFFu);
    bool neg = (h & 0x8000u) != 0u;
    if (kSigned) return neg ? -float(mag) : float(mag);
    return neg ? 0.0 : float(mag);
}

int quantE(float e, int prec) {
    int lo;
    int hi;
    int q0;
    if (kSigned) {
        hi = (1 << (prec - 1)) - 1;
        lo = -hi;
        q0 = int(floor(e * (float(1 << (prec - 1)) / 32768.0) + 0.5));
    } else {
        hi = (1 << prec) - 1;
        lo = 0;
        q0 = int(floor(e * (float(1 << prec) / 65536.0)));
    }
    int best = 0;
    float bestErr = 1e30;
    for (int k = -1; k <= 1; k++) {
        int q = min(max(q0 + k, lo), hi);
        float err = abs(float(unq(q, prec)) - e);
        if (err < bestErr) { bestErr = err; best = q; }
    }
    return best;
}

void fitEnds(out vec3 e0, out vec3 e1) {
    vec3 mean = vec3(0.0);
    vec3 mn = vec3(1e9);
    vec3 mx = vec3(-1e9);
    for (int i = 0; i < 16; i++) { mean += g_u[i]; mn = min(mn, g_u[i]); mx = max(mx, g_u[i]); }
    mean /= 16.0;
    mat3 cov = mat3(0.0);
    for (int i = 0; i < 16; i++) { vec3 d = g_u[i] - mean; cov += outerProduct(d, d); }
    vec3 axis = mx - mn;
    float al = length(axis);
    if (al < 1e-3) { e0 = mean; e1 = mean; return; }
    axis /= al;
    for (int it = 0; it < 4; it++) {
        vec3 r = cov * axis;
        float len = length(r);
        if (len < 1e-3) break;
        axis = r / len;
    }
    float tmin = 1e9;
    float tmax = -1e9;
    for (int i = 0; i < 16; i++) {
        float t = dot(g_u[i] - mean, axis);
        tmin = min(tmin, t);
        tmax = max(tmax, t);
    }
    float lo = kSigned ? -32767.0 : 0.0;
    float hi = kSigned ? 32767.0 : 65535.0;
    e0 = clamp(mean + axis * tmin, lo, hi);
    e1 = clamp(mean + axis * tmax, lo, hi);
    for (int it = 0; it < 2; it++) {
        vec3 d = e1 - e0;
        float len2 = dot(d, d);
        if (len2 < 1e-6) break;
        float A = 0.0;
        float B = 0.0;
        float G = 0.0;
        vec3 s0 = vec3(0.0);
        vec3 s1 = vec3(0.0);
        for (int i = 0; i < 16; i++) {
            float t = dot(g_u[i] - e0, d);
            float w = floor(clamp(t / len2, 0.0, 1.0) * 15.0 + 0.5) / 15.0;
            float a = 1.0 - w;
            A += a * a; B += a * w; G += w * w;
            s0 += a * g_u[i];
            s1 += w * g_u[i];
        }
        float det = A * G - B * B;
        if (abs(det) < 1e-6) break;
        e0 = clamp((G * s0 - B * s1) / det, lo, hi);
        e1 = clamp((A * s1 - B * s0) / det, lo, hi);
    }
}

void finalizeEnds(int mode, ivec3 qa, inout ivec3 qb) {
    if (kBc6Tr[mode] == 1) {
        for (int c = 0; c < 3; c++) {
            int d = kBc6Dlt[mode * 3 + c];
            int lo = -(1 << (d - 1));
            int hi = (1 << (d - 1)) - 1;
            qb[c] = qa[c] + clamp(qb[c] - qa[c], lo, hi);
        }
    }
}

float scoreEnds(ivec3 qa, ivec3 qb, int prec) {
    ivec3 ua = ivec3(unq(qa.x, prec), unq(qa.y, prec), unq(qa.z, prec));
    ivec3 ub = ivec3(unq(qb.x, prec), unq(qb.y, prec), unq(qb.z, prec));
    ivec3 pal[16];
    for (int k = 0; k < 16; k++) {
        int w = kW4[k];
        pal[k] = ivec3(finI(asr(ua.x * (64 - w) + ub.x * w + 32, 6)),
                       finI(asr(ua.y * (64 - w) + ub.y * w + 32, 6)),
                       finI(asr(ua.z * (64 - w) + ub.z * w + 32, 6)));
    }
    float tot = 0.0;
    for (int i = 0; i < 16; i++) {
        float be = 1e30;
        int bk = 0;
        for (int k = 0; k < 16; k++) {
            vec3 d = g_h[i] - vec3(pal[k]);
            float e = dot(d, d);
            if (e < be) { be = e; bk = k; }
        }
        g_idx[i] = bk;
        tot += be;
    }
    return tot;
}

void main() {
    ivec2 bc = ivec2(gl_GlobalInvocationID.xy);
    if (bc.x >= u_imageSizeInBlocks.x || bc.y >= u_imageSizeInBlocks.y) return;

    ivec2 imgMax = imageSize(u_inputImage) - ivec2(1);
    ivec2 bp = bc * 4;
    float scale = kSigned ? (32.0 / 31.0) : (64.0 / 31.0);
    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
            vec4 c = imageLoad(u_inputImage, min(bp + ivec2(x, y), imgMax));
            vec3 h = vec3(halfBits(c.r), halfBits(c.g), halfBits(c.b));
            g_h[y * 4 + x] = h;
            g_u[y * 4 + x] = h * scale;
        }
    }

    vec3 e0;
    vec3 e1;
    fitEnds(e0, e1);

    int bestMode = 10;
    float bestErr = 1e38;
    ivec3 bestA = ivec3(0);
    ivec3 bestB = ivec3(0);
    int bestIdx[16];
    for (int i = 0; i < 16; i++) bestIdx[i] = 0;

    for (int mi = 10; mi < 14; mi++) {
        int prec = kBc6Prec[mi];
        ivec3 qa = ivec3(quantE(e0.x, prec), quantE(e0.y, prec), quantE(e0.z, prec));
        ivec3 qb = ivec3(quantE(e1.x, prec), quantE(e1.y, prec), quantE(e1.z, prec));
        finalizeEnds(mi, qa, qb);
        float err = scoreEnds(qa, qb, prec);
        if (g_idx[0] >= 8) {
            ivec3 t = qa; qa = qb; qb = t;
            finalizeEnds(mi, qa, qb);
            err = scoreEnds(qa, qb, prec);
            if (g_idx[0] >= 8) g_idx[0] = 7;
        }
        if (err < bestErr) {
            bestErr = err; bestMode = mi; bestA = qa; bestB = qb;
            for (int i = 0; i < 16; i++) bestIdx[i] = g_idx[i];
        }
    }

    for (int k = 0; k < 4; k++) g_o[k] = 0u;
    g_op = 0;
    putBits(uint(kBc6Code[bestMode]), 5);
    int v[13];
    for (int i = 0; i < 13; i++) v[i] = 0;
    bool tr = kBc6Tr[bestMode] == 1;
    for (int c = 0; c < 3; c++) {
        v[c * 4 + 0] = bestA[c];
        v[c * 4 + 1] = tr ? (bestB[c] - bestA[c]) : bestB[c];
    }
    int cnt = kBc6Cnt[bestMode];
    for (int f = 0; f < cnt; f++) {
        int d = kBc6Fields[bestMode * 24 + f];
        int var = d >> 8;
        int a = (d >> 4) & 15;
        int b = d & 15;
        int n = abs(a - b) + 1;
        uint uv = uint(v[var]) & 0xFFFFu;
        uint bits = 0u;
        if (a >= b) bits = (uv >> uint(b)) & ((1u << uint(n)) - 1u);
        else for (int k = 0; k < n; k++) bits |= ((uv >> uint(b - k)) & 1u) << uint(k);
        putBits(bits, n);
    }
    for (int i = 0; i < 16; i++) putBits(uint(bestIdx[i]), i == 0 ? 3 : 4);

    int base = (bc.y * u_imageSizeInBlocks.x + bc.x) * 4;
    blocks[base + 0] = g_o[0];
    blocks[base + 1] = g_o[1];
    blocks[base + 2] = g_o[2];
    blocks[base + 3] = g_o[3];
}
)GLSL";

} // namespace dt_detail

// ----------------------------------------------------------------------------
// Codec
// ----------------------------------------------------------------------------
class DesktopTexCodec {
public:
    DtGL gl;

    DesktopTexCodec() { std::memset(m_enc, 0, sizeof(m_enc)); std::memset(m_dec, 0, sizeof(m_dec)); }
    ~DesktopTexCodec() { shutdown(); }
    DesktopTexCodec(const DesktopTexCodec&) = delete;
    DesktopTexCodec& operator=(const DesktopTexCodec&) = delete;

    // Call with current GL context. getProc: dlsym / eglGetProcAddress.
    bool init(void* (*getProc)(const char*)) { return gl.load(getProc); }

    void shutdown() {
        for (int i = 0; i < DT_FORMAT_COUNT; i++) {
            if (m_enc[i]) { gl.DeleteProgram(m_enc[i]); m_enc[i] = 0; }
            if (m_dec[i]) { gl.DeleteProgram(m_dec[i]); m_dec[i] = 0; }
        }
    }

    // Compiles (lazily) encoder + decoder for a format. Returns false on error.
    bool ensure(DtFormat f) {
        if (!m_enc[f]) m_enc[f] = buildFormatProgram(f, true);
        if (!m_dec[f]) m_dec[f] = buildFormatProgram(f, false);
        return m_enc[f] && m_dec[f];
    }

    static int blocksX(int w) { return (w + 3) / 4; }
    static int blocksY(int h) { return (h + 3) / 4; }
    static size_t compressedSize(DtFormat f, int w, int h) {
        return size_t(blocksX(w)) * size_t(blocksY(h)) * size_t(dtBlockBytes(f));
    }

    // ---- buffers / textures ------------------------------------------------
    GLuint createCompressedBuffer(DtFormat f, int w, int h) const {
        GLuint buf = 0;
        gl.GenBuffers(1, &buf);
        gl.BindBuffer(GL_SHADER_STORAGE_BUFFER, buf);
        gl.BufferData(GL_SHADER_STORAGE_BUFFER, GLsizeiptr(compressedSize(f, w, h)), nullptr, GL_DYNAMIC_COPY);
        gl.BindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
        return buf;
    }

    // Immutable texture (RGBA8 implicit); for HW compressed pass internalFmt = dtGlInternalFormat(f)
    GLuint createTexture2D(int w, int h, GLenum internalFmt = GL_RGBA8) const {
        GLuint t = 0;
        gl.GenTextures(1, &t);
        gl.BindTexture(GL_TEXTURE_2D, t);
        gl.TexStorage2D(GL_TEXTURE_2D, 1, internalFmt, w, h);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        return t;
    }

    void uploadRGBA8(GLuint tex, int w, int h, const void* px) const {
        gl.BindTexture(GL_TEXTURE_2D, tex);
        gl.TexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
    }

    bool readRGBA8(GLuint tex, int w, int h, std::vector<uint8_t>& out) const {
        GLuint fbo = 0;
        gl.GenFramebuffers(1, &fbo);
        gl.BindFramebuffer(GL_FRAMEBUFFER, fbo);
        gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
        bool ok = gl.CheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
        if (ok) {
            out.resize(size_t(w) * size_t(h) * 4);
            gl.ReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, out.data());
        }
        gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
        gl.DeleteFramebuffers(1, &fbo);
        return ok;
    }

    // Float variants (for RGBA16F textures used by BC6H): 4 floats per pixel
    void uploadRGBA16F(GLuint tex, int w, int h, const float* px) const {
        gl.BindTexture(GL_TEXTURE_2D, tex);
        gl.TexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_FLOAT, px);
    }

    bool readRGBA16F(GLuint tex, int w, int h, std::vector<float>& out) const {
        GLuint fbo = 0;
        gl.GenFramebuffers(1, &fbo);
        gl.BindFramebuffer(GL_FRAMEBUFFER, fbo);
        gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
        bool ok = gl.CheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
        if (ok) {
            out.resize(size_t(w) * size_t(h) * 4);
            gl.ReadPixels(0, 0, w, h, GL_RGBA, GL_FLOAT, out.data());
        }
        gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
        gl.DeleteFramebuffers(1, &fbo);
        return ok;
    }

    // ---- encode / decode ---------------------------------------------------
    // srcTex: immutable RGBA8 or RGBA16F. outBuffer: from createCompressedBuffer.
    void encode(DtFormat f, GLuint srcTex, int w, int h, GLuint outBuffer) {
        if (!ensure(f)) return;
        const int bx = blocksX(w), by = blocksY(h);
        gl.UseProgram(m_enc[f]);
        gl.Uniform2i(0, bx, by);
        gl.BindImageTexture(0, srcTex, 0, GL_FALSE, 0, GL_READ_ONLY, dtImageFormat(f));
        gl.BindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, outBuffer);
        gl.DispatchCompute(GLuint((bx + 7) / 8), GLuint((by + 7) / 8), 1);
        gl.MemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);
    }

    // dstTex: immutable RGBA8 or RGBA16F.
    void decode(DtFormat f, GLuint inBuffer, int w, int h, GLuint dstTex) {
        if (!ensure(f)) return;
        const int bx = blocksX(w), by = blocksY(h);
        gl.UseProgram(m_dec[f]);
        gl.Uniform2i(0, bx, by);
        gl.BindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, inBuffer);
        gl.BindImageTexture(0, dstTex, 0, GL_FALSE, 0, GL_WRITE_ONLY, dtImageFormat(f));
        gl.DispatchCompute(GLuint((bx + 7) / 8), GLuint((by + 7) / 8), 1);
        gl.MemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT |
                         GL_FRAMEBUFFER_BARRIER_BIT | GL_TEXTURE_UPDATE_BARRIER_BIT);
    }

    // Compressed buffer -> hardware texture (no CPU copy).
    // hwTex = createTexture2D(w, h, dtGlInternalFormat(f)).
    // Requires S3TC (BC1-3) / RGTC (BC4-5) / BPTC (BC7/BC6H).
    void uploadToCompressedTexture(DtFormat f, GLuint buffer, GLuint hwTex, int w, int h) const {
        gl.BindBuffer(GL_PIXEL_UNPACK_BUFFER, buffer);
        gl.BindTexture(GL_TEXTURE_2D, hwTex);
        gl.CompressedTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, dtGlInternalFormat(f),
                                   GLsizei(compressedSize(f, w, h)), nullptr);
        gl.BindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    }

    std::vector<uint8_t> readbackBuffer(DtFormat f, GLuint buffer, int w, int h) const {
        std::vector<uint8_t> out(compressedSize(f, w, h));
        gl.BindBuffer(GL_SHADER_STORAGE_BUFFER, buffer);
        void* p = gl.MapBufferRange(GL_SHADER_STORAGE_BUFFER, 0, GLsizeiptr(out.size()), GL_MAP_READ_BIT);
        if (p) {
            std::memcpy(out.data(), p, out.size());
            gl.UnmapBuffer(GL_SHADER_STORAGE_BUFFER);
        }
        gl.BindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
        return out;
    }

    // Generic compile + link for a compute shader (concatenated sources).
    GLuint buildComputeProgram(const char* const* srcs, int count, const char* tag = "") const {
        GLuint sh = gl.CreateShader(GL_COMPUTE_SHADER);
        gl.ShaderSource(sh, count, srcs, nullptr);
        gl.CompileShader(sh);
        GLint ok = 0;
        gl.GetShaderiv(sh, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            char log[4096] = {0};
            gl.GetShaderInfoLog(sh, sizeof(log), nullptr, log);
            std::fprintf(stderr, "[dt] %s: compilation error:\n%s\n", tag, log);
            gl.DeleteShader(sh);
            return 0;
        }
        GLuint prog = gl.CreateProgram();
        gl.AttachShader(prog, sh);
        gl.LinkProgram(prog);
        gl.DeleteShader(sh);
        gl.GetProgramiv(prog, GL_LINK_STATUS, &ok);
        if (!ok) {
            char log[4096] = {0};
            gl.GetProgramInfoLog(prog, sizeof(log), nullptr, log);
            std::fprintf(stderr, "[dt] %s: link error:\n%s\n", tag, log);
            gl.DeleteProgram(prog);
            return 0;
        }
        return prog;
    }

private:
    GLuint m_enc[DT_FORMAT_COUNT];
    GLuint m_dec[DT_FORMAT_COUNT];

    GLuint buildFormatProgram(DtFormat f, bool encoder) const {
        static const char* names[] = { "FMT_BC1", "FMT_BC2", "FMT_BC3", "FMT_BC4", "FMT_BC5", "FMT_BC7",
                                       "FMT_BC6H_UF16", "FMT_BC6H_SF16" };
        std::string defs = std::string("#define ") + names[f] + " 1\n" +
                           "#define BLOCK_WORDS " + (dtBlockBytes(f) == 8 ? "2" : "4") + "\n";
        std::string tables;
        const char* body;
        if (f == DT_BC7) {
            defs += "#define BC7_TOPN " + std::to_string(BC7_TOPN) + "\n";
            tables = dt_detail::bc7GlslTables();
            body = encoder ? dt_detail::kBc7Encoder : dt_detail::kBc7Decoder;
        } else if (dtIsFloatFormat(f)) {
            if (f == DT_BC6H_SF16) defs += "#define BC6H_SIGNED 1\n";
            tables = dt_detail::bc7GlslTables() + dt_detail::bc6hGlslTables() + dt_detail::kBc6Shared;
            body = encoder ? dt_detail::kBc6Encoder : dt_detail::kBc6Decoder;
        } else {
            body = encoder ? dt_detail::kEncoder : dt_detail::kDecoder;
        }
        const char* srcs[5] = { DXT1_GLSL_VERSION, defs.c_str(), dt_detail::kCommon,
                                tables.c_str(), body };
        return buildComputeProgram(srcs, 5, names[f]);
    }
};
