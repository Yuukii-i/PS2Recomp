// RDG1: OpenGL / rlgl implementation of rdgfx:: interface.

#include "rd_gfx.h"
#include "raylib.h"
#include "rlgl.h"

#include <cstring>

#ifdef _WIN32
extern "C" __declspec(dllimport) void __stdcall glDepthFunc(unsigned int func); // FEAT-RD20 (opengl32 export; rlgl.h does not expose glad here)
extern "C" __declspec(dllimport) void * __stdcall wglGetProcAddress(const char *name); // FIX-RD79
extern "C" __declspec(dllimport) void __stdcall glGetIntegerv(unsigned int pname, int *data); // FIX-RD79
extern "C" __declspec(dllimport) void __stdcall glClearColor(float r, float g, float b, float a); // FIX-RD79
extern "C" __declspec(dllimport) void __stdcall glClear(unsigned int mask); // FIX-RD79
extern "C" __declspec(dllimport) void __stdcall glGenTextures(int n, unsigned int *textures); // FIX-RD82G
extern "C" __declspec(dllimport) void __stdcall glBindTexture(unsigned int target, unsigned int texture); // FIX-RD82G
extern "C" __declspec(dllimport) void __stdcall glTexImage2D(unsigned int target, int level, int internalformat, int width, int height, int border, unsigned int format, unsigned int type, const void *pixels); // FIX-RD82G
extern "C" __declspec(dllimport) void __stdcall glTexSubImage2D(unsigned int target, int level, int xoffset, int yoffset, int width, int height, unsigned int format, unsigned int type, const void *pixels); // FIX-RD82G
extern "C" __declspec(dllimport) void __stdcall glTexParameteri(unsigned int target, unsigned int pname, int param); // FIX-RD82G
extern "C" __declspec(dllimport) void __stdcall glDrawElements(unsigned int mode, int count, unsigned int type, const void *indices); // FIX-RD90
extern "C" __declspec(dllimport) unsigned int __stdcall glGetError(void); // PROBE-RD35
#endif

namespace rdgfx
{
namespace
{

Shader s_shader{};
int s_uniLoc[static_cast<size_t>(Uniform::Count)] = {
    -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1
};

const char *kVs =
    "#version 330\n"
    "layout(location = 0) in vec4 aPos;\n"
    "layout(location = 1) in vec4 aCol;\n"
    "layout(location = 2) in vec2 aUv;\n" // FEAT-RD3
    "uniform vec3 uFlip;\n"
    "uniform int uMode2d;\n" // FEAT-RD8
    "uniform vec2 uFbSize;\n" // FEAT-RD8
    "uniform vec2 uVpZ;\n"    // FEAT-RD11
    "uniform vec4 uVpA;\n"    // FEAT-RD21: sx, sy, ox, oy
    "uniform vec4 uVpB;\n"    // FEAT-RD21: offx, offy (12.4), aspect k, enable
    "uniform float uZBias;\n" // PROBE-RD59
    "out vec4 vCol;\n"
    "out vec2 vUv;\n" // FEAT-RD3
    "void main() {\n"
    "    if (uMode2d != 0) {\n" // FEAT-RD8
    "        float ndcZ = (uMode2d == 2 && abs(uVpZ.x) > 1e-6) ? clamp(((aPos.z - uVpZ.y) / uVpZ.x) * uFlip.z, -1.0, 1.0) : 0.0;\n" // FEAT-RD11 // FEAT-RD20
    "        gl_Position = vec4(aPos.x / uFbSize.x * 2.0 - 1.0, 1.0 - aPos.y / uFbSize.y * 2.0, ndcZ, 1.0);\n"
    "    } else if (uVpB.w > 0.5) {\n" // FEAT-RD21: guard-band VU clip -> GS fb pixels -> ndc (linear in x,y,w)
    "        float px = aPos.x * uVpA.x + aPos.w * uVpA.z - aPos.w * uVpB.x * (1.0 / 16.0);\n" // FEAT-RD21b
    "        float py = aPos.y * uVpA.y + aPos.w * uVpA.w - aPos.w * uVpB.y * (1.0 / 16.0);\n" // FEAT-RD21b
    "        float nx = (2.0 * px / uFbSize.x - aPos.w) / uVpB.z;\n"
    "        float ny = aPos.w - 2.0 * py / uFbSize.y;\n"
    "        gl_Position = vec4(nx * uFlip.x, ny * (-uFlip.y), aPos.z * uFlip.z, aPos.w);\n"
    "    } else {\n"
    "        gl_Position = vec4(aPos.x * uFlip.x, aPos.y * uFlip.y, aPos.z * uFlip.z, aPos.w);\n"
    "    }\n"
    "    if (uMode2d == 0 && uZBias != 0.0) gl_Position.z -= uZBias * gl_Position.w;\n" // PROBE-RD59
    "    vCol = aCol;\n"
    "    vUv = aUv;\n" // FEAT-RD3
    "}\n";

const char *kFs =
    "#version 330\n"
    "in vec4 vCol;\n"
    "in vec2 vUv;\n" // FEAT-RD3
    "uniform sampler2D uSampler;\n" // FEAT-RD3
    "uniform sampler2D uPalTex;\n" // FIX-RD82B
    "uniform int uPalMode;\n" // FIX-RD82B
    "uniform int uPalFallback;\n" // FIX-RD84
    "uniform int uTex;\n" // FEAT-RD3
    "uniform int uTfx;\n" // FEAT-RD3
    "uniform int uTcc;\n" // FEAT-RD3
    "uniform int uAtst;\n"
    "uniform int uAref;\n"
    "uniform int uAtInvert;\n" // FEAT-RD30
    "uniform float uFixAlpha;\n" // FEAT-RD7
    "out vec4 finalColor;\n"
    "void main() {\n"
    "    vec3 rgb = vCol.rgb;\n"
    "    float a_ps2 = vCol.a * 255.0;\n"
    "    if (uTex != 0) {\n" // FEAT-RD3
    "        vec4 t = texture(uSampler, vUv);\n"
    "        if (uPalMode == 1) {\n" // FIX-RD82B
    "            float l = dot(t.rgb, vec3(0.299, 0.587, 0.114));\n" // FIX-RD82B
    "            if (uPalFallback != 0)\n" // FIX-RD84
    "                t.rgb = vec3(0.40, 0.46, 0.80) * l + vec3(0.0, 0.01, 0.05);\n" // FIX-RD84
    "            else\n" // FIX-RD84
    "                t = texture(uPalTex, vec2((floor(l * 255.0) + 0.5) / 256.0, 0.5));\n" // FIX-RD82B
    "        }\n" // FIX-RD82B
    "        if (uPalMode != 0)\n" // FIX-RD82J
    "            t.a = 128.0 / 255.0;\n" // FIX-RD82J
    "        float t_a_ps2 = t.a * 255.0;\n"
    "        vec3 vc_rgb = vCol.rgb * 255.0;\n"
    "        float vc_a = vCol.a * 255.0;\n"
    "        if (uTfx == 0) {\n"
    "            rgb = t.rgb * vc_rgb / 128.0;\n"
    "            a_ps2 = (uTcc != 0) ? (t_a_ps2 * vc_a / 128.0) : vc_a;\n"
    "        } else if (uTfx == 1) {\n"
    "            rgb = t.rgb;\n"
    "            a_ps2 = (uTcc != 0) ? t_a_ps2 : vc_a;\n"
    "        } else if (uTfx == 2) {\n"
    "            rgb = t.rgb * vc_rgb / 128.0 + vec3(vc_a / 255.0);\n"
    "            a_ps2 = (uTcc != 0) ? (t_a_ps2 * vc_a / 128.0) : vc_a;\n"
    "        } else {\n"
    "            rgb = t.rgb * vc_rgb / 128.0 + vec3(vc_a / 255.0);\n"
    "            a_ps2 = (uTcc != 0) ? t_a_ps2 : vc_a;\n"
    "        }\n"
    "        rgb = clamp(rgb, 0.0, 1.0);\n"
    "    }\n"
    "    if (uAtst != 1) {\n"
    "        int a = int(clamp(a_ps2, 0.0, 255.0) + 0.5);\n"
    "        bool pass = true;\n"
    "        if (uAtst == 0) pass = false;\n"
    "        else if (uAtst == 2) pass = (a < uAref);\n"
    "        else if (uAtst == 3) pass = (a <= uAref);\n"
    "        else if (uAtst == 4) pass = (a == uAref);\n"
    "        else if (uAtst == 5) pass = (a >= uAref);\n"
    "        else if (uAtst == 6) pass = (a > uAref);\n"
    "        else if (uAtst == 7) pass = (a != uAref);\n"
    "        if (uAtInvert != 0) pass = !pass;\n" // FEAT-RD30
    "        if (!pass) discard;\n"
    "    }\n"
    "    finalColor = vec4(rgb, (uFixAlpha >= 0.0) ? min(uFixAlpha, 1.0) : min(a_ps2 / 128.0, 1.0));\n" // FEAT-RD7
    "}\n";

inline int toRlBlendFactor(BlendFactor f)
{
    switch (f)
    {
    case BlendFactor::Zero: return RL_ZERO;
    case BlendFactor::One: return RL_ONE;
    case BlendFactor::SrcAlpha: return RL_SRC_ALPHA;
    case BlendFactor::OneMinusSrcAlpha: return RL_ONE_MINUS_SRC_ALPHA;
    case BlendFactor::DstAlpha: return RL_DST_ALPHA;
    case BlendFactor::OneMinusDstAlpha: return RL_ONE_MINUS_DST_ALPHA;
    }
    return RL_ONE;
}

inline int toRlBlendEq(BlendEq eq)
{
    switch (eq)
    {
    case BlendEq::Add: return RL_FUNC_ADD;
    case BlendEq::Subtract: return RL_FUNC_SUBTRACT;
    case BlendEq::ReverseSubtract: return RL_FUNC_REVERSE_SUBTRACT;
    }
    return RL_FUNC_ADD;
}

inline int toRlFilter(TextureFilter f)
{
    switch (f)
    {
    case TextureFilter::Nearest: return RL_TEXTURE_FILTER_NEAREST;
    case TextureFilter::Linear: return RL_TEXTURE_FILTER_LINEAR;
    case TextureFilter::MipLinear: return RL_TEXTURE_FILTER_MIP_LINEAR;
    }
    return RL_TEXTURE_FILTER_NEAREST;
}

inline int toRlWrap(TextureWrap w)
{
    switch (w)
    {
    case TextureWrap::Repeat: return RL_TEXTURE_WRAP_REPEAT;
    case TextureWrap::Clamp: return RL_TEXTURE_WRAP_CLAMP;
    }
    return RL_TEXTURE_WRAP_REPEAT;
}

#ifdef _WIN32
using PFN_glActiveTexture = void(__stdcall *)(unsigned int);
using PFN_glGenFramebuffers = void(__stdcall *)(int, unsigned int *);
using PFN_glBindFramebuffer = void(__stdcall *)(unsigned int, unsigned int);
using PFN_glFramebufferTexture2D = void(__stdcall *)(unsigned int, unsigned int, unsigned int, unsigned int, int);
using PFN_glBlitFramebuffer = void(__stdcall *)(int, int, int, int, int, int, int, int, unsigned int, unsigned int);

PFN_glActiveTexture getActiveTexFn()
{
    static auto fn = reinterpret_cast<PFN_glActiveTexture>(wglGetProcAddress("glActiveTexture"));
    return fn;
}

void getFboFns(PFN_glGenFramebuffers &genFbo, PFN_glBindFramebuffer &bindFbo,
               PFN_glFramebufferTexture2D &fbTex2D, PFN_glBlitFramebuffer &blitFbo)
{
    static auto s_genFbo = reinterpret_cast<PFN_glGenFramebuffers>(wglGetProcAddress("glGenFramebuffers"));
    static auto s_bindFbo = reinterpret_cast<PFN_glBindFramebuffer>(wglGetProcAddress("glBindFramebuffer"));
    static auto s_fbTex2D = reinterpret_cast<PFN_glFramebufferTexture2D>(wglGetProcAddress("glFramebufferTexture2D"));
    static auto s_blitFbo = reinterpret_cast<PFN_glBlitFramebuffer>(wglGetProcAddress("glBlitFramebuffer"));
    genFbo = s_genFbo;
    bindFbo = s_bindFbo;
    fbTex2D = s_fbTex2D;
    blitFbo = s_blitFbo;
}
#endif

} // namespace

bool initShader()
{
    s_shader = LoadShaderFromMemory(kVs, kFs);
    s_uniLoc[static_cast<size_t>(Uniform::Flip)] = GetShaderLocation(s_shader, "uFlip");
    s_uniLoc[static_cast<size_t>(Uniform::Mode2d)] = GetShaderLocation(s_shader, "uMode2d");
    s_uniLoc[static_cast<size_t>(Uniform::FbSize)] = GetShaderLocation(s_shader, "uFbSize");
    s_uniLoc[static_cast<size_t>(Uniform::VpZ)] = GetShaderLocation(s_shader, "uVpZ");
    s_uniLoc[static_cast<size_t>(Uniform::VpA)] = GetShaderLocation(s_shader, "uVpA");
    s_uniLoc[static_cast<size_t>(Uniform::VpB)] = GetShaderLocation(s_shader, "uVpB");
    s_uniLoc[static_cast<size_t>(Uniform::ZBias)] = GetShaderLocation(s_shader, "uZBias");
    s_uniLoc[static_cast<size_t>(Uniform::Atst)] = GetShaderLocation(s_shader, "uAtst");
    s_uniLoc[static_cast<size_t>(Uniform::Aref)] = GetShaderLocation(s_shader, "uAref");
    s_uniLoc[static_cast<size_t>(Uniform::AtInvert)] = GetShaderLocation(s_shader, "uAtInvert");
    s_uniLoc[static_cast<size_t>(Uniform::FixAlpha)] = GetShaderLocation(s_shader, "uFixAlpha");
    s_uniLoc[static_cast<size_t>(Uniform::Sampler)] = GetShaderLocation(s_shader, "uSampler");
    s_uniLoc[static_cast<size_t>(Uniform::PalTex)] = GetShaderLocation(s_shader, "uPalTex");
    s_uniLoc[static_cast<size_t>(Uniform::PalMode)] = GetShaderLocation(s_shader, "uPalMode");
    s_uniLoc[static_cast<size_t>(Uniform::PalFallback)] = GetShaderLocation(s_shader, "uPalFallback");
    s_uniLoc[static_cast<size_t>(Uniform::Tex)] = GetShaderLocation(s_shader, "uTex");
    s_uniLoc[static_cast<size_t>(Uniform::Tfx)] = GetShaderLocation(s_shader, "uTfx");
    s_uniLoc[static_cast<size_t>(Uniform::Tcc)] = GetShaderLocation(s_shader, "uTcc");
    if (s_uniLoc[static_cast<size_t>(Uniform::ZBias)] >= 0)
    {
        const float zb0 = 0.0f;
        SetShaderValue(s_shader, s_uniLoc[static_cast<size_t>(Uniform::ZBias)], &zb0, SHADER_UNIFORM_FLOAT);
    }
    if (s_uniLoc[static_cast<size_t>(Uniform::Sampler)] >= 0)
    {
        int samplerUnit = 0;
        SetShaderValue(s_shader, s_uniLoc[static_cast<size_t>(Uniform::Sampler)], &samplerUnit, SHADER_UNIFORM_INT);
    }
    return s_shader.id != 0;
}

bool isShaderReady()
{
    return s_shader.id != 0;
}

void enableShader()
{
    rlEnableShader(s_shader.id);
}

void disableShader()
{
    rlDisableShader();
}

bool hasUniform(Uniform u)
{
    return s_uniLoc[static_cast<size_t>(u)] >= 0;
}

void setUniformInt(Uniform u, int v)
{
    const int loc = s_uniLoc[static_cast<size_t>(u)];
    if (loc >= 0)
        rlSetUniform(loc, &v, RL_SHADER_UNIFORM_INT, 1);
}

void setUniformFloat(Uniform u, float v)
{
    const int loc = s_uniLoc[static_cast<size_t>(u)];
    if (loc >= 0)
        rlSetUniform(loc, &v, RL_SHADER_UNIFORM_FLOAT, 1);
}

void setUniformVec2(Uniform u, const float *v)
{
    const int loc = s_uniLoc[static_cast<size_t>(u)];
    if (loc >= 0)
        rlSetUniform(loc, v, RL_SHADER_UNIFORM_VEC2, 1);
}

void setUniformVec3Immediate(Uniform u, const float *v)
{
    const int loc = s_uniLoc[static_cast<size_t>(u)];
    if (loc >= 0)
        SetShaderValue(s_shader, loc, v, SHADER_UNIFORM_VEC3);
}

void setUniformVec4(Uniform u, const float *v)
{
    const int loc = s_uniLoc[static_cast<size_t>(u)];
    if (loc >= 0)
        rlSetUniform(loc, v, RL_SHADER_UNIFORM_VEC4, 1);
}

void flushBatch()
{
    rlDrawRenderBatchActive();
}

void getBackbufferSize(int &rw, int &rh)
{
    rw = GetRenderWidth();
    rh = GetRenderHeight();
}

void setViewport(int x, int y, int w, int h)
{
    rlViewport(x, y, w, h);
}

void setScissorTest(bool on)
{
    if (on)
        rlEnableScissorTest();
    else
        rlDisableScissorTest();
}

void setScissorRect(int x, int y, int w, int h)
{
    rlScissor(x, y, w, h);
}

void setColorMask(bool r, bool g, bool b, bool a)
{
    rlColorMask(r, g, b, a);
}

void setDepthTest(bool on)
{
    if (on)
        rlEnableDepthTest();
    else
        rlDisableDepthTest();
}

void setDepthMask(bool on)
{
    if (on)
        rlEnableDepthMask();
    else
        rlDisableDepthMask();
}

void setDepthFunc(DepthFunc func)
{
#ifdef _WIN32
    glDepthFunc(func == DepthFunc::Always ? 0x0207u : 0x0203u); // GL_ALWAYS : GL_LEQUAL
#else
    (void)func;
#endif
}

void setBlendAlpha()
{
    rlEnableColorBlend();
    rlSetBlendMode(RL_BLEND_ALPHA);
}

void setBlend(bool enable,
              BlendFactor srcRgb,
              BlendFactor dstRgb,
              BlendFactor srcAlpha,
              BlendFactor dstAlpha,
              BlendEq eqRgb,
              BlendEq eqAlpha)
{
    if (!enable)
    {
        rlDisableColorBlend();
        return;
    }
    rlEnableColorBlend();
    rlSetBlendFactorsSeparate(toRlBlendFactor(srcRgb), toRlBlendFactor(dstRgb),
                              toRlBlendFactor(srcAlpha), toRlBlendFactor(dstAlpha),
                              toRlBlendEq(eqRgb), toRlBlendEq(eqAlpha));
    rlSetBlendMode(RL_BLEND_CUSTOM_SEPARATE);
}

void setBackfaceCulling(bool on)
{
    if (on)
        rlEnableBackfaceCulling();
    else
        rlDisableBackfaceCulling();
}

void clearScreenBuffers(float r, float g, float b, float a)
{
    rlClearColor(static_cast<unsigned char>(r * 255.0f),
                 static_cast<unsigned char>(g * 255.0f),
                 static_cast<unsigned char>(b * 255.0f),
                 static_cast<unsigned char>(a * 255.0f));
    rlClearScreenBuffers();
}

uint32_t createVertexArray()
{
    return rlLoadVertexArray();
}

void bindVertexArray(uint32_t vao)
{
    if (vao != 0u)
        rlEnableVertexArray(vao);
    else
        rlDisableVertexArray();
}

uint32_t createVertexBuffer(const void *data, int sizeBytes, bool dynamic)
{
    return rlLoadVertexBuffer(data, sizeBytes, dynamic);
}

void updateVertexBuffer(uint32_t vbo, const void *data, int sizeBytes, int offsetBytes)
{
    rlUpdateVertexBuffer(vbo, data, sizeBytes, offsetBytes);
}

void freeVertexBuffer(uint32_t vbo)
{
    rlUnloadVertexBuffer(vbo);
}

uint32_t createIndexBuffer(const void *data, int sizeBytes, bool dynamic)
{
    return rlLoadVertexBufferElement(data, sizeBytes, dynamic);
}

void updateIndexBuffer(uint32_t ebo, const void *data, int sizeBytes, int offsetBytes)
{
    rlUpdateVertexBufferElements(ebo, data, sizeBytes, offsetBytes);
}

void freeIndexBuffer(uint32_t ebo)
{
    rlUnloadVertexBuffer(ebo);
}

void bindVertexBuffer(uint32_t vbo)
{
    if (vbo != 0u)
        rlEnableVertexBuffer(vbo);
    else
        rlDisableVertexBuffer();
}

void bindIndexBuffer(uint32_t ebo)
{
    if (ebo != 0u)
        rlEnableVertexBufferElement(ebo);
    else
        rlDisableVertexBufferElement();
}

void setVertexAttribute(uint32_t index, int compCount, VertexAttrFormat type, bool normalized, int strideBytes, int offsetBytes)
{
    const int rlType = (type == VertexAttrFormat::UnsignedByte) ? RL_UNSIGNED_BYTE : RL_FLOAT;
    rlSetVertexAttribute(index, compCount, rlType, normalized, strideBytes, offsetBytes);
}

void enableVertexAttribute(uint32_t index)
{
    rlEnableVertexAttribute(index);
}

void drawArrays(PrimitiveTopology topo, int first, int count)
{
    (void)topo;
    rlDrawVertexArray(first, count);
}

void drawIndexed(PrimitiveTopology topo, uint32_t first, uint32_t count, bool idx32)
{
    (void)topo;
#ifdef _WIN32
    if (idx32)
    {
        glDrawElements(0x0004u /*GL_TRIANGLES*/, static_cast<int>(count), 0x1405u /*GL_UNSIGNED_INT*/,
                       reinterpret_cast<const void *>(static_cast<size_t>(first) * sizeof(uint32_t)));
        return;
    }
#else
    (void)idx32;
#endif
    rlDrawVertexArrayElements(static_cast<int>(first), static_cast<int>(count), nullptr);
}

uint32_t createTextureRGBA8(const void *data, int w, int h)
{
    return rlLoadTexture(data, w, h, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8, 1);
}

void freeTexture(uint32_t texId)
{
    rlUnloadTexture(texId);
}

void generateMipmaps(uint32_t texId, int w, int h)
{
    int mips = 1;
    rlGenTextureMipmaps(texId, w, h, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8, &mips);
}

void setTextureWrap(uint32_t texId, bool isT, TextureWrap wrap)
{
    rlTextureParameters(texId, isT ? RL_TEXTURE_WRAP_T : RL_TEXTURE_WRAP_S, toRlWrap(wrap));
}

void setTextureFilter(uint32_t texId, bool isMin, TextureFilter filter)
{
    rlTextureParameters(texId, isMin ? RL_TEXTURE_MIN_FILTER : RL_TEXTURE_MAG_FILTER, toRlFilter(filter));
}

void bindTexture(uint32_t unit, uint32_t texId)
{
    rlActiveTextureSlot(static_cast<int>(unit));
    if (texId != 0u)
        rlEnableTexture(texId);
    else
        rlDisableTexture();
}

bool isPaletteTextureSupported()
{
#ifdef _WIN32
    return getActiveTexFn() != nullptr;
#else
    return false;
#endif
}

bool uploadPaletteTexture(const uint32_t *rgba256)
{
#ifdef _WIN32
    auto pActiveTex = getActiveTexFn();
    if (!pActiveTex)
        return false;
    static unsigned int s_palTexId = 0u;
    static uint32_t s_palLastRgba[256] = {};
    static bool s_palHasData = false;
    pActiveTex(0x84C0u); // GL_TEXTURE0
    int prevTex0 = 0;
    glGetIntegerv(0x8069u, &prevTex0); // GL_TEXTURE_BINDING_2D
    pActiveTex(0x84C1u); // GL_TEXTURE1
    if (s_palTexId == 0u)
    {
        glGenTextures(1, &s_palTexId);
        if (s_palTexId != 0u)
        {
            glBindTexture(0x0DE1u, s_palTexId); // GL_TEXTURE_2D
            glTexParameteri(0x0DE1u, 0x2802u, 0x812Fu); // GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE
            glTexParameteri(0x0DE1u, 0x2803u, 0x812Fu); // GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE
            glTexParameteri(0x0DE1u, 0x2800u, 0x2600u); // GL_TEXTURE_MAG_FILTER, GL_NEAREST
            glTexParameteri(0x0DE1u, 0x2801u, 0x2600u); // GL_TEXTURE_MIN_FILTER, GL_NEAREST
            glTexImage2D(0x0DE1u, 0, 0x8058, 256, 1, 0, 0x1908u, 0x1401u, rgba256); // GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE
            std::memcpy(s_palLastRgba, rgba256, sizeof(s_palLastRgba));
            s_palHasData = true;
        }
    }
    else
    {
        glBindTexture(0x0DE1u, s_palTexId); // GL_TEXTURE_2D
        if (!s_palHasData || std::memcmp(rgba256, s_palLastRgba, sizeof(s_palLastRgba)) != 0)
        {
            glTexSubImage2D(0x0DE1u, 0, 0, 0, 256, 1, 0x1908u, 0x1401u, rgba256); // GL_RGBA, GL_UNSIGNED_BYTE
            std::memcpy(s_palLastRgba, rgba256, sizeof(s_palLastRgba));
            s_palHasData = true;
        }
    }
    pActiveTex(0x84C0u); // GL_TEXTURE0
    glBindTexture(0x0DE1u, static_cast<unsigned int>(prevTex0));
    return s_palTexId != 0u;
#else
    (void)rgba256;
    return false;
#endif
}

void unbindPaletteTexture()
{
#ifdef _WIN32
    auto pActiveTex = getActiveTexFn();
    if (pActiveTex)
    {
        pActiveTex(0x84C1u); // GL_TEXTURE1
        glBindTexture(0x0DE1u, 0u); // GL_TEXTURE_2D
        pActiveTex(0x84C0u); // GL_TEXTURE0
    }
#endif
}

void drawTextureOverlay(uint32_t texId, int texW, int texH,
                        float srcX, float srcY, float srcW, float srcH,
                        float dstX, float dstY, float dstW, float dstH,
                        uint8_t alpha)
{
    Texture2D prevTex{};
    prevTex.id = texId;
    prevTex.width = texW;
    prevTex.height = texH;
    prevTex.mipmaps = 1;
    prevTex.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
    const Rectangle srcRec{srcX, srcY, srcW, srcH};
    const Rectangle dstRec{dstX, dstY, dstW, dstH};
    const Color tint{255, 255, 255, alpha};
    rlDisableBackfaceCulling();
    DrawTexturePro(prevTex, srcRec, dstRec, Vector2{0.0f, 0.0f}, 0.0f, tint);
    rlDrawRenderBatchActive();
    rlEnableBackfaceCulling();
}

bool canCopyBackbuffer()
{
#ifdef _WIN32
    PFN_glGenFramebuffers pGenFbo = nullptr;
    PFN_glBindFramebuffer pBindFbo = nullptr;
    PFN_glFramebufferTexture2D pFbTex2D = nullptr;
    PFN_glBlitFramebuffer pBlitFbo = nullptr;
    getFboFns(pGenFbo, pBindFbo, pFbTex2D, pBlitFbo);
    return pGenFbo && pBindFbo && pFbTex2D && pBlitFbo;
#else
    return false;
#endif
}

bool beginBackbufferCopy(BackbufferCopyState &state)
{
    state = BackbufferCopyState{};
#ifdef _WIN32
    if (!canCopyBackbuffer())
        return false;
    glGetIntegerv(0x8CAAu, &state.prevReadFbo); // GL_READ_FRAMEBUFFER_BINDING
    glGetIntegerv(0x8CA6u, &state.prevDrawFbo); // GL_DRAW_FRAMEBUFFER_BINDING
    glGetIntegerv(0x80A8u, &state.sampleBufs);  // GL_SAMPLE_BUFFERS
    state.valid = true;
    return true;
#else
    return false;
#endif
}

bool copyBackbufferToTexture(BackbufferCopyState &state,
                             uint32_t &texId, uint32_t &auxId,
                             int &texW, int &texH,
                             int wantW, int wantH,
                             int vx, int vy, int vw, int vh,
                             int dstW, int dstH,
                             bool linearFilter)
{
#ifdef _WIN32
    PFN_glGenFramebuffers pGenFbo = nullptr;
    PFN_glBindFramebuffer pBindFbo = nullptr;
    PFN_glFramebufferTexture2D pFbTex2D = nullptr;
    PFN_glBlitFramebuffer pBlitFbo = nullptr;
    getFboFns(pGenFbo, pBindFbo, pFbTex2D, pBlitFbo);
    if (!state.valid || !pGenFbo || !pBindFbo || !pFbTex2D || !pBlitFbo)
        return false;

    static unsigned int s_resFbo = 0u, s_resTex = 0u;
    static int s_resW = 0, s_resH = 0;

    if (texId == 0u || texW != wantW || texH != wantH)
    {
        if (texId != 0u)
            rlUnloadTexture(texId);
        texId = rlLoadTexture(nullptr, wantW, wantH, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8, 1);
        texW = wantW;
        texH = wantH;
        if (texId != 0u)
        {
            rlTextureParameters(texId, RL_TEXTURE_WRAP_S, RL_TEXTURE_WRAP_CLAMP);
            rlTextureParameters(texId, RL_TEXTURE_WRAP_T, RL_TEXTURE_WRAP_CLAMP);
            rlTextureParameters(texId, RL_TEXTURE_MAG_FILTER, RL_TEXTURE_FILTER_LINEAR);
            rlTextureParameters(texId, RL_TEXTURE_MIN_FILTER, RL_TEXTURE_FILTER_LINEAR);
            if (auxId == 0u)
                pGenFbo(1, &auxId);
            pBindFbo(0x8CA9u, auxId); // GL_DRAW_FRAMEBUFFER
            pFbTex2D(0x8CA9u, 0x8CE0u, 0x0DE1u, texId, 0);
            glClearColor(0.0f, 0.0f, 0.0f, 1.0f); // FIX-RD82J
            glClear(0x00004000u);
        }
    }
    if (texId != 0u && auxId != 0u)
    {
        const unsigned int glFilter = linearFilter ? 0x2601u : 0x2600u; // GL_LINEAR : GL_NEAREST
        if (state.sampleBufs > 0)
        {
            if (!state.msaaResolved)
            {
                if (s_resTex == 0u || s_resW != vw || s_resH != vh)
                {
                    if (s_resTex != 0u)
                        rlUnloadTexture(s_resTex);
                    s_resTex = rlLoadTexture(nullptr, vw, vh, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8, 1);
                    s_resW = vw;
                    s_resH = vh;
                    if (s_resFbo == 0u)
                        pGenFbo(1, &s_resFbo);
                    pBindFbo(0x8CA9u, s_resFbo);
                    pFbTex2D(0x8CA9u, 0x8CE0u, 0x0DE1u, s_resTex, 0);
                }
                pBindFbo(0x8CA8u, static_cast<unsigned int>(state.prevDrawFbo));
                pBindFbo(0x8CA9u, s_resFbo);
                pBlitFbo(vx, vy, vx + vw, vy + vh, 0, 0, vw, vh, 0x00004000u, 0x2600u);
                state.msaaResolved = true;
            }
            pBindFbo(0x8CA8u, s_resFbo);
            pBindFbo(0x8CA9u, auxId);
            pBlitFbo(0, vh, vw, 0, 0, 0, dstW, dstH, 0x00004000u, glFilter);
        }
        else
        {
            pBindFbo(0x8CA8u, static_cast<unsigned int>(state.prevDrawFbo));
            pBindFbo(0x8CA9u, auxId);
            pBlitFbo(vx, vy + vh, vx + vw, vy, 0, 0, dstW, dstH, 0x00004000u, glFilter);
        }
        rlColorMask(false, false, false, true); // FIX-RD82J
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f); // FIX-RD82J
        glClear(0x00004000u); // FIX-RD82J
        rlColorMask(true, true, true, true); // FIX-RD82J
        return true;
    }
    return false;
#else
    (void)state; (void)texId; (void)auxId; (void)texW; (void)texH;
    (void)wantW; (void)wantH; (void)vx; (void)vy; (void)vw; (void)vh;
    (void)dstW; (void)dstH; (void)linearFilter;
    return false;
#endif
}

void endBackbufferCopy(const BackbufferCopyState &state)
{
#ifdef _WIN32
    PFN_glGenFramebuffers pGenFbo = nullptr;
    PFN_glBindFramebuffer pBindFbo = nullptr;
    PFN_glFramebufferTexture2D pFbTex2D = nullptr;
    PFN_glBlitFramebuffer pBlitFbo = nullptr;
    getFboFns(pGenFbo, pBindFbo, pFbTex2D, pBlitFbo);
    if (state.valid && pBindFbo)
    {
        pBindFbo(0x8CA8u, static_cast<unsigned int>(state.prevReadFbo));
        pBindFbo(0x8CA9u, static_cast<unsigned int>(state.prevDrawFbo));
    }
#else
    (void)state;
#endif
}

uint32_t getLastError()
{
#ifdef _WIN32
    return glGetError();
#else
    return 0u;
#endif
}

} // namespace rdgfx
