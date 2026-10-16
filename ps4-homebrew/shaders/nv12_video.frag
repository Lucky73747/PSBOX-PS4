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
// FSR1 operates on perceptual RGB, reconstructed from the received NV12.
vec3 sourceRgb(ivec2 p) {
    p = clamp(p, ivec2(0), params.sourceSize-1);
    vec2 uv = (vec2(p)+0.5)/vec2(params.sourceSize);
    vec2 chroma = samplePlane(uvPlane,uv,params.sourceSize/2).rg*255.0-vec2(128.0);
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
        uvec4 c0,c1,c2,c3;
        FsrEasuCon(c0,c1,c2,c3,float(params.sourceSize.x),float(params.sourceSize.y),
            float(params.sourceSize.x),float(params.sourceSize.y),target.x,target.y);
        vec3 value;
        FsrEasuF(value,uvec2(clamp(floor(uv*target),vec2(0),target-1)),c0,c1,c2,c3);
        return clamp(value,0.0,1.0);
    }
    vec2 chroma=samplePlane(uvPlane,uv,params.sourceSize/2).rg*255.0-vec2(128.0);
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
