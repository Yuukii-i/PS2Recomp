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

struct RdVert // FEAT-RD1
{
    float pos[4];
    uint8_t rgba[4];
};
static_assert(sizeof(RdVert) == 20, "RdVert size");

namespace
{
std::mutex g_rdMutex;
std::vector<RdVert> g_rdPending;
uint32_t g_rdPendingSkipped = 0;
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
    Shader shader{};
    int flipLoc = -1;
    float flip[3] = {1.0f, 1.0f, 1.0f};
    std::vector<RdVert> localScratch;
    uint32_t frames = 0;
    uint64_t totalTris = 0;
    uint64_t totalSkippedDraws = 0;
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

void ps2xRdPublish(const RdVert *v, size_t n, uint32_t skippedDraws) // FEAT-RD1
{
    static thread_local std::vector<RdVert> t_staging;
    if (v && n > 0)
        t_staging.assign(v, v + n);
    else
        t_staging.clear();

    std::lock_guard<std::mutex> lock(g_rdMutex);
    g_rdPending.swap(t_staging);
    g_rdPendingSkipped = skippedDraws;
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
            g_rdGpu.lastSkippedDraws = g_rdPendingSkipped;
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
            "uniform vec3 uFlip;\n"
            "out vec4 vCol;\n"
            "void main() {\n"
            "    gl_Position = vec4(aPos.x * uFlip.x, aPos.y * uFlip.y, aPos.z * uFlip.z, aPos.w);\n"
            "    vCol = vec4(aCol.rgb, 1.0);\n"
            "}\n";
        static const char *kFs =
            "#version 330\n"
            "in vec4 vCol;\n"
            "out vec4 finalColor;\n"
            "void main() {\n"
            "    finalColor = vCol;\n"
            "}\n";
        g_rdGpu.shader = LoadShaderFromMemory(kVs, kFs);
        g_rdGpu.flipLoc = GetShaderLocation(g_rdGpu.shader, "uFlip");
        const char *fe = std::getenv("PS2X_RD_FLIP");
        const int flipBits = (fe && *fe) ? std::atoi(fe) : 0;
        g_rdGpu.flip[0] = 1.0f;
        g_rdGpu.flip[1] = (flipBits & 1) ? -1.0f : 1.0f;
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
                rlDisableVertexBuffer();
                rlDisableVertexArray();
            }
            rlUpdateVertexBuffer(g_rdGpu.vbo, g_rdGpu.localScratch.data(),
                                 static_cast<int>(g_rdGpu.gpuVertCount * sizeof(RdVert)), 0);
        }
    }

    rlDrawRenderBatchActive();

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

    if (g_rdGpu.gpuVertCount > 0 && g_rdGpu.vao != 0 && g_rdGpu.shader.id != 0)
    {
        rlEnableShader(g_rdGpu.shader.id);
        rlEnableVertexArray(g_rdGpu.vao);
        rlDrawVertexArray(0, static_cast<int>(g_rdGpu.gpuVertCount));
        rlDisableVertexArray();
        rlDisableShader();
    }

    rlDisableDepthTest();
    rlEnableBackfaceCulling();
    rlViewport(0, 0, rw, rh);

    ++g_rdGpu.frames;
    g_rdGpu.totalTris += static_cast<uint64_t>(g_rdGpu.gpuVertCount / 3u);
    g_rdGpu.totalSkippedDraws += static_cast<uint64_t>(g_rdGpu.lastSkippedDraws);
    const auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration_cast<std::chrono::milliseconds>(now - g_rdGpu.lastLog).count() >= 5000)
    {
        g_rdGpu.lastLog = now;
        const double fr = g_rdGpu.frames ? static_cast<double>(g_rdGpu.frames) : 1.0;
        ps2xSafeLog("[rd] frames=%u tris/frame=%.0f skippedDraws/frame=%.1f\n",
                    g_rdGpu.frames,
                    static_cast<double>(g_rdGpu.totalTris) / fr,
                    static_cast<double>(g_rdGpu.totalSkippedDraws) / fr);
    }
    return true;
}
