#version 450

// FEAT-ENH1: pass-through composite, writes the colour copy unchanged
layout(set = 0, binding = 0) uniform sampler2D uColor;

layout(location = 0) out vec4 oColor;

void main()
{
    oColor = texelFetch(uColor, ivec2(gl_FragCoord.xy), 0);
}
