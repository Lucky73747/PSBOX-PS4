// SPDX-License-Identifier: GPL-3.0-or-later
#include "stream_stats_ui.h"
#include "ui/strings.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string_view>
#include <vector>

namespace xc::ps4 {
namespace {
using namespace xc::ui;
const char* uiText(const char* en, const char* pt) {
    return language() == Language::PortugueseBR ? pt : en;
}
constexpr Color kWhite = rgba(244,245,242), kMuted = rgba(195,201,196);
constexpr Color kGreen = rgba(127,214,164), kYellow = rgba(242,193,78), kRed = rgba(255,107,107);
enum class Level { Ok, Warn, Bad };
struct Metric { const char* key; const char* label; std::string value, unit; Level level=Level::Ok; };

Color accent(const StreamStatsUiConfig& c) { return rgba(c.accentR,c.accentG,c.accentB); }
Color levelColor(Level l) { return l==Level::Bad?kRed:l==Level::Warn?kYellow:kGreen; }
Level up(float value,float warn,float bad) { return value>=bad?Level::Bad:value>=warn?Level::Warn:Level::Ok; }
Level fpsLevel(float value,const StreamStatsUiConfig& c) {
    return value<c.fpsBad?Level::Bad:value<c.fpsWarn?Level::Warn:Level::Ok;
}
std::string number(float n,unsigned digits=1) {
    char b[48];
    if(digits)std::snprintf(b,sizeof(b),"%.*f",int(digits),double(n));
    else std::snprintf(b,sizeof(b),"%.0f",double(n));
    std::string s=b; std::replace(s.begin(),s.end(),'.',','); return s;
}
const Font& selectedFont(const Fonts& f,const StreamStatsUiConfig& c) {
    return c.fontFace==2?f.bold:c.fontFace==1?f.semibold:f.regular;
}
int width(const Font& f,std::string_view s,int px) { return f.measure(s,px); }
void drawText(Canvas& c,const Font& f,std::string_view s,int x,int y,int px,Color color) {
    f.draw(c,s,x,y,px,color);
}
void rounded(Canvas& c,Rect r,Color fill,Color border,int radius,int borderPx) {
    c.fillRect(r,fill,radius);
    if(borderPx>0) c.strokeRect(r,border,borderPx,radius);
}
}

void drawStreamCompactStatsUi(Canvas& c,Fonts& fonts,const StreamStatsUiData& d,const StreamStatsUiConfig& cfg) {
    char metrics[128];
    if(d.rttMs>=0)
        std::snprintf(metrics,sizeof(metrics),"%u FPS   |   %d ms   |   %.2f Mbps",d.fps,d.rttMs,double(d.bitrateMbps));
    else
        std::snprintf(metrics,sizeof(metrics),"%u FPS   |   — ms   |   %.2f Mbps",d.fps,double(d.bitrateMbps));
    const bool alert=d.stalled||d.disconnected;
    const int w=std::max(360,fonts.semibold.measure(metrics,20)+40);
    const int h=alert?94:56;
    const int x=cfg.alignRight?c.width()-cfg.marginX-w:cfg.marginX;
    const int y=cfg.alignBottom?c.height()-cfg.marginY-h:cfg.marginY;
    c.fillRect({x,y,w,h},rgba(7,10,12,246),12);
    c.strokeRect({x,y,w,h},d.disconnected?rgba(230,166,70):d.stalled?rgba(214,92,76):rgba(58,65,61),1,12);
    fonts.semibold.draw(c,metrics,x+20,y+18,20,kWhite);
    if(alert) {
        c.fillRect({x+20,y+54,w-40,1},rgba(255,255,255,24));
        fonts.semibold.draw(c,d.disconnected?uiText("Connection lost","Conexão perdida"):uiText("Video stalled","Vídeo parado"),x+20,y+64,18,
                           d.disconnected?rgba(255,205,116):rgba(255,151,137));
    }
}
void drawStreamLegacyStatsUi(Canvas& c,Fonts& fonts,const StreamLegacyStatsUiData& d) {
    c.fillRect({24,24,1115,286},rgba(7,10,12,246),14);
    c.strokeRect({24,24,1115,286},d.disconnected?rgba(230,166,70):rgba(42,155,72),2,14);
    fonts.semibold.draw(c,d.title,46,40,23,d.disconnected?rgba(255,205,116):rgba(142,238,154));
    for(size_t i=0;i<d.rows.size();++i)
        fonts.regular.draw(c,d.rows[i],46,74+int(i)*27,i==6?16:18,
                           i==0?rgba(235,238,235):i==6?rgba(177,199,180):rgba(205,213,205));
    fonts.regular.draw(c,d.controls,46,263,16,rgba(218,224,218));
}

void applyStreamStatsUiPreset(StreamStatsUiConfig& c,uint8_t variant) {
    c.variant=std::min<uint8_t>(variant,2);
    const bool card=c.variant==0;
    c.scalePercent=card?120:100;
    c.fontSize=card?13:16;
    c.lineHeightPercent=card?100:120;
    c.marginX=card?8:12;c.marginY=card?6:12;
    c.padding=card?8:6;c.backgroundPercent=card?90:0;
    c.outlinePx=card?2:1;c.fontFace=2;
}

void readStreamStatsUiConfig(const xc::json::Value& v,StreamStatsUiConfig& c) {
    auto u8=[&](const char* k,uint8_t& out,int lo,int hi){out=static_cast<uint8_t>(std::clamp<int64_t>((v[k].isBool() ? int64_t(v[k].asBool()) : v[k].asInt(out)),lo,hi));};
    auto u16=[&](const char* k,uint16_t& out,int lo,int hi){out=static_cast<uint16_t>(std::clamp<int64_t>((v[k].isBool() ? int64_t(v[k].asBool()) : v[k].asInt(out)),lo,hi));};
    u8("variant",c.variant,0,2);
    if(v["position_version"].asInt(0)>=1) {
        u8("align_right",c.alignRight,0,1);u8("align_bottom",c.alignBottom,0,1);
    } else {c.alignRight=0;c.alignBottom=0;}
    u8("subtitles",c.subtitles,0,1);u8("titles",c.titles,0,1);u8("header",c.header,0,1);
    u8("accent_text",c.accentText,0,1);u8("font_face",c.fontFace,0,2);
    u16("scale_percent",c.scalePercent,60,180);u8("font_size",c.fontSize,9,24);
    u8("line_height_percent",c.lineHeightPercent,80,180);u8("margin_x",c.marginX,0,80);
    u8("margin_y",c.marginY,0,80);u8("padding",c.padding,2,24);
    u8("background_percent",c.backgroundPercent,0,100);u8("outline_px",c.outlinePx,0,5);
    u8("accent_r",c.accentR,0,255);u8("accent_g",c.accentG,0,255);u8("accent_b",c.accentB,0,255);
    const auto& modes=v["metrics"];
    constexpr const char* names[]={"fps","rtt","br","loss","dec","drop","cad"};
    for(size_t i=0;i<c.metricMode.size();++i)
        c.metricMode[i]=static_cast<uint8_t>(std::clamp<int64_t>(modes[names[i]].asInt(c.metricMode[i]),0,2));
    u8("fps_warn",c.fpsWarn,1,120);u8("fps_bad",c.fpsBad,0,119);
    u16("rtt_warn",c.rttWarn,1,2000);u16("rtt_bad",c.rttBad,2,3000);
    u8("loss_warn_tenths",c.lossWarnTenths,1,100);u8("loss_bad_tenths",c.lossBadTenths,2,100);
    u8("drop_warn",c.dropWarn,0,120);u8("drop_bad",c.dropBad,1,120);
    u8("decode_warn",c.decodeWarn,1,100);u8("decode_bad",c.decodeBad,2,100);
    u8("jitter_warn",c.jitterWarn,1,100);u8("jitter_bad",c.jitterBad,2,100);
}

void writeStreamStatsUiConfig(xc::json::Value& value,const StreamStatsUiConfig& c) {
    value.set("variant",int(c.variant));value.set("align_right",int(c.alignRight));
    value.set("align_bottom",int(c.alignBottom));value.set("position_version",1);
    value.set("subtitles",bool(c.subtitles));value.set("titles",bool(c.titles));value.set("header",bool(c.header));
    value.set("accent_text",bool(c.accentText));value.set("font_face",int(c.fontFace));
    value.set("scale_percent",int(c.scalePercent));value.set("font_size",int(c.fontSize));
    value.set("line_height_percent",int(c.lineHeightPercent));value.set("margin_x",int(c.marginX));
    value.set("margin_y",int(c.marginY));value.set("padding",int(c.padding));
    value.set("background_percent",int(c.backgroundPercent));value.set("outline_px",int(c.outlinePx));
    value.set("accent_r",int(c.accentR));value.set("accent_g",int(c.accentG));value.set("accent_b",int(c.accentB));
    xc::json::Value metrics=xc::json::Value::object();
    constexpr const char* names[]={"fps","rtt","br","loss","dec","drop","cad"};
    for(size_t i=0;i<c.metricMode.size();++i)metrics.set(names[i],int(c.metricMode[i]));
    value.set("metrics",std::move(metrics));
    value.set("fps_warn",int(c.fpsWarn));value.set("fps_bad",int(c.fpsBad));
    value.set("rtt_warn",int(c.rttWarn));value.set("rtt_bad",int(c.rttBad));
    value.set("loss_warn_tenths",int(c.lossWarnTenths));value.set("loss_bad_tenths",int(c.lossBadTenths));
    value.set("drop_warn",int(c.dropWarn));value.set("drop_bad",int(c.dropBad));
    value.set("decode_warn",int(c.decodeWarn));value.set("decode_bad",int(c.decodeBad));
    value.set("jitter_warn",int(c.jitterWarn));value.set("jitter_bad",int(c.jitterBad));
}

namespace {
struct SettingRef { uint8_t kind,index; };
// Kind IDs keep menu editing deterministic and avoid string-based mutation.
constexpr SettingRef kRows[6][12]={
    {{0,0},{0,1},{0,2},{0,3},{0,4},{0,5},{0,6}},
    {{1,0},{1,1},{1,2},{1,3},{1,4},{1,5}},
    {{2,0},{2,1},{2,2},{2,3},{2,4}},
    {{3,0},{3,1},{3,2},{3,3},{3,4},{3,5},{3,6}},
    {{4,0},{4,4},{4,5},{4,6}},
    {{5,0},{5,1},{5,2},{5,3},{5,4},{5,5},{5,6},{5,7}}
};
constexpr uint8_t kRowCounts[6]={7,6,5,7,4,8};
template<class T> void cycle(T& value,int dir,T lo,T hi,T step=T(1)) {
    int next=int(value)+(dir<0?-int(step):int(step));
    const int low=int(lo),high=int(hi),span=high-low+int(step);
    while(next<low)next+=span;
    while(next>high)next-=span;
    value=static_cast<T>(next);
}
const char* modeName(uint8_t v){return v==0?uiText("Off","Desligado"):v==1?uiText("On","Ligado"):uiText("Alert","Alerta");}
}

uint8_t streamStatsSettingsRowCount(uint8_t page){return page<6?kRowCounts[page]:0;}
uint8_t streamStatsSettingsNextRow(uint8_t page,uint8_t row,int direction){
    const uint8_t n=streamStatsSettingsRowCount(page);if(!n)return 0;
    return static_cast<uint8_t>((row+(direction<0?n-1:direction>0?1:0))%n);
}
std::string streamStatsSettingsLabel(const StreamStatsUiConfig& c,uint8_t page,uint8_t row){
    if(page>=6||row>=kRowCounts[page])return {};
    constexpr const char* layoutPt[]={"Formato","Posição","Subtítulos","Títulos","Cabeçalho","Escala","Tamanho da fonte"};
    constexpr const char* layoutEn[]={"Format","Position","Subtitles","Titles","Header","Scale","Font size"};
    constexpr const char* spacingPt[]={"Altura de linha","Margem esquerda","Margem superior","Preenchimento","Fundo","Contorno"};
    constexpr const char* spacingEn[]={"Line height","Horizontal margin","Vertical margin","Padding","Background","Outline"};
    constexpr const char* visualPt[]={"Valores em destaque","Peso da fonte","Cor de destaque R","Cor de destaque G","Cor de destaque B"};
    constexpr const char* visualEn[]={"Highlighted values","Font weight","Accent color R","Accent color G","Accent color B"};
    constexpr const char* networkPt[]={"Latência","Bitrate","Perda","Latência: aviso","Latência: crítico","Perda: aviso","Perda: crítico"};
    constexpr const char* networkEn[]={"Latency","Bitrate","Loss","Latency: warning","Latency: critical","Loss: warning","Loss: critical"};
    constexpr const char* videoPt[]={"FPS","Decode","Quadros perdidos","Cadência"};
    constexpr const char* videoEn[]={"FPS","Decode","Dropped frames","Cadence"};
    constexpr const char* limitsPt[]={"FPS: aviso","FPS: crítico","Perdidos/s: aviso","Perdidos/s: crítico","Decode: aviso","Decode: crítico","Cadência: aviso","Cadência: crítico"};
    constexpr const char* limitsEn[]={"FPS: warning","FPS: critical","Drops/s: warning","Drops/s: critical","Decode: warning","Decode: critical","Cadence: warning","Cadence: critical"};
    const bool pt=language()==Language::PortugueseBR;
    if(page==1&&row==1)return c.alignRight?(pt?"Margem direita":"Right margin"):(pt?"Margem esquerda":"Left margin");
    if(page==1&&row==2)return c.alignBottom?(pt?"Margem inferior":"Bottom margin"):(pt?"Margem superior":"Top margin");
    const char* const* pagesPt[]={layoutPt,spacingPt,visualPt,networkPt,videoPt,limitsPt};
    const char* const* pagesEn[]={layoutEn,spacingEn,visualEn,networkEn,videoEn,limitsEn};
    return (pt?pagesPt[page]:pagesEn[page])[row];
}
std::string streamStatsSettingsValue(const StreamStatsUiConfig& c,uint8_t page,uint8_t row){
    if(page>=6||row>=kRowCounts[page])return {};
    const char* variants[]={uiText("Card","Cartão"),uiText("Lines","Linhas"),uiText("Dense","Denso")};
    const SettingRef r=kRows[page][row];char b[32];
    switch(r.kind){
        case 0:
            switch(r.index){case 0:return variants[c.variant];case 1:{const char* positionsPt[]={"Sup. esquerda","Sup. direita","Inf. esquerda","Inf. direita"};const char* positionsEn[]={"Top left","Top right","Bottom left","Bottom right"};return (language()==Language::PortugueseBR?positionsPt:positionsEn)[(c.alignBottom?2:0)+(c.alignRight?1:0)];}
            case 2:return c.subtitles?uiText("On","Ligado"):uiText("Off","Desligado");case 3:return c.titles?uiText("On","Ligado"):uiText("Off","Desligado");
            case 4:return c.header?uiText("On","Ligado"):uiText("Off","Desligado");case 5:std::snprintf(b,sizeof(b),"%u%%",c.scalePercent);return b;
            default:std::snprintf(b,sizeof(b),"%u px",c.fontSize);return b;}
        case 1:
            switch(r.index){case 0:std::snprintf(b,sizeof(b),"%u%%",c.lineHeightPercent);return b;
            case 1:std::snprintf(b,sizeof(b),"%u px",c.marginX);return b;case 2:std::snprintf(b,sizeof(b),"%u px",c.marginY);return b;
            case 3:std::snprintf(b,sizeof(b),"%u px",c.padding);return b;case 4:std::snprintf(b,sizeof(b),"%u%%",c.backgroundPercent);return b;
            default:std::snprintf(b,sizeof(b),"%u px",c.outlinePx);return b;}
        case 2:
            switch(r.index){case 0:return c.accentText?uiText("Accent","Acento"):uiText("White","Branco");case 1:return c.fontFace==0?"Inter regular":c.fontFace==1?"Inter semibold":"Inter bold";
            case 2:std::snprintf(b,sizeof(b),"%u",c.accentR);return b;case 3:std::snprintf(b,sizeof(b),"%u",c.accentG);return b;
            default:std::snprintf(b,sizeof(b),"%u",c.accentB);return b;}
        case 3:
            if(r.index<3)return modeName(c.metricMode[r.index+1]);
            switch(r.index){case 3:std::snprintf(b,sizeof(b),"%u ms",c.rttWarn);return b;case 4:std::snprintf(b,sizeof(b),"%u ms",c.rttBad);return b;
            case 5:std::snprintf(b,sizeof(b),"%.1f%%",c.lossWarnTenths/10.f);return b;default:std::snprintf(b,sizeof(b),"%.1f%%",c.lossBadTenths/10.f);return b;}
        case 4:return modeName(c.metricMode[r.index==0?0:r.index]);
        case 5:
            switch(r.index){case 0:std::snprintf(b,sizeof(b),"%u FPS",c.fpsWarn);return b;case 1:std::snprintf(b,sizeof(b),"%u FPS",c.fpsBad);return b;
            case 2:case 3:std::snprintf(b,sizeof(b),"%u/s",r.index==2?c.dropWarn:c.dropBad);return b;
            case 4:case 5:std::snprintf(b,sizeof(b),"%u ms",r.index==4?c.decodeWarn:c.decodeBad);return b;
            default:std::snprintf(b,sizeof(b),"%u ms",r.index==6?c.jitterWarn:c.jitterBad);return b;}
    }
    return {};
}
void adjustStreamStatsSettings(StreamStatsUiConfig& c,uint8_t page,uint8_t row,int dir){
    if(page>=6||row>=kRowCounts[page]||!dir)return;
    const SettingRef r=kRows[page][row];
    switch(r.kind){
        case 0:switch(r.index){case 0:cycle(c.variant,dir,uint8_t(0),uint8_t(2));applyStreamStatsUiPreset(c,c.variant);break;case 1:{uint8_t position=(c.alignBottom?2:0)+(c.alignRight?1:0);cycle(position,dir,uint8_t(0),uint8_t(3));c.alignRight=position&1;c.alignBottom=(position>>1)&1;break;}
        case 2:c.subtitles=!c.subtitles;break;case 3:c.titles=!c.titles;break;case 4:c.header=!c.header;break;
        case 5:cycle(c.scalePercent,dir,uint16_t(60),uint16_t(180),uint16_t(10));break;default:cycle(c.fontSize,dir,uint8_t(9),uint8_t(24));break;}break;
        case 1:switch(r.index){case 0:cycle(c.lineHeightPercent,dir,uint8_t(80),uint8_t(180),uint8_t(5));break;
        case 1:cycle(c.marginX,dir,uint8_t(0),uint8_t(80));break;case 2:cycle(c.marginY,dir,uint8_t(0),uint8_t(80));break;
        case 3:cycle(c.padding,dir,uint8_t(2),uint8_t(24));break;case 4:cycle(c.backgroundPercent,dir,uint8_t(0),uint8_t(100),uint8_t(5));break;
        default:cycle(c.outlinePx,dir,uint8_t(0),uint8_t(5));break;}break;
        case 2:switch(r.index){case 0:c.accentText=!c.accentText;break;case 1:cycle(c.fontFace,dir,uint8_t(0),uint8_t(2));break;
        case 2:cycle(c.accentR,dir,uint8_t(0),uint8_t(255));break;case 3:cycle(c.accentG,dir,uint8_t(0),uint8_t(255));break;
        default:cycle(c.accentB,dir,uint8_t(0),uint8_t(255));break;}break;
        case 3:if(r.index<3){uint8_t& m=c.metricMode[r.index+1];cycle(m,dir,uint8_t(0),uint8_t(2));}
        else switch(r.index){case 3:cycle(c.rttWarn,dir,uint16_t(10),uint16_t(500),uint16_t(5));break;
        case 4:cycle(c.rttBad,dir,uint16_t(20),uint16_t(1000),uint16_t(5));break;
        case 5:cycle(c.lossWarnTenths,dir,uint8_t(1),uint8_t(100));break;default:cycle(c.lossBadTenths,dir,uint8_t(2),uint8_t(100));break;}break;
        case 4:cycle(c.metricMode[r.index==0?0:r.index],dir,uint8_t(0),uint8_t(2));break;
        case 5:switch(r.index){case 0:cycle(c.fpsWarn,dir,uint8_t(10),uint8_t(120));break;case 1:cycle(c.fpsBad,dir,uint8_t(5),uint8_t(110));break;
        case 2:cycle(c.dropWarn,dir,uint8_t(0),uint8_t(60));break;case 3:cycle(c.dropBad,dir,uint8_t(1),uint8_t(120));break;
        case 4:cycle(c.decodeWarn,dir,uint8_t(1),uint8_t(50));break;case 5:cycle(c.decodeBad,dir,uint8_t(2),uint8_t(100));break;
        case 6:cycle(c.jitterWarn,dir,uint8_t(1),uint8_t(50));break;default:cycle(c.jitterBad,dir,uint8_t(2),uint8_t(100));break;}break;
    }
}

void drawStreamStatsUi(Canvas& c,Fonts& fonts,const StreamStatsUiConfig& cfg,const StreamStatsUiData& d) {
    const Font& face=selectedFont(fonts,cfg);
    std::vector<Metric> all{
        {"fps","FPS",std::to_string(d.fps),cfg.subtitles?"":" FPS",fpsLevel(float(d.fps),cfg)},
        {"rtt",uiText("Latency","Latência"),d.rttMs<0?"—":std::to_string(d.rttMs)," ms",d.rttMs<0?Level::Ok:up(float(d.rttMs),cfg.rttWarn,cfg.rttBad)},
        {"br","Bitrate",number(d.bitrateMbps,2)," Mbps",Level::Ok},
        {"loss",uiText("Loss","Perda"),number(d.lossPercent),"%",up(d.lossPercent,cfg.lossWarnTenths/10.f,cfg.lossBadTenths/10.f)},
        {"dec","Decode",number(d.decodeMs)," ms",up(d.decodeMs,cfg.decodeWarn,cfg.decodeBad)},
        {"drop",uiText("Drops","Perdidos"),std::to_string(d.dropPerSecond),"/s",up(float(d.dropPerSecond),cfg.dropWarn,cfg.dropBad)},
        {"cad",uiText("Cadence","Cadência"),d.fps?number(1000.f/d.fps)+" / "+number(1000.f/d.fps+d.jitterMs):"— / —"," ms",up(d.jitterMs,cfg.jitterWarn,cfg.jitterBad)}
    };
    std::vector<const Metric*> chosen;
    for(size_t i=0;i<all.size();++i)
        if(cfg.metricMode[i]==1||(cfg.metricMode[i]==2&&all[i].level!=Level::Ok))chosen.push_back(&all[i]);
    const bool card=cfg.variant==0;
    const bool fpsVisible=std::find(chosen.begin(),chosen.end(),&all[0])!=chosen.end();
    const bool head=card&&cfg.header;
    const std::string title=d.disconnected?uiText("Connection lost","Conexão perdida"):d.stalled?uiText("Video stalled","Vídeo parado"):d.title;
    Color status=kGreen;
    for(const auto& metric:all)
        if(metric.level==Level::Bad) {status=kRed;break;}
        else if(metric.level==Level::Warn)status=kYellow;
    if(d.stalled)status=kRed;
    if(d.disconnected)status=kYellow;
    const Color normal=cfg.accentText?accent(cfg):kWhite;
    struct Row { std::vector<const Metric*> metrics;const char* heading="";int width=0,height=0; };
    std::vector<Row> rows;
    if(card) {
        for(int group=0;group<2;++group) {
            Row row;row.heading=group==0?uiText("WI-FI","WI-FI"):uiText("VIDEO","VÍDEO");
            for(auto* m:chosen) {
                const std::string_view key=m->key;
                const bool network=key=="rtt"||key=="br"||key=="loss";
                if(key!="fps"&&network==(group==0))row.metrics.push_back(m);
            }
            if(!row.metrics.empty())rows.push_back(std::move(row));
        }
    } else if(cfg.variant==1) {
        for(auto* m:chosen) {Row row;row.metrics.push_back(m);rows.push_back(std::move(row));}
    } else {
        const size_t split=(chosen.size()+1)/2;
        for(size_t start=0;start<chosen.size();start+=split) {
            Row row;
            for(size_t i=start;i<std::min(start+split,chosen.size());++i)row.metrics.push_back(chosen[i]);
            rows.push_back(std::move(row));
        }
    }
    if(rows.empty()&&!head)return;
    float scale=std::clamp(cfg.scalePercent/100.f,.6f,1.8f);
    int em=0,fs=0,unit=0,label=0,groupFont=0,line=0,labelH=0,padX=0,padY=0;
    int gap=0,tileGap=0,groupW=0,groupGap=0,headW=0,headH=0,contentW=0,contentH=0;
    const int mx=std::clamp<int>(cfg.marginX,0,c.width()/4),my=std::clamp<int>(cfg.marginY,0,c.height()/4);
    // Runs share a baseline. CSS font-size uses em, while Font::draw uses
    // ascent-to-descent pixel height; convert locally without changing the dock.
    auto run=[&](const Font& f,std::string_view text,int px,int x,int baseline,Color color,bool paint) {
        if(paint) {
            const int y=baseline-f.baselineOffset(px);
            if(!card&&cfg.outlinePx) {
                const int o=std::max(1,int(std::lround(cfg.outlinePx*scale)));
                for(const auto offset:std::array<std::pair<int,int>,4>{{{o,0},{-o,0},{0,o},{0,-o}}})
                    f.draw(c,text,x+offset.first,y+offset.second,px,rgba(0,0,0,242));
            }
            f.draw(c,text,x,y,px,color);
        }
        return f.measure(text,px);
    };
    auto metric=[&](const Metric& m,bool inlineLabel,int x,int baseline,bool paint) {
        const int start=x;
        const Color color=m.level==Level::Ok?normal:levelColor(m.level);
        if(inlineLabel&&cfg.subtitles)x+=run(fonts.regular,std::string(m.label)+": ",fonts.regular.cssPixelHeight(float(em)),x,baseline,kMuted,paint);
        const auto slash=m.value.find(" / ");
        if(slash!=std::string::npos) {
            x+=run(face,std::string_view(m.value).substr(0,slash),fs,x,baseline,color,paint);
            x+=run(fonts.regular," / ",unit,x,baseline,kMuted,paint);
            const bool bitrate=std::string_view(m.key)=="br";
            x+=run(bitrate?fonts.regular:face,std::string_view(m.value).substr(slash+3),bitrate?unit:fs,x,baseline,bitrate?kMuted:color,paint);
        } else x+=run(face,m.value,fs,x,baseline,color,paint);
        x+=run(fonts.regular,m.unit,unit,x,baseline,kMuted,paint);
        return x-start;
    };
    auto layout=[&] {
        em=std::max(1,int(std::lround(cfg.fontSize*scale)));
        fs=face.cssPixelHeight(float(em));unit=fonts.regular.cssPixelHeight(em*.85f);
        label=fonts.regular.cssPixelHeight(em*.68f);groupFont=fonts.bold.cssPixelHeight(em*.6f);
        line=std::max(1,int(std::lround(em*cfg.lineHeightPercent/100.f)));
        labelH=cfg.subtitles?std::max(1,int(std::lround(em*.68f*1.1f))):0;
        padY=std::max(1,int(std::lround(cfg.padding*scale)));
        padX=std::max(1,int(std::lround(cfg.padding*(card?1.5f:1.f)*scale)));
        gap=std::max(1,int(std::lround(5*scale)));
        tileGap=std::max(1,int(std::lround(em*.9f)));
        groupW=cfg.titles?int(std::lround(em*.6f*3.4f)):0;
        groupGap=cfg.titles?int(std::lround(em*.7f)):0;
        headW=0;headH=head?line:0;
        if(head) {
            headW=int(std::lround(em*(.55f+.5f)))+face.measure(title,fs)+int(std::lround(em*.5f))+fonts.regular.measure(d.resolution,unit);
            if(fpsVisible)headW+=em+face.measure(all[0].value,fs)+fonts.regular.measure(" FPS",unit);
        }
        contentW=headW;contentH=headH;
        for(auto& row:rows) {
            row.width=card?groupW+groupGap:0;
            for(size_t i=0;i<row.metrics.size();++i) {
                const Metric& m=*row.metrics[i];
                int w=metric(m,!card,0,0,false);
                // Match the prototype's max-content width before its small label wraps.
                if(card&&cfg.subtitles)w+=fonts.regular.measure(m.label,label);
                if(i)row.width+=card?tileGap:fonts.regular.measure(" · ",fonts.regular.cssPixelHeight(float(em)));
                row.width+=w;
            }
            row.height=card?int(std::lround(4*scale))+1+labelH+line:line;
            if(card&&contentH)contentH+=gap;
            contentH+=row.height;contentW=std::max(contentW,row.width);
        }
    };
    layout();
    const float availableW=float(std::max(1,c.width()-mx*2));
    const float availableH=float(std::max(1,c.height()-my*2));
    const float fit=std::min({1.f,availableW/(contentW+2*padX),availableH/(contentH+2*padY)});
    if(fit<1.f){scale*=fit*.98f;layout();}
    const int w=contentW+2*padX,h=contentH+2*padY;
    const int x=cfg.alignRight?c.width()-mx-w:mx,y=cfg.alignBottom?c.height()-my-h:my;
    rounded(c,{x,y,w,h},rgba(10,12,11,static_cast<uint8_t>(std::clamp<int>(cfg.backgroundPercent,0,100)*255/100)),
            rgba(255,255,255,34),std::max(1,int(std::lround(9*scale))),card?std::max(1,int(std::lround(scale))):0);
    const int left=x+padX;
    auto baselineAt=[&](int top){return top+(line-fs)/2+face.baselineOffset(fs);};
    int cy=y+padY;
    if(head) {
        const int baseline=baselineAt(cy);
        const float r=em*.55f/2;
        c.fillCircle(left+r,cy+line*.5f,r,status);
        int tx=left+int(std::lround(em*(.55f+.5f)));
        tx+=run(face,title,fs,tx,baseline,kWhite,true)+int(std::lround(em*.5f));
        run(fonts.regular,d.resolution,unit,tx,baseline,kMuted,true);
        if(fpsVisible) {
            const int fpsW=face.measure(all[0].value,fs)+fonts.regular.measure(" FPS",unit);
            tx=left+contentW-fpsW;
            const Color color=all[0].level==Level::Ok?accent(cfg):levelColor(all[0].level);
            tx+=run(face,all[0].value,fs,tx,baseline,color,true);
            run(fonts.regular," FPS",unit,tx,baseline,kMuted,true);
        }
        cy+=headH;
    }
    for(const auto& row:rows) {
        if(card) {
            if(cy>y+padY)cy+=gap;
            c.fillRect({left,cy,contentW,1},rgba(255,255,255,20));
            const int top=cy+1+int(std::lround(4*scale));
            if(cfg.titles) {
                const int groupLine=std::max(1,int(std::lround(em*.6f)));
                fonts.bold.draw(c,row.heading,left,top+labelH+line-groupLine-int(std::lround(em*.12f))+(groupLine-groupFont)/2,groupFont,kMuted);
            }
            int tx=left+groupW+groupGap;
            for(const auto* m:row.metrics) {
                const int metricW=metric(*m,false,tx,baselineAt(top+labelH),true);
                int tileW=metricW;
                if(cfg.subtitles) {
                    fonts.regular.draw(c,m->label,tx,top+(labelH-label)/2,label,kMuted);
                    tileW+=fonts.regular.measure(m->label,label);
                }
                tx+=tileW+tileGap;
            }
        } else {
            int tx=left+(cfg.alignRight?contentW-row.width:0);
            const int baseline=baselineAt(cy);
            for(size_t i=0;i<row.metrics.size();++i) {
                if(i)tx+=run(fonts.regular," · ",fonts.regular.cssPixelHeight(float(em)),tx,baseline,kMuted,true);
                tx+=metric(*row.metrics[i],true,tx,baseline,true);
            }
        }
        cy+=row.height;
    }
}

} // namespace xc::ps4
