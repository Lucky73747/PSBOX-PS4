// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 RafaelNGP
#include "ui/canvas.h"

#include <algorithm>
#include <cmath>

namespace xc::ui {

namespace {

constexpr float kPi = 3.14159265f;

Rect intersect(Rect a, Rect b) {
    int x0 = std::max(a.x, b.x), y0 = std::max(a.y, b.y);
    int x1 = std::min(a.x + a.w, b.x + b.w), y1 = std::min(a.y + a.h, b.y + b.h);
    return {x0, y0, std::max(0, x1 - x0), std::max(0, y1 - y0)};
}

inline uint32_t lerp8(uint32_t a, uint32_t b, uint32_t t /*0..255*/) { return a + (((b - a) * t + 128) >> 8); }

inline uint32_t clampCoverage(float v) {
    if (v <= 0) return 0;
    if (v >= 1) return 255;
    return static_cast<uint32_t>(v * 255.0f + 0.5f);
}

}  // namespace

void Canvas::setClip(Rect r) { clip_ = intersect(r, {0, 0, w_, h_}); }

inline void Canvas::blend(uint32_t& dst, Color src, uint32_t a) const {
    if (a == 0) return;
    if (a >= 255 || (dst >> 24) == 0) {
        dst = (src & 0x00FFFFFFu) | (a << 24);
        return;
    }
    const uint32_t dstA = dst >> 24;
    if (dstA == 255) {
        const uint32_t inv = 255 - a;
        const uint32_t rb = (((src & 0x00FF00FFu) * a + (dst & 0x00FF00FFu) * inv + 0x00800080u) >> 8) & 0x00FF00FFu;
        const uint32_t g = (((src & 0x0000FF00u) * a + (dst & 0x0000FF00u) * inv + 0x00008000u) >> 8) & 0x0000FF00u;
        dst = 0xFF000000u | rb | g;
        return;
    }
    const uint32_t inv = 255 - a;
    const uint32_t outA = a + (dstA * inv + 127) / 255;
    auto composite = [&](uint32_t shift) {
        const uint32_t srcC = (src >> shift) & 255u;
        const uint32_t dstC = (dst >> shift) & 255u;
        const uint32_t premul = srcC * a + (dstC * dstA * inv + 127) / 255;
        return outA ? (premul + outA / 2) / outA : 0u;
    };
    dst = (outA << 24) | (composite(16) << 16) | (composite(8) << 8) | composite(0);
}

void Canvas::clear(Color c) { std::fill(px_.begin(), px_.end(), c | 0xFF000000u); }
void Canvas::clearTransparent() {
    if (gpuUi_) { gpuCommands_.clear(); return; }
    std::fill(px_.begin(), px_.end(), 0u);
}

uint32_t Canvas::cornerCoverage(Rect r, int radius, int px, int py) {
    // Distance from the pixel centre to the nearest corner circle centre.
    float cx = px + 0.5f, cy = py + 0.5f;
    float left = r.x + radius, right = r.x + r.w - radius;
    float top = r.y + radius, bottom = r.y + r.h - radius;
    float dx = cx < left ? left - cx : cx > right ? cx - right : 0;
    float dy = cy < top ? top - cy : cy > bottom ? cy - bottom : 0;
    if (dx == 0 || dy == 0) return 255;
    float d = std::sqrt(dx * dx + dy * dy);
    return clampCoverage(radius + 0.5f - d);
}

void Canvas::fillRect(Rect r, Color c, int radius) {
    if (gpuUi_) {
        gpuCommands_.push_back({r, clip_, {}, c, std::max(0, std::min(radius, std::min(r.w, r.h) / 2)), 0, false});
        return;
    }
    Rect b = intersect(r, clip_);
    uint32_t a = alphaOf(c);
    if (b.w <= 0 || b.h <= 0 || a == 0) return;
    radius = std::min(radius, std::min(r.w, r.h) / 2);
    for (int y = b.y; y < b.y + b.h; ++y) {
        uint32_t* row = &px_[static_cast<size_t>(y) * w_];
        bool cornerRow = radius > 0 && (y < r.y + radius || y >= r.y + r.h - radius);
        for (int x = b.x; x < b.x + b.w; ++x) {
            uint32_t cov = 255;
            if (cornerRow && (x < r.x + radius || x >= r.x + r.w - radius)) cov = cornerCoverage(r, radius, x, y);
            blend(row[x], c, (a * cov + 127) / 255);
        }
    }
}

void Canvas::strokeRect(Rect r, Color c, int t, int radius) {
    if (gpuUi_) {
        gpuCommands_.push_back({r, clip_, {}, c, std::max(0, std::min(radius, std::min(r.w, r.h) / 2)), t, false});
        return;
    }
    Rect b = intersect(r, clip_);
    uint32_t a = alphaOf(c);
    if (b.w <= 0 || b.h <= 0 || a == 0) return;
    Rect inner{r.x + t, r.y + t, r.w - 2 * t, r.h - 2 * t};
    int innerRadius = std::max(0, radius - t);
    for (int y = b.y; y < b.y + b.h; ++y) {
        uint32_t* row = &px_[static_cast<size_t>(y) * w_];
        for (int x = b.x; x < b.x + b.w; ++x) {
            uint32_t outer = cornerCoverage(r, radius, x, y);
            if (radius == 0) outer = 255;
            uint32_t in = 0;
            if (x >= inner.x && x < inner.x + inner.w && y >= inner.y && y < inner.y + inner.h)
                in = innerRadius ? cornerCoverage(inner, innerRadius, x, y) : 255;
            uint32_t cov = outer > in ? outer - in : 0;
            if (cov) blend(row[x], c, (a * cov + 127) / 255);
        }
    }
}

void Canvas::gradientV(Rect r, Color top, Color bottom) {
    Rect b = intersect(r, clip_);
    if (b.w <= 0 || b.h <= 0) return;
    // Opaque gradients replace every destination pixel, so blending each
    // pixel is wasted work. This is the full-screen menu background and was
    // taking roughly 90 ms per software-rendered UI frame on PS4.
    if (alphaOf(top) == 255 && alphaOf(bottom) == 255) {
        for (int y = b.y; y < b.y + b.h; ++y) {
            uint32_t t = r.h > 1 ? static_cast<uint32_t>((y - r.y) * 255 / (r.h - 1)) : 0;
            Color c = 0;
            for (int s = 0; s < 24; s += 8)
                c |= lerp8((top >> s) & 0xFF, (bottom >> s) & 0xFF, t) << s;
            uint32_t* row = &px_[static_cast<size_t>(y) * w_ + b.x];
            std::fill_n(row, b.w, c | 0xFF000000u);
        }
        return;
    }
    for (int y = b.y; y < b.y + b.h; ++y) {
        uint32_t t = r.h > 1 ? static_cast<uint32_t>((y - r.y) * 255 / (r.h - 1)) : 0;
        Color c = 0;
        for (int s = 0; s < 32; s += 8) c |= lerp8((top >> s) & 0xFF, (bottom >> s) & 0xFF, t) << s;
        uint32_t a = alphaOf(c);
        uint32_t* row = &px_[static_cast<size_t>(y) * w_];
        for (int x = b.x; x < b.x + b.w; ++x) blend(row[x], c, a);
    }
}

void Canvas::gradientH(Rect r, Color left, Color right) {
    Rect b = intersect(r, clip_);
    if (b.w <= 0 || b.h <= 0) return;
    std::vector<Color> cols(static_cast<size_t>(b.w));
    for (int x = b.x; x < b.x + b.w; ++x) {
        uint32_t t = r.w > 1 ? static_cast<uint32_t>((x - r.x) * 255 / (r.w - 1)) : 0;
        Color c = 0;
        for (int s = 0; s < 32; s += 8) c |= lerp8((left >> s) & 0xFF, (right >> s) & 0xFF, t) << s;
        cols[static_cast<size_t>(x - b.x)] = c;
    }
    for (int y = b.y; y < b.y + b.h; ++y) {
        uint32_t* row = &px_[static_cast<size_t>(y) * w_];
        for (int x = b.x; x < b.x + b.w; ++x) {
            Color c = cols[static_cast<size_t>(x - b.x)];
            blend(row[x], c, alphaOf(c));
        }
    }
}

void Canvas::fillCircle(float cx, float cy, float radius, Color c) {
    Rect r{static_cast<int>(cx - radius - 1), static_cast<int>(cy - radius - 1), static_cast<int>(radius * 2 + 3),
           static_cast<int>(radius * 2 + 3)};
    if (gpuUi_) {
        drawPrimitiveMask(r, c, [=](Canvas& m) { m.fillCircle(cx-r.x, cy-r.y, radius, rgba(255,255,255)); });
        return;
    }
    Rect b = intersect(r, clip_);
    uint32_t a = alphaOf(c);
    for (int y = b.y; y < b.y + b.h; ++y)
        for (int x = b.x; x < b.x + b.w; ++x) {
            float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
            uint32_t cov = clampCoverage(radius + 0.5f - std::sqrt(dx * dx + dy * dy));
            if (cov) blend(px_[static_cast<size_t>(y) * w_ + x], c, (a * cov + 127) / 255);
        }
}

void Canvas::strokeCircle(float cx, float cy, float radius, float thickness, Color c) {
    float outer = radius + thickness / 2, inner = radius - thickness / 2;
    Rect r{static_cast<int>(cx - outer - 1), static_cast<int>(cy - outer - 1), static_cast<int>(outer * 2 + 3),
           static_cast<int>(outer * 2 + 3)};
    if (gpuUi_) {
        drawPrimitiveMask(r, c, [=](Canvas& m) { m.strokeCircle(cx-r.x, cy-r.y, radius, thickness, rgba(255,255,255)); });
        return;
    }
    Rect b = intersect(r, clip_);
    uint32_t a = alphaOf(c);
    for (int y = b.y; y < b.y + b.h; ++y)
        for (int x = b.x; x < b.x + b.w; ++x) {
            float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
            float d = std::sqrt(dx * dx + dy * dy);
            uint32_t cov = std::min(clampCoverage(outer + 0.5f - d), clampCoverage(d - inner + 0.5f));
            if (!cov) continue;
            blend(px_[static_cast<size_t>(y) * w_ + x], c, (a * cov + 127) / 255);
        }
}

void Canvas::strokeArc(float cx, float cy, float radius, float thickness, float start, float sweep, Color c) {
    float outer = radius + thickness / 2, inner = radius - thickness / 2;
    Rect r{static_cast<int>(cx - outer - 1), static_cast<int>(cy - outer - 1), static_cast<int>(outer * 2 + 3),
           static_cast<int>(outer * 2 + 3)};
    if (gpuUi_) {
        drawPrimitiveMask(r, c, [=](Canvas& m) { m.strokeArc(cx-r.x, cy-r.y, radius, thickness, start, sweep, rgba(255,255,255)); });
        return;
    }
    Rect b = intersect(r, clip_);
    uint32_t a = alphaOf(c);
    start = std::fmod(start, 2 * kPi);
    if (start < 0) start += 2 * kPi;
    for (int y = b.y; y < b.y + b.h; ++y)
        for (int x = b.x; x < b.x + b.w; ++x) {
            float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
            float d = std::sqrt(dx * dx + dy * dy);
            uint32_t cov = std::min(clampCoverage(outer + 0.5f - d), clampCoverage(d - inner + 0.5f));
            if (!cov) continue;
            float ang = std::atan2(dy, dx);
            if (ang < 0) ang += 2 * kPi;
            float rel = ang - start;
            if (rel < 0) rel += 2 * kPi;
            if (rel > sweep) continue;
            // Fade the tail for a spinner look.
            uint32_t fade = static_cast<uint32_t>(255 * (0.25f + 0.75f * rel / sweep));
            blend(px_[static_cast<size_t>(y) * w_ + x], c, a * cov / 255 * fade / 255);
        }
}

void Canvas::line(float x0, float y0, float x1, float y1, float thickness, Color c) {
    float half = thickness / 2;
    Rect r{static_cast<int>(std::min(x0, x1) - half - 1), static_cast<int>(std::min(y0, y1) - half - 1),
           static_cast<int>(std::fabs(x1 - x0) + thickness + 3), static_cast<int>(std::fabs(y1 - y0) + thickness + 3)};
    Rect b = intersect(r, clip_);
    if (gpuUi_) {
        drawPrimitiveMask(r, c, [=](Canvas& m) { m.line(x0-r.x, y0-r.y, x1-r.x, y1-r.y, thickness, rgba(255,255,255)); });
        return;
    }
    float vx = x1 - x0, vy = y1 - y0, len2 = vx * vx + vy * vy;
    uint32_t a = alphaOf(c);
    for (int y = b.y; y < b.y + b.h; ++y)
        for (int x = b.x; x < b.x + b.w; ++x) {
            float px = x + 0.5f - x0, py = y + 0.5f - y0;
            float t = len2 > 0 ? std::clamp((px * vx + py * vy) / len2, 0.0f, 1.0f) : 0;
            float dx = px - t * vx, dy = py - t * vy;
            uint32_t cov = clampCoverage(half + 0.5f - std::sqrt(dx * dx + dy * dy));
            if (cov) blend(px_[static_cast<size_t>(y) * w_ + x], c, (a * cov + 127) / 255);
        }
}

void Canvas::drawPrimitiveMask(Rect bounds, Color color, const std::function<void(Canvas&)>& draw) {
    if (bounds.w <= 0 || bounds.h <= 0 || alphaOf(color) == 0) return;
    Canvas mask(bounds.w, bounds.h);
    mask.clearTransparent();
    draw(mask);
    std::vector<uint8_t> coverage(static_cast<size_t>(bounds.w) * bounds.h);
    const uint32_t* pixels = mask.data();
    for (size_t i = 0; i < coverage.size(); ++i) coverage[i] = static_cast<uint8_t>(pixels[i] >> 24);
    drawMask(coverage.data(), bounds.w, bounds.h, bounds.w, bounds.x, bounds.y, color);
}

void Canvas::drawImage(const Image& img, int x, int y, uint8_t opacity, int radius) {
    Rect r{x, y, img.w, img.h};
    Rect b = intersect(r, clip_);
    if (b.w <= 0 || b.h <= 0 || opacity == 0) return;
    bool circle = radius < 0;
    if (circle) radius = std::min(img.w, img.h) / 2;
    radius = std::min(radius, std::min(img.w, img.h) / 2);
    for (int yy = b.y; yy < b.y + b.h; ++yy) {
        uint32_t* row = &px_[static_cast<size_t>(yy) * w_];
        const uint32_t* src = &img.px[static_cast<size_t>(yy - y) * img.w];
        bool cornerRow = radius > 0 && (circle || yy < y + radius || yy >= y + img.h - radius);
        for (int xx = b.x; xx < b.x + b.w; ++xx) {
            uint32_t s = src[xx - x];
            uint32_t a = (alphaOf(s) * opacity + 127) / 255;
            if (cornerRow && (circle || xx < x + radius || xx >= x + img.w - radius))
                a = a * cornerCoverage(r, radius, xx, yy) / 255;
            blend(row[xx], s, a);
        }
    }
}

void Canvas::drawMask(const uint8_t* mask, int mw, int mh, int stride, int x, int y, Color c) {
    if (gpuUi_) {
        if (!mask || mw <= 0 || mh <= 0) return;
        // Content key survives font-cache allocation changes; color is a draw parameter.
        uint64_t key = 1469598103934665603ull;
        for (int j = 0; j < mh; ++j) for (int i = 0; i < mw; ++i)
            key = (key ^ mask[j * stride + i]) * 1099511628211ull;
        key = (key ^ static_cast<uint32_t>(mw)) * 1099511628211ull;
        key = (key ^ static_cast<uint32_t>(mh)) * 1099511628211ull;
        auto found = gpuGlyphs_.find(key);
        Rect tile;
        if (found != gpuGlyphs_.end()) tile = found->second;
        else {
            if (atlasX_ + mw > w_) { atlasX_ = 0; atlasY_ += atlasRow_; atlasRow_ = 0; }
            if (mw > w_ || atlasY_ + mh > h_) { gpuAtlasOverflow_ = true; return; }
            if (gpuAtlas_.empty()) gpuAtlas_.resize(static_cast<size_t>(w_) * h_, 0u);
            tile = {atlasX_, atlasY_, mw, mh};
            for (int j = 0; j < mh; ++j) for (int i = 0; i < mw; ++i)
                gpuAtlas_[static_cast<size_t>(atlasY_ + j) * w_ + atlasX_ + i] =
                    0x00ffffffu | (static_cast<uint32_t>(mask[j * stride + i]) << 24);
            atlasX_ += mw; atlasRow_ = std::max(atlasRow_, mh);
            gpuGlyphs_.emplace(key, tile); ++gpuAtlasGeneration_;
        }
        gpuCommands_.push_back({{x, y, mw, mh}, clip_, tile, c, 0, 0, true});
        return;
    }
    Rect b = intersect({x, y, mw, mh}, clip_);
    uint32_t a = alphaOf(c);
    for (int yy = b.y; yy < b.y + b.h; ++yy) {
        uint32_t* row = &px_[static_cast<size_t>(yy) * w_];
        const uint8_t* m = mask + static_cast<size_t>(yy - y) * stride;
        for (int xx = b.x; xx < b.x + b.w; ++xx) {
            uint32_t cov = m[xx - x];
            if (cov) blend(row[xx], c, (a * cov + 127) / 255);
        }
    }
}

}  // namespace xc::ui
