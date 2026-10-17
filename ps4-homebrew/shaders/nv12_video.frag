// SPDX-License-Identifier: GPL-3.0-or-later
#version 450
#extension GL_GOOGLE_include_directive : require
layout(set = 0, binding = 0) uniform sampler2D yPlane;
layout(set = 0, binding = 1) uniform sampler2D uvPlane;
layout(set = 0, binding = 2) uniform sampler2D uiOverlay;
layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 outColor;
layout(push_constant) uniform VideoParams {
    vec4 rect;
    ivec2 sourceSize;
    int videoEnabled;
    int gpuUi;
    ivec2 targetSize;
    int upscaler;
    int padding;
} params;
vec4 samplePlane(sampler2D plane, vec2 uv, ivec2 size) {
    vec2 p = uv * vec2(size) - vec2(0.5);
    ivec2 lo = ivec2(floor(p));
    vec2 f = fract(p);
    ivec2 hi = size - 1;
    vec4 a = texelFetch(plane, clamp(lo, ivec2(0), hi), 0);
    vec4 b = texelFetch(plane, clamp(lo + ivec2(1, 0), ivec2(0), hi), 0);
    vec4 c = texelFetch(plane, clamp(lo + ivec2(0, 1), ivec2(0), hi), 0);
    vec4 d = texelFetch(plane, clamp(lo + ivec2(1, 1), ivec2(0), hi), 0);
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}
vec3 convertYuv(float yCode, vec2 chroma) {
    uint colorFlags = (uint(params.padding) >> 8) & 7u;
    bool fullRange = (colorFlags & 4u) != 0u;
    uint matrix = colorFlags & 3u;
    float y = fullRange ? yCode : yCode - 16.0;
    float luma = fullRange ? 1.0 : 1.164383;
    float rV, gU, gV, bU;
    if (matrix == 1u) {
        rV = fullRange ? 1.402000 : 1.596027;
        gU = fullRange ? -0.344136 : -0.391762;
        gV = fullRange ? -0.714136 : -0.812968;
        bU = fullRange ? 1.772000 : 2.017232;
    } else if (matrix == 2u) {
        rV = fullRange ? 1.474600 : 1.678674;
        gU = fullRange ? -0.164553 : -0.187326;
        gV = fullRange ? -0.571353 : -0.650424;
        bU = fullRange ? 1.881400 : 2.141772;
    } else {
        // BT.709 is the default; matrix code 0 also falls back here.
        rV = fullRange ? 1.574800 : 1.792741;
        gU = fullRange ? -0.187324 : -0.213249;
        gV = fullRange ? -0.468124 : -0.532909;
        bU = fullRange ? 1.855600 : 2.112402;
    }
    return clamp(vec3(luma*y + rV*chroma.y,
                      luma*y + gU*chroma.x + gV*chroma.y,
                      luma*y + bU*chroma.x) / 255.0, 0.0, 1.0);
}

// FSR1 operates on perceptual RGB, reconstructed from the received NV12.
vec3 sourceRgb(ivec2 p) {
    p = clamp(p, ivec2(0), params.sourceSize-1);
    float y = texelFetch(yPlane,p,0).r * 255.0;
    vec2 uv = (vec2(p)+0.5)/vec2(params.sourceSize);
    vec2 chroma = samplePlane(uvPlane,uv,params.sourceSize/2).rg*255.0-vec2(128.0);
    return convertYuv(y, chroma);
}
#define A_GPU 1
#define A_GLSL 1
#include "ffx_a.h"
#define FSR_EASU_F 1
vec4 gatherRgb(vec2 p, int channel) {
    ivec2 lo=ivec2(floor(p*vec2(params.sourceSize)-0.5));
    return vec4(sourceRgb(lo+ivec2(0,1))[channel],
        sourceRgb(lo+ivec2(1,1))[channel],
        sourceRgb(lo+ivec2(1,0))[channel],sourceRgb(lo)[channel]);
}
AF4 FsrEasuRF(AF2 p) { return gatherRgb(p,0); }
AF4 FsrEasuGF(AF2 p) { return gatherRgb(p,1); }
AF4 FsrEasuBF(AF2 p) { return gatherRgb(p,2); }
#include "ffx_fsr1.h"
vec3 sampleVideo(vec2 uv) {
    vec2 target=vec2(params.targetSize)*(params.rect.zw-params.rect.xy);
    bool forceEasu = (uint(params.padding) & 16u) != 0u;
    bool upscale = all(greaterThanEqual(target,vec2(params.sourceSize))) &&
        any(greaterThan(target,vec2(params.sourceSize)));
    if (params.upscaler != 0 && (upscale || forceEasu)) {
        uvec4 c0,c1,c2,c3;
        FsrEasuCon(c0,c1,c2,c3,float(params.sourceSize.x),float(params.sourceSize.y),
            float(params.sourceSize.x),float(params.sourceSize.y),target.x,target.y);
        vec3 value;
        FsrEasuF(value,uvec2(clamp(floor(uv*target),vec2(0),target-1)),c0,c1,c2,c3);
        return clamp(value,0.0,1.0);
    }
    float y=samplePlane(yPlane,uv,params.sourceSize).r*255.0;
    vec2 chroma=samplePlane(uvPlane,uv,params.sourceSize/2).rg*255.0-vec2(128.0);
    return convertYuv(y, chroma);
}
void main() {
    vec3 base = vec3(0.0);
    if (params.videoEnabled != 0 &&
        vUv.x >= params.rect.x && vUv.y >= params.rect.y &&
        vUv.x < params.rect.z && vUv.y < params.rect.w) {
        vec2 uv = (vUv - params.rect.xy) / (params.rect.zw - params.rect.xy);
        base = sampleVideo(uv);
    }
    vec4 ui = params.gpuUi != 0 ? vec4(0.0) : texture(uiOverlay, vUv);
    outColor = vec4(mix(base, ui.rgb, ui.a), 1.0);
}
