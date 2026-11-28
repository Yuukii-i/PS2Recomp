#version 450

// FEAT-ENH2: debug views of the world position rebuilt from depth.
// Built twice: plain, and with ENH_MSAA for a multisampled depth image (sample 0).
layout(set = 0, binding = 0) uniform sampler2D uColor;
#ifdef ENH_MSAA
layout(set = 0, binding = 1) uniform sampler2DMS uDepth;
#else
layout(set = 0, binding = 1) uniform sampler2D uDepth;
#endif

layout(push_constant) uniform PC {
    mat4 uWorldFromNdc;
    vec4 uCamRowW;  // row 3 of the camera: view distance of a world point
    vec4 uViewport; // x, y, width, height of the 3D viewport
    ivec4 uMode;    // x: 1 depth, 2 grid
} pc;

layout(location = 0) out vec4 oColor;

void main()
{
    ivec2 pix = ivec2(gl_FragCoord.xy);
    vec4 color = texelFetch(uColor, pix, 0);
    float depth = texelFetch(uDepth, pix, 0).r;
    vec2 ndc = (gl_FragCoord.xy - pc.uViewport.xy) / pc.uViewport.zw * 2.0 - 1.0;
    vec4 h = pc.uWorldFromNdc * vec4(ndc, depth, 1.0);
    vec3 world = h.xyz / h.w;

    if (pc.uMode.x == 1) {
        float viewDistance = dot(pc.uCamRowW, vec4(world, 1.0));
        oColor = vec4(vec3(fract(viewDistance / 500.0)), color.a);
        return;
    }

    vec3 coord = world / 50.0;
    vec3 dist = abs(coord - floor(coord + 0.5));
    vec3 width = fwidth(coord);
    vec3 rgb = color.rgb;
    if (dist.x < width.x * 0.75) rgb = vec3(1.0, 0.0, 0.0);
    if (dist.y < width.y * 0.75) rgb = vec3(0.0, 1.0, 0.0);
    if (dist.z < width.z * 0.75) rgb = vec3(0.0, 0.0, 1.0);
    oColor = vec4(rgb, color.a);
}
