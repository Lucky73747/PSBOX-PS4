// SPDX-License-Identifier: GPL-3.0-or-later
#version 450
layout(set=0,binding=2) uniform sampler2D atlas;
layout(push_constant) uniform Ui { vec4 rect; vec4 uvRect; vec4 color; vec4 shape; } p;
layout(location=0) in vec2 uv;
layout(location=1) in vec2 local;
layout(location=0) out vec4 result;
void main() {
    float a=1.0;
    if(p.shape.z>0.5) a=texture(atlas,uv).a;
    else {
        vec2 halfSize=p.rect.zw*0.5;
        float radius=p.shape.x;
        vec2 d=abs(local-halfSize)-(halfSize-vec2(radius));
        float sd=length(max(d,vec2(0)))+min(max(d.x,d.y),0.0)-radius;
        a=1.0-smoothstep(-0.5,0.5,sd);
        if(p.shape.y>0.0) a*=smoothstep(-p.shape.y-0.5,-p.shape.y+0.5,sd);
    }
    result=vec4(p.color.rgb,p.color.a*a);
}
