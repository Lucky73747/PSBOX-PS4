// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "video_playout_queue.h"
#include "video_color.h"
#include <cstddef>
#include <cstdint>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <pthread.h>
#include <thread>
#include <vector>

namespace xc::ps4 {

struct DecoderState;
void setVideoDiagnosticSink(void (*sink)(const char*));

struct VideoPerformanceSnapshot {
    uint32_t snapshotBusy = 0;
    uint32_t stage = 0;
    uint64_t stageAgeMs = 0;
    uint64_t generation = 0;
    uint32_t decoded = 0;
    uint32_t published = 0;
    uint32_t skipped = 0;
    uint32_t maxQueueDepth = 0;
    uint32_t decodeAvgUs = 0;
    uint32_t queueWaitAvgUs = 0;
    uint32_t queueWaitMaxUs = 0;
    uint32_t convertAvgUs = 0;
    uint32_t windowDurationMs = 0;
    uint32_t decodedFps = 0;
    uint32_t publishedFps = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint64_t receivedAccessUnits = 0, acceptedAccessUnits = 0, dequeuedAccessUnits = 0, failedAccessUnits = 0;
    uint64_t catchupDroppedAccessUnits = 0;
    uint64_t droppedAccessUnits = 0, emptyOutputs = 0, totalDecoded = 0, totalPublished = 0;
    uint64_t lastInputAgeMs = 0, lastOutputAgeMs = 0;
    uint32_t queuedAccessUnits = 0;
    uint32_t playoutDepth = 0;
    uint64_t playoutUploaded = 0, playoutReplaced = 0, playoutRepeated = 0, playoutPrimeWaits = 0, playoutSequence = 0;
    int32_t decoderCpu = -1;
    int32_t conversionCallerCpu = -1;
    int32_t converterCpus[3] = {-1, -1, -1};
};

enum class VideoResetRequestReason : uint8_t { UserMasterButton = 1, AdaptiveWatchdog = 2 };

// PS4 Videodec2 adapter. Decoded pictures stay as NV12 for the Vulkan video
// compositor when enabled, or are converted to RGBA for the CPU fallback. This
// class never opens or submits a VideoOut buffer.
class VideoDecoder {
public:
    VideoDecoder();
    ~VideoDecoder();
    VideoDecoder(const VideoDecoder&) = delete;
    VideoDecoder& operator=(const VideoDecoder&) = delete;

    bool start();
    void requestReset(VideoResetRequestReason reason = VideoResetRequestReason::UserMasterButton);
    bool takeKeyframeRequest();
    void stop();
    bool pushAccessUnit(const uint8_t* data, size_t size, uint32_t rtpTimestamp);
    void setGpuVideoEnabled(bool enabled) { gpuVideoEnabled_.store(enabled, std::memory_order_relaxed); }
    void setPlayoutDelayFrames(uint32_t frames) { playoutDelayFrames_.store(frames ? 1 : 0); }
    void setPresentationFps(uint32_t fps) { presentationFps_.store(fps == 30 ? 30 : 60, std::memory_order_relaxed); }
    bool copyLatestToCanvas(uint32_t* destination, uint32_t width, uint32_t height,
                            bool native1to1 = false);
    bool copyLatestNv12To(uint8_t* destination, size_t capacity, uint32_t& width, uint32_t& height,
                          VideoColorInfo& color);
    bool hasFrame() const;
    VideoPerformanceSnapshot performanceSnapshot() const;

private:
    struct AccessUnit {
        std::vector<uint8_t> bytes;
        uint32_t rtpTimestamp = 0;
        uint64_t enqueuedAtUs = 0;
    };
    struct ConversionWorkerContext {
        VideoDecoder* owner = nullptr;
        size_t index = 0;
    };
    void run();
    void markStage(uint32_t stage);
    bool initializeDecoder(uint32_t pipelineDepth = 2);
    bool resetDecoder(const char* reason);
    bool decode(const AccessUnit& accessUnit, bool publishOutput, size_t queueDepth);
    bool publishNv12(const void* data, size_t available, uint32_t width,
                     uint32_t paddedHeight, uint32_t pitchBytes, uint32_t format,
                     const VideoColorInfo& color);
    void conversionWorker(size_t index);
    static void* conversionWorkerEntry(void* opaque);
    void convertRows(const uint8_t* yPlane, const uint8_t* uvPlane, uint32_t width,
                     uint32_t height, uint32_t pitch, uint32_t* destination,
                     const VideoColorInfo& color);
    void stopConversionWorkers();

    std::atomic<uint32_t> diagnosticStage_{0};
    std::atomic<uint64_t> diagnosticStageAtUs_{0};
    mutable std::mutex playoutMutex_;
    std::vector<uint8_t> nv12UploadScratch_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<AccessUnit> queue_;
    std::vector<uint32_t> latestRgba_;
    VideoPlayoutQueue playout_;
    std::atomic<uint32_t> presentationFps_{60}, playoutDelayFrames_{0};
    std::vector<uint8_t> nv12PublishScratch_;
    std::atomic<bool> gpuVideoEnabled_{false};
    std::vector<uint32_t> rgbaScratch_;
    // Snapshot copied under mutex, then scaled after releasing the decoder
    // lock so the publishing thread is not blocked by the 1080p canvas pass.
    std::vector<uint32_t> latestCopyScratch_;
    std::vector<uint32_t> scaleX_;
    std::vector<uint32_t> scaleY_;
    uint32_t scaleSourceWidth_ = 0;
    uint32_t scaleSourceHeight_ = 0;
    uint32_t scaleOutputWidth_ = 0;
    uint32_t scaleOutputHeight_ = 0;
    bool scaleNative1to1_ = false;
    uint32_t latestWidth_ = 0;
    uint32_t latestHeight_ = 0;
    std::atomic<uint64_t> latestSequence_{0};
    uint64_t lastCopiedSequence_ = 0;
    uint64_t decodedFrames_ = 0;
    uint64_t publishedFrames_ = 0;
    uint64_t skippedOutputFrames_ = 0;
    uint64_t decodedFramesInFirst10s_ = 0;
    std::atomic<uint64_t> droppedAccessUnits_{0}, catchupDroppedAccessUnits_{0};
    std::atomic<uint64_t> rejectedInvalidAccessUnits_{0};
    std::atomic<uint64_t> receivedAccessUnits_{0}, acceptedAccessUnits_{0}, dequeuedAccessUnits_{0}, failedAccessUnits_{0};
    std::atomic<uint64_t> emptyOutputs_{0}, totalDecoded_{0}, totalPublished_{0};
    std::atomic<uint64_t> lastInputTimeUs_{0}, lastOutputTimeUs_{0};
    std::atomic<bool> resetRequested_{false}, keyframeNeeded_{false};
    std::atomic<VideoResetRequestReason> resetRequestReason_{VideoResetRequestReason::UserMasterButton};
    std::atomic<bool> discontinuity_{false}, inputGap_{false};
    uint64_t recoveryRequestUs_ = 0;
    struct StageCost { uint64_t sum = 0, peak = 0, calls = 0; };
    std::array<StageCost, 7> stageCosts_{};
    uint64_t timedAccessUnits_ = 0;
    uint64_t decodeTimeUsWindow_ = 0;
    uint64_t queueWaitTimeUsWindow_ = 0;
    uint64_t queueWaitSamplesWindow_ = 0;
    uint64_t queueWaitMaxUsWindow_ = 0;
    uint64_t convertTimeUsWindow_ = 0;
    uint64_t outputFramesInWindow_ = 0;
    uint64_t publishedFramesInWindow_ = 0;
    size_t maxQueueDepthInWindow_ = 0;
    std::chrono::steady_clock::time_point performanceWindowStartedAt_{};
    uint32_t performanceWindowWidth_ = 0;
    uint32_t performanceWindowHeight_ = 0;
    std::atomic<int32_t> decoderLastCpu_{-1};
    std::atomic<int32_t> conversionCallerLastCpu_{-1};
    std::array<std::atomic<int32_t>, 3> conversionLastCpus_{};
    mutable std::mutex performanceMutex_;
    VideoPerformanceSnapshot performanceSnapshot_{};
    std::chrono::steady_clock::time_point firstMediaAt_{};
    bool hasFirstMediaAt_ = false;
    bool firstDecodedStageLogged_ = false;
    bool first10sStageLogged_ = false;
    bool started_ = false;
    bool stopRequested_ = false;
    bool workerReady_ = false;
    bool workerInitOk_ = false;
    std::thread worker_;
    std::array<pthread_t, 3> conversionWorkers_{};
    std::array<ConversionWorkerContext, 3> conversionWorkerContexts_{};
    size_t conversionWorkerCount_ = 0;
    std::mutex conversionMutex_;
    std::condition_variable conversionWake_;
    std::condition_variable conversionDone_;
    bool conversionShutdown_ = false;
    uint64_t conversionGeneration_ = 0;
    size_t conversionPending_ = 0;
    std::atomic<uint32_t> conversionNextRow_{0};
    const uint8_t* conversionYPlane_ = nullptr;
    const uint8_t* conversionUvPlane_ = nullptr;
    uint32_t conversionWidth_ = 0;
    uint32_t conversionHeight_ = 0;
    uint32_t conversionPitch_ = 0;
    uint32_t* conversionDestination_ = nullptr;
    VideoColorInfo conversionColor_{};
    std::unique_ptr<DecoderState> decoderState_;
};

} // namespace xc::ps4
