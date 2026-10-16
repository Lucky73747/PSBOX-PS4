// SPDX-License-Identifier: GPL-3.0-or-later
#version 450
layout(push_constant) uniform Ui { vec4 rect; vec4 uvRect; vec4 color; vec4 shape; } p;
layout(location=0) out vec2 uv;
layout(location=1) out vec2 local;
void main() {
    const vec2 v[6] = vec2[6](vec2(0,0),vec2(1,0),vec2(0,1),vec2(0,1),vec2(1,0),vec2(1,1));
    vec2 q=v[gl_VertexIndex];
    gl_Position=vec4((p.rect.xy+q*p.rect.zw)/vec2(1920,1080)*2.0-1.0,0,1);
    uv=mix(p.uvRect.xy,p.uvRect.zw,q);
    local=q*p.rect.zw;
}
