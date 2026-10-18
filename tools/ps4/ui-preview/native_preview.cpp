// SPDX-License-Identifier: GPL-3.0-or-later
// Host preview that calls the exact PS4 stream menu renderer and Canvas.
#include "stream_menu_ui.h"
#include "platform/platform.h"
#include "util/log.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <algorithm>
#include <cstdarg>
#include <charconv>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>

namespace xc::platform {
bool readFile(const std::string& path, std::string& out) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    out.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    return true;
}
}
namespace xc::log {
void write(Level, const char* fmt, ...) {
    va_list args; va_start(args, fmt); std::vfprintf(stderr, fmt, args); std::fputc('\n', stderr); va_end(args);
}
}

namespace {
constexpr int W = 1920, H = 1080;
void mountain(xc::ui::Canvas& c, int x0, int x1, int base, int peakX, int peakY, xc::ui::Color color) {
    for (int y = peakY; y <= base; ++y) {
        const float t = static_cast<float>(y - peakY) / std::max(1, base - peakY);
        const int half = static_cast<int>((x1 - x0) * 0.5f * t);
        c.fillRect({peakX - half, y, half * 2 + 1, 1}, color);
    }
}
void drawBackdrop(xc::ui::Canvas& c, xc::ui::Fonts& fonts) {
    using namespace xc::ui;
    c.gradientV({0, 0, W, H}, rgba(104, 139, 128), rgba(27, 45, 37));
    c.fillCircle(1125, 184, 95, rgba(255, 239, 194, 215));
    mountain(c, -50, 810, 630, 365, 270, rgba(66, 92, 82));
    mountain(c, 350, 1120, 680, 725, 315, rgba(52, 79, 67));
    mountain(c, 860, 1510, 645, 1205, 260, rgba(58, 83, 70));
    c.gradientV({0, 630, W, 450}, rgba(60, 83, 55), rgba(18, 29, 24));
    c.fillCircle(716, 652, 19, rgba(180, 139, 99));
    c.fillRect({697, 672, 39, 95}, rgba(22, 34, 28), 16);
    c.line(709, 755, 689, 812, 12, rgba(18, 27, 23));
    c.line(726, 755, 743, 812, 12, rgba(18, 27, 23));
    c.fillCircle(40, 51, 5, rgba(101, 221, 120));
    fonts.bold.draw(c, "CLOUD PLAY", 58, 38, 18, rgba(245, 250, 245));
    fonts.regular.draw(c, "STREAMING", 187, 42, 12, rgba(184, 202, 189));
    c.fillCircle(1278, 51, 5, rgba(101, 221, 120));
    fonts.semibold.draw(c, "CONNECTION OK", 1155, 42, 12, rgba(245, 250, 245));
    fonts.semibold.draw(c, "59 FPS", 1330, 42, 12, rgba(245, 250, 245));
    fonts.bold.draw(c, "Elder Realms", 100, 298, 34, rgba(245, 250, 245));
    fonts.regular.draw(c, "Exploration  /  Chapter 04", 100, 342, 16, rgba(226, 235, 226));
}
void drawPreviewStats(xc::ui::Canvas& canvas,xc::ui::Fonts& fonts,
                      const xc::ps4::StreamMenuUiState& s,unsigned condition=0) {
        if(s.statsMode==1){
            xc::ps4::StreamStatsUiData d;
            d.fps=condition?0:59;d.rttMs=22;d.bitrateMbps=condition?0.f:7.35f;
            d.stalled=condition==1;d.disconnected=condition==2;
            xc::ps4::drawStreamCompactStatsUi(canvas,fonts,d,s.statsUiConfig);
        }
        if(s.statsMode==2){
            xc::ps4::StreamStatsUiData d;d.title="Xbox Cloud";d.resolution="720p";
            d.fps=59;d.rttMs=22;d.lossPercent=.2f;d.dropPerSecond=0;d.decodeMs=2.1f;
            d.bitrateMbps=7.35f;d.capMbps=8;d.jitterMs=.7f;
            d.stalled=condition==1;d.disconnected=condition==2;
            auto appearance=s.statsUiConfig;
            if(!s.newStatsUiEnabled)appearance.variant=1;
            xc::ps4::drawStreamStatsUi(canvas,fonts,appearance,d);
        }
}

}

#ifndef PSBOX_PREVIEW_LIBRARY
int main(int argc, char** argv) {
    const std::string root = argc > 1 ? argv[1] : ".";
    const std::string output = argc > 2 ? argv[2] : "preview-native.png";
    const std::string section = argc > 3 ? argv[3] : "settings";
    if (section != "settings" && section != "controls" && section != "volume" && section != "configuration" && section != "stats" && section != "exit") {
        std::cerr << "section must be settings, controls, volume, configuration, stats, or exit\n"; return 2;
    }
    xc::ui::Canvas canvas(W, H);
    xc::ui::Fonts fonts;
    if (!fonts.load(root + "/assets/fonts")) { std::cerr << "could not load project fonts\n"; return 1; }
    drawBackdrop(canvas, fonts);
    xc::ps4::StreamMenuUiState menu;
    menu.activeCard = section == "settings" ? 0 : section == "controls" ? 1 : section == "volume" ? 2 : section == "exit" ? 4 : 3;
    menu.statsSettingsOpen = section == "stats";
    menu.newStatsUiEnabled = section == "stats";
    menu.statsMode = 2;
    menu.audioAvailable = true;
    menu.requestedTier = "720";
    menu.actualTier = "720";
    menu.bitrate = 3000000;
    menu.fps = 60;
    menu.volume = 80;
    unsigned previewCondition=0;
    // Optional key=value arguments let the desktop viewer edit real UI state.
    for (int i = 4; i < argc; ++i) {
        const std::string option = argv[i];
        const size_t split = option.find('=');
        if (split == std::string::npos) return 2;
        const std::string key = option.substr(0, split);
        unsigned value = 0;
        const char* start = option.data() + split + 1;
        const char* end = option.data() + option.size();
        const auto parsed = std::from_chars(start, end, value);
        if (parsed.ec != std::errc{} || parsed.ptr != end) return 2;
        if (key == "row") menu.settingsRow = static_cast<uint8_t>(std::min(value, 7u));
        else if (key == "tier") menu.requestedTier = value == 1080 ? "1080" : "720";
        else if (key == "fps") menu.fps = value == 30 ? 30 : 60;
        else if (key == "bitrate") menu.bitrate = std::clamp(value, 3u, 30u) * 1000000u;
        else if (key == "custom") menu.bitrateCustom = value != 0;
        else if (key == "filter") menu.upscaler = std::min(value, 2u);
        else if (key == "volume") menu.volume = std::min(value, 100u);
        else if (key == "stats") menu.statsMode = static_cast<uint8_t>(std::min(value, 2u));
        else if (key == "stats_ui") menu.newStatsUiEnabled = value != 0;
        else if (key == "stats_open") menu.statsSettingsOpen = value != 0;
        else if (key == "stats_page") menu.statsSettingsPage = static_cast<uint8_t>(std::min(value, 5u));
        else if (key == "stats_row") menu.statsSettingsRow = static_cast<uint8_t>(std::min(value, 11u));
        else if (key == "stats_condition") previewCondition=std::min(value,2u);
        else if (key == "stats_variant") xc::ps4::applyStreamStatsUiPreset(menu.statsUiConfig,static_cast<uint8_t>(std::min(value,2u)));
        else if (key == "stats_align") menu.statsUiConfig.alignRight = value != 0;
        else if (key == "stats_bottom") menu.statsUiConfig.alignBottom = value != 0;
        else if (key == "stats_metrics") for (auto& mode : menu.statsUiConfig.metricMode) mode = static_cast<uint8_t>(std::min(value, 2u));
        else if (key == "stats_scale") menu.statsUiConfig.scalePercent = static_cast<uint16_t>(std::clamp(value, 60u, 180u));
        else if (key == "stats_font") menu.statsUiConfig.fontSize = static_cast<uint8_t>(std::clamp(value, 9u, 24u));
        else if (key == "stats_subtitles") menu.statsUiConfig.subtitles = value != 0;
        else if (key == "stats_titles") menu.statsUiConfig.titles = value != 0;
        else if (key == "stats_header") menu.statsUiConfig.header = value != 0;
        else if (key == "stats_margin_x") menu.statsUiConfig.marginX = static_cast<uint8_t>(std::min(value, 80u));
        else if (key == "stats_margin_y") menu.statsUiConfig.marginY = static_cast<uint8_t>(std::min(value, 80u));
        else if (key == "stats_padding") menu.statsUiConfig.padding = static_cast<uint8_t>(std::clamp(value, 2u, 24u));
        else if (key == "fsr") menu.fsrSettingsOpen = value != 0;
        else if (key == "fsr_row") menu.fsrSettingsRow = static_cast<uint8_t>(std::min(value, 3u));
        else if (key == "sharpness") menu.rcasSharpness = std::min(value, 4u);
        else if (key == "scale") menu.fsrScaleFactor = std::clamp(value, 1u, 2u);
        else if (key == "reset") menu.automaticReset = value != 0;
        else if (key == "pacing") menu.frameWait = std::min(value, 1u);
        else if (key == "source") {
            menu.width = value == 1080 ? 1920 : 1280;
            menu.height = value == 1080 ? 1080 : 720;
        }
        else return 2;
    }
    if (!menu.width || !menu.height) {
        menu.width = menu.requestedTier == "1080" ? 1920 : 1280;
        menu.height = menu.requestedTier == "1080" ? 1080 : 720;
    }
    menu.settingsRow = xc::ps4::streamMenuNextRow(menu.activeCard, menu.settingsRow, 0, menu.bitrateCustom);
    drawPreviewStats(canvas,fonts,menu,previewCondition);
    if (section != "stats")
        xc::ps4::drawStreamMenuUi(canvas, fonts, menu, 0, 1.0f, 1.0f, 1.0f);
    // Canvas stores RGBA bytes in memory (0xAABBGGRR on little endian).
    if (!stbi_write_png(output.c_str(), W, H, 4, canvas.data(), W * 4)) {
        std::cerr << "failed to write preview PNG\n"; return 1;
    }
    std::cout << "Rendered PS4 UI module: " << output << " (expanded=" << section << ")\n";
    return 0;
}

#endif
