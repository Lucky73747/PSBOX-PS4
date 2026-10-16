// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <cstddef>

namespace xc::ui { class Canvas; }
namespace xc::ps4 {

struct PresentationHeartbeat { uint32_t stage; uint64_t ageMs; };
PresentationHeartbeat presentationHeartbeat();

struct PresentationStats {
    uint64_t generation = 0;
    uint64_t confirmedVideoSequence = 0;
    uint64_t confirmedUniqueVideoFrames = 0;
    uint64_t confirmedRepeatedVideoFlips = 0;
    uint32_t repeatedVideoFlips = 0;
    bool confirmedVideoKnown = false;
    uint32_t windowMs = 0;
    // Driver-confirmed completed flips, including repeated video images.
    // A successful Vulkan present call alone does not establish scanout.
    bool scanoutValid = false;
    uint32_t scanoutFps = 0;
    uint32_t presentFps = 0;
    uint32_t newVideoFps = 0;
    uint32_t confirmedUniqueVideoFps = 0;
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
    bool videoActive = false;
    const xc::ui::Canvas* gpuUi = nullptr;
    uint32_t targetFps = 60;
    uint32_t rcasSharpness = 2; // 0..4, half-stop attenuation each
};

// Reports whether the CPU canvas changed and whether a newly decoded video
// frame was copied into it before the next Vulkan presentation.
using CanvasFrameUpdate = CanvasFrameUpdateResult (*)(uint32_t* rgba, void* userData,
                                                       const PresentationStats& previousWindow,
                                                       bool gpuVideoEnabled);
using Nv12FrameUpload = bool (*)(void* userData, uint8_t* destination, size_t capacity,

// Starts Mesa Orbis WSI through VK_EXT_headless_surface and keeps presenting
// the supplied RGBA UI canvas. When the driver supports the required Vulkan
// formats, decoded NV12 frames are uploaded as planes and converted/scaled in
// a fragment shader; otherwise the callback may keep using the CPU fallback.
[[noreturn]] void presentCanvas(uint32_t* rgba, uint32_t width, uint32_t height,
                                CanvasFrameUpdate update = nullptr,
                                Nv12FrameUpload uploadVideo = nullptr, void* userData = nullptr);

} // namespace xc::ps4
