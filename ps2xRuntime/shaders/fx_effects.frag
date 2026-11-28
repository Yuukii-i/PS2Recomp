#version 450

// FEAT-ENH4: screen-space effects on the finished world picture.
// Built twice: plain, and with ENH_MSAA for a multisampled depth image (sample 0).
// World "up" is -Y (the game's camera up vector).
layout(set = 0, binding = 0) uniform sampler2D uColor;
#ifdef ENH_MSAA
layout(set = 0, binding = 1) uniform sampler2DMS uDepth;
#else
layout(set = 0, binding = 1) uniform sampler2D uDepth;
#endif
layout(set = 0, binding = 2) uniform sampler2D uLevel1; // 1/2
layout(set = 0, binding = 3) uniform sampler2D uLevel2; // 1/4
layout(set = 0, binding = 4) uniform sampler2D uLevel3; // 1/8
layout(set = 0, binding = 5) uniform sampler2D uLevel4; // 1/16

layout(std140, set = 0, binding = 6) uniform Settings {
    mat4 uWorldFromNdc;
    mat4 uClipFromWorld;
    vec4 uCamRowW;  // row 3 of the camera: view distance of a world point
    vec4 uCamPos;   // xyz camera position, w: NDC x units per world unit at view distance 1
    vec4 uViewport; // x, y, width, height of the 3D viewport
    vec4 uTarget;   // width, height, 1/width, 1/height
    vec4 uSunDir;   // xyz towards the sun; w: 1 if usable
    vec4 uSunColor;
    vec4 uAo;       // strength, radius, bias
    vec4 uShadow;   // strength, length, softness
    vec4 uFogColor; // rgb, strength
    vec4 uFogRange; // start, end, height, falloff
    vec4 uShafts;   // fog sky, strength, length, decay
    vec4 uDof;      // strength, focus, range, blur
    vec4 uBloom;    // strength, threshold, radius
    vec4 uGrade;    // strength, exposure, contrast, saturation
    vec4 uTint;
};

layout(location = 0) out vec4 oColor;

const float kSkyDepth = 0.9999999;  // cleared depth
const float kSkyDistance = 50000.0; // anything farther counts as sky
const int kAoSamples = 12;
const int kShadowSteps = 24;
const int kShaftSteps = 32;

struct Surface {
    vec3 world;
    float dist; // view distance
    bool sky;
};

vec2 gRectMin; // 3D viewport clipped to the target, in pixels
vec2 gRectMax;

bool inRect(vec2 p)
{
    return all(greaterThanEqual(p, gRectMin)) && all(lessThan(p, gRectMax));
}

Surface surfaceAt(vec2 p) // p: pixel centre
{
    float depth = texelFetch(uDepth, ivec2(p), 0).r;
    vec2 ndc = (p - uViewport.xy) / uViewport.zw * 2.0 - 1.0;
    vec4 h = uWorldFromNdc * vec4(ndc, depth, 1.0);

    Surface s;
    s.world = vec3(0.0);
    s.dist = kSkyDistance;
    s.sky = true;
    if (depth >= kSkyDepth || abs(h.w) < 1e-20)
        return s;
    vec3 world = h.xyz / h.w;
    float dist = dot(uCamRowW, vec4(world, 1.0));
    if (!(dist > 0.0 && dist < kSkyDistance))
        return s;
    s.world = world;
    s.dist = dist;
    s.sky = false;
    return s;
}

// How far the view distance moves for a few steps of the stored depth: the noise level here.
float depthError(Surface s, vec2 p)
{
    float depth = texelFetch(uDepth, ivec2(p), 0).r;
    vec2 ndc = (p - uViewport.xy) / uViewport.zw * 2.0 - 1.0;
    vec4 h = uWorldFromNdc * vec4(ndc, depth - 2.4e-7, 1.0);
    if (abs(h.w) < 1e-20)
        return kSkyDistance;
    return abs(dot(uCamRowW, vec4(h.xyz / h.w, 1.0)) - s.dist);
}

// Difference to the neighbour on the side that is closer in depth (stays on one surface).
vec3 edgeDelta(Surface centre, vec2 p, vec2 axis)
{
    vec3 best = vec3(0.0);
    float bestGap = 1e30;
    for (int side = 0; side < 2; ++side)
    {
        float sgn = side == 0 ? 1.0 : -1.0;
        vec2 q = p + axis * sgn;
        if (!inRect(q))
            continue;
        Surface n = surfaceAt(q);
        float gap = abs(n.dist - centre.dist);
        if (!n.sky && gap < bestGap)
        {
            bestGap = gap;
            best = (n.world - centre.world) * sgn;
        }
    }
    return best;
}

vec3 normalAt(Surface s, vec2 p)
{
    vec3 toCam = uCamPos.xyz - s.world;
    // Two pixels apart: less sensitive to depth steps than direct neighbours.
    vec3 n = cross(edgeDelta(s, p, vec2(2.0, 0.0)), edgeDelta(s, p, vec2(0.0, 2.0)));
    float len = length(n);
    if (!(len > 1e-12))
        return normalize(toCam);
    n /= len;
    return dot(n, toCam) < 0.0 ? -n : n;
}

float noiseAt(vec2 p) // fixed per pixel, so the pattern does not flicker
{
    return fract(52.9829189 * fract(dot(floor(p), vec2(0.06711056, 0.00583715))));
}

float ambientOcclusion(Surface s, vec3 n, vec2 p, float error)
{
    float radius = uAo.y;
    if (!(radius > 0.0))
        return 1.0;
    float radiusPx = radius * uCamPos.w * uViewport.z * 0.5 / s.dist;
    radiusPx = min(radiusPx, uViewport.w * 0.15);
    // Fade out where the radius covers few pixels or the depth is too coarse for it.
    float trust = smoothstep(3.0, 10.0, radiusPx) * (1.0 - smoothstep(radius * 0.1, radius * 0.4, error));
    if (trust <= 0.0)
        return 1.0;

    // Anything this close to the tangent plane is the surface itself.
    float planeBias = max(radius * 0.08, s.dist * 0.003) + error * 2.0;

    // The same spiral for every pixel: a per-pixel rotation shows up as grain without a blur pass.
    float occluded = 0.0;
    for (int i = 0; i < kAoSamples; ++i)
    {
        float a = float(i) * 2.3999632; // golden angle spiral
        float r = mix(2.0, radiusPx, (float(i) + 0.5) / float(kAoSamples));
        vec2 q = floor(p + vec2(cos(a), sin(a)) * r) + 0.5;
        if (!inRect(q))
            continue;
        Surface o = surfaceAt(q);
        if (o.sky)
            continue;
        vec3 v = o.world - s.world;
        float d = length(v);
        float height = dot(n, v) - planeBias;
        if (height <= 0.0 || d < 1e-4)
            continue;
        float falloff = clamp(1.0 - (d * d) / (radius * radius), 0.0, 1.0);
        occluded += max(height / d - uAo.z, 0.0) * falloff;
    }
    float amount = clamp(occluded / float(kAoSamples) * 3.0, 0.0, 1.0);
    return clamp(1.0 - uAo.x * amount * trust, 0.0, 1.0);
}

float sunShadow(Surface s, vec3 n, float error)
{
    float rayLength = uShadow.y;
    if (!(rayLength > 0.0) || uSunDir.w < 0.5)
        return 1.0;
    // Surfaces turned away from the sun are already dark in the game's own lighting;
    // grazing ones would shadow themselves.
    float facing = smoothstep(0.05, 0.35, dot(n, uSunDir.xyz));
    float trust = 1.0 - smoothstep(rayLength * 0.03, rayLength * 0.15, error);
    if (facing <= 0.0 || trust <= 0.0)
        return 1.0;

    vec3 origin = s.world + n * (max(rayLength * 0.03, s.dist * 0.006) + error * 2.0);
    float hit = 0.0;
    for (int i = 0; i < kShadowSteps; ++i)
    {
        float f = (float(i) + 1.0) / float(kShadowSteps);
        vec3 along = origin + uSunDir.xyz * (f * rayLength);
        vec4 clip = uClipFromWorld * vec4(along, 1.0);
        if (clip.w <= 0.0)
            break;
        vec2 q = floor((clip.xy / clip.w * 0.5 + 0.5) * uViewport.zw + uViewport.xy) + 0.5;
        if (!inRect(q))
            break;
        Surface blocker = surfaceAt(q);
        if (blocker.sky)
            continue;
        float rayDist = dot(uCamRowW, vec4(along, 1.0));
        float behind = rayDist - blocker.dist;
        float tolerance = max(rayLength * 0.02, rayDist * 0.004) + error * 2.0;
        // The thickness limit stops things far in front of the ray from casting.
        float thickness = max(rayLength * 0.2, rayDist * 0.02) + tolerance * 2.0;
        // Soft edges of the window instead of a yes/no test.
        float inside = smoothstep(tolerance, tolerance * 2.0, behind) *
                       (1.0 - smoothstep(thickness * 0.7, thickness, behind));
        float fade = 1.0 - clamp(uShadow.z, 0.0, 1.0) * f;
        hit = max(hit, inside * fade);
    }
    return clamp(1.0 - uShadow.x * hit * facing * trust, 0.0, 1.0);
}

float fogAmount(Surface s)
{
    if (s.sky)
        return clamp(uFogColor.a * uShafts.x, 0.0, 1.0);
    float byDistance = clamp((s.dist - uFogRange.x) / max(uFogRange.y - uFogRange.x, 1.0), 0.0, 1.0);
    // Full density up to altitude uFogRange.z, thinning above it. Falloff 0: no height term.
    float above = max(-s.world.y - uFogRange.z, 0.0);
    float byHeight = exp(-above * max(uFogRange.w, 0.0) * 0.001);
    return clamp(uFogColor.a * byDistance * byHeight, 0.0, 1.0);
}

float luminance(vec3 c)
{
    return dot(c, vec3(0.2126, 0.7152, 0.0722));
}

vec2 clampUv(vec2 uv)
{
    return clamp(uv, (gRectMin + 0.5) * uTarget.zw, (gRectMax - 0.5) * uTarget.zw);
}

vec3 lightShafts(vec2 p)
{
    if (uSunDir.w < 0.5)
        return vec3(0.0);
    vec4 clip = uClipFromWorld * vec4(uSunDir.xyz, 0.0); // the sun is at infinity
    if (!(clip.w > 1e-6))
        return vec3(0.0);
    vec2 sunNdc = clip.xy / clip.w;
    float visible = 1.0 - smoothstep(1.0, 2.5, length(sunNdc));
    if (visible <= 0.0)
        return vec3(0.0);

    vec2 sunPix = (sunNdc * 0.5 + 0.5) * uViewport.zw + uViewport.xy;
    vec2 stepPix = (sunPix - p) * clamp(uShafts.z, 0.0, 1.0) / float(kShaftSteps);
    vec2 q = p + stepPix * noiseAt(p);
    float weight = 1.0;
    vec3 sum = vec3(0.0);
    for (int i = 0; i < kShaftSteps; ++i)
    {
        q += stepPix;
        if (inRect(q))
        {
            vec3 c = textureLod(uLevel1, q * uTarget.zw, 0.0).rgb;
            sum += c * clamp((luminance(c) - 0.6) / 0.4, 0.0, 1.0) * weight;
        }
        weight *= clamp(uShafts.w, 0.0, 1.0);
    }
    return sum / float(kShaftSteps) * uSunColor.rgb * (uShafts.y * 2.0 * visible);
}

vec3 levelColor(int level, vec2 uv)
{
    if (level == 1) return textureLod(uLevel1, uv, 0.0).rgb;
    if (level == 2) return textureLod(uLevel2, uv, 0.0).rgb;
    if (level == 3) return textureLod(uLevel3, uv, 0.0).rgb;
    return textureLod(uLevel4, uv, 0.0).rgb;
}

// How much the depth of field changes this pixel's colour.
vec3 depthOfFieldDelta(Surface s, vec2 p, vec3 base)
{
    float focus = uDof.y;
    if (focus <= 0.0)
    {
        Surface centre = surfaceAt(floor(uViewport.xy + uViewport.zw * 0.5) + 0.5);
        if (centre.sky)
            return vec3(0.0);
        focus = centre.dist;
    }
    float range = max(uDof.z, 1.0);
    float coc = clamp((abs(s.dist - focus) - range) / range, 0.0, 1.0);
    float lod = coc * clamp(uDof.w, 0.0, 4.0); // 0 sharp, 1..4 the chain
    if (lod <= 0.0)
        return vec3(0.0);

    vec2 uv = clampUv(p * uTarget.zw);
    int lower = int(floor(lod));
    int upper = min(lower + 1, 4);
    vec3 a = lower == 0 ? base : levelColor(lower, uv);
    vec3 blurred = mix(a, levelColor(upper, uv), lod - float(lower));
    return (blurred - base) * clamp(coc * uDof.x * 2.0, 0.0, 1.0);
}

vec3 bloomColor(vec2 p)
{
    vec2 uv = p * uTarget.zw;
    float threshold = uBloom.y;
    vec3 sum = vec3(0.0);
    for (int level = 1; level <= 4; ++level)
    {
        // Four taps around the pixel; radius is in half texels of the level.
        vec2 offset = uTarget.zw * float(1 << level) * 0.5 * max(uBloom.z, 0.0);
        vec3 c = 0.25 * (levelColor(level, clampUv(uv + offset * vec2(-1.0, -1.0))) +
                         levelColor(level, clampUv(uv + offset * vec2(1.0, -1.0))) +
                         levelColor(level, clampUv(uv + offset * vec2(-1.0, 1.0))) +
                         levelColor(level, clampUv(uv + offset * vec2(1.0, 1.0))));
        sum += c * clamp((luminance(c) - threshold) / max(1.0 - threshold, 0.05), 0.0, 1.0);
    }
    return sum * (uBloom.x * 0.5);
}

vec3 grading(vec3 color)
{
    vec3 g = color * uGrade.y;
    g = (g - 0.5) * uGrade.z + 0.5;
    g = mix(vec3(luminance(g)), g, uGrade.w) * uTint.rgb;
    return mix(color, g, clamp(uGrade.x, 0.0, 1.0));
}

void main()
{
    vec2 p = gl_FragCoord.xy;
    vec4 source = texelFetch(uColor, ivec2(p), 0);

    gRectMin = max(uViewport.xy, vec2(0.0));
    gRectMax = min(uViewport.xy + uViewport.zw, uTarget.xy);
    if (!inRect(p))
    {
        oColor = source;
        return;
    }

    Surface s = surfaceAt(p);
    vec3 color = source.rgb;
    float shade = 1.0;

    if (!s.sky && (uAo.x > 0.0 || uShadow.x > 0.0))
    {
        vec3 n = normalAt(s, p);
        float error = depthError(s, p);
        if (uAo.x > 0.0)
            shade *= ambientOcclusion(s, n, p, error);
        if (uShadow.x > 0.0)
            shade *= sunShadow(s, n, error);
        color *= shade;
    }

    float fog = uFogColor.a > 0.0 ? fogAmount(s) : 0.0;
    color = mix(color, uFogColor.rgb, fog);

    if (uShafts.y > 0.0)
        color += lightShafts(p);

    // Only the picture's detail is blurred; the darkening and fog above stay.
    if (!s.sky && uDof.x > 0.0)
        color += depthOfFieldDelta(s, p, source.rgb) * shade * (1.0 - fog);

    if (uBloom.x > 0.0)
        color += bloomColor(p);

    if (uGrade.x > 0.0)
        color = grading(color);

    oColor = vec4(clamp(color, 0.0, 1.0), source.a);
}
