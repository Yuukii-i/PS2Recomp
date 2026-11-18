// FEAT-RD1: Direct geometry view from the VU1 scene buffer via OpenGL/rlgl.
#include "raylib.h"
#include "rlgl.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <vector>

extern void ps2xSafeLog(const char *fmt, ...); // FEAT-RD1
extern unsigned int ps2xRdGetTextureForBatch(uint32_t serial, uint64_t tex0, uint64_t texa); // FEAT-RD5
extern void ps2xRdTexTickFrame(); // FEAT-RD3

struct RdVert // FEAT-RD1
{
    float pos[4];
    uint8_t rgba[4];
    float uv[2]; // FEAT-RD3
};
static_assert(sizeof(RdVert) == 28, "RdVert size"); // FEAT-RD3

struct RdBatch // FEAT-RD2
{
    uint32_t first;
    uint32_t count;
    uint64_t alpha;
    uint64_t test;
    uint64_t frame;
    uint64_t zbuf;
    uint64_t prim;
    uint64_t tex0;  // FEAT-RD3
    uint64_t clamp; // FEAT-RD3
    uint64_t tex1;  // FEAT-RD3
    uint64_t texa;  // FEAT-RD3
    uint32_t kickSerial; // FEAT-RD5
};

namespace
{
std::mutex g_rdMutex;
std::vector<RdVert> g_rdPending;
std::vector<RdBatch> g_rdPendingBatches; // FEAT-RD2
uint32_t g_rdPendingSkipped = 0;
uint32_t g_rdPendingMasked = 0; // FEAT-RD2
bool g_rdHasPending = false;
bool g_rdEverPublished = false;

struct RdGpuState
{
    bool init = false;
    bool everUploaded = false;
    unsigned int vao = 0;
    unsigned int vbo = 0;
    size_t vboCapVerts = 0;
    size_t gpuVertCount = 0;
    uint32_t lastSkippedDraws = 0;
    uint32_t lastMaskedDraws = 0; // FEAT-RD2
    Shader shader{};
    int flipLoc = -1;
    int atstLoc = -1; // FEAT-RD2
    int arefLoc = -1; // FEAT-RD2
    int samplerLoc = -1; // FEAT-RD3
    int texLoc = -1;     // FEAT-RD3
    int tfxLoc = -1;     // FEAT-RD3
    int tccLoc = -1;     // FEAT-RD3
    float flip[3] = {1.0f, -1.0f, 1.0f}; // FEAT-RD2
    std::vector<RdVert> localScratch;
    std::vector<RdBatch> localBatches; // FEAT-RD2
    uint32_t frames = 0;
    uint64_t totalTris = 0;
    uint64_t totalSkippedDraws = 0;
    uint64_t totalMaskedDraws = 0; // FEAT-RD2
    uint64_t totalUnsupportedBlend = 0; // FEAT-RD2
    std::chrono::steady_clock::time_point lastLog = std::chrono::steady_clock::now();
};
RdGpuState g_rdGpu;
} // namespace

bool ps2xRdViewOn() // FEAT-RD1
{
    static const bool v = []() {
        const char *e = std::getenv("PS2X_RD_VIEW");
        return e && e[0] == '1';
    }();
    return v;
}

void ps2xRdPublish(const RdVert *v, size_t n, const RdBatch *b, size_t nb, uint32_t skippedDraws, uint32_t maskedDraws) // FEAT-RD2
{
    static thread_local std::vector<RdVert> t_staging;
    static thread_local std::vector<RdBatch> t_batchStaging;
    if (v && n > 0)
        t_staging.assign(v, v + n);
    else
        t_staging.clear();
    if (b && nb > 0)
        t_batchStaging.assign(b, b + nb);
    else
        t_batchStaging.clear();

    std::lock_guard<std::mutex> lock(g_rdMutex);
    g_rdPending.swap(t_staging);
    g_rdPendingBatches.swap(t_batchStaging);
    g_rdPendingSkipped = skippedDraws;
    g_rdPendingMasked = maskedDraws;
    g_rdHasPending = true;
    g_rdEverPublished = true;
}

bool ps2xRdDraw(float x, float y, float w, float h) // FEAT-RD1
{
    bool hasNew = false;
    {
        std::lock_guard<std::mutex> lock(g_rdMutex);
        if (!g_rdEverPublished && !g_rdGpu.everUploaded)
            return false;
        if (g_rdHasPending)
        {
            g_rdGpu.localScratch.swap(g_rdPending);
            g_rdGpu.localBatches.swap(g_rdPendingBatches); // FEAT-RD2
            g_rdGpu.lastSkippedDraws = g_rdPendingSkipped;
            g_rdGpu.lastMaskedDraws = g_rdPendingMasked; // FEAT-RD2
            g_rdHasPending = false;
            hasNew = true;
        }
    }

    if (!g_rdGpu.init)
    {
        g_rdGpu.init = true;
        static const char *kVs =
            "#version 330\n"
            "layout(location = 0) in vec4 aPos;\n"
            "layout(location = 1) in vec4 aCol;\n"
            "layout(location = 2) in vec2 aUv;\n" // FEAT-RD3
            "uniform vec3 uFlip;\n"
            "out vec4 vCol;\n"
            "out vec2 vUv;\n" // FEAT-RD3
            "void main() {\n"
            "    gl_Position = vec4(aPos.x * uFlip.x, aPos.y * uFlip.y, aPos.z * uFlip.z, aPos.w);\n"
            "    vCol = aCol;\n"
            "    vUv = aUv;\n" // FEAT-RD3
            "}\n";
        static const char *kFs =
            "#version 330\n"
            "in vec4 vCol;\n"
            "in vec2 vUv;\n" // FEAT-RD3
            "uniform sampler2D uSampler;\n" // FEAT-RD3
            "uniform int uTex;\n" // FEAT-RD3
            "uniform int uTfx;\n" // FEAT-RD3
            "uniform int uTcc;\n" // FEAT-RD3
            "uniform int uAtst;\n"
            "uniform int uAref;\n"
            "out vec4 finalColor;\n"
            "void main() {\n"
            "    vec3 rgb = vCol.rgb;\n"
            "    float a_ps2 = vCol.a * 255.0;\n"
            "    if (uTex != 0) {\n" // FEAT-RD3
            "        vec4 t = texture(uSampler, vUv);\n"
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
            "        if (!pass) discard;\n"
            "    }\n"
            "    finalColor = vec4(rgb, min(a_ps2 / 128.0, 1.0));\n"
            "}\n";
        g_rdGpu.shader = LoadShaderFromMemory(kVs, kFs);
        g_rdGpu.flipLoc = GetShaderLocation(g_rdGpu.shader, "uFlip");
        g_rdGpu.atstLoc = GetShaderLocation(g_rdGpu.shader, "uAtst"); // FEAT-RD2
        g_rdGpu.arefLoc = GetShaderLocation(g_rdGpu.shader, "uAref"); // FEAT-RD2
        g_rdGpu.samplerLoc = GetShaderLocation(g_rdGpu.shader, "uSampler"); // FEAT-RD3
        g_rdGpu.texLoc = GetShaderLocation(g_rdGpu.shader, "uTex");         // FEAT-RD3
        g_rdGpu.tfxLoc = GetShaderLocation(g_rdGpu.shader, "uTfx");         // FEAT-RD3
        g_rdGpu.tccLoc = GetShaderLocation(g_rdGpu.shader, "uTcc");         // FEAT-RD3
        if (g_rdGpu.samplerLoc >= 0) // FEAT-RD3
        {
            int samplerUnit = 0;
            SetShaderValue(g_rdGpu.shader, g_rdGpu.samplerLoc, &samplerUnit, SHADER_UNIFORM_INT);
        }
        const char *fe = std::getenv("PS2X_RD_FLIP");
        const int flipBits = (fe && *fe) ? std::atoi(fe) : 0;
        g_rdGpu.flip[0] = 1.0f;
        g_rdGpu.flip[1] = (flipBits & 1) ? 1.0f : -1.0f; // FEAT-RD2: y flip default; bit 1 = do NOT flip y
        g_rdGpu.flip[2] = (flipBits & 2) ? -1.0f : 1.0f;
        if (g_rdGpu.flipLoc >= 0)
            SetShaderValue(g_rdGpu.shader, g_rdGpu.flipLoc, g_rdGpu.flip, SHADER_UNIFORM_VEC3);
        g_rdGpu.vao = rlLoadVertexArray();
    }

    if (hasNew)
    {
        g_rdGpu.everUploaded = true;
        g_rdGpu.gpuVertCount = g_rdGpu.localScratch.size();
        if (g_rdGpu.gpuVertCount > 0)
        {
            if (g_rdGpu.gpuVertCount > g_rdGpu.vboCapVerts)
            {
                if (g_rdGpu.vbo != 0)
                    rlUnloadVertexBuffer(g_rdGpu.vbo);
                const size_t newCap = std::max<size_t>(
                    g_rdGpu.gpuVertCount, g_rdGpu.vboCapVerts ? g_rdGpu.vboCapVerts * 2 : 4096);
                g_rdGpu.vbo = rlLoadVertexBuffer(nullptr, static_cast<int>(newCap * sizeof(RdVert)), true);
                g_rdGpu.vboCapVerts = newCap;

                rlEnableVertexArray(g_rdGpu.vao);
                rlEnableVertexBuffer(g_rdGpu.vbo);
                rlSetVertexAttribute(0, 4, RL_FLOAT, false, sizeof(RdVert), 0);
                rlEnableVertexAttribute(0);
                rlSetVertexAttribute(1, 4, RL_UNSIGNED_BYTE, true, sizeof(RdVert), 16);
                rlEnableVertexAttribute(1);
                rlSetVertexAttribute(2, 2, RL_FLOAT, false, sizeof(RdVert), 20); // FEAT-RD3
                rlEnableVertexAttribute(2);                                      // FEAT-RD3
                rlDisableVertexBuffer();
                rlDisableVertexArray();
            }
            rlUpdateVertexBuffer(g_rdGpu.vbo, g_rdGpu.localScratch.data(),
                                 static_cast<int>(g_rdGpu.gpuVertCount * sizeof(RdVert)), 0);
        }
    }

    rlDrawRenderBatchActive();
    ps2xRdTexTickFrame(); // FEAT-RD3

    const int rw = GetRenderWidth();
    const int rh = GetRenderHeight();
    const int sw = GetScreenWidth();
    const int sh = GetScreenHeight();
    const float scaleX = (sw > 0) ? (static_cast<float>(rw) / static_cast<float>(sw)) : 1.0f;
    const float scaleY = (sh > 0) ? (static_cast<float>(rh) / static_cast<float>(sh)) : 1.0f;
    const int vx = static_cast<int>(x * scaleX);
    const int vy = static_cast<int>((static_cast<float>(sh) - y - h) * scaleY);
    const int vw = static_cast<int>(w * scaleX);
    const int vh = static_cast<int>(h * scaleY);

    rlViewport(vx, vy, vw, vh);
    rlEnableDepthTest();
    rlDisableBackfaceCulling();

    uint64_t unsupBlend = 0; // FEAT-RD2
    if (g_rdGpu.gpuVertCount > 0 && g_rdGpu.vao != 0 && g_rdGpu.shader.id != 0)
    {
        rlEnableShader(g_rdGpu.shader.id);
        rlEnableVertexArray(g_rdGpu.vao);
        int prevBlend = -1; // FEAT-RD2b
        for (const RdBatch &batch : g_rdGpu.localBatches) // FEAT-RD2
        {
            if (batch.count == 0u)
                continue;
            const uint64_t test = batch.test;
            const uint32_t ate = static_cast<uint32_t>(test & 1ull);
            const uint32_t atst = ate ? static_cast<uint32_t>((test >> 1) & 7ull) : 1u;
            if (atst == 0u)
                continue;
            const uint32_t zte = static_cast<uint32_t>((test >> 16) & 1ull);
            const uint32_t ztst = static_cast<uint32_t>((test >> 17) & 3ull);
            if (zte && ztst == 0u)
                continue;
            if (!zte || ztst == 1u)
                rlDisableDepthTest();
            else
                rlEnableDepthTest();
            if ((batch.zbuf >> 32) & 1ull)
                rlDisableDepthMask();
            else
                rlEnableDepthMask();

            int blendState = 0; // FEAT-RD2b: 0 off, 1 alpha, 2 additive
            if ((batch.prim >> 6) & 1ull)
            {
                const uint32_t a = static_cast<uint32_t>((batch.alpha >> 0) & 3ull);
                const uint32_t b = static_cast<uint32_t>((batch.alpha >> 2) & 3ull);
                const uint32_t c = static_cast<uint32_t>((batch.alpha >> 4) & 3ull);
                const uint32_t d = static_cast<uint32_t>((batch.alpha >> 6) & 3ull);
                if (a == 0u && b == 1u && c == 0u && d == 1u)
                    blendState = 1;
                else if (a == 0u && b == 2u && c == 0u && d == 1u)
                    blendState = 2;
                else
                    ++unsupBlend;
            }
            if (blendState != prevBlend) // FEAT-RD2b: blend calls flush raylib batch and unbind shader/VAO
            {
                if (blendState == 0)
                    rlDisableColorBlend();
                else
                {
                    rlEnableColorBlend();
                    rlSetBlendFactors(RL_SRC_ALPHA, blendState == 1 ? RL_ONE_MINUS_SRC_ALPHA : RL_ONE, RL_FUNC_ADD);
                    rlSetBlendMode(RL_BLEND_CUSTOM);
                }
                rlEnableShader(g_rdGpu.shader.id);
                rlEnableVertexArray(g_rdGpu.vao);
                prevBlend = blendState;
            }
            int atstVal = static_cast<int>(atst);
            int arefVal = static_cast<int>((test >> 4) & 0xFFull);
            if (g_rdGpu.atstLoc >= 0)
                rlSetUniform(g_rdGpu.atstLoc, &atstVal, RL_SHADER_UNIFORM_INT, 1);
            if (g_rdGpu.arefLoc >= 0)
                rlSetUniform(g_rdGpu.arefLoc, &arefVal, RL_SHADER_UNIFORM_INT, 1);

            int texVal = 0; // FEAT-RD3
            int tfxVal = 0;
            int tccVal = 0;
            if ((batch.prim >> 4) & 1ull)
            {
                const unsigned int texId = ps2xRdGetTextureForBatch(batch.kickSerial, batch.tex0, batch.texa); // FEAT-RD5
                if (texId != 0u)
                {
                    const uint32_t wms = static_cast<uint32_t>(batch.clamp & 3ull);
                    const uint32_t wmt = static_cast<uint32_t>((batch.clamp >> 2) & 3ull);
                    const int wrapS = (wms == 0u) ? RL_TEXTURE_WRAP_REPEAT : RL_TEXTURE_WRAP_CLAMP;
                    const int wrapT = (wmt == 0u) ? RL_TEXTURE_WRAP_REPEAT : RL_TEXTURE_WRAP_CLAMP;
                    const int magFilter = (((batch.tex1 >> 5) & 1ull) == 0ull) ? RL_TEXTURE_FILTER_NEAREST : RL_TEXTURE_FILTER_LINEAR;
                    const int minFilter = (((batch.tex1 >> 6) & 7ull) == 0ull) ? RL_TEXTURE_FILTER_NEAREST : RL_TEXTURE_FILTER_LINEAR;
                    rlTextureParameters(texId, RL_TEXTURE_WRAP_S, wrapS);
                    rlTextureParameters(texId, RL_TEXTURE_WRAP_T, wrapT);
                    rlTextureParameters(texId, RL_TEXTURE_MAG_FILTER, magFilter);
                    rlTextureParameters(texId, RL_TEXTURE_MIN_FILTER, minFilter);
                    rlActiveTextureSlot(0);
                    rlEnableTexture(texId);
                    texVal = 1;
                    tfxVal = static_cast<int>((batch.tex0 >> 35) & 3ull);
                    tccVal = static_cast<int>((batch.tex0 >> 34) & 1ull);
                }
            }
            if (g_rdGpu.texLoc >= 0)
                rlSetUniform(g_rdGpu.texLoc, &texVal, RL_SHADER_UNIFORM_INT, 1);
            if (g_rdGpu.tfxLoc >= 0)
                rlSetUniform(g_rdGpu.tfxLoc, &tfxVal, RL_SHADER_UNIFORM_INT, 1);
            if (g_rdGpu.tccLoc >= 0)
                rlSetUniform(g_rdGpu.tccLoc, &tccVal, RL_SHADER_UNIFORM_INT, 1);

            rlDrawVertexArray(static_cast<int>(batch.first), static_cast<int>(batch.count));
        }
        rlActiveTextureSlot(0); // FEAT-RD3
        rlDisableTexture();     // FEAT-RD3
        rlDisableVertexArray();
        rlDisableShader();
    }

    rlEnableDepthMask();
    rlEnableColorBlend(); // FEAT-RD2b
    rlSetBlendMode(RL_BLEND_ALPHA);
    rlDisableDepthTest();
    rlEnableBackfaceCulling();
    rlViewport(0, 0, rw, rh);

    ++g_rdGpu.frames;
    g_rdGpu.totalTris += static_cast<uint64_t>(g_rdGpu.gpuVertCount / 3u);
    g_rdGpu.totalSkippedDraws += static_cast<uint64_t>(g_rdGpu.lastSkippedDraws);
    g_rdGpu.totalMaskedDraws += static_cast<uint64_t>(g_rdGpu.lastMaskedDraws); // FEAT-RD2
    g_rdGpu.totalUnsupportedBlend += unsupBlend; // FEAT-RD2
    const auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration_cast<std::chrono::milliseconds>(now - g_rdGpu.lastLog).count() >= 5000)
    {
        g_rdGpu.lastLog = now;
        const double fr = g_rdGpu.frames ? static_cast<double>(g_rdGpu.frames) : 1.0;
        ps2xSafeLog("[rd] frames=%u tris/frame=%.0f skippedDraws/frame=%.1f maskedDraws/frame=%.1f unsupBlend/frame=%.1f\n",
                    g_rdGpu.frames,
                    static_cast<double>(g_rdGpu.totalTris) / fr,
                    static_cast<double>(g_rdGpu.totalSkippedDraws) / fr,
                    static_cast<double>(g_rdGpu.totalMaskedDraws) / fr,
                    static_cast<double>(g_rdGpu.totalUnsupportedBlend) / fr);
    }
    return true;
}
