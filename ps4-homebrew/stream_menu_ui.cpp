// SPDX-License-Identifier: GPL-3.0-or-later
#include "stream_menu_ui.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>

namespace xc::ps4 {
namespace {
using namespace xc::ui;
constexpr Color kText = rgba(238, 239, 236), kMuted = rgba(177, 181, 177);
constexpr Color kAccent = rgba(168, 196, 207), kOnAccent = rgba(23, 33, 38);
constexpr Color kPanel = rgba(24, 27, 25), kCard = rgba(31, 34, 32);
constexpr uint8_t kBitratePresetRow = 7;
constexpr std::array<uint8_t, 4> kImagePresetRows{0, 2, kBitratePresetRow, 4};
constexpr std::array<uint8_t, 5> kImageCustomRows{0, 2, kBitratePresetRow, 1, 4};
constexpr std::array<uint8_t, 5> kConfigurationRows{6, 7, 8, 5, 3};
// Stable card IDs, arranged in the one authoritative visible and navigation order.
constexpr std::array<uint8_t, 5> kDockCardOrder{0, 2, 3, 1, 4}; // image, volume, configuration, controls, exit
float mix(float a, float b, float t) { return a + (b - a) * t; }
Color fade(Color c, float a) {
    return withAlpha(c, static_cast<uint8_t>(alphaOf(c) * std::clamp(a, 0.0f, 1.0f)));
}
Color blend(Color a, Color b, float t) {
    Color result = 0;
    for (unsigned shift = 0; shift < 32; shift += 8)
        result |= static_cast<Color>(std::lround(mix((a >> shift) & 255, (b >> shift) & 255, t))) << shift;
    return result;
}
// Match the reference's CSS cubic-bezier curves, including the x/time mapping.
float ease(float t, bool button) {
    if (t <= 0 || t >= 1) return std::clamp(t, 0.0f, 1.0f);
    const float x1 = button ? .215f : .22f, y1 = button ? .61f : 1.0f;
    const float x2 = button ? .355f : .36f;
    auto cubic = [](float u, float p, float q) {
        const float v = 1 - u;
        return 3*v*v*u*p + 3*v*u*u*q + u*u*u;
    };
    float lo = 0, hi = 1;
    for (int i = 0; i < 14; ++i) {
        const float mid = (lo + hi) * .5f;
        if (cubic(mid, x1, x2) < t) lo = mid; else hi = mid;
    }
    return cubic((lo + hi) * .5f, y1, 1);
}
float sample(const StreamMenuUiAnimation::Track& t, uint64_t now, bool button) {
    const uint64_t age = now >= t.started ? now - t.started : 0;
    if (age <= t.delay) return t.from;
    const float progress = t.duration ? std::min(1.0f, float(age - t.delay) / t.duration) : 1;
    return mix(t.from, t.target, ease(progress, button));
}
float tween(StreamMenuUiAnimation* m, unsigned id, float target, uint64_t now,
            unsigned duration = 150, bool button = true, unsigned delay = 0,
            bool startClosed = false) {
    if (!m) return target;
    auto& t = m->tracks[id];
    if (!t.initialized) {
        t.initialized = true;
        t.from = t.target = startClosed ? 0 : target;
    }
    if (t.target != target) {
        t.from = sample(t, now, button); t.target = target;
        t.started = now; t.duration = duration; t.delay = delay;
    }
    return sample(t, now, button);
}

// CSS-local coordinates transformed directly into existing Canvas GPU primitives.
// No per-frame images, surfaces, or GPU resources are allocated by this module.
struct Painter {
    Canvas& c; Fonts& f;
    float x, y, sx, sy, alpha;
    int X(float v) const { return int(std::lround(x + v*sx)); }
    int Y(float v) const { return int(std::lround(y + v*sy)); }
    Rect rect(float a, float b, float w, float h) const {
        return {X(a), Y(b), std::max(0, X(a+w)-X(a)), std::max(0, Y(b+h)-Y(b))};
    }
    void fill(float a, float b, float w, float h, Color color, float radius = 0) {
        c.fillRect(rect(a,b,w,h), fade(color, alpha), int(std::lround(radius*std::min(sx,sy))));
    }
    void stroke(float a, float b, float w, float h, Color color, float width=1, float radius=0) {
        c.strokeRect(rect(a,b,w,h), fade(color, alpha), std::max(1,int(std::lround(width*std::min(sx,sy)))),
                     int(std::lround(radius*std::min(sx,sy))));
    }
    void line(float a, float b, float d, float e, Color color, float width=1.9f) {
        c.line(X(a),Y(b),X(d),Y(e),width*std::min(sx,sy),fade(color,alpha));
    }
    void circle(float a, float b, float r, Color color, bool outline=false) {
        if (outline) c.strokeCircle(X(a),Y(b),r*std::min(sx,sy),1.9f*std::min(sx,sy),fade(color,alpha));
        else c.fillCircle(X(a),Y(b),r*std::min(sx,sy),fade(color,alpha));
    }
    float measure(const char* text, int size, bool bold=false) const {
        const int px = std::max(1,int(std::lround(size*sy)));
        return (bold ? f.semibold : f.regular).measure(text,px) / std::max(sx,.001f);
    }
    void text(const char* text, float a, float b, int size, Color color=kText, bool bold=false) {
        (bold ? f.semibold : f.regular).draw(c,text,X(a),Y(b),std::max(1,int(std::lround(size*sy))),fade(color,alpha));
    }
};

// Match the 24x24 reference SVG paths rather than substituting letter icons.
void icon(Painter& p, unsigned kind, float x, float y, Color color, float scale=1, bool muted=false) {
    Painter q{p.c,p.f,p.x+x*p.sx,p.y+y*p.sy,p.sx*scale,p.sy*scale,p.alpha};
    if (kind == 0) {
        q.stroke(3,5,18,12,color,1.9f,2.5f); q.line(9,20,15,20,color); q.line(12,17,12,20,color);
    } else if (kind == 1) {
        // DualShock 4: raised shoulders, central touchpad, two low sticks and grips.
        const float outline[][2]={{5,5.5f},{8,5.5f},{9,7},{15,7},{16,5.5f},{19,5.5f},
            {21,8},{23,16.5f},{22,19},{20,19.5f},{16.5f,16.5f},{7.5f,16.5f},
            {4,19.5f},{2,19},{1,16.5f},{3,8},{5,5.5f}};
        for(unsigned i=1;i<sizeof(outline)/sizeof(outline[0]);++i)
            q.line(outline[i-1][0],outline[i-1][1],outline[i][0],outline[i][1],color,1.5f);
        q.stroke(9,8,6,4.5f,color,1,1);
        q.line(5.5f,9,5.5f,13,color,1.3f);q.line(3.5f,11,7.5f,11,color,1.3f);
        q.circle(9,15,1.5f,color,true);q.circle(15,15,1.5f,color,true);
        q.line(18.5f,8.5f,17.5f,10.2f,color,1);q.line(17.5f,10.2f,19.5f,10.2f,color,1);
        q.line(19.5f,10.2f,18.5f,8.5f,color,1);
        q.circle(21,11.5f,.7f,color,true);q.stroke(16,11,1.3f,1.3f,color,1);
        q.line(18,13,19,14,color,1);q.line(19,13,18,14,color,1);
    } else if (kind == 2) {
        q.line(4,9.5f,7.5f,9.5f,color); q.line(4,9.5f,4,14.5f,color); q.line(4,14.5f,7.5f,14.5f,color);
        q.line(7.5f,9.5f,12,6,color); q.line(12,6,12,18,color); q.line(12,18,7.5f,14.5f,color);
        if (muted) {
            q.line(16,8,22,16,color,2); q.line(22,8,16,16,color,2);
        } else {
            q.c.strokeArc(q.X(12),q.Y(12),5*q.sx,1.9f*q.sx,-.65f,1.3f,fade(color,q.alpha));
            q.c.strokeArc(q.X(12),q.Y(12),9*q.sx,1.9f*q.sx,-.65f,1.3f,fade(color,q.alpha));
        }
    } else if (kind == 4) {
        q.line(12,3,12,12,color,2.2f);
        q.c.strokeArc(q.X(12),q.Y(13),8*q.sx,2.2f*q.sx,-1.0f,5.14f,fade(color,q.alpha));
    } else {
        q.line(4,8,6,8,color); q.line(12,8,20,8,color);
        q.line(4,16,12,16,color); q.line(18,16,20,16,color);
        q.circle(9,8,3,color,true); q.circle(15,16,3,color,true);
    }
}

enum class PadButton { Cross, Circle, Triangle, Square, Options, Touchpad, L1, R1, L3, R3 };
void padButton(Painter& p, PadButton button, float x, float y, float size=28) {
    Painter q{p.c,p.f,p.x+x*p.sx,p.y+y*p.sy,p.sx*size/28,p.sy*size/28,p.alpha};
    if(button==PadButton::Touchpad) {
        q.fill(-6,4,40,23,rgba(17,19,20),5);q.stroke(-6,4,40,23,kMuted,1.5f,5);
        for(int yy=9;yy<24;yy+=4) for(int xx=0;xx<30;xx+=4) q.circle(xx,yy,.45f,kMuted);
        return;
    }
    if(button==PadButton::Options) {
        q.fill(4,1,20,27,rgba(17,19,20),8);q.stroke(4,1,20,27,kMuted,1.5f,8);
        q.line(10,10,18,10,kText,1.5f);q.line(10,14,18,14,kText,1.5f);q.line(10,18,18,18,kText,1.5f);
        return;
    }
    if(button==PadButton::L1 || button==PadButton::R1 || button==PadButton::L3 || button==PadButton::R3) {
        const bool stick=button==PadButton::L3 || button==PadButton::R3;
        if(stick) {q.circle(14,14,14,rgba(17,19,20));q.circle(14,14,13,kMuted,true);}
        else {q.fill(-2,3,32,23,rgba(17,19,20),5);q.stroke(-2,3,32,23,kMuted,1.5f,5);}
        const char* label=button==PadButton::L1?"L1":button==PadButton::R1?"R1":button==PadButton::L3?"L3":"R3";
        q.text(label,(28-q.measure(label,12,true))/2,8,12,kText,true);
        return;
    }
    q.circle(14,14,14,rgba(17,19,20));q.circle(14,14,13,rgba(96,103,108),true);
    if(button==PadButton::Cross) {
        const Color blue=rgba(135,172,221);
        q.line(9,9,19,19,blue,2);q.line(19,9,9,19,blue,2);
    } else if(button==PadButton::Circle) q.circle(14,14,7,rgba(224,136,143),true);
    else if(button==PadButton::Square) q.stroke(7.5f,7.5f,13,13,rgba(204,156,189),2,1);
    else {
        const Color mint=rgba(126,200,175);
        q.line(14,6,22,20,mint,2);q.line(22,20,6,20,mint,2);q.line(6,20,14,6,mint,2);
    }
}

struct Panel {
    Painter p;
    const StreamMenuUiState& state;
    StreamMenuUiAnimation* motion;
    uint64_t now;
    float y=14;
    int remaining=0;
    void group(const char* heading,int rows,float rowHeight=60,float extraHeight=0) {
        if(y>14) y+=11;
        p.text(heading,18,y,13,kMuted,true); y+=22;
        p.fill(14,y,352,rows*rowHeight+extraHeight,kCard,12);
        p.stroke(14,y,352,rows*rowHeight+extraHeight,rgba(49,52,50),1,12);
        remaining=rows;
    }
    float row(const char* label, int rowId, float h=60, const char* small=nullptr) {
        const float top=y;
        const bool selected=state.focusInPanel && rowId>=0 &&
            (state.statsSettingsOpen ? state.statsSettingsRow==rowId :
             state.fsrSettingsOpen ? state.fsrSettingsRow==rowId : state.settingsRow==rowId);
        const unsigned channel=unsigned(rowId<0 ? 11 : rowId)+4;
        const float focus=tween(motion,channel,selected?1.0f:0.0f,now);
        if(focus>.001f) {
            p.fill(15,y+1,350,h-2,fade(rgba(255,255,255,13),focus),9);
            p.stroke(15,y+1,350,h-2,fade(kAccent,focus),2,9);
        }
        const float ty = y+(h-(small?34:18))*.5f;
        p.text(label,26,ty,15);
        if(small) p.text(small,26,ty+20,12,kMuted);
        if(--remaining>0) p.fill(15,y+h-1,350,1,rgba(49,52,50));
        y+=h;
        return top;
    }
    void segments(const char* label, const char* const* values,int n,int chosen,int rowId,
                  unsigned track,const char* small=nullptr,bool full=false) {
        const float h=full?89:60;
        const float top=row(full?"":label,rowId,h,small);
        if(full) p.text(label,26,top+10,15);
        float widths[5]{},total=6;
        for(int i=0;i<n;++i) {
            widths[i]=full ? 322.0f/n : std::max(42.0f,p.measure(values[i],14,true)+22);
            if(values[i][0]=='\0') widths[i]=30;
            total+=widths[i];
        }
        total+=2*(n-1);
        const float x=full ? 26 : 354-total;
        const float yy=top+(full?41:10);
        p.fill(x,yy,total,40,rgba(9,11,10),8);
        float target=x+3;
        for(int i=0;i<chosen;++i) target+=widths[i]+2;
        const float pos=tween(motion,track,target,now);
        const float width=tween(motion,track+1,widths[chosen],now);
        p.fill(pos,yy+3,width,34,kAccent,6);
        float xx=x+3;
        for(int i=0;i<n;++i) {
            const float overlap=std::max(0.0f,std::min(xx+widths[i],pos+width)-std::max(xx,pos))/widths[i];
            const Color color=blend(kMuted,kOnAccent,std::clamp(overlap,0.0f,1.0f));
            if(values[i][0]=='\0') icon(p,3,xx+(widths[i]-15)/2,yy+12,color,.625f);
            else p.text(values[i],xx+(widths[i]-p.measure(values[i],14,true))/2,yy+11,14,color,true);
            xx+=widths[i]+2;
        }
    }
    void value(const char* label,const char* value,int id) {
        const float top=row(label,id);
        p.text(value,354-p.measure(value,14,true),top+21,14,kMuted,true);
    }
};
void image(Panel& p) {
    const auto& s=p.state;
    static const char* res[]{"720p","1080p"}, *fps[]{"30","60"}, *filter[]{"Bilinear","FSR 1","Nativo"};
    p.group("Qualidade",2);
    p.segments("Resolução",res,2,s.requestedTier=="1080",0,16);
    p.segments("Taxa de quadros",fps,2,s.fps==60,2,18);
    static const char* bw[]{"3","10","30",""};
    int selected=!s.bitrateCustom ? (s.bitrate==3000000?0:s.bitrate==10000000?1:2) : 3;
    p.group("Rede",s.bitrateCustom?2:1);
    p.segments("Limite de banda",bw,4,selected,kBitratePresetRow,20,"Mbps");
    if(s.bitrateCustom) {
        const float top=p.row("Personalizado",1);
        p.p.fill(151,top+27,115,6,rgba(62,66,63),3);
        const float fill=std::clamp((s.bitrate/1000000.0f-3)/27,0.0f,1.0f)*115;
        if(fill>0) p.p.fill(151,top+27,fill,6,kAccent,3);
        p.p.circle(151+fill,top+30,9,kText);
        char val[24]; std::snprintf(val,sizeof(val),"%.0f Mbps",s.bitrate/1000000.0);
        p.p.text(val,354-p.p.measure(val,14,true),top+21,14,kText,true);
    }
    p.group("Aprimoramento",1);
    padButton(p.p,PadButton::Triangle,338,p.y-22,18);
    p.segments("Escala de saída",filter,3,std::min(s.upscaler,2u),4,22);
}
void fsr(Panel& p, int outputWidth, int outputHeight) {
    const auto& s=p.state;
    static const char* filters[]{"Bilinear","FSR 1","Nativo"};
    p.group("Nitidez RCAS",1,86);
    const float sharpnessY=p.row("",0,86);
    const uint32_t sharpnessLevel=std::min(s.rcasSharpness,4u);
    const uint32_t sharpnessPercent=(4u-sharpnessLevel)*25u;
    constexpr float sharpnessX=126.0f, sharpnessWidth=188.0f;
    char sharpnessText[16];std::snprintf(sharpnessText,sizeof(sharpnessText),"%u%%",sharpnessPercent);
    p.p.text("Força",26,sharpnessY+10,12,kMuted);
    p.p.text(sharpnessText,354-p.p.measure(sharpnessText,15,true),sharpnessY+8,15,kText,true);
    const float trackY=sharpnessY+39;
    p.p.fill(sharpnessX,trackY-4,sharpnessWidth,8,rgba(62,66,63),4);
    const float sharpnessFill=sharpnessWidth*sharpnessPercent/100.0f;
    if(sharpnessFill>0) p.p.fill(sharpnessX,trackY-4,sharpnessFill,8,kAccent,4);
    for(uint32_t i=0;i<5;++i) {
        const float x=sharpnessX+sharpnessWidth*float(i)/4.0f;
        p.p.line(x,trackY-5,x,trackY+5,rgba(31,34,32),2.0f);
    }
    const float thumbX=sharpnessX+sharpnessFill;
    p.p.circle(thumbX,trackY,10,rgba(20,23,22));
    p.p.circle(thumbX,trackY,8,kText);
    p.p.circle(thumbX,trackY,10,kAccent,true);
    p.p.text("Suave",sharpnessX,sharpnessY+57,10,kMuted);
    p.p.text("Forte",sharpnessX+sharpnessWidth-p.p.measure("Forte",10),sharpnessY+57,10,kMuted);
    const bool fsrEnabled = s.upscaler == 1;
    const uint32_t targetHeight = s.fsrTargetHeight == 1440 || s.fsrTargetHeight == 2160 ? s.fsrTargetHeight : 1080;
    p.group("Alvo EASU experimental",1);
    const char* target = targetHeight == 2160 ? "2160p (4K)" : targetHeight == 1440 ? "1440p" : "1080p";
    p.value("Resolução EASU",target,1);
    p.group("Processamento",1);
    p.segments("Escala de saída",filters,3,std::min(s.upscaler,2u),2,22);
    if(s.fsrExperimentalUnlocked) {
        p.group("Experimental",1); p.value("RCAS nativo 1080p",s.nativeRcas?"Ativo":"Desativado",3);
    }
    p.y+=9;
    p.p.fill(14,p.y,352,58,kCard,10);
    p.p.stroke(14,p.y,352,58,rgba(49,52,50),1,10);
    p.p.text("Efeito na imagem",26,p.y+7,11,kMuted,true);
    char effect[64]{};
    char detail[80]{};
    const uint32_t sourceWidth=s.width, sourceHeight=s.height;
    const uint32_t targetWidth=targetHeight*16u/9u;
    const bool hasFrame=sourceWidth>0&&sourceHeight>0;
    const bool directUpscale=hasFrame&&outputWidth>=0&&outputHeight>=0&&
        static_cast<uint32_t>(outputWidth)>=sourceWidth&&static_cast<uint32_t>(outputHeight)>=sourceHeight&&
        (static_cast<uint32_t>(outputWidth)>sourceWidth||static_cast<uint32_t>(outputHeight)>sourceHeight);
    const bool targetPass=hasFrame&&targetWidth>static_cast<uint32_t>(std::max(outputWidth,0))&&
        targetHeight>static_cast<uint32_t>(std::max(outputHeight,0))&&targetWidth>=sourceWidth&&targetHeight>=sourceHeight;
    if(!s.gpuVideoActive) {
        std::snprintf(effect,sizeof(effect),"GPU de vídeo inativa");
        std::snprintf(detail,sizeof(detail),"FSR 1 selecionado, sem efeito no fallback");
    } else if(!fsrEnabled) {
        std::snprintf(effect,sizeof(effect),"%s selecionado",s.upscaler==2?"Nativo":"Bilinear");
        std::snprintf(detail,sizeof(detail),"FSR 1 está desligado");
    } else if(!hasFrame) {
        std::snprintf(effect,sizeof(effect),"FSR 1 selecionado");
        std::snprintf(detail,sizeof(detail),"Aguardando o primeiro quadro de vídeo");
    } else if(targetPass) {
        std::snprintf(effect,sizeof(effect),"Alvo %up · experimental",targetHeight);
        std::snprintf(detail,sizeof(detail),"Caminho GPU efetivo: gpu_scaler no Klog");
    } else if(directUpscale) {
        std::snprintf(effect,sizeof(effect),"EASU + RCAS ativos");
        std::snprintf(detail,sizeof(detail),"%ux%u ampliado para %up",sourceWidth,sourceHeight,outputHeight);
    } else if(s.nativeRcas&&s.requestedTier=="1080"&&sourceWidth==1920&&sourceHeight==1080&&
              outputWidth==1920&&outputHeight==1080) {
        std::snprintf(effect,sizeof(effect),"RCAS nativo ativo · experimental");
        std::snprintf(detail,sizeof(detail),"EASU não amplia vídeo já em 1080p");
    } else {
        std::snprintf(effect,sizeof(effect),"Sem escala nesta resolução");
        std::snprintf(detail,sizeof(detail),"%ux%u → %dx%d; EASU/RCAS não processam",sourceWidth,sourceHeight,outputWidth,outputHeight);
    }
    p.p.text(effect,26,p.y+22,13,kText,true);
    p.p.text(detail,26,p.y+40,10,kMuted);
}
void controls(Panel& p) {
    p.group("Atalhos do controle",6);
    float y=p.row("Selecionar / confirmar",-1);padButton(p.p,PadButton::Cross,323,y+15);
    y=p.row("Voltar / cancelar",-1);padButton(p.p,PadButton::Circle,323,y+15);
    y=p.row("Abrir menu (0,7 s)",-1); padButton(p.p,PadButton::Options,321,y+15);
    y=p.row("Trocar painel",-1);
    padButton(p.p,PadButton::L1,280,y+15);padButton(p.p,PadButton::R1,323,y+15);
    y=p.row("Guide",-1);padButton(p.p,PadButton::Touchpad,313,y+13);
    y=p.row("Cartão vermelho: encerrar",-1);
    padButton(p.p,PadButton::Cross,323,y+15);
}

void volume(Panel& p) {
    p.group("Saída",1);
    p.p.text("Mutar",292,p.y-16,11,kMuted);
    padButton(p.p,PadButton::Triangle,338,p.y-22,18);
    if(!p.state.audioAvailable) {p.row("Áudio indisponível nesta sessão",-1);return;}
    const uint32_t currentVolume=std::min(p.state.volume,100u);
    const float y=p.row("",-1);
    p.p.fill(26,y+11,38,38,rgba(43,46,44),10);
    icon(p.p,2,34,y+19,kText,.92f,currentVolume==0);
    const float value=tween(p.motion,24,float(std::min(p.state.volume,100u)),p.now);
    p.p.fill(78,y+27,205,6,rgba(62,66,63),3);
    if(value>0) p.p.fill(78,y+27,205*value/100,6,kAccent,3);
    p.p.circle(78+205*value/100,y+30,10,kText);
    char text[12];std::snprintf(text,sizeof(text),"%u%%",currentVolume);
    p.p.text(text,354-p.p.measure(text,15,true),y+21,15,kText,true);
}
void configuration(Panel& p) {
    static const char* stats[]{"Oculta","Compacta","Completa"};
    static const char* layouts[]{"Cartão","Linhas","Denso"};
    p.group("Na tela",3,60,29);
    p.segments("Estatísticas",stats,3,std::min<int>(p.state.statsMode,2),6,26,nullptr,true);
    p.segments("Formato OSD",layouts,3,std::min<int>(p.state.statsUiConfig.variant,2),7,48);
    const float configureY=p.row("Configurar OSD",8);
    padButton(p.p,PadButton::Cross,323,configureY+15);
    // Existing diagnostic actions remain accessible; future controller options stay pending.
    p.group("Diagnóstico",2);
    p.value("Quadro apresentado",p.state.frameWait?"+1 quadro":"Mais recente",5);
    p.value("Reset automático",p.state.automaticReset?"Ativo":"Desativado",3);
}
void statsSettings(Panel& p) {
    static const char* pages[]={"Layout","Espaçamento","Visual","Rede","Vídeo","Limites"};
    const uint8_t page=std::min<uint8_t>(p.state.statsSettingsPage,5);
    char heading[64];std::snprintf(heading,sizeof(heading),"%s  ·  %u/6",pages[page],unsigned(page+1));
    const uint8_t count=streamStatsSettingsRowCount(page);
    const float headingY=p.y;
    p.group(heading,count,36);
    padButton(p.p,PadButton::L1,290,headingY-2,20);
    padButton(p.p,PadButton::R1,332,headingY-2,20);
    for(int i=0;i<count;++i) {
        const auto label=streamStatsSettingsLabel(p.state.statsUiConfig,page,uint8_t(i));
        const float top=p.row(label.c_str(),i,36);
        const auto value=streamStatsSettingsValue(p.state.statsUiConfig,page,uint8_t(i));
        p.p.text(value.c_str(),354-p.p.measure(value.c_str(),13,true),top+11,13,kAccent,true);
    }
    const float footerY=p.y+12;
    padButton(p.p,PadButton::Cross,18,footerY,20);
    p.p.text("Salvar",44,footerY+3,12,kMuted);
    padButton(p.p,PadButton::Circle,112,footerY,20);
    p.p.text("Voltar",138,footerY+3,12,kMuted);
}
float height(unsigned card,const StreamMenuUiState& s) {
    if(card==4) return 150;
    if(card==0) return s.fsrSettingsOpen ? (s.fsrExperimentalUnlocked?500:400) :
        (s.bitrateCustom ? 416 : 356);
    if(card==1) return 410;
    if(card==2) return 110;
    if(s.statsSettingsOpen)return 442;
    return 430;
}
} // namespace

bool StreamMenuUiAnimation::active(uint64_t now) const {
    for(const auto& t:tracks) if(t.initialized && t.from!=t.target &&
        (now<t.started || now-t.started<uint64_t(t.delay)+t.duration)) return true;
    return volumePulseStarted && now>=volumePulseStarted && now-volumePulseStarted<360;
}
uint8_t streamMenuNextRow(uint8_t card,uint8_t current,int direction,bool bitrateCustom) {
    auto next=[&](const auto& rows) {
        const auto it=std::find(rows.begin(),rows.end(),current);
        if(it==rows.end()) return rows.front();
        const int n=int(rows.size()),i=int(it-rows.begin());
        return rows[size_t((i+(direction<0?n-1:direction>0?1:0))%n)];
    };
    if(card==4) return 0;
    if(card==3) return next(kConfigurationRows);
    return bitrateCustom?next(kImageCustomRows):next(kImagePresetRows);
}
uint8_t streamMenuNextCard(uint8_t card,int direction) {
    const auto it=std::find(kDockCardOrder.begin(),kDockCardOrder.end(),std::min<uint8_t>(card,4));
    const int index=it==kDockCardOrder.end()?0:static_cast<int>(it-kDockCardOrder.begin());
    const int next=(index+(direction<0?4:direction>0?1:0))%static_cast<int>(kDockCardOrder.size());
    return kDockCardOrder[static_cast<size_t>(next)];
}
void drawStreamMenuUi(Canvas& c,Fonts& f,const StreamMenuUiState& s,uint64_t now,
                      float visibility,float fsrPageProgress,float cardProgress,StreamMenuUiAnimation* m) {
    (void)fsrPageProgress;(void)cardProgress;
    visibility=std::clamp(visibility,0.0f,1.0f);
    const unsigned active=std::min<unsigned>(s.activeCard,4);
    if(m) {
        m->panels[active]=s;
        if (!m->volumeObserved) {
            m->volumeObserved = true;
            m->lastVolume = s.volume;
        } else if (m->lastVolume != s.volume) {
            m->lastVolume = s.volume;
            m->volumePulseStarted = now ? now : 1;
        }
    }
    auto visualIndex=[](unsigned card) {
        const auto it=std::find(kDockCardOrder.begin(),kDockCardOrder.end(),
                                 static_cast<uint8_t>(std::min(card,4u)));
        return it==kDockCardOrder.end()?0u:static_cast<unsigned>(it-kDockCardOrder.begin());
    };
    const float w=float(c.width()),h=float(c.height());
    const float dockScale=std::max(.6f,std::min(h/900,(w-24)/376));
    // Same sizing formula as fit() in the pinned HTML (max scroll height ~400px).
    const float bottom=std::clamp(h*.022f,12.0f,26.0f);
    float panelScale=std::sqrt(.13f*w*h/(380*400.0f));
    panelScale=std::min({panelScale,(w-24)/380,(h-bottom-16-76*dockScale)/400});
    panelScale=std::max(.3f,panelScale)*.75f;
    const float dockX=(w-376*dockScale)/2,dockY=h-bottom-64*dockScale;
    for(unsigned order=0;order<5;++order) {
        const unsigned i=(active+1+order)%5; // active panel paints last during overlaps
        const bool open=s.panelOpen&&i==active;
        const float progress=tween(m,50+i,open?1.0f:0.0f,now,420,false,0,true);
        const float opacity=tween(m,55+i,open?1.0f:0.0f,now,open?220:180,false,0,true);
        const float content=tween(m,60+i,open?1.0f:0.0f,now,open?240:120,false,open?120:0,true);
        if(opacity<.001f) continue;
        const auto& pane=m?m->panels[i]:s;
        const float targetHeight=height(i,pane);
        const float ph=tween(m,65+i,targetHeight,now,420,false);
        const float sx=panelScale*mix(.35f,1,progress),sy=panelScale*mix(.2f,1,progress);
        const float center=std::clamp(dockX+(32+78*visualIndex(i))*dockScale,12+190*panelScale,w-12-190*panelScale);
        const float pulse= m && m->volumePulseStarted && now>=m->volumePulseStarted && now-m->volumePulseStarted<360
            ? std::sin(3.14159265f*float(now-m->volumePulseStarted)/360.0f) : 0.0f;
        const float base=dockY-12*dockScale+(1-progress)*10*dockScale-5.0f*pulse;
        Painter frame{c,f,center-190*sx,base-ph*sy,sx,sy,visibility*opacity};
        frame.fill(0,0,380,ph,kPanel,16);frame.stroke(0,0,380,ph,rgba(68,74,70),1,16);
        c.setClip(frame.rect(1,1,378,ph-2));
        frame.alpha*=content;
        frame.y+=(1-content)*8*sy;
        Panel p{frame,pane,m,now};
        if(i==0) {if(pane.fsrSettingsOpen) fsr(p,c.width(),c.height());else image(p);}
        else if(i==1) controls(p); else if(i==2) volume(p);
        else if(i==4) {
            p.group("Encerrar sessão cloud",1);
            const float y=p.row("Confirmar saída",0);
            padButton(p.p,PadButton::Cross,323,y+15);
            p.p.text("O jogo será desconectado.",18,p.y+12,13,kMuted);
        }
        else if(pane.statsSettingsOpen) statsSettings(p); else configuration(p);
        c.resetClip();
    }
    const float pulse= m && m->volumePulseStarted && now>=m->volumePulseStarted && now-m->volumePulseStarted<360
        ? std::sin(3.14159265f*float(now-m->volumePulseStarted)/360.0f) : 0.0f;
    Painter dock{c,f,dockX,dockY-5.0f*pulse,dockScale,dockScale,visibility};
    for(unsigned pos=0;pos<5;++pos) {
        const unsigned i=kDockCardOrder[pos];
        const float on=tween(m,75+i,s.panelOpen&&i==active?1.0f:0.0f,now,220,false);
        const float focused=tween(m,70+i,!s.focusInPanel&&i==active?1.0f:0.0f,now,180,false);
        const float x=78.0f*pos;
        if(focused>.001f) {
            dock.stroke(x-5.5f,-5.5f,75,75,fade(kAccent,focused),2.5f,19.5f);
            dock.stroke(x-3,-3,70,70,fade(rgba(16,18,17),focused),3,17);
        }
        dock.fill(x,0,64,64,(i==4?blend(rgba(116,35,39),rgba(195,53,61),on):blend(rgba(32,35,33),kAccent,on)),14);
        dock.stroke(x,0,64,64,blend(rgba(62,66,63),kAccent,on),1,14);
        icon(dock,i,x+17.5f,17.5f,(i==4?kText:blend(rgba(229,233,230),kOnAccent,on)),29.0f/24,
             i==2 && s.volume==0);
    }
}
} // namespace xc::ps4
