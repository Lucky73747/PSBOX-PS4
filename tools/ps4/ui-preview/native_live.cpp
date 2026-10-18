// SPDX-License-Identifier: GPL-3.0-or-later
// Linux-only live bridge. PS4 uses the very same drawStreamMenuUi + animation.
#define PSBOX_PREVIEW_LIBRARY
#include "native_preview.cpp"

namespace {
struct LivePreview {
    xc::ui::Canvas canvas;
    xc::ui::Fonts fonts;
    xc::ps4::StreamMenuUiAnimation animation;
    LivePreview(int width, int height):canvas(width,height){}
};
}
extern "C" {
void* psbox_preview_create(const char* root,int width,int height) {
    if(!root || width<640 || height<480 || width>4096 || height>2160) return nullptr;
    try {
        auto preview=std::make_unique<LivePreview>(width,height);
        if(!preview->fonts.load(std::string(root)+"/assets/fonts")) return nullptr;
        return preview.release();
    } catch(...) { return nullptr; }
}
void psbox_preview_destroy(void* context) {delete static_cast<LivePreview*>(context);}
int psbox_preview_active(void* context,uint64_t now) {
    return context && static_cast<LivePreview*>(context)->animation.active(now);
}
const uint32_t* psbox_preview_render(void* context,const uint32_t* v,size_t count,uint64_t now) {
    if(!context || !v || (count!=58&&count!=59&&count!=60)) return nullptr;
    try {
        auto& p=*static_cast<LivePreview*>(context);
        xc::ps4::StreamMenuUiState s;
        s.activeCard=std::min(v[0],4u);s.settingsRow=std::min(v[1],8u);
        s.requestedTier=v[2]==1080?"1080":"720";s.fps=v[3]==30?30:60;
        s.width=s.requestedTier=="1080"?1920:1280;s.height=s.requestedTier=="1080"?1080:720;
        s.bitrate=std::clamp(v[4],3u,30u)*1000000;s.bitrateCustom=v[5]!=0;s.upscaler=std::min(v[6],2u);
        s.volume=std::min(v[7],100u);s.statsMode=std::min(v[8],2u);
        s.fsrSettingsOpen=v[9]!=0;s.fsrSettingsRow=std::min(v[10],2u);
        s.rcasSharpness=std::min(v[11],4u);s.fsrScaleFactor=std::clamp(v[12],1u,2u);
        s.automaticReset=v[13]!=0;s.frameWait=std::min(v[14],1u);
        s.panelOpen=v[15]!=0;s.focusInPanel=v[16]!=0;s.audioAvailable=true;
        s.newStatsUiEnabled=v[17]!=0;s.statsSettingsOpen=v[18]!=0;
        s.statsSettingsPage=std::min(v[19],5u);s.statsSettingsRow=std::min(v[20],11u);
        auto& c=s.statsUiConfig;
        c.variant=std::min(v[21],2u);c.alignRight=v[22]!=0;c.subtitles=v[23]!=0;
        c.titles=v[24]!=0;c.header=v[25]!=0;c.accentText=v[26]!=0;c.fontFace=std::min(v[27],2u);
        c.scalePercent=std::clamp(v[28],60u,180u);c.fontSize=std::clamp(v[29],9u,24u);
        c.lineHeightPercent=std::clamp(v[30],80u,180u);c.marginX=std::min(v[31],80u);
        c.marginY=std::min(v[32],80u);c.padding=std::clamp(v[33],2u,24u);
        c.backgroundPercent=std::min(v[34],100u);c.outlinePx=std::min(v[35],5u);
        c.accentR=std::min(v[36],255u);c.accentG=std::min(v[37],255u);c.accentB=std::min(v[38],255u);
        for(size_t i=0;i<7;++i)c.metricMode[i]=std::min(v[39+i],2u);
        c.fpsWarn=std::clamp(v[46],1u,120u);c.fpsBad=std::min(v[47],119u);
        c.rttWarn=std::clamp(v[48],1u,2000u);c.rttBad=std::clamp(v[49],2u,3000u);
        c.lossWarnTenths=std::clamp(v[50],1u,100u);c.lossBadTenths=std::clamp(v[51],2u,100u);
        c.dropWarn=std::min(v[52],120u);c.dropBad=std::clamp(v[53],1u,120u);
        c.decodeWarn=std::clamp(v[54],1u,100u);c.decodeBad=std::clamp(v[55],2u,100u);
        c.jitterWarn=std::clamp(v[56],1u,100u);c.jitterBad=std::clamp(v[57],2u,100u);
        p.canvas.clearTransparent();
        c.alignBottom=count==60?v[59]!=0:false;
        const unsigned condition=count>=59?std::min(v[58],2u):0;
        drawPreviewStats(p.canvas,p.fonts,s,condition);
        xc::ps4::drawStreamMenuUi(p.canvas,p.fonts,s,now,1,1,1,&p.animation);
        return p.canvas.data();
    } catch(...) { return nullptr; }
}
}
