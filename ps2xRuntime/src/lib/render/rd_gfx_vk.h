#pragma once
// RDG2: Vulkan implementation of rdgfx:: interface.

#include "rd_gfx.h"

namespace rdgfx_vk
{

bool active();
bool init();
void beginFrame();
void endFrame();

bool initShader();
bool isShaderReady();
void enableShader();
void disableShader();

bool hasUniform(rdgfx::Uniform u);
void setUniformInt(rdgfx::Uniform u, int v);
void setUniformFloat(rdgfx::Uniform u, float v);
void setUniformVec2(rdgfx::Uniform u, const float *v);
void setUniformVec3Immediate(rdgfx::Uniform u, const float *v);
void setUniformVec4(rdgfx::Uniform u, const float *v);

void flushBatch();
void getBackbufferSize(int &rw, int &rh);
void getWindowSize(int &w, int &h); // FIX-RDG3D
void setViewport(int x, int y, int w, int h);
void setScissorTest(bool on);
void setScissorRect(int x, int y, int w, int h);
void setColorMask(bool r, bool g, bool b, bool a);
void setDepthTest(bool on);
void setDepthMask(bool on);
void setDepthFunc(rdgfx::DepthFunc func);
void setBlendAlpha();
void setBlend(bool enable,
              rdgfx::BlendFactor srcRgb = rdgfx::BlendFactor::One,
              rdgfx::BlendFactor dstRgb = rdgfx::BlendFactor::Zero,
              rdgfx::BlendFactor srcAlpha = rdgfx::BlendFactor::One,
              rdgfx::BlendFactor dstAlpha = rdgfx::BlendFactor::Zero,
              rdgfx::BlendEq eqRgb = rdgfx::BlendEq::Add,
              rdgfx::BlendEq eqAlpha = rdgfx::BlendEq::Add);
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
void setVertexAttribute(uint32_t index, int compCount, rdgfx::VertexAttrFormat type, bool normalized, int strideBytes, int offsetBytes);
void enableVertexAttribute(uint32_t index);

void drawArrays(rdgfx::PrimitiveTopology topo, int first, int count);
void drawIndexed(rdgfx::PrimitiveTopology topo, uint32_t first, uint32_t count, bool idx32);

uint32_t createTextureRGBA8(const void *data, int w, int h);
void freeTexture(uint32_t texId);
void generateMipmaps(uint32_t texId, int w, int h);
void setTextureWrap(uint32_t texId, bool isT, rdgfx::TextureWrap wrap);
void setTextureFilter(uint32_t texId, bool isMin, rdgfx::TextureFilter filter);
void bindTexture(uint32_t unit, uint32_t texId);

bool isPaletteTextureSupported();
bool uploadPaletteTexture(const uint32_t *rgba256);
void unbindPaletteTexture();

void drawTextureOverlay(uint32_t texId, int texW, int texH,
                        float srcX, float srcY, float srcW, float srcH,
                        float dstX, float dstY, float dstW, float dstH,
                        uint8_t alpha);
void drawFxBackground(uint32_t texId, int texW, int texH); // FXC1
void drawClassicAdditive(const uint8_t *rgba, int w, int h, int strideBytes); // FXC1

bool canCopyBackbuffer();
bool beginBackbufferCopy(rdgfx::BackbufferCopyState &state);
bool copyBackbufferToTexture(rdgfx::BackbufferCopyState &state,
                             uint32_t &texId, uint32_t &auxId,
                             int &texW, int &texH,
                             int wantW, int wantH,
                             int vx, int vy, int vw, int vh,
                             int dstW, int dstH,
                             bool linearFilter);
void endBackbufferCopy(const rdgfx::BackbufferCopyState &state);

uint32_t getLastError();

} // namespace rdgfx_vk
