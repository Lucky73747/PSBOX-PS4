// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui/canvas.h"
#include "ui/font.h"
#include "stream_stats_ui.h"

#include <array>
#include <cstdint>
#include <string>

namespace xc::ps4 {

// Presentation-only copy of the active stream menu state. Input and profile
// ownership stay in main.cpp; this module renders the four-panel dock shared with the PC preview.
struct StreamMenuUiState {
    bool panelOpen = true;
    bool focusInPanel = true;
    bool fsrSettingsOpen = false;
    bool statsSettingsOpen = false;
    bool newStatsUiEnabled = true;
    uint8_t statsSettingsPage = 0, statsSettingsRow = 0;
    StreamStatsUiConfig statsUiConfig{};
    bool fsrExperimentalUnlocked = false;
    uint8_t statsMode = 2; // 0 off, 1 compact, 2 advanced.
    bool automaticReset = false;
    bool audioAvailable = false;
    bool nativeRcas = false;
    uint8_t activeCard = 0; // Stable IDs: 0=image, 1=controls, 2=volume, 3=configuration.
                            // Visible/navigation order: image, volume, configuration, controls, followed by exit (ID 4).
    uint8_t settingsRow = 0;
    uint8_t fsrSettingsRow = 0;
    std::string requestedTier = "720";
    std::string actualTier = "720";
    uint32_t bitrate = 3000000;
    bool bitrateCustom = false;
    uint32_t fps = 60;
    uint32_t upscaler = 1;
    uint32_t rcasSharpness = 4;
    uint32_t fsrScaleFactor = 2;
    uint32_t frameWait = 1;
    uint32_t volume = 100;
    uint32_t width = 0;
    uint32_t height = 0;
    int rttMs = -1;
};

// Owned by the rendering thread. No decoder/network state or GPU resources.
// Retargetable scalar tracks preserve continuity if navigation interrupts a tween.
struct StreamMenuUiAnimation {
    struct Track {
        float from = 0, target = 0;
        uint64_t started = 0;
        uint32_t duration = 0, delay = 0;
        bool initialized = false;
    };
    // 0–49: content selectors; 50–79: five dock buttons and panels.
    // Selector tracks must never overlap panel opening/opacity/height tracks.
    std::array<Track, 80> tracks{};
    std::array<StreamMenuUiState, 5> panels{};
    uint64_t volumePulseStarted = 0;
    uint32_t lastVolume = 100;
    bool volumeObserved = false;
    bool active(uint64_t nowMs) const;
};

// Stable profile row IDs, in the visual order of the selected dock panel.
uint8_t streamMenuNextRow(uint8_t card, uint8_t current, int direction,
                          bool bitrateCustom = false);
uint8_t streamMenuNextCard(uint8_t card, int direction);

void drawStreamMenuUi(xc::ui::Canvas& canvas, xc::ui::Fonts& fonts,
                      const StreamMenuUiState& state, uint64_t nowMs,
                      float visibility, float fsrPageProgress, float cardProgress,
                      StreamMenuUiAnimation* animation = nullptr);

} // namespace xc::ps4
