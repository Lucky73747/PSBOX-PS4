// SPDX-License-Identifier: GPL-3.0-or-later
#version 450
#extension GL_GOOGLE_include_directive : require
layout(set=0,binding=0) uniform sampler2D scaledVideo;
layout(set=0,binding=2) uniform sampler2D uiOverlay;
layout(location=0) in vec2 vUv;
layout(location=0) out vec4 outColor;
layout(push_constant) uniform VideoParams {
    vec4 rect;
    ivec2 sourceSize;
    int videoEnabled;
    int gpuUi;
    ivec2 targetSize;
    int upscaler;
    int padding;
} params;
#define A_GPU 1
#define A_GLSL 1
#include "ffx_a.h"
#define FSR_RCAS_F 1
#define FSR_RCAS_DENOISE 1
AF4 FsrRcasLoadF(ASU2 p) {
    // Keep the sharpening kernel inside the picture, excluding letterboxing.
    ivec2 lo=ivec2(ceil(params.rect.xy*vec2(params.targetSize)-0.5));
    ivec2 hi=ivec2(ceil(params.rect.zw*vec2(params.targetSize)-0.5))-1;
}
void FsrRcasInputF(inout AF1 r,inout AF1 g,inout AF1 b) {}
#include "ffx_fsr1.h"
void main() {
    ivec2 p=clamp(ivec2(gl_FragCoord.xy),ivec2(0),params.targetSize-1);
    if (params.videoEnabled != 0 && vUv.x>=params.rect.x && vUv.y>=params.rect.y &&
        vUv.x<params.rect.z && vUv.y<params.rect.w) {
        uvec4 con;
        FsrRcasF(value.r,value.g,value.b,uvec2(p),con);
    }
    vec4 ui=params.gpuUi != 0 ? vec4(0) : texture(uiOverlay,vUv);
    outColor=vec4(mix(clamp(value,0.0,1.0),ui.rgb,ui.a),1.0);
}
