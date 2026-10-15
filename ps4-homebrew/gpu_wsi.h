// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

namespace xc::ps4 {

struct PresentationStats {
    uint64_t generation = 0;
    uint32_t windowMs = 0;
    uint32_t presentFps = 0;
    uint32_t newVideoFps = 0;
    uint32_t updateAvgUs = 0;
    uint32_t updateMaxUs = 0;
    uint32_t uploadAvgUs = 0;
    uint32_t uploadMaxUs = 0;
    uint32_t acquireAvgUs = 0;
    uint32_t acquireMaxUs = 0;
    uint32_t encodeSubmitAvgUs = 0;
    uint32_t encodeSubmitMaxUs = 0;
    uint32_t presentCallAvgUs = 0;
    uint32_t presentCallMaxUs = 0;
    uint32_t frameIntervalAvgUs = 0;
    uint32_t frameIntervalMaxUs = 0;
    uint32_t queueIdleAvgUs = 0;
    uint32_t queueIdleMaxUs = 0;
    uint32_t loopAvgUs = 0;
    uint32_t loopMaxUs = 0;
};

struct CanvasFrameUpdateResult {
    bool canvasChanged = false;
    bool newVideoFrame = false;
};

// Reports whether the CPU canvas changed and whether a newly decoded video
// frame was copied into it before the next Vulkan presentation.
using CanvasFrameUpdate = CanvasFrameUpdateResult (*)(uint32_t* rgba, void* userData,

// Starts Mesa Orbis WSI through VK_EXT_headless_surface and keeps presenting
[[noreturn]] void presentCanvas(uint32_t* rgba, uint32_t width, uint32_t height,

} // namespace xc::ps4
