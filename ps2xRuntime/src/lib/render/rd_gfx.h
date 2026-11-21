#pragma once
// RDG1: Graphics API abstraction under rd_view.cpp and rd_texture.cpp.

#include <cstdint>

namespace rdgfx
{

enum class BlendFactor : uint8_t
{
    Zero = 0,
    One,
    SrcAlpha,
    OneMinusSrcAlpha,
    DstAlpha,
    OneMinusDstAlpha
};

enum class BlendEq : uint8_t
{
    Add = 0,
    Subtract,
    ReverseSubtract
};

enum class DepthFunc : uint8_t
{
    Lequal = 0,
    Always
};

enum class TextureFilter : uint8_t
{
    Nearest = 0,
    Linear,
    MipLinear
};

enum class TextureWrap : uint8_t
{
    Repeat = 0,
    Clamp
};

enum class VertexAttrFormat : uint8_t
{
    Float = 0,
    UnsignedByte
};

enum class PrimitiveTopology : uint8_t
{
    Triangles = 0
};

enum class Uniform : uint8_t
{
    Flip = 0,
    Mode2d,
    FbSize,
    VpZ,
    VpA,
    VpB,
    ZBias,
    Atst,
    Aref,
    AtInvert,
    FixAlpha,
    Sampler,
    PalTex,
    PalMode,
    PalFallback,
    Tex,
    Tfx,
    Tcc,
    Count
};

struct BackbufferCopyState
{
    int prevReadFbo = 0;
    int prevDrawFbo = 0;
    int sampleBufs = 0;
    bool msaaResolved = false;
    bool valid = false;
};

bool initShader();
bool isShaderReady();
void enableShader();
void disableShader();

bool hasUniform(Uniform u);
void setUniformInt(Uniform u, int v);
void setUniformFloat(Uniform u, float v);
void setUniformVec2(Uniform u, const float *v);
void setUniformVec3Immediate(Uniform u, const float *v);
void setUniformVec4(Uniform u, const float *v);

void flushBatch();
void getBackbufferSize(int &rw, int &rh);
void getWindowSize(int &w, int &h); // FIX-RDG3D
void setViewport(int x, int y, int w, int h);
void setScissorTest(bool on);
void setScissorRect(int x, int y, int w, int h);
void setColorMask(bool r, bool g, bool b, bool a);
void setDepthTest(bool on);
void setDepthMask(bool on);
void setDepthFunc(DepthFunc func);
void setBlendAlpha();
void setBlend(bool enable,
              BlendFactor srcRgb = BlendFactor::One,
              BlendFactor dstRgb = BlendFactor::Zero,
              BlendFactor srcAlpha = BlendFactor::One,
              BlendFactor dstAlpha = BlendFactor::Zero,
              BlendEq eqRgb = BlendEq::Add,
              BlendEq eqAlpha = BlendEq::Add);
void setBackfaceCulling(bool on);
void clearScreenBuffers(float r, float g, float b, float a);

uint32_t createVertexArray();
void bindVertexArray(uint32_t vao);
uint32_t createVertexBuffer(const void *data, int sizeBytes, bool dynamic);
void updateVertexBuffer(uint32_t vbo, const void *data, int sizeBytes, int offsetBytes);
void freeVertexBuffer(uint32_t vbo);
uint32_t createIndexBuffer(const void *data, int sizeBytes, bool dynamic);
void updateIndexBuffer(uint32_t ebo, const void *data, int sizeBytes, int offsetBytes);
void freeIndexBuffer(uint32_t ebo);
void bindVertexBuffer(uint32_t vbo);
void bindIndexBuffer(uint32_t ebo);
void setVertexAttribute(uint32_t index, int compCount, VertexAttrFormat type, bool normalized, int strideBytes, int offsetBytes);
void enableVertexAttribute(uint32_t index);

void drawArrays(PrimitiveTopology topo, int first, int count);
void drawIndexed(PrimitiveTopology topo, uint32_t first, uint32_t count, bool idx32);

uint32_t createTextureRGBA8(const void *data, int w, int h);
void freeTexture(uint32_t texId);
void generateMipmaps(uint32_t texId, int w, int h);
void setTextureWrap(uint32_t texId, bool isT, TextureWrap wrap);
void setTextureFilter(uint32_t texId, bool isMin, TextureFilter filter);
void bindTexture(uint32_t unit, uint32_t texId);

bool isPaletteTextureSupported();
bool uploadPaletteTexture(const uint32_t *rgba256);
void unbindPaletteTexture();

void drawTextureOverlay(uint32_t texId, int texW, int texH,
                        float srcX, float srcY, float srcW, float srcH,
                        float dstX, float dstY, float dstW, float dstH,
                        uint8_t alpha);

bool canCopyBackbuffer();
bool beginBackbufferCopy(BackbufferCopyState &state);
bool copyBackbufferToTexture(BackbufferCopyState &state,
                             uint32_t &texId, uint32_t &auxId,
                             int &texW, int &texH,
                             int wantW, int wantH,
                             int vx, int vy, int vw, int vh,
                             int dstW, int dstH,
                             bool linearFilter);
void endBackbufferCopy(const BackbufferCopyState &state);

uint32_t getLastError();

} // namespace rdgfx
