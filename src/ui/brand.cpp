// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 RafaelNGP
// Modified for PSBOX PS4: original screen-and-controls artwork, 2026-10-17.
#include "ui/brand.h"
#include <cmath>
#include <algorithm>
namespace xc::ui {
void drawBrandMark(Canvas& c, float cx, float cy, float r, Color color) {
    const float s = r / 128.0f;
    auto x = [&](float v) { return cx + (v - 128) * s; };
    auto y = [&](float v) { return cy + (v - 128) * s; };
    const int stroke = std::max(1, static_cast<int>(std::lround(10*s)));
    c.strokeRect({static_cast<int>(x(55)), static_cast<int>(y(62)),
                  static_cast<int>(146*s), static_cast<int>(109*s)}, color, stroke,
                 std::max(1, static_cast<int>(21*s)));
    auto line = [&](float a,float b,float d,float e) {
        c.line(x(a),y(b),x(d),y(e),10*s,color);
        c.fillCircle(x(a),y(b),5*s,color); c.fillCircle(x(d),y(e),5*s,color);
    };
    line(105,195,151,195); line(128,171,128,195);
    line(85,117,115,117); line(100,102,100,132);
    c.fillCircle(x(160),y(102),6*s,color); c.fillCircle(x(175),y(117),6*s,color);
    c.fillCircle(x(160),y(132),6*s,color); c.fillCircle(x(145),y(117),6*s,color);
}
void drawAppIcon(Canvas& c, const Fonts&) {
    c.clear(rgba(32,35,33));
    drawBrandMark(c,c.width()*0.5f,c.height()*0.5f,std::min(c.width(),c.height())*0.5f,rgba(238,239,236));
}
void drawHomeArt(Canvas& c, const Fonts& fonts, bool launch) {
    c.clear(rgba(24,27,25));
    const float h=static_cast<float>(c.height());
    drawBrandMark(c,c.width()*(launch?0.5f:0.70f),h*0.44f,h*0.27f,rgba(238,239,236));
    if (launch) {
        const int px=static_cast<int>(h*0.065f);
        const int w=fonts.bold.measure("PSBOX",px);
        fonts.bold.draw(c,"PSBOX",(c.width()-w)/2,static_cast<int>(h*0.76f),px,rgba(238,239,236));
    }
}
} // namespace xc::ui
