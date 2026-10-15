// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#include "video_decoder.h"

#include <orbis/GnmDriver.h>
#include <orbis/Sysmodule.h>
#include <orbis/_types/kernel.h>
#include <orbis/_types/sysmodule.h>
#include <orbis/libkernel.h>
#include <sched.h>

#include <algorithm>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>

namespace xc::ps4 {
namespace {


std::atomic<void (*)(const char*)> gVideoDiagnosticSink{nullptr};

constexpr uint32_t kAvc = 1;
constexpr uint32_t kEmbeddedResource = 1;
constexpr uint32_t kMaxWidth = 1920;
constexpr uint32_t kMaxHeight = 1088;
constexpr size_t kMaxAccessUnitBytes = 16u * 1024u * 1024u;
constexpr uint64_t kDmemAlign = 16u * 1024u;
constexpr uint32_t kConversionBandRows = 8;
// Skip only CPU publication when four or more complete access units are
// already queued. V44's threshold of two discarded most displayed pictures
// during the user's first cloud-video segment.
constexpr size_t kPublishBacklogThreshold = 4;

struct YuvLookup {
    int32_t y[256]{};
    int32_t redV[256]{};
    int32_t greenU[256]{};
    int32_t greenV[256]{};
    int32_t blueU[256]{};
};


void initializeYuvLookup() {
    }
}

uint64_t elapsedUs(std::chrono::steady_clock::time_point start) {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - start).count());
}

using Queue = void*;
using Decoder = void*;

struct ComputeConfigInfo {
    uint64_t thisSize;
    uint16_t pipeId;
    uint16_t queueId;
    bool checkMemoryType;
    uint8_t reserved0;
    uint16_t reserved1;
};
struct ComputeMemoryInfo { uint64_t thisSize, size; void* address; };
struct DecoderConfigInfo {
    uint64_t thisSize;
    uint32_t resourceType, codecType, profile, maxLevel;
    int32_t maxWidth, maxHeight, maxDpbFrames;
    uint32_t pipelineDepth;
    Queue computeQueue;
    uint64_t cpuAffinityMask;
    int32_t cpuPriority;
    bool optimizeProgressive;
    bool checkMemoryType;
    uint8_t reserved0, reserved1;
    void* extraConfigInfo;
};
struct DecoderMemoryInfo {
    uint64_t thisSize, cpuSize;
    void* cpu;
    uint64_t gpuSize;
    void* gpu;
    uint64_t sharedSize;
    void* shared;
    uint64_t maxFrameBufferSize;
    uint32_t frameBufferAlignment, reserved0;
};
struct InputData { uint64_t thisSize; void* bytes; uint64_t size, pts, dts, attached; };
struct OutputInfo {
    uint64_t thisSize;
    bool valid, error;
    uint8_t pictureCount;
    uint32_t codec, width, pitch, height;
    void* frameBuffer;
    uint64_t frameBufferSize;
    uint32_t format, pitchBytes;
};
struct FrameBuffer { uint64_t thisSize; void* address; uint64_t size; bool accepted; };

static_assert(sizeof(ComputeConfigInfo) == 0x10);
static_assert(sizeof(ComputeMemoryInfo) == 0x18);
static_assert(sizeof(DecoderConfigInfo) == 0x48);
static_assert(sizeof(DecoderMemoryInfo) == 0x48);
static_assert(sizeof(InputData) == 0x30);
static_assert(sizeof(OutputInfo) == 0x38);
static_assert(sizeof(FrameBuffer) == 0x20);

using QueryCompute = int32_t (*)(ComputeMemoryInfo*);
using AllocateQueue = int32_t (*)(const ComputeConfigInfo*, const ComputeMemoryInfo*, Queue*);
using ReleaseQueue = int32_t (*)(Queue);
using QueryDecoder = int32_t (*)(const DecoderConfigInfo*, DecoderMemoryInfo*);
using CreateDecoder = int32_t (*)(const DecoderConfigInfo*, const DecoderMemoryInfo*, Decoder*);
using DeleteDecoder = int32_t (*)(Decoder);
using DecodeFn = int32_t (*)(Decoder, const InputData*, FrameBuffer*, OutputInfo*);
using FlushFn = int32_t (*)(Decoder, FrameBuffer*, OutputInfo*);
using ResetFn = int32_t (*)(Decoder);

struct Api {
    QueryCompute queryCompute = nullptr;
    AllocateQueue allocateQueue = nullptr;
    ReleaseQueue releaseQueue = nullptr;
    QueryDecoder queryDecoder = nullptr;
    CreateDecoder createDecoder = nullptr;
    DeleteDecoder deleteDecoder = nullptr;
    DecodeFn decode = nullptr;
    FlushFn flush = nullptr;
    ResetFn reset = nullptr;
};
struct Block { void* address = nullptr; off_t offset = 0; size_t size = 0; bool direct = false; };

uint64_t alignUp(uint64_t value, uint64_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

void logLine(const char* format, ...) {
    va_list args;
    va_start(args, format);
    std::vsnprintf(body, sizeof(body), format, args);
    va_end(args);
    if (const auto sink = gVideoDiagnosticSink.load(std::memory_order_acquire)) {
        sink(body);
        return;
    }
    std::snprintf(line, sizeof(line), "[%s] [XCLOUD-PS4-VIDEO] %s\n", kBuild, body);
    sceKernelDebugOutText(0, line);
    std::fputs(line, stdout);
    std::fflush(stdout);
}

int configureWorkerAffinity(const char* role) {
    // The SDK POSIX affinity/getcpu entry points in V48 are ENOSYS stubs.
    // Preserve inherited scheduling until a native set-affinity ABI is verified.
    const int cpu = sceKernelGetCurrentCpu();
    logLine("thread_affinity role=%s policy=inherited cpu=%d cpu_api=sceKernelGetCurrentCpu posix_stubs=disabled", role, cpu);
    return cpu;
}

bool allocateDirect(Block& block, uint64_t requested, int32_t type, int32_t protection) {
    const uint64_t size = alignUp(requested, kDmemAlign);
    off_t offset = 0;
    int32_t rc = sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(),
                                                size, kDmemAlign, type, &offset);
    if (rc != 0) {
        logLine("direct memory allocation failed bytes=%llu type=0x%x rc=0x%08x",
                static_cast<unsigned long long>(size), static_cast<unsigned>(type), static_cast<unsigned>(rc));
        return false;
    }
    void* address = nullptr;
    rc = sceKernelMapDirectMemory(&address, size, protection, 0, offset, kDmemAlign);
    if (rc != 0 || !address) {
        sceKernelReleaseDirectMemory(offset, size);
        logLine("direct memory map failed bytes=%llu rc=0x%08x",
                static_cast<unsigned long long>(size), static_cast<unsigned>(rc));
        return false;
    }
    block = {address, offset, static_cast<size_t>(size), true};
    std::memset(address, 0, block.size);
    return true;
}

bool allocateFlexible(Block& block, uint64_t requested) {
    const size_t size = static_cast<size_t>(alignUp(requested, kDmemAlign));
    void* address = nullptr;
    const int32_t rc = sceKernelMapNamedFlexibleMemory(&address, size, VM_PROT_RW, 0, "XcloudVdecCpu");
    if (rc != 0 || !address) {
        logLine("flexible memory allocation failed bytes=%zu rc=0x%08x", size, static_cast<unsigned>(rc));
        return false;
    }
    block = {address, 0, size, false};
    std::memset(address, 0, size);
    return true;
}

void release(Block& block) {
    if (!block.address) return;
    sceKernelMunmap(block.address, block.size);
    if (block.direct) sceKernelReleaseDirectMemory(block.offset, block.size);
    block = {};
}

bool resolveSymbol(int32_t module, const char* name, void** out) {
    *out = nullptr;
    return sceKernelDlsym(module, name, out) == 0 && *out != nullptr;
}

bool resolveApi(int32_t module, Api& api) {
    return resolveSymbol(module, "sceVideodec2QueryComputeMemoryInfo", reinterpret_cast<void**>(&api.queryCompute)) &&
           resolveSymbol(module, "sceVideodec2AllocateComputeQueue", reinterpret_cast<void**>(&api.allocateQueue)) &&
           resolveSymbol(module, "sceVideodec2ReleaseComputeQueue", reinterpret_cast<void**>(&api.releaseQueue)) &&
           resolveSymbol(module, "sceVideodec2QueryDecoderMemoryInfo", reinterpret_cast<void**>(&api.queryDecoder)) &&
           resolveSymbol(module, "sceVideodec2CreateDecoder", reinterpret_cast<void**>(&api.createDecoder)) &&
           resolveSymbol(module, "sceVideodec2DeleteDecoder", reinterpret_cast<void**>(&api.deleteDecoder)) &&
           resolveSymbol(module, "sceVideodec2Decode", reinterpret_cast<void**>(&api.decode)) &&
           resolveSymbol(module, "sceVideodec2Flush", reinterpret_cast<void**>(&api.flush)) &&
           resolveSymbol(module, "sceVideodec2Reset", reinterpret_cast<void**>(&api.reset));
}

bool findDecoderModule(int32_t& module) {
    if (sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_VDECCORE) < 0 ||
        sceSysmoduleLoadModule(ORBIS_SYSMODULE_VIDEODEC2) < 0) {
        logLine("Videodec2 modules failed to load");
        return false;
    }
    OrbisKernelModule modules[256]{};
    size_t count = 0;
    const int32_t rc = sceKernelGetModuleList(modules, 256, &count);
    if (rc != 0 || count > 256) {
        logLine("sceKernelGetModuleList failed rc=0x%08x count=%zu", static_cast<unsigned>(rc), count);
        return false;
    }
    for (size_t i = 0; i < count; ++i) {
        OrbisKernelModuleInfo info{};
        info.size = sizeof(info);
        if (sceKernelGetModuleInfo(modules[i], &info) == 0 && std::strstr(info.name, "Videodec2")) {
            module = static_cast<int32_t>(modules[i]);
            logLine("Videodec2 module resolved name=%s handle=0x%x", info.name, static_cast<unsigned>(module));
            return true;
        }
    }
    logLine("Videodec2 module not present after load");
    return false;
}

uint8_t clampByte(int value) {
    return static_cast<uint8_t>(std::clamp(value, 0, 255));
}

} // namespace

void setVideoDiagnosticSink(void (*sink)(const char*)) {
    gVideoDiagnosticSink.store(sink, std::memory_order_release);
}

struct DecoderState {
    Api api{};
    Queue queue = nullptr;
    Decoder decoder = nullptr;
    DecoderConfigInfo config{};
    uint64_t pts = 1;
    std::map<uint32_t, ParameterSet> sps, pps;
    bool waitingIdr = false;
    bool recoveryPending = false;
    uint64_t lastResetUs = 0;
    uint64_t resetCount = 0;
};

namespace {
struct AccessUnitInfo {
    bool idr = false;
    bool sps = false;
    bool pps = false;
    bool validPpsId = false;
    uint32_t ppsId = 0;
};

// Only the leading syntax fields are needed to match parameter-set IDs.
// Remove emulation-prevention bytes before reading Exp-Golomb fields.
struct NalHeaderReader {
    std::vector<uint8_t> rbsp;
    size_t bit = 0;
    NalHeaderReader(const uint8_t* data, size_t size) {
        unsigned zeros = 0;
        for (size_t i = 0; i < size && rbsp.size() < 64; ++i) {
            if (zeros >= 2 && data[i] == 3) { zeros = 0; continue; }
            rbsp.push_back(data[i]);
            zeros = data[i] == 0 ? zeros + 1 : 0;
        }
    }
    bool ue(uint32_t& value) {
        unsigned zeros = 0;
        while (bit < rbsp.size() * 8 && ((rbsp[bit / 8] >> (7 - bit % 8)) & 1u) == 0) {
            ++bit;
            if (++zeros > 31) return false;
        }
        if (bit >= rbsp.size() * 8) return false;
        ++bit;
        uint32_t suffix = 0;
        for (unsigned i = 0; i < zeros; ++i) {
            if (bit >= rbsp.size() * 8) return false;
            suffix = (suffix << 1) | ((rbsp[bit / 8] >> (7 - bit % 8)) & 1u);
            ++bit;
        }
        value = ((uint32_t{1} << zeros) - 1u) + suffix;
        return true;
    }
};

AccessUnitInfo cacheParameterSets(const std::vector<uint8_t>& bytes, DecoderState& state) {
    AccessUnitInfo info;
    auto startCode = [&bytes](size_t at) -> size_t {
        if (at + 3 <= bytes.size() && bytes[at] == 0 && bytes[at + 1] == 0) {
            if (bytes[at + 2] == 1) return 3;
            if (at + 4 <= bytes.size() && bytes[at + 2] == 0 && bytes[at + 3] == 1) return 4;
        }
        return 0;
    };
    for (size_t i = 0; i + 3 <= bytes.size();) {
        const size_t prefix = startCode(i);
        if (!prefix) { ++i; continue; }
        const size_t begin = i + prefix;
        size_t end = begin;
        while (end < bytes.size() && !startCode(end)) ++end;
        i = end;
        if (begin >= end) continue;
        const uint8_t type = bytes[begin] & 31u;
        NalHeaderReader reader(bytes.data() + begin + 1, end - begin - 1);
        uint32_t id = 0, parent = 0;
            uint32_t firstMb = 0, sliceType = 0;
            if (reader.ue(firstMb) && reader.ue(sliceType) && reader.ue(id) && id <= 255) {
                info.validPpsId = true;
                info.ppsId = id;
            }
            continue;
        }
        if (end - begin > 4096) continue;
        if (type == 7) reader.bit = 24; // profile_idc, constraints, level_idc
        if (!reader.ue(id) || (type == 7 ? id > 31 : id > 255)) continue;
        if (type == 8 && (!reader.ue(parent) || parent > 31)) continue;
        DecoderState::ParameterSet entry;
        entry.bytes = {0, 0, 0, 1};
        entry.bytes.insert(entry.bytes.end(), bytes.begin() + begin, bytes.begin() + end);
        entry.spsId = type == 7 ? id : parent;
        else { state.pps[id] = std::move(entry); info.pps = true; }
    }
    return info;
}

bool prepareRecoveryUnit(const std::vector<uint8_t>& source, const AccessUnitInfo& info,
                         const DecoderState& state, std::vector<uint8_t>& result) {
    if (!info.idr || !info.validPpsId) return false;
    const auto pps = state.pps.find(info.ppsId);
    if (pps == state.pps.end()) return false;
    const auto sps = state.sps.find(pps->second.spsId);
    if (sps == state.sps.end()) return false;
    result.clear();
    result.insert(result.end(), sps->second.bytes.begin(), sps->second.bytes.end());
    result.insert(result.end(), pps->second.bytes.begin(), pps->second.bytes.end());
    result.insert(result.end(), source.begin(), source.end());
    return true;
}
} // namespace


VideoDecoder::VideoDecoder() = default;
VideoDecoder::~VideoDecoder() { stop(); }

bool VideoDecoder::start() {
    std::unique_lock<std::mutex> lock(mutex_);
    if (started_) return workerInitOk_;
    stopRequested_ = false;
    resetRequested_ = false;
    keyframeNeeded_ = false;
    workerReady_ = false;
    workerInitOk_ = false;
    started_ = true;
    queue_.clear();
    latestRgba_.clear();
    rgbaScratch_.clear();
    latestSequence_ = lastCopiedSequence_ = 0;
    latestWidth_ = latestHeight_ = 0;
    receivedAccessUnits_ = acceptedAccessUnits_ = dequeuedAccessUnits_ = failedAccessUnits_ = 0;
    emptyOutputs_ = totalDecoded_ = totalPublished_ = 0;
    lastInputTimeUs_ = lastOutputTimeUs_ = 0;
    decodedFrames_ = 0;
    publishedFrames_ = 0;
    skippedOutputFrames_ = 0;
    decodedFramesInFirst10s_ = 0;
    droppedAccessUnits_ = 0;
    rejectedInvalidAccessUnits_ = 0;
    decodeTimeUsWindow_ = 0;
    queueWaitTimeUsWindow_ = 0;
    queueWaitSamplesWindow_ = 0;
    queueWaitMaxUsWindow_ = 0;
    convertTimeUsWindow_ = 0;
    outputFramesInWindow_ = 0;
    publishedFramesInWindow_ = 0;
    maxQueueDepthInWindow_ = 0;
    performanceWindowStartedAt_ = std::chrono::steady_clock::now();
    performanceWindowWidth_ = 0;
    performanceWindowHeight_ = 0;
    decoderLastCpu_.store(-1, std::memory_order_relaxed);
    conversionCallerLastCpu_.store(-1, std::memory_order_relaxed);
    for (auto& cpu : conversionLastCpus_) cpu.store(-1, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> performanceLock(performanceMutex_);
        performanceSnapshot_ = {};
    }
    hasFirstMediaAt_ = false;
    firstDecodedStageLogged_ = false;
    first10sStageLogged_ = false;
    lock.unlock();

    initializeYuvLookup();
    {
        std::lock_guard<std::mutex> conversionLock(conversionMutex_);
        conversionShutdown_ = false;
        conversionPending_ = 0;
        conversionGeneration_ = 0;
    }
    conversionWorkerCount_ = 0;
        conversionWorkerContexts_[i] = {this, i};
        const int rc = pthread_create(&conversionWorkers_[i], nullptr,
                                      &VideoDecoder::conversionWorkerEntry,
                                      &conversionWorkerContexts_[i]);
        if (rc != 0) {
            logLine("frame_converter worker_create_failed index=%zu rc=%d", i, rc);
            break;
        }
        ++conversionWorkerCount_;
    }
    logLine("frame_converter mode=adaptive_nv12_to_rgba workers=%zu participants=%zu backlog_skip_at=%zu",
            conversionWorkerCount_, conversionWorkerCount_ + 1u, kPublishBacklogThreshold);

    lock.lock();
    worker_ = std::thread(&VideoDecoder::run, this);
    wake_.wait(lock, [this] { return workerReady_; });
    const bool initOk = workerInitOk_;
    lock.unlock();
    if (!initOk) {
        logLine("STAGE L6 FAIL event=decoder_init");
        stopConversionWorkers();
    }
    return initOk;
}

    resetRequested_.store(true, std::memory_order_release);
    wake_.notify_one();
}

bool VideoDecoder::takeKeyframeRequest() {
    return keyframeNeeded_.exchange(false, std::memory_order_acq_rel);
}

bool VideoDecoder::resetDecoder(const char* reason) {
    auto* state = static_cast<DecoderState*>(decoderState_.get());
    if (!state || !state->decoder || !state->api.reset) return false;
    const uint64_t started = sceKernelGetProcessTime();
    const int32_t rc = state->api.reset(state->decoder);
    const int32_t submitRc = sceGnmSubmitDone();
    state->lastResetUs = started;
    state->waitingIdr = true;
    state->recoveryPending = true;
    keyframeNeeded_.store(true, std::memory_order_release);
    ++state->resetCount;
    logLine("decoder_reset reason=%s count=%llu rc=0x%08x submit_done=0x%08x elapsed_us=%llu action=wait_idr",
            reason, static_cast<unsigned long long>(state->resetCount), static_cast<unsigned>(rc),
            static_cast<unsigned>(submitRc), static_cast<unsigned long long>(sceKernelGetProcessTime() - started));
    return rc == 0 && submitRc == 0;
}

void VideoDecoder::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!started_) return;
        stopRequested_ = true;
    }
    wake_.notify_all();
    if (worker_.joinable()) worker_.join();
    stopConversionWorkers();
    bool reportIncompleteWindow = false;
    uint64_t framesInWindow = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        started_ = false;
        queue_.clear();
        reportIncompleteWindow = hasFirstMediaAt_ && !first10sStageLogged_;
        framesInWindow = decodedFramesInFirst10s_;
    }
    if (reportIncompleteWindow)
        logLine("STAGE L6 TIMEOUT event=first_10s_window_incomplete decoded_frames=%llu",
                static_cast<unsigned long long>(framesInWindow));
}

bool VideoDecoder::pushAccessUnit(const uint8_t* data, size_t size, uint32_t timestamp) {
    ++receivedAccessUnits_;
    if (!data || size == 0 || size > kMaxAccessUnitBytes) {
        const uint64_t rejected = ++rejectedInvalidAccessUnits_;
        if (rejected == 1 || rejected % 120 == 0)
            logLine("STAGE L6 FAIL event=access_unit_rejected reason=invalid_size rejected=%llu bytes=%zu",
                    static_cast<unsigned long long>(rejected), size);
        return false;
    }
    bool firstAccepted = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!started_ || stopRequested_ || !workerInitOk_) return false;
            return false;
        }
        AccessUnit unit;
        unit.bytes.assign(data, data + size);
        unit.rtpTimestamp = timestamp;
        unit.enqueuedAtUs = sceKernelGetProcessTime();
        queue_.push_back(std::move(unit));
        ++acceptedAccessUnits_;
        lastInputTimeUs_.store(sceKernelGetProcessTime(), std::memory_order_relaxed);
        if (!hasFirstMediaAt_) {
            firstMediaAt_ = std::chrono::steady_clock::now();
            hasFirstMediaAt_ = true;
            firstAccepted = true;
        }
    }
    wake_.notify_one();
    if (firstAccepted)
        logLine("STAGE L6 OK event=first_h264_access_unit bytes=%zu rtp_ts=%u", size, timestamp);
    return true;
}

}

VideoPerformanceSnapshot VideoDecoder::performanceSnapshot() const {
    VideoPerformanceSnapshot result;
    {
    }
    result.decoderCpu = decoderLastCpu_.load(std::memory_order_relaxed);
    result.conversionCallerCpu = conversionCallerLastCpu_.load(std::memory_order_relaxed);
    for (size_t i = 0; i < conversionLastCpus_.size(); ++i)
        result.converterCpus[i] = conversionLastCpus_[i].load(std::memory_order_relaxed);
    result.receivedAccessUnits = receivedAccessUnits_.load(std::memory_order_relaxed);
    result.acceptedAccessUnits = acceptedAccessUnits_.load(std::memory_order_relaxed);
    result.dequeuedAccessUnits = dequeuedAccessUnits_.load(std::memory_order_relaxed);
    result.failedAccessUnits = failedAccessUnits_.load(std::memory_order_relaxed);
    result.droppedAccessUnits = droppedAccessUnits_.load(std::memory_order_relaxed);
    result.emptyOutputs = emptyOutputs_.load(std::memory_order_relaxed);
    result.totalDecoded = totalDecoded_.load(std::memory_order_relaxed);
    result.totalPublished = totalPublished_.load(std::memory_order_relaxed);
    const uint64_t now = sceKernelGetProcessTime();
    const auto input = lastInputTimeUs_.load(std::memory_order_relaxed);
    const auto output = lastOutputTimeUs_.load(std::memory_order_relaxed);
    result.lastInputAgeMs = input && now >= input ? (now - input) / 1000u : 0;
    result.lastOutputAgeMs = output && now >= output ? (now - output) / 1000u : 0;
    {
    }
    return result;
}

    if (!destination || !width || !height) return false;
    uint32_t sourceWidth = 0;
    uint32_t sourceHeight = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (latestSequence_ == 0 || latestSequence_ == lastCopiedSequence_ || latestRgba_.empty()) return false;
        sourceWidth = latestWidth_;
        sourceHeight = latestHeight_;
        if (!sourceWidth || !sourceHeight) return false;
        const size_t sourcePixels = static_cast<size_t>(sourceWidth) * sourceHeight;
        latestCopyScratch_.resize(sourcePixels);
        std::memcpy(latestCopyScratch_.data(), latestRgba_.data(), sourcePixels * sizeof(uint32_t));
        lastCopiedSequence_ = latestSequence_;
    }

    const uint32_t* source = latestCopyScratch_.data();
    if (sourceWidth == width && sourceHeight == height) {
        std::memcpy(destination, source, static_cast<size_t>(width) * height * sizeof(uint32_t));
        return true;
    }

    std::fill(destination, destination + static_cast<size_t>(width) * height, 0xff000000u);
    uint32_t outWidth = width;
    uint32_t outHeight = height;
        outHeight = static_cast<uint32_t>(static_cast<uint64_t>(sourceHeight) * width / sourceWidth);
        outWidth = static_cast<uint32_t>(static_cast<uint64_t>(sourceWidth) * height / sourceHeight);
    outWidth = std::max(1u, outWidth);
    outHeight = std::max(1u, outHeight);
    const uint32_t x0 = (width - outWidth) / 2;
    const uint32_t y0 = (height - outHeight) / 2;

    if (scaleSourceWidth_ != sourceWidth || scaleSourceHeight_ != sourceHeight ||
        scaleX_.resize(outWidth);
        scaleY_.resize(outHeight);
        for (uint32_t x = 0; x < outWidth; ++x)
                static_cast<uint32_t>(static_cast<uint64_t>(x) * sourceWidth / outWidth));
        for (uint32_t y = 0; y < outHeight; ++y)
                static_cast<uint32_t>(static_cast<uint64_t>(y) * sourceHeight / outHeight));
        scaleSourceWidth_ = sourceWidth;
        scaleSourceHeight_ = sourceHeight;
        scaleOutputWidth_ = outWidth;
        scaleOutputHeight_ = outHeight;
    }

    for (uint32_t y = 0; y < outHeight; ++y) {
        const uint32_t sy = scaleY_[y];
        const uint32_t* src = source + static_cast<size_t>(sy) * sourceWidth;
        uint32_t* dst = destination + static_cast<size_t>(y0 + y) * width + x0;
        for (uint32_t x = 0; x < outWidth; ++x) dst[x] = src[scaleX_[x]];
    }
    return true;
}

    auto state = std::make_unique<DecoderState>();
    int32_t module = -1;
    if (!findDecoderModule(module) || !resolveApi(module, state->api)) {
        logLine("Videodec2 API resolution failed");
        return false;
    }
    ComputeMemoryInfo computeInfo{};
    computeInfo.thisSize = sizeof(computeInfo);
    int32_t rc = state->api.queryCompute(&computeInfo);
    if (rc != 0 || computeInfo.size == 0 || !allocateDirect(state->compute, computeInfo.size,
            ORBIS_KERNEL_WB_ONION, ORBIS_KERNEL_PROT_CPU_RW | ORBIS_KERNEL_PROT_GPU_RW)) {
        logLine("Videodec2 compute memory setup failed rc=0x%08x size=%llu",
                static_cast<unsigned>(rc), static_cast<unsigned long long>(computeInfo.size));
        return false;
    }
    computeInfo.address = state->compute.address;

    ComputeConfigInfo queueConfig{};
    queueConfig.thisSize = sizeof(queueConfig);
    queueConfig.checkMemoryType = true;
    rc = state->api.allocateQueue(&queueConfig, &computeInfo, &state->queue);
    if (rc != 0 || !state->queue) {
        logLine("Videodec2 compute queue failed rc=0x%08x", static_cast<unsigned>(rc));
        return false;
    }

    state->config.thisSize = sizeof(state->config);
    state->config.resourceType = kEmbeddedResource;
    state->config.codecType = kAvc;
    state->config.profile = 66; // PS4 probe path validated Baseline H.264.
    state->config.maxLevel = 52;
    state->config.maxWidth = kMaxWidth;
    state->config.maxHeight = kMaxHeight;
    state->config.maxDpbFrames = 4;
    state->config.computeQueue = state->queue;
    state->config.cpuAffinityMask = 0x3f;
    state->config.cpuPriority = 700;
    state->config.optimizeProgressive = true;

    DecoderMemoryInfo memory{};
    memory.thisSize = sizeof(memory);
    rc = state->api.queryDecoder(&state->config, &memory);
    if (rc != 0 || memory.maxFrameBufferSize == 0) {
        logLine("Videodec2 decoder memory query failed rc=0x%08x", static_cast<unsigned>(rc));
        return false;
    }
        allocateDirect(state->gpu, memory.gpuSize, ORBIS_KERNEL_WC_GARLIC,
                       ORBIS_KERNEL_PROT_CPU_WRITE | ORBIS_KERNEL_PROT_GPU_RW) &&
        allocateDirect(state->shared, memory.sharedSize, ORBIS_KERNEL_WB_ONION,
                       ORBIS_KERNEL_PROT_CPU_RW | ORBIS_KERNEL_PROT_GPU_RW);
    if (!allocationsOk) {
        return false;
    }
    memory.cpu = state->cpu.address;
    memory.gpu = state->gpu.address;
    memory.shared = state->shared.address;
    rc = state->api.createDecoder(&state->config, &memory, &state->decoder);
    if (rc != 0 || !state->decoder) {
        logLine("Videodec2 create decoder failed rc=0x%08x", static_cast<unsigned>(rc));
        return false;
    }
    decoderState_ = std::move(state);
    return true;
}

bool VideoDecoder::decode(const AccessUnit& accessUnit, bool publishOutput, size_t queueDepth) {
    auto* state = static_cast<DecoderState*>(decoderState_.get());
    const AccessUnitInfo unitInfo = cacheParameterSets(accessUnit.bytes, *state);
    std::vector<uint8_t> recoveryUnit;
    const std::vector<uint8_t>* bytes = &accessUnit.bytes;
        bytes = &recoveryUnit;
    }
    const auto decodeStart = std::chrono::steady_clock::now();
    InputData input{};
    input.thisSize = sizeof(input);
    input.size = bytes->size();
    input.pts = state->pts;
    input.dts = state->pts++;
    frame.accepted = false;
    OutputInfo output{};
    output.thisSize = sizeof(output);
    int32_t rc = state->api.decode(state->decoder, &input, &frame, &output);
    int32_t submitRc = sceGnmSubmitDone();
    if (static_cast<uint32_t>(rc) == 0x811d0300u) {
        // A new H.264 sequence requires a native reset. Never loop on the AU.
        const uint64_t now = sceKernelGetProcessTime();
        if (state->lastResetUs && now - state->lastResetUs < 1000000u) {
            state->waitingIdr = true;
            keyframeNeeded_.store(true, std::memory_order_release);
            return false;
        }
        logLine("decoder_sequence event=new_sequence rtp_ts=%u idr=%u sps=%u pps=%u pps_id=%u",
                accessUnit.rtpTimestamp, unitInfo.idr ? 1u : 0u, unitInfo.sps ? 1u : 0u,
                unitInfo.pps ? 1u : 0u, unitInfo.ppsId);
        if (!resetDecoder("new_sequence")) return false;
        if (!prepareRecoveryUnit(accessUnit.bytes, unitInfo, *state, recoveryUnit)) return true;
        input.size = recoveryUnit.size();
        frame.accepted = false;
        output = {};
        output.thisSize = sizeof(output);
        rc = state->api.decode(state->decoder, &input, &frame, &output);
        submitRc = sceGnmSubmitDone();
        logLine("decoder_sequence event=retry_idr rc=0x%08x accepted_framebuffer=%u",
                static_cast<unsigned>(rc), frame.accepted ? 1u : 0u);
    }
        if (failedAccessUnits_.load(std::memory_order_relaxed) % 120 == 0)
            logLine("Videodec2 decode failed rtp_ts=%u rc=0x%08x submit_done=0x%08x accepted=%u bytes=%zu",
                accessUnit.rtpTimestamp, static_cast<unsigned>(rc), static_cast<unsigned>(submitRc),
                frame.accepted ? 1u : 0u, accessUnit.bytes.size());
        return false;
    }
    if (!output.valid) {
        output = {};
        output.thisSize = sizeof(output);
        rc = state->api.flush(state->decoder, &frame, &output);
        const int32_t flushSubmitRc = sceGnmSubmitDone();
        if (rc != 0 || flushSubmitRc != 0) {
            if (failedAccessUnits_.load(std::memory_order_relaxed) % 120 == 0)
                logLine("Videodec2 flush failed rc=0x%08x submit_done=0x%08x",
                        static_cast<unsigned>(rc), static_cast<unsigned>(flushSubmitRc));
            return false;
        }
        if (!output.valid) {
            ++emptyOutputs_;
            return true;
        }
    }
    const uint32_t pitchBytes = output.pitchBytes ? output.pitchBytes : output.pitch;
    const size_t decodedFrameSize = output.frameBufferSize
    if (output.error || output.pictureCount == 0 || output.format != 0 ||
        output.width == 0 || output.width > kMaxWidth || output.height == 0 ||
        output.height > kMaxHeight || pitchBytes < output.width || !decodedFrame ||
        if (failedAccessUnits_.load(std::memory_order_relaxed) % 120 == 0)
            logLine("Videodec2 output rejected valid=%u error=%u pictures=%u format=%u size=%ux%u pitch=%u",
                output.valid ? 1u : 0u, output.error ? 1u : 0u, output.pictureCount,
                output.format, output.width, output.height, pitchBytes);
        return false;
    }
    if (state->recoveryPending) {
                static_cast<unsigned long long>(state->resetCount), output.width, output.height, accessUnit.rtpTimestamp);
        state->recoveryPending = false;
    }
    ++decodedFrames_;
    ++totalDecoded_;
    maxQueueDepthInWindow_ = std::max(maxQueueDepthInWindow_, queueDepth);
    const uint64_t decodeUs = elapsedUs(decodeStart);
    decodeTimeUsWindow_ += decodeUs;
    uint64_t convertUs = 0;
    if (publishOutput) {
        const auto convertStart = std::chrono::steady_clock::now();
        if (!publishNv12(decodedFrame, decodedFrameSize,
                         output.width, output.height, pitchBytes,
        convertUs = elapsedUs(convertStart);
        convertTimeUsWindow_ += convertUs;
        ++publishedFrames_;
        ++totalPublished_;
        lastOutputTimeUs_.store(sceKernelGetProcessTime(), std::memory_order_relaxed);
        ++publishedFramesInWindow_;
    } else {
        ++skippedOutputFrames_;
    }
    bool logFirstDecoded = false;
    bool logFirstTenSeconds = false;
    uint64_t firstTenSecondsCount = 0;
    int64_t mediaToFrameMs = -1;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!firstDecodedStageLogged_) {
            firstDecodedStageLogged_ = true;
            logFirstDecoded = true;
            if (hasFirstMediaAt_)
                mediaToFrameMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - firstMediaAt_).count();
        }
        if (hasFirstMediaAt_) {
            const auto elapsed = std::chrono::steady_clock::now() - firstMediaAt_;
            if (elapsed <= std::chrono::seconds(10))
                ++decodedFramesInFirst10s_;
            else if (!first10sStageLogged_) {
                first10sStageLogged_ = true;
                logFirstTenSeconds = true;
                firstTenSecondsCount = decodedFramesInFirst10s_;
            }
        }
    }
    if (logFirstDecoded)
        logLine("STAGE L6 OK event=first_decoded_frame elapsed_since_access_unit_ms=%lld size=%ux%u",
                static_cast<long long>(mediaToFrameMs), output.width, output.height);
    if (logFirstTenSeconds)
        logLine("STAGE L6 OK event=first_10s_summary decoded_frames=%llu published=%llu skipped=%llu",
                static_cast<unsigned long long>(firstTenSecondsCount),
                static_cast<unsigned long long>(publishedFrames_),
                static_cast<unsigned long long>(skippedOutputFrames_));
    if (++outputFramesInWindow_ >= 60) {
        const auto windowNow = std::chrono::steady_clock::now();
        const uint32_t windowMs = static_cast<uint32_t>(std::max<int64_t>(1,
            std::chrono::duration_cast<std::chrono::milliseconds>(
                windowNow - performanceWindowStartedAt_).count()));
        const uint32_t decodeAvgUs = static_cast<uint32_t>(decodeTimeUsWindow_ / outputFramesInWindow_);
        const uint32_t queueWaitAvgUs = queueWaitSamplesWindow_
            ? static_cast<uint32_t>(queueWaitTimeUsWindow_ / queueWaitSamplesWindow_) : 0;
        const uint32_t queueWaitMaxUs = static_cast<uint32_t>(queueWaitMaxUsWindow_);
        const uint32_t convertAvgUs = publishedFramesInWindow_
            ? static_cast<uint32_t>(convertTimeUsWindow_ / publishedFramesInWindow_) : 0;
        const uint32_t decodedFps = static_cast<uint32_t>(outputFramesInWindow_ * 1000u / windowMs);
        const uint32_t publishedFps = static_cast<uint32_t>(publishedFramesInWindow_ * 1000u / windowMs);
        performanceWindowWidth_ = output.width;
        performanceWindowHeight_ = output.height;
        logLine("video_pipeline decoded=%llu published=%llu skipped=%llu queue_depth=%zu window_ms=%u decode_fps=%u published_fps=%u queue_wait_avg_ms=%.3f queue_wait_max_ms=%.3f decode_avg_ms=%.3f convert_avg_ms=%.3f cpu_decoder=%d cpu_caller=%d cpu_converters=%d,%d,%d",
                static_cast<unsigned long long>(decodedFrames_),
                static_cast<unsigned long long>(publishedFrames_),
                static_cast<unsigned long long>(skippedOutputFrames_), queueDepth,
                windowMs, decodedFps, publishedFps,
                queueWaitAvgUs / 1000.0, queueWaitMaxUs / 1000.0,
                static_cast<double>(decodeTimeUsWindow_) / 60000.0,
                publishedFramesInWindow_ ? static_cast<double>(convertTimeUsWindow_) /
                    (static_cast<double>(publishedFramesInWindow_) * 1000.0) : 0.0,
                decoderLastCpu_.load(std::memory_order_relaxed),
                conversionCallerLastCpu_.load(std::memory_order_relaxed),
                conversionLastCpus_[0].load(std::memory_order_relaxed),
                conversionLastCpus_[1].load(std::memory_order_relaxed),
                conversionLastCpus_[2].load(std::memory_order_relaxed));
        {
            std::lock_guard<std::mutex> performanceLock(performanceMutex_);
            ++performanceSnapshot_.generation;
            performanceSnapshot_.decoded = static_cast<uint32_t>(outputFramesInWindow_);
            performanceSnapshot_.published = static_cast<uint32_t>(publishedFramesInWindow_);
            performanceSnapshot_.skipped = static_cast<uint32_t>(outputFramesInWindow_ - publishedFramesInWindow_);
            performanceSnapshot_.maxQueueDepth = static_cast<uint32_t>(maxQueueDepthInWindow_);
            performanceSnapshot_.decodeAvgUs = decodeAvgUs;
            performanceSnapshot_.queueWaitAvgUs = queueWaitAvgUs;
            performanceSnapshot_.queueWaitMaxUs = queueWaitMaxUs;
            performanceSnapshot_.convertAvgUs = convertAvgUs;
            performanceSnapshot_.windowDurationMs = windowMs;
            performanceSnapshot_.decodedFps = decodedFps;
            performanceSnapshot_.publishedFps = publishedFps;
            performanceSnapshot_.width = performanceWindowWidth_;
            performanceSnapshot_.height = performanceWindowHeight_;
            performanceSnapshot_.decoderCpu = decoderLastCpu_.load(std::memory_order_relaxed);
            performanceSnapshot_.conversionCallerCpu = conversionCallerLastCpu_.load(std::memory_order_relaxed);
            for (size_t i = 0; i < conversionLastCpus_.size(); ++i)
                performanceSnapshot_.converterCpus[i] = conversionLastCpus_[i].load(std::memory_order_relaxed);
        }
        outputFramesInWindow_ = 0;
        publishedFramesInWindow_ = 0;
        maxQueueDepthInWindow_ = 0;
        decodeTimeUsWindow_ = 0;
        queueWaitTimeUsWindow_ = 0;
        queueWaitSamplesWindow_ = 0;
        queueWaitMaxUsWindow_ = 0;
        convertTimeUsWindow_ = 0;
        performanceWindowStartedAt_ = windowNow;
    }
    if (decodedFrames_ == 1 || decodedFrames_ % 300 == 0)
        logLine("decoded_frames=%llu last_size=%ux%u rtp_ts=%u", static_cast<unsigned long long>(decodedFrames_),
                output.width, output.height, accessUnit.rtpTimestamp);
    return true;
}

bool VideoDecoder::publishNv12(const void* raw, size_t available, uint32_t width,
    if (!raw || format != 0 || pitch < width || paddedHeight < 16) return false;
    const uint64_t yBytes = static_cast<uint64_t>(pitch) * paddedHeight;
    const uint32_t visibleHeight = std::min(paddedHeight, 1080u);
    const uint64_t uvBytes = static_cast<uint64_t>(pitch) * ((visibleHeight + 1u) / 2u);
    if (yBytes > available || uvBytes > available - yBytes) {
        logLine("NV12 bounds invalid width=%u height=%u pitch=%u bytes=%zu", width, paddedHeight, pitch, available);
        return false;
    }
    const auto* src = static_cast<const uint8_t*>(raw);
    const uint8_t* uv = src + yBytes;
    rgbaScratch_.resize(static_cast<size_t>(width) * visibleHeight);
    conversionNextRow_.store(0, std::memory_order_relaxed);
    if (conversionWorkerCount_ == 0) {
        conversionCallerLastCpu_.store(sceKernelGetCurrentCpu(), std::memory_order_relaxed);
    } else {
        {
            std::lock_guard<std::mutex> lock(conversionMutex_);
            conversionYPlane_ = src;
            conversionUvPlane_ = uv;
            conversionWidth_ = width;
            conversionHeight_ = visibleHeight;
            conversionPitch_ = pitch;
            conversionDestination_ = rgbaScratch_.data();
            conversionPending_ = conversionWorkerCount_;
            ++conversionGeneration_;
        }
        conversionWake_.notify_all();
        conversionCallerLastCpu_.store(sceKernelGetCurrentCpu(), std::memory_order_relaxed);
        std::unique_lock<std::mutex> lock(conversionMutex_);
        conversionDone_.wait(lock, [this] { return conversionPending_ == 0; });
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        latestRgba_.swap(rgbaScratch_);
        latestWidth_ = width;
        latestHeight_ = visibleHeight;
        ++latestSequence_;
    }
    return true;
}

void VideoDecoder::convertRows(const uint8_t* yPlane, const uint8_t* uvPlane,
                               uint32_t width, uint32_t height, uint32_t pitch,
    for (;;) {
        const uint32_t first = conversionNextRow_.fetch_add(kConversionBandRows,
                                                              std::memory_order_relaxed);
        if (first >= height) break;
        const uint32_t last = std::min(height, first + kConversionBandRows);
        for (uint32_t y = first; y < last; ++y) {
            const uint8_t* yRow = yPlane + static_cast<size_t>(y) * pitch;
            uint32_t* out = destination + static_cast<size_t>(y) * width;
            }
        }
    }
}

void* VideoDecoder::conversionWorkerEntry(void* opaque) {
    auto* context = static_cast<ConversionWorkerContext*>(opaque);
    context->owner->conversionWorker(context->index);
    return nullptr;
}

void VideoDecoder::conversionWorker(size_t index) {
    char role[32];
    std::snprintf(role, sizeof(role), "frame-convert-%zu", index);
    conversionLastCpus_[index].store(configureWorkerAffinity(role), std::memory_order_relaxed);
    uint64_t seenGeneration = 0;
    std::unique_lock<std::mutex> lock(conversionMutex_);
    for (;;) {
        conversionWake_.wait(lock, [this, &seenGeneration] {
            return conversionShutdown_ || conversionGeneration_ != seenGeneration;
        });
        if (conversionShutdown_) break;
        seenGeneration = conversionGeneration_;
        const uint8_t* yPlane = conversionYPlane_;
        const uint8_t* uvPlane = conversionUvPlane_;
        const uint32_t width = conversionWidth_;
        const uint32_t height = conversionHeight_;
        const uint32_t pitch = conversionPitch_;
        uint32_t* destination = conversionDestination_;
        lock.unlock();
        if (index < conversionLastCpus_.size())
            conversionLastCpus_[index].store(sceKernelGetCurrentCpu(), std::memory_order_relaxed);
        lock.lock();
        if (--conversionPending_ == 0)
            conversionDone_.notify_one();
    }
}

void VideoDecoder::stopConversionWorkers() {
    if (conversionWorkerCount_ == 0) return;
    {
        std::lock_guard<std::mutex> lock(conversionMutex_);
        conversionShutdown_ = true;
    }
    conversionWake_.notify_all();
    for (size_t i = 0; i < conversionWorkerCount_; ++i)
        pthread_join(conversionWorkers_[i], nullptr);
    conversionWorkerCount_ = 0;
    {
        std::lock_guard<std::mutex> lock(conversionMutex_);
        conversionShutdown_ = false;
        conversionPending_ = 0;
        conversionGeneration_ = 0;
    }
}

void VideoDecoder::run() {
    decoderLastCpu_.store(configureWorkerAffinity("videodec-submit"), std::memory_order_relaxed);
    const bool initOk = initializeDecoder();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        workerInitOk_ = initOk;
        workerReady_ = true;
    }
    wake_.notify_all();
    if (!initOk) return;
    for (;;) {
        AccessUnit unit;
        size_t queueDepth = 0;
        uint64_t queueWaitUs = 0;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            if (resetRequested_.exchange(false, std::memory_order_acq_rel)) {
                droppedAccessUnits_.fetch_add(queue_.size(), std::memory_order_relaxed);
                queue_.clear();
                lock.unlock();
                continue;
            }
            ++dequeuedAccessUnits_;
            unit = std::move(queue_.front());
            queue_.pop_front();
            queueDepth = queue_.size();
            const uint64_t dequeuedAtUs = sceKernelGetProcessTime();
            queueWaitUs = dequeuedAtUs >= unit.enqueuedAtUs ? dequeuedAtUs - unit.enqueuedAtUs : 0;
        }
        queueWaitTimeUsWindow_ += queueWaitUs;
        ++queueWaitSamplesWindow_;
        queueWaitMaxUsWindow_ = std::max(queueWaitMaxUsWindow_, queueWaitUs);
        decoderLastCpu_.store(sceKernelGetCurrentCpu(), std::memory_order_relaxed);
        if (!decode(unit, publishOutput, queueDepth)) {
            const uint64_t errors = ++failedAccessUnits_;
            if (errors == 1 || errors % 120 == 0)
                logLine("STAGE L6 FAIL event=decode_access_unit errors=%llu rtp_ts=%u bytes=%zu queue_depth=%zu",
                        static_cast<unsigned long long>(errors), unit.rtpTimestamp, unit.bytes.size(), queueDepth);
        }
    }
    }
}

} // namespace xc::ps4
