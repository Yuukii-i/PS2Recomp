#version 450
// PERF-VK V2: one instance per GS primitive; draws the primitive's clipped pixel bounding box.
// Prim layout (uints per primitive) is shared with gs_draw.frag and ps2_vk_backend.cpp (kPrimStride).
layout(std430, binding = 3) readonly buffer Prims { uint pd[]; };

layout(location = 0) flat out uint vPrim;

const uint STRIDE = 80u;
const uint BBOX = 62u;

void main()
{
    const uint prim = uint(gl_InstanceIndex);
    const uint base = prim * STRIDE + BBOX;
    const float minX = float(int(pd[base + 0u]));
    const float maxX = float(int(pd[base + 1u])) + 1.0;
    const float minY = float(int(pd[base + 2u]));
    const float maxY = float(int(pd[base + 3u])) + 1.0;
    const vec2 corner[4] = vec2[4](vec2(minX, minY), vec2(maxX, minY), vec2(minX, maxY), vec2(maxX, maxY));
    const vec2 p = corner[gl_VertexIndex];
    // viewport is 2048x2048 pixels, origin top-left, so NDC = p / 1024 - 1
    gl_Position = vec4(p.x / 1024.0 - 1.0, p.y / 1024.0 - 1.0, 0.0, 1.0);
    vPrim = prim;
}
