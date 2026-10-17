// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui/canvas.h"
#include "ui/font.h"
#include "util/json.h"

#include <array>
#include <cstdint>
#include <string>

namespace xc::ps4 {

// Equivalent of the psbox-stats.html configuration, bounded to values that
// remain legible and fit the PS4's 1920x1080 logical canvas.
struct StreamStatsUiConfig {
    uint8_t variant = 0; // 0 card, 1 lines, 2 dense.
    uint8_t alignRight = 0, alignBottom = 0; // Default: top-left.
    uint8_t subtitles = 1, titles = 1, header = 1;
    uint8_t accentText = 0;
    uint8_t fontFace = 2; // 0 Inter regular, 1 semibold, 2 bold.
    uint16_t scalePercent = 120;
    uint8_t fontSize = 13; // CSS base px.
    uint8_t lineHeightPercent = 100;
    uint8_t marginX = 8, marginY = 6, padding = 8;
    uint8_t backgroundPercent = 90, outlinePx = 2;
    uint8_t accentR = 168, accentG = 196, accentB = 207;
    std::array<uint8_t, 7> metricMode{{1,1,1,1,1,2,2}}; // 0 off, 1 on, 2 alert.
    uint8_t fpsWarn = 55, fpsBad = 45;
    uint16_t rttWarn = 60, rttBad = 100;
    uint8_t lossWarnTenths = 5, lossBadTenths = 20;
    uint8_t dropWarn = 1, dropBad = 4;
    uint8_t decodeWarn = 5, decodeBad = 9;
    uint8_t jitterWarn = 4, jitterBad = 8;
};

struct StreamStatsUiData {
    std::string title = "PSBOX";
    std::string resolution = "720p";
    uint32_t fps = 0, dropPerSecond = 0;
    int rttMs = -1;
    float lossPercent = 0, decodeMs = 0, bitrateMbps = 0, capMbps = 0, jitterMs = 0;
    bool stalled = false, disconnected = false;
};

struct StreamLegacyStatsUiData {
    std::string title;
    std::array<std::string,7> rows;
    std::string controls;
    bool disconnected = false;
};
void drawStreamLegacyStatsUi(xc::ui::Canvas& canvas, xc::ui::Fonts& fonts,
                             const StreamLegacyStatsUiData& data);
void drawStreamCompactStatsUi(xc::ui::Canvas& canvas, xc::ui::Fonts& fonts,
                              const StreamStatsUiData& data,
                              const StreamStatsUiConfig& config = {});

void readStreamStatsUiConfig(const xc::json::Value& value, StreamStatsUiConfig& config);
void writeStreamStatsUiConfig(xc::json::Value& value, const StreamStatsUiConfig& config);
// Visual defaults from the three supplied HTML files; keep metric/alert choices.
void applyStreamStatsUiPreset(StreamStatsUiConfig& config, uint8_t variant);
uint8_t streamStatsSettingsRowCount(uint8_t page);
uint8_t streamStatsSettingsNextRow(uint8_t page,uint8_t row,int direction);
std::string streamStatsSettingsLabel(const StreamStatsUiConfig& config,uint8_t page,uint8_t row);
std::string streamStatsSettingsValue(const StreamStatsUiConfig& config,uint8_t page,uint8_t row);
void adjustStreamStatsSettings(StreamStatsUiConfig& config,uint8_t page,uint8_t row,int direction);
void drawStreamStatsUi(xc::ui::Canvas& canvas, xc::ui::Fonts& fonts,
                       const StreamStatsUiConfig& config, const StreamStatsUiData& data);

} // namespace xc::ps4
