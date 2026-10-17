// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#include "video_decoder.h"
#include "video_queue_policy.h"
#include "video_decode_result.h"
#include "native_thread_cpu.h"
#include "video_surface_rotation.h"
#include <emmintrin.h>
#include <ctime>

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

constexpr size_t kColorMatrixCount = 3;
constexpr size_t kColorRangeCount = 2;
YuvLookup g_yuvLookup[kColorMatrixCount][kColorRangeCount];

void initializeYuvLookup() {
    struct Coefficients { int y, rV, gU, gV, bU; };
    static constexpr Coefficients kCoefficients[kColorMatrixCount][kColorRangeCount] = {
        {{298, 459, -55, -136, 541}, {256, 403, -48, -120, 475}}, // BT.709
        {{298, 409, -100, -208, 516}, {256, 359, -88, -183, 454}}, // BT.601
        {{298, 430, -48, -167, 548}, {256, 378, -42, -146, 482}}, // BT.2020
    };
    for (size_t matrix = 0; matrix < kColorMatrixCount; ++matrix) {
        for (size_t range = 0; range < kColorRangeCount; ++range) {
            const auto c = kCoefficients[matrix][range];
            auto& table = g_yuvLookup[matrix][range];
            for (int value = 0; value < 256; ++value) {
                const int luma = range == 0 ? std::max(0, value - 16) : value;
                const int chroma = value - 128;
                table.y[value] = c.y * luma + 128;
                table.redV[value] = c.rV * chroma;
                table.greenU[value] = c.gU * chroma;
                table.greenV[value] = c.gV * chroma;
                table.blueU[value] = c.bU * chroma;
            }
        }
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
    char body[1024];
    va_list args;
    va_start(args, format);
    std::vsnprintf(body, sizeof(body), format, args);
    va_end(args);
    if (const auto sink = gVideoDiagnosticSink.load(std::memory_order_acquire)) {
        sink(body);
        return;
    }
    char line[1200];
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

// Output surfaces are written by the native decoder and read by the CPU.
// Flush the initially dirty zero-filled allocation before any GPU ownership.
// On subsequent reads only clean CPU lines are invalidated.
void invalidateCpuView(const void* address, size_t bytes) {
    if (!bytes) return;
    const uintptr_t end = reinterpret_cast<uintptr_t>(address) + bytes;
    for (uintptr_t line = reinterpret_cast<uintptr_t>(address) & ~uintptr_t{63}; line < end; line += 64)
        _mm_clflush(reinterpret_cast<const void*>(line));
    _mm_mfence();
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
    Block compute{}, cpu{}, gpu{}, shared{};
    struct Slot {
        Block accessUnit{}, output{};
        bool pending = false;
        uint64_t submittedAtUs = 0;
        VideoColorInfo color{};
    };
    std::array<Slot, 4> slots{};
    size_t nextSurface = 0;
    // A successful call with no accepted framebuffer may still retain its AU.
    // Quarantine those inputs until native deletion/reset proves completion.
    std::vector<Block> retainedInputs;
    static constexpr size_t maxRetainedInputs = 4;
    bool injectRecoveryHeaders = false;
    uint64_t submitted = 0, deferred = 0, completed = 0;
    ~DecoderState() {
        // Preserve native deletion + SubmitDone before releasing in-flight
        // buffers. If either fails, retain memory rather than unmap live work.
        if (decoder) {
            const int32_t deleted = api.deleteDecoder(decoder);
            const int32_t submitted = sceGnmSubmitDone();
            if (deleted != 0 || submitted != 0) {
                logLine("decoder_release event=retain_memory delete_rc=0x%08x submit_rc=0x%08x",
                    static_cast<unsigned>(deleted), static_cast<unsigned>(submitted));
                return;
            }
        }
        if (queue) {
            const int32_t released = api.releaseQueue(queue);
            if (released != 0) {
                logLine("decoder_release event=retain_queue_memory rc=0x%08x", static_cast<unsigned>(released));
                return;
            }
        }
        for (auto& slot : slots) { release(slot.accessUnit); release(slot.output); }
        for (auto& input : retainedInputs) release(input);
        release(shared); release(gpu); release(cpu); release(compute);
    }
    uint64_t pts = 1;
    struct ParameterSet {
        std::vector<uint8_t> bytes;
        uint32_t spsId = 0;
        VideoColorInfo color{};
    };
    std::map<uint32_t, ParameterSet> sps, pps;
    VideoColorInfo activeColor{};
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
    uint32_t slices = 0;
    VideoColorInfo color{};
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

struct SpsBitReader {
    std::vector<uint8_t> rbsp;
    size_t bit = 0;

    SpsBitReader(const uint8_t* data, size_t size) {
        rbsp.reserve(size);
        unsigned zeros = 0;
        for (size_t i = 0; i < size; ++i) {
            if (zeros >= 2 && data[i] == 3) { zeros = 0; continue; }
            rbsp.push_back(data[i]);
            zeros = data[i] == 0 ? zeros + 1 : 0;
        }
    }
    bool bits(unsigned count, uint32_t& value) {
        if (count > 32 || bit + count > rbsp.size() * 8) return false;
        value = 0;
        for (unsigned i = 0; i < count; ++i, ++bit)
            value = (value << 1) | ((rbsp[bit / 8] >> (7 - bit % 8)) & 1u);
        return true;
    }
    bool flag(bool& value) {
        uint32_t v = 0;
        if (!bits(1, v)) return false;
        value = v != 0;
        return true;
    }
    bool ue(uint32_t& value) {
        unsigned leading = 0;
        uint32_t b = 0;
        while (true) {
            if (!bits(1, b)) return false;
            if (b) break;
            if (++leading > 31) return false;
        }
        uint32_t suffix = 0;
        if (leading && !bits(leading, suffix)) return false;
        value = ((uint32_t{1} << leading) - 1u) + suffix;
        return true;
    }
    bool se(int32_t& value) {
        uint32_t code = 0;
        if (!ue(code) || code > 0x7fffffffu) return false;
        const int64_t signedValue = (code & 1u) ? (static_cast<int64_t>(code) + 1) / 2
                                                : -static_cast<int64_t>(code) / 2;
        value = static_cast<int32_t>(signedValue);
        return true;
    }
};

bool skipScalingList(SpsBitReader& reader, unsigned count) {
    int32_t lastScale = 8, nextScale = 8;
    for (unsigned i = 0; i < count; ++i) {
        if (nextScale != 0) {
            int32_t delta = 0;
            if (!reader.se(delta)) return false;
            nextScale = (lastScale + delta + 256) % 256;
        }
        if (nextScale != 0) lastScale = nextScale;
    }
    return true;
}

bool parseSpsColor(const uint8_t* nal, size_t size, VideoColorInfo& color) {
    if (!nal || size < 5 || size > 4096) return false;
    SpsBitReader reader(nal + 1, size - 1);
    uint32_t profile = 0, ignored = 0, spsId = 0;
    if (!reader.bits(8, profile) || !reader.bits(16, ignored) || !reader.ue(spsId)) return false;
    (void)spsId;

    uint32_t chromaFormat = 1;
    if (profile == 100 || profile == 110 || profile == 122 || profile == 244 ||
        profile == 44 || profile == 83 || profile == 86 || profile == 118 ||
        profile == 128 || profile == 138 || profile == 139 || profile == 134 ||
        profile == 135 || profile == 144) {
        if (!reader.ue(chromaFormat) || chromaFormat > 3) return false;
        if (chromaFormat == 3 && !reader.bits(1, ignored)) return false;
        uint32_t bitDepth = 0;
        bool scalingMatrix = false;
        if (!reader.ue(bitDepth) || !reader.ue(bitDepth) || !reader.bits(1, ignored) ||
            !reader.flag(scalingMatrix)) return false;
        if (scalingMatrix) {
            const unsigned listCount = chromaFormat == 3 ? 12 : 8;
            for (unsigned i = 0; i < listCount; ++i) {
                bool present = false;
                if (!reader.flag(present)) return false;
                if (present && !skipScalingList(reader, i < 6 ? 16 : 64)) return false;
            }
        }
    }

    uint32_t value = 0, pocType = 0;
    if (!reader.ue(value) || !reader.ue(pocType) || pocType > 2) return false;
    if (pocType == 0) {
        if (!reader.ue(value)) return false;
    } else if (pocType == 1) {
        bool deltaAlwaysZero = false;
        int32_t signedValue = 0;
        if (!reader.flag(deltaAlwaysZero) || !reader.se(signedValue) || !reader.se(signedValue)) return false;
        (void)deltaAlwaysZero;
        uint32_t cycle = 0;
        if (!reader.ue(cycle) || cycle > 256) return false;
        for (uint32_t i = 0; i < cycle; ++i) if (!reader.se(signedValue)) return false;
    }
    bool flag = false;
    if (!reader.ue(value) || !reader.flag(flag) || !reader.ue(value) || !reader.ue(value) ||
        !reader.flag(flag)) return false;
    if (!flag && !reader.flag(flag)) return false;
    if (!reader.flag(flag)) return false; // direct_8x8_inference_flag
    bool cropping = false;
    if (!reader.flag(cropping)) return false;
    if (cropping) for (unsigned i = 0; i < 4; ++i) if (!reader.ue(value)) return false;
    bool vui = false;
    if (!reader.flag(vui)) return false;
    if (!vui) return true;

    bool present = false;
    if (!reader.flag(present)) return false; // aspect_ratio_info_present_flag
    if (present) {
        uint32_t aspect = 0;
        if (!reader.bits(8, aspect)) return false;
        if (aspect == 255 && !reader.bits(32, ignored)) return false;
    }
    if (!reader.flag(present)) return false; // overscan_info_present_flag
    if (present && !reader.flag(flag)) return false;
    if (!reader.flag(present)) return false; // video_signal_type_present_flag
    if (!present) return true;
    uint32_t videoFormat = 0;
    if (!reader.bits(3, videoFormat) || !reader.flag(color.fullRange)) return false;
    (void)videoFormat;
    color.rangeSignaled = true;
    bool colorDescription = false;
    if (!reader.flag(colorDescription)) return false;
    if (!colorDescription) return true;
    uint32_t primaries = 0, transfer = 0, matrix = 0;
    if (!reader.bits(8, primaries) || !reader.bits(8, transfer) || !reader.bits(8, matrix)) return false;
    (void)primaries;
    (void)transfer;
    color.signaledMatrixCoefficients = static_cast<uint8_t>(matrix);
    switch (matrix) {
        case 1: color.matrix = VideoColorMatrix::Bt709; color.matrixSignaled = true; break;
        case 5:
        case 6: color.matrix = VideoColorMatrix::Bt601; color.matrixSignaled = true; break;
        case 9: color.matrix = VideoColorMatrix::Bt2020; color.matrixSignaled = true; break;
        default: color.matrix = VideoColorMatrix::Bt709; color.matrixSignaled = false; break;
    }
    return true;
}

bool containsIdr(const std::vector<uint8_t>& bytes) {
    for (size_t i = 0; i + 3 < bytes.size(); ++i) {
        if (bytes[i] == 0 && bytes[i + 1] == 0 && bytes[i + 2] == 1 &&
            (bytes[i + 3] & 31u) == 5) return true;
    }
    return false;
}

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
        if (type == 1 || type == 5) ++info.slices;
        if (type != 1 && type != 5 && type != 7 && type != 8) continue;
        NalHeaderReader reader(bytes.data() + begin + 1, end - begin - 1);
        uint32_t id = 0, parent = 0;
        if (type == 1 || type == 5) {
            uint32_t firstMb = 0, sliceType = 0;
            if (type == 5) info.idr = true;
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
        if (type == 7) {
            const bool parsedColor = parseSpsColor(bytes.data() + begin, end - begin, entry.color);
            if (!parsedColor)
                entry.color = VideoColorInfo{};
            const char* matrixName = entry.color.matrix == VideoColorMatrix::Bt601 ? "bt601" :
                entry.color.matrix == VideoColorMatrix::Bt2020 ? "bt2020" : "bt709";
            logLine("video_color_sps sps_id=%u matrix=%s range=%s matrix_signaled=%u range_signaled=%u raw_matrix_coefficients=%u parse=%s default=bt709-limited",
                id, matrixName, entry.color.fullRange ? "full" : "limited",
                entry.color.matrixSignaled ? 1u : 0u, entry.color.rangeSignaled ? 1u : 0u,
                static_cast<unsigned>(entry.color.signaledMatrixCoefficients), parsedColor ? "ok" : "fallback");
            state.sps[id] = std::move(entry);
            info.sps = true;
        }
        else { state.pps[id] = std::move(entry); info.pps = true; }
    }
    info.color = state.activeColor;
    if (info.validPpsId) {
        const auto pps = state.pps.find(info.ppsId);
        if (pps != state.pps.end()) {
            const auto sps = state.sps.find(pps->second.spsId);
            if (sps != state.sps.end()) info.color = sps->second.color;
        }
    }
    state.activeColor = info.color;
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
    discontinuity_ = false;
    inputGap_ = false;
    recoveryRequestUs_ = 0;
    stageCosts_ = {};
    timedAccessUnits_ = 0;
    workerReady_ = false;
    workerInitOk_ = false;
    started_ = true;
    queue_.clear();
    latestRgba_.clear();
    playout_.clear(true);
    nv12PublishScratch_.clear();
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
    catchupDroppedAccessUnits_ = 0;
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
    for (size_t i = 0; !gpuVideoEnabled_.load() && i < conversionWorkers_.size(); ++i) {
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

void VideoDecoder::requestReset(VideoResetRequestReason reason) {
    resetRequestReason_.store(reason, std::memory_order_release);
    resetRequested_.store(true, std::memory_order_release);
    wake_.notify_one();
}

bool VideoDecoder::takeKeyframeRequest() {
    return keyframeNeeded_.exchange(false, std::memory_order_acq_rel);
}

bool VideoDecoder::resetDecoder(const char* reason) {
    markStage(7);
    auto* state = static_cast<DecoderState*>(decoderState_.get());
    if (!state || !state->decoder || !state->api.reset) return false;
    {
        std::lock_guard<std::mutex> lock(playoutMutex_);
        playout_.clear();
    }
    const uint64_t started = sceKernelGetProcessTime();
    const int32_t rc = state->api.reset(state->decoder);
    const int32_t submitRc = sceGnmSubmitDone();
    if (rc == 0 && submitRc == 0) {
        for (auto& slot : state->slots) slot.pending = false;
        for (auto& input : state->retainedInputs) release(input);
        state->retainedInputs.clear();
        state->injectRecoveryHeaders = false;
    }
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
        size_t queuedBytes = 0;
        for (const auto& queued : queue_) queuedBytes += queued.bytes.size();
        const uint64_t nowUs = sceKernelGetProcessTime();
        const uint64_t oldestAgeUs = queue_.empty() || nowUs < queue_.front().enqueuedAtUs
            ? 0 : nowUs - queue_.front().enqueuedAtUs;
        const char* overflow = VideoQueuePolicy::reject(queue_.size(), queuedBytes, size, oldestAgeUs);
        if (overflow) {
            // Keep the bounded existing queue for the worker to harvest SPS/PPS.
            // The incoming AU is missing; the next recovery boundary must be IDR.
            droppedAccessUnits_.fetch_add(1, std::memory_order_relaxed);
            const bool firstGap = !inputGap_.exchange(true, std::memory_order_acq_rel);
            keyframeNeeded_.store(true, std::memory_order_release);
            wake_.notify_one();
            if (firstGap) logLine("video_input_gap reason=%s dropped_incoming=1 oldest_ms=%.3f queued_bytes=%zu action=gate_until_idr reset=0",
                    overflow, oldestAgeUs / 1000.0, queuedBytes);
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

bool VideoDecoder::hasFrame() const { return latestSequence_.load(std::memory_order_relaxed) != 0; }

void VideoDecoder::markStage(uint32_t stage) {
    diagnosticStageAtUs_.store(sceKernelGetProcessTime(), std::memory_order_relaxed);
    diagnosticStage_.store(stage, std::memory_order_release);
}

VideoPerformanceSnapshot VideoDecoder::performanceSnapshot() const {
    VideoPerformanceSnapshot result;
    {
        std::unique_lock<std::mutex> lock(performanceMutex_, std::try_to_lock);
        if (lock.owns_lock()) result = performanceSnapshot_;
        /* build 78 frame-count snapshot */
        else result.snapshotBusy |= 1;
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
    result.catchupDroppedAccessUnits = catchupDroppedAccessUnits_.load(std::memory_order_relaxed);
    result.emptyOutputs = emptyOutputs_.load(std::memory_order_relaxed);
    result.totalDecoded = totalDecoded_.load(std::memory_order_relaxed);
    result.totalPublished = totalPublished_.load(std::memory_order_relaxed);
    const uint64_t now = sceKernelGetProcessTime();
    result.stage = diagnosticStage_.load(std::memory_order_acquire);
    const uint64_t stageAt = diagnosticStageAtUs_.load(std::memory_order_relaxed);
    result.stageAgeMs = stageAt && now >= stageAt ? (now-stageAt)/1000u : 0;
    const auto input = lastInputTimeUs_.load(std::memory_order_relaxed);
    const auto output = lastOutputTimeUs_.load(std::memory_order_relaxed);
    result.lastInputAgeMs = input && now >= input ? (now - input) / 1000u : 0;
    result.lastOutputAgeMs = output && now >= output ? (now - output) / 1000u : 0;
    if (!output || result.lastOutputAgeMs >= 1000) result.decodedFps = result.publishedFps = 0;
    {
        std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
        if (lock.owns_lock()) result.queuedAccessUnits = static_cast<uint32_t>(queue_.size());
        else result.snapshotBusy |= 2;
    }
    {
        std::unique_lock<std::mutex> lock(playoutMutex_, std::try_to_lock);
        if (lock.owns_lock()) {
        result.playoutDepth = static_cast<uint32_t>(playout_.size());
        result.playoutUploaded = playout_.uploaded;
        result.playoutReplaced = playout_.replaced;
        result.playoutRepeated = playout_.repeated;
        result.playoutPrimeWaits = playout_.primeWaits;
        result.playoutSequence = playout_.lastSequence;
        } else result.snapshotBusy |= 4;
    }
    return result;
}

bool VideoDecoder::copyLatestToCanvas(uint32_t* destination, uint32_t width, uint32_t height,
                                      bool native1to1) {
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
    if (native1to1) {
        outWidth = std::min(sourceWidth, width);
        outHeight = std::min(sourceHeight, height);
    } else if (static_cast<uint64_t>(sourceWidth) * height > static_cast<uint64_t>(sourceHeight) * width) {
        outHeight = static_cast<uint32_t>(static_cast<uint64_t>(sourceHeight) * width / sourceWidth);
    } else {
        outWidth = static_cast<uint32_t>(static_cast<uint64_t>(sourceWidth) * height / sourceHeight);
    }
    outWidth = std::max(1u, outWidth);
    outHeight = std::max(1u, outHeight);
    const uint32_t x0 = (width - outWidth) / 2;
    const uint32_t y0 = (height - outHeight) / 2;

    if (scaleSourceWidth_ != sourceWidth || scaleSourceHeight_ != sourceHeight ||
        scaleOutputWidth_ != outWidth || scaleOutputHeight_ != outHeight ||
        scaleNative1to1_ != native1to1) {
        scaleX_.resize(outWidth);
        scaleY_.resize(outHeight);
        const uint32_t cropX = native1to1 && sourceWidth > outWidth ? (sourceWidth - outWidth) / 2 : 0;
        const uint32_t cropY = native1to1 && sourceHeight > outHeight ? (sourceHeight - outHeight) / 2 : 0;
        for (uint32_t x = 0; x < outWidth; ++x)
            scaleX_[x] = native1to1 ? cropX + x : std::min(sourceWidth - 1,
                static_cast<uint32_t>(static_cast<uint64_t>(x) * sourceWidth / outWidth));
        for (uint32_t y = 0; y < outHeight; ++y)
            scaleY_[y] = native1to1 ? cropY + y : std::min(sourceHeight - 1,
                static_cast<uint32_t>(static_cast<uint64_t>(y) * sourceHeight / outHeight));
        scaleSourceWidth_ = sourceWidth;
        scaleSourceHeight_ = sourceHeight;
        scaleOutputWidth_ = outWidth;
        scaleOutputHeight_ = outHeight;
        scaleNative1to1_ = native1to1;
    }

    for (uint32_t y = 0; y < outHeight; ++y) {
        const uint32_t sy = scaleY_[y];
        const uint32_t* src = source + static_cast<size_t>(sy) * sourceWidth;
        uint32_t* dst = destination + static_cast<size_t>(y0 + y) * width + x0;
        for (uint32_t x = 0; x < outWidth; ++x) dst[x] = src[scaleX_[x]];
    }
    return true;
}

bool VideoDecoder::copyLatestNv12To(uint8_t* destination, size_t capacity,
                                    uint32_t& width, uint32_t& height,
                                    VideoColorInfo& color) {
    if (!destination) return false;
    {
        std::unique_lock<std::mutex> lock(playoutMutex_, std::try_to_lock);
        if (!lock.owns_lock()) return false;
        if (!playout_.takeNext(nv12UploadScratch_, capacity, width, height, color,
                              presentationFps_.load(std::memory_order_relaxed), playoutDelayFrames_.load(std::memory_order_relaxed))) return false;
    }
    // The multi-MiB copy no longer holds the AU receiver/publication lock.
    std::memcpy(destination, nv12UploadScratch_.data(), nv12UploadScratch_.size());
    return true;
}

bool VideoDecoder::initializeDecoder(uint32_t pipelineDepth) {
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
    state->config.pipelineDepth = pipelineDepth;
    state->config.computeQueue = state->queue;
    state->config.cpuAffinityMask = 0x3f;
    state->config.cpuPriority = 700;
    state->config.optimizeProgressive = true;

    DecoderMemoryInfo memory{};
    memory.thisSize = sizeof(memory);
    rc = state->api.queryDecoder(&state->config, &memory);
    if (rc != 0 || memory.maxFrameBufferSize == 0) {
        logLine("Videodec2 decoder memory query failed rc=0x%08x", static_cast<unsigned>(rc));
        if (pipelineDepth > 1) { state.reset(); return initializeDecoder(1); }
        return false;
    }
    bool allocationsOk = allocateFlexible(state->cpu, memory.cpuSize) &&
        allocateDirect(state->gpu, memory.gpuSize, ORBIS_KERNEL_WC_GARLIC,
                       ORBIS_KERNEL_PROT_CPU_WRITE | ORBIS_KERNEL_PROT_GPU_RW) &&
        allocateDirect(state->shared, memory.sharedSize, ORBIS_KERNEL_WB_ONION,
                       ORBIS_KERNEL_PROT_CPU_RW | ORBIS_KERNEL_PROT_GPU_RW);
    const size_t slotCount = pipelineDepth > 1 ? state->slots.size() : 1;
    for (size_t i = 0; allocationsOk && i < slotCount; ++i) {
        auto& slot = state->slots[i];
        allocationsOk = allocateDirect(slot.output, memory.maxFrameBufferSize,
            ORBIS_KERNEL_WB_ONION, ORBIS_KERNEL_PROT_CPU_RW | ORBIS_KERNEL_PROT_GPU_RW) &&
            allocateDirect(slot.accessUnit, kMaxAccessUnitBytes, ORBIS_KERNEL_WB_ONION,
                ORBIS_KERNEL_PROT_CPU_RW | ORBIS_KERNEL_PROT_GPU_RW);
    }
    if (allocationsOk) {
        for (size_t i = 0; i < slotCount; ++i)
            invalidateCpuView(state->slots[i].output.address, state->slots[i].output.size);
    }
    if (!allocationsOk) {
        if (pipelineDepth > 1) { state.reset(); return initializeDecoder(1); }
        return false;
    }
    memory.cpu = state->cpu.address;
    memory.gpu = state->gpu.address;
    memory.shared = state->shared.address;
    rc = state->api.createDecoder(&state->config, &memory, &state->decoder);
    if (rc != 0 || !state->decoder) {
        logLine("Videodec2 create decoder failed rc=0x%08x", static_cast<unsigned>(rc));
        if (pipelineDepth > 1) { state.reset(); return initializeDecoder(1); }
        return false;
    }
    logLine("video_queue_policy max_units=%zu max_bytes=%zu catchup_age_ms=%llu age_action=request_idr hard_overflow=gate_idr_no_reset",
            VideoQueuePolicy::maxUnits, VideoQueuePolicy::maxBytes,
            static_cast<unsigned long long>(VideoQueuePolicy::maxAgeUs / 1000));
    logLine("Videodec2 ready profile=Baseline(66) max=%ux%u frame_bytes=%llu pipeline_depth=%u slots=%zu flush_on_empty=%u surface_selection=round_robin cpu_visibility=flush_garlic_clflush",
            kMaxWidth, kMaxHeight, static_cast<unsigned long long>(memory.maxFrameBufferSize),
            pipelineDepth, slotCount, pipelineDepth == 1 ? 1u : 0u);
    decoderState_ = std::move(state);
    return true;
}

bool VideoDecoder::decode(const AccessUnit& accessUnit, bool publishOutput, size_t queueDepth) {
    auto* state = static_cast<DecoderState*>(decoderState_.get());
    if (!state) return false;
    const size_t freeIndex = nextAvailableSurface(state->slots.size(), state->nextSurface,
        [state](size_t i) { return state->slots[i].output.address && !state->slots[i].pending; });
    if (freeIndex == state->slots.size()) {
        logLine("decoder_pipeline event=no_free_slot depth=%u action=fallback_serial", state->config.pipelineDepth);
        return false;
    }
    auto& slot = state->slots[freeIndex];
    if (accessUnit.bytes.size() > kMaxAccessUnitBytes) return false;
    markStage(1); // H.264 parse
    const auto parseStart = std::chrono::steady_clock::now();
    const AccessUnitInfo unitInfo = cacheParameterSets(accessUnit.bytes, *state);
    slot.color = unitInfo.color;
    auto cost = [this](size_t i, uint64_t us) {
        auto& c = stageCosts_[i]; c.sum += us; c.peak = std::max(c.peak, us); ++c.calls;
    };
    cost(0, elapsedUs(parseStart));
    std::vector<uint8_t> recoveryUnit;
    const std::vector<uint8_t>* bytes = &accessUnit.bytes;
    if (state->waitingIdr || state->injectRecoveryHeaders) {
        if (!prepareRecoveryUnit(accessUnit.bytes, unitInfo, *state, recoveryUnit)) {
            const uint64_t now = sceKernelGetProcessTime();
            if (now - recoveryRequestUs_ >= 1000000u) {
                keyframeNeeded_.store(true, std::memory_order_release);
                recoveryRequestUs_ = now;
            }
            return true;
        }
        bytes = &recoveryUnit;
    }
    if (!slot.accessUnit.address && !allocateDirect(slot.accessUnit, kMaxAccessUnitBytes,
            ORBIS_KERNEL_WB_ONION, ORBIS_KERNEL_PROT_CPU_RW | ORBIS_KERNEL_PROT_GPU_RW)) return false;
    if (bytes->size() > slot.accessUnit.size) return false;
    const auto decodeStart = std::chrono::steady_clock::now();
    std::memcpy(slot.accessUnit.address, bytes->data(), bytes->size());
    cost(1, elapsedUs(decodeStart));
    InputData input{};
    input.thisSize = sizeof(input);
    input.bytes = slot.accessUnit.address;
    input.size = bytes->size();
    input.pts = state->pts;
    input.dts = state->pts++;
    FrameBuffer frame{};
    frame.thisSize = sizeof(frame);
    frame.address = slot.output.address;
    frame.size = slot.output.size;
    frame.accepted = false;
    OutputInfo output{};
    output.thisSize = sizeof(output);
    const auto apiStart = std::chrono::steady_clock::now();
    markStage(2); // native decode
    int32_t rc = state->api.decode(state->decoder, &input, &frame, &output);
    cost(2, elapsedUs(apiStart));
    const auto submitStart = std::chrono::steady_clock::now();
    markStage(3); // native submission synchronization
    int32_t submitRc = sceGnmSubmitDone();
    cost(3, elapsedUs(submitStart));
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
        if (recoveryUnit.size() > slot.accessUnit.size) return false;
        std::memcpy(slot.accessUnit.address, recoveryUnit.data(), recoveryUnit.size());
        input.size = recoveryUnit.size();
        frame.accepted = false;
        output = {};
        output.thisSize = sizeof(output);
        markStage(2);
        rc = state->api.decode(state->decoder, &input, &frame, &output);
        markStage(3);
        submitRc = sceGnmSubmitDone();
        logLine("decoder_sequence event=retry_idr rc=0x%08x accepted_framebuffer=%u",
                static_cast<unsigned>(rc), frame.accepted ? 1u : 0u);
    }
    const auto result = classifyDecodeResult(state->config.pipelineDepth, rc, submitRc,
        frame.accepted, output.valid, output.error);
    if (result == DecodeResult::Failure) {
        if (failedAccessUnits_.load(std::memory_order_relaxed) % 120 == 0)
            logLine("Videodec2 decode failed rtp_ts=%u rc=0x%08x submit_done=0x%08x accepted=%u bytes=%zu",
                accessUnit.rtpTimestamp, static_cast<unsigned>(rc), static_cast<unsigned>(submitRc),
                frame.accepted ? 1u : 0u, accessUnit.bytes.size());
        return false;
    }
    slot.pending = frame.accepted;
    slot.submittedAtUs = sceKernelGetProcessTime();
    if (!frame.accepted) {
        // Preserve the exact AU address passed to the async API. A fresh AU
        // allocation is used on the next call; this surface was not accepted.
        if (state->retainedInputs.size() >= DecoderState::maxRetainedInputs) {
            logLine("decoder_pipeline event=unaccepted_input_limit depth=%u retained=%zu action=fallback_serial",
                state->config.pipelineDepth, state->retainedInputs.size());
            // This last input is also potentially live: leave it in the slot
            // until DeleteDecoder during fallback, never release it early.
            return false;
        }
        state->retainedInputs.push_back(slot.accessUnit);
        slot.accessUnit = {};
        logLine("decoder_pipeline event=deferred_input depth=%u accepted=0 valid=%u retained_inputs=%zu action=continue_async reset=0",
            state->config.pipelineDepth, output.valid ? 1u : 0u, state->retainedInputs.size());
    }
    ++state->submitted;
    if (output.error) return false;
    if (!output.valid) ++state->deferred;
    if (state->submitted % 60 == 0) {
        size_t inFlight = 0;
        for (const auto& candidate : state->slots) if (candidate.pending) ++inFlight;
        logLine("decoder_pipeline depth=%u submitted=%llu completed=%llu deferred=%llu in_flight=%zu flush_on_empty=%u retained_inputs=%zu",
            state->config.pipelineDepth, static_cast<unsigned long long>(state->submitted),
            static_cast<unsigned long long>(state->completed), static_cast<unsigned long long>(state->deferred),
            inFlight, state->config.pipelineDepth == 1 ? 1u : 0u, state->retainedInputs.size());
    }
    if (unitInfo.idr) {
        state->waitingIdr = false;
        state->injectRecoveryHeaders = false;
    } // Successful IDR may produce a delayed output.
    if (!output.valid && state->config.pipelineDepth > 1) {
        // Accepted input is still owned by Videodec2. Keep its AU and output
        // surface intact; subsequent Decode calls collect completed surfaces.
        ++emptyOutputs_;
        return true;
    }
    if (!output.valid) {
        output = {};
        output.thisSize = sizeof(output);
        const auto flushStart = std::chrono::steady_clock::now();
        markStage(4); // native flush
        rc = state->api.flush(state->decoder, &frame, &output);
        cost(4, elapsedUs(flushStart));
        const auto flushSubmitStart = std::chrono::steady_clock::now();
        markStage(5);
        const int32_t flushSubmitRc = sceGnmSubmitDone();
        cost(5, elapsedUs(flushSubmitStart));
        if (rc != 0 || flushSubmitRc != 0) {
            if (failedAccessUnits_.load(std::memory_order_relaxed) % 120 == 0)
                logLine("Videodec2 flush failed rc=0x%08x submit_done=0x%08x",
                        static_cast<unsigned>(rc), static_cast<unsigned>(flushSubmitRc));
            return false;
        }
        if (!output.valid) {
            // Depth 1 has synchronously flushed this submission.
            slot.pending = false;
            ++emptyOutputs_;
            return true;
        }
    }
    if (++timedAccessUnits_ % 60 == 0) {
        logLine("videodec_input slices=%u idr=%u coded=%ux%u pitch=%u pipeline_depth=%u max_dpb=4 resource=embedded",
                unitInfo.slices, unitInfo.idr ? 1u : 0u, output.width, output.height,
                output.pitchBytes ? output.pitchBytes : output.pitch, state->config.pipelineDepth);
        char timing[700]{};
        size_t used = 0;
        static const char* stages[] = {"parse", "au_copy", "decode_api", "submit_done", "flush_api", "flush_submit", "nv12_visibility"};
        for (size_t i = 0; i < stageCosts_.size(); ++i) {
            const auto& c = stageCosts_[i];
            const int added = std::snprintf(timing + used, sizeof(timing) - used,
                    "%s%s=%.3f/%.3f(%llu)", i ? " " : "", stages[i],
                    c.calls ? c.sum / static_cast<double>(c.calls) / 1000.0 : 0.0,
                    c.peak / 1000.0, static_cast<unsigned long long>(c.calls));
            if (added < 0 || static_cast<size_t>(added) >= sizeof(timing) - used) break;
            used += static_cast<size_t>(added);
        }
        logLine("videodec_cost avg_max_ms(calls) %s", timing);
        stageCosts_ = {};
    }
    const uint32_t pitchBytes = output.pitchBytes ? output.pitchBytes : output.pitch;
    const void* decodedFrame = output.frameBuffer;
    // Serial firmware may omit the pointer; pipelined output must identify
    // the exact accepted surface. Never guess the current submission's slot.
    if (!decodedFrame && state->config.pipelineDepth == 1) decodedFrame = slot.output.address;
    auto completed = std::find_if(state->slots.begin(), state->slots.end(),
        [decodedFrame](const DecoderState::Slot& candidate) {
            return candidate.pending && candidate.output.address == decodedFrame;
        });
    // Keep the legacy synchronous path compatible with library-owned output.
    // It is copied before the next native call; no async slot is reused here.
    if (completed == state->slots.end() && state->config.pipelineDepth == 1)
        completed = state->slots.begin();
    if (completed == state->slots.end()) {
        logLine("decoder_pipeline event=unknown_output depth=%u action=fallback_serial", state->config.pipelineDepth);
        return false;
    }
    const size_t decodedFrameSize = output.frameBufferSize
        ? static_cast<size_t>(output.frameBufferSize) : completed->output.size;
    if (output.error || output.pictureCount == 0 || output.format != 0 ||
        output.width == 0 || output.width > kMaxWidth || output.height == 0 ||
        output.height > kMaxHeight || pitchBytes < output.width || !decodedFrame ||
        decodedFrameSize > completed->output.size) {
        if (failedAccessUnits_.load(std::memory_order_relaxed) % 120 == 0)
            logLine("Videodec2 output rejected valid=%u error=%u pictures=%u format=%u size=%ux%u pitch=%u",
                output.valid ? 1u : 0u, output.error ? 1u : 0u, output.pictureCount,
                output.format, output.width, output.height, pitchBytes);
        return false;
    }
    if (state->recoveryPending) {
        logLine("decoder_recovered reset_count=%llu size=%ux%u trigger_rtp_ts=%u",
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
        markStage(6); // NV12 publication/CPU fallback
        if (!publishNv12(decodedFrame, decodedFrameSize,
                         output.width, output.height, pitchBytes,
                         output.format, completed->color)) return false;
        convertUs = elapsedUs(convertStart);
        convertTimeUsWindow_ += convertUs;
        ++publishedFrames_;
        ++totalPublished_;
        lastOutputTimeUs_.store(sceKernelGetProcessTime(), std::memory_order_relaxed);
        ++publishedFramesInWindow_;
    } else {
        ++skippedOutputFrames_;
    }
    // NV12 publication copies into CPU-owned storage before this surface and
    // its associated AU can be handed back to the native decoder.
    ++state->completed;
    if (state->completed % 60 == 0) {
        const uint64_t doneAt = sceKernelGetProcessTime();
        logLine("decoder_pipeline_output depth=%u completed=%llu surface=%zu surface_hold_ms=%.3f au_association=unknown",
            state->config.pipelineDepth, static_cast<unsigned long long>(state->completed),
            static_cast<size_t>(completed - state->slots.begin()),
            (doneAt - completed->submittedAtUs) / 1000.0);
    }
    completed->pending = false;
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
                               uint32_t paddedHeight, uint32_t pitch, uint32_t format,
                               const VideoColorInfo& color) {
    if (!raw || format != 0 || pitch < width || paddedHeight < 16) return false;
    const uint64_t yBytes = static_cast<uint64_t>(pitch) * paddedHeight;
    const uint32_t visibleHeight = std::min(paddedHeight, 1080u);
    const uint64_t uvBytes = static_cast<uint64_t>(pitch) * ((visibleHeight + 1u) / 2u);
    if (yBytes > available || uvBytes > available - yBytes) {
        logLine("NV12 bounds invalid width=%u height=%u pitch=%u bytes=%zu", width, paddedHeight, pitch, available);
        return false;
    }
    const auto visibilityStart = std::chrono::steady_clock::now();
    // SubmitDone is preserved. FlushGarlic supplies the native visibility step
    // also used by the local Moonlight PS4 path before CPU NV12 access.
    // CPU invalidation prevents an old WB cache line from surviving slot reuse.
    sceGnmFlushGarlic();
    invalidateCpuView(raw, static_cast<size_t>(yBytes + uvBytes));
    const uint64_t visibilityUs = elapsedUs(visibilityStart);
    auto& visibility = stageCosts_[6];
    visibility.sum += visibilityUs; visibility.peak = std::max(visibility.peak, visibilityUs); ++visibility.calls;
    const auto* src = static_cast<const uint8_t*>(raw);
    const uint8_t* uv = src + yBytes;
    if (gpuVideoEnabled_.load(std::memory_order_relaxed)) {
        const uint32_t uvHeight = (visibleHeight + 1u) / 2u;
        // Pack outside the shared receiver/presenter mutex. Swapping two
        // retained allocations makes publication constant-time under the lock.
        nv12PublishScratch_.resize(static_cast<size_t>(width) * (visibleHeight + uvHeight));
        uint8_t* yOut = nv12PublishScratch_.data();
        uint8_t* uvOut = yOut + static_cast<size_t>(width) * visibleHeight;
        if (pitch == width) {
            std::memcpy(yOut, src, static_cast<size_t>(width) * visibleHeight);
            std::memcpy(uvOut, uv, static_cast<size_t>(width) * uvHeight);
        } else {
            for (uint32_t y = 0; y < visibleHeight; ++y)
                std::memcpy(yOut + static_cast<size_t>(y) * width, src + static_cast<size_t>(y) * pitch, width);
            for (uint32_t y = 0; y < uvHeight; ++y)
                std::memcpy(uvOut + static_cast<size_t>(y) * width, uv + static_cast<size_t>(y) * pitch, width);
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ++latestSequence_;
        }
        {
            std::lock_guard<std::mutex> lock(playoutMutex_);
            playout_.push(nv12PublishScratch_, width, visibleHeight, latestSequence_, color);
        }
        lastOutputTimeUs_.store(sceKernelGetProcessTime(), std::memory_order_relaxed);
        return true;
    }
    rgbaScratch_.resize(static_cast<size_t>(width) * visibleHeight);
    conversionNextRow_.store(0, std::memory_order_relaxed);
    if (conversionWorkerCount_ == 0) {
        conversionCallerLastCpu_.store(sceKernelGetCurrentCpu(), std::memory_order_relaxed);
        convertRows(src, uv, width, visibleHeight, pitch, rgbaScratch_.data(), color);
    } else {
        {
            std::lock_guard<std::mutex> lock(conversionMutex_);
            conversionYPlane_ = src;
            conversionUvPlane_ = uv;
            conversionWidth_ = width;
            conversionHeight_ = visibleHeight;
            conversionPitch_ = pitch;
            conversionDestination_ = rgbaScratch_.data();
            conversionColor_ = color;
            conversionPending_ = conversionWorkerCount_;
            ++conversionGeneration_;
        }
        conversionWake_.notify_all();
        conversionCallerLastCpu_.store(sceKernelGetCurrentCpu(), std::memory_order_relaxed);
        convertRows(src, uv, width, visibleHeight, pitch, rgbaScratch_.data(), color);
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
                               uint32_t* destination, const VideoColorInfo& color) {
    const size_t matrix = std::min(static_cast<size_t>(color.matrix), kColorMatrixCount - 1);
    const size_t range = color.fullRange ? 1u : 0u;
    const auto& lookup = g_yuvLookup[matrix][range];
    for (;;) {
        const uint32_t first = conversionNextRow_.fetch_add(kConversionBandRows,
                                                              std::memory_order_relaxed);
        if (first >= height) break;
        const uint32_t last = std::min(height, first + kConversionBandRows);
        for (uint32_t y = first; y < last; ++y) {
            const uint8_t* yRow = yPlane + static_cast<size_t>(y) * pitch;
            uint32_t* out = destination + static_cast<size_t>(y) * width;
            for(uint32_t x=0;x<width;++x) {
                const uint8_t* uvRow = uvPlane + static_cast<size_t>(y / 2) * pitch;
                const int u = uvRow[(x & ~1u)];
                const int v = uvRow[(x & ~1u) + 1];
                const int32_t yy=lookup.y[yRow[x]];
                const uint32_t r=clampByte((yy+lookup.redV[v])>>8);
                const uint32_t g=clampByte((yy+lookup.greenU[u]+lookup.greenV[v])>>8);
                const uint32_t b=clampByte((yy+lookup.blueU[u])>>8);
                out[x]=r|(g<<8)|(b<<16)|0xff000000u;
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
        const VideoColorInfo color = conversionColor_;
        lock.unlock();
        convertRows(yPlane, uvPlane, width, height, pitch, destination, color);
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
    NativeThreadCpuSampler<logLine> cpuSampler("videodec_submit");
    cpuSampler.start();
    for (;;) {
        AccessUnit unit;
        std::deque<AccessUnit> catchupDiscarded;
        size_t queueDepth = 0;
        uint64_t queueWaitUs = 0;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            markStage(0); // waiting for next AU or recovery request
            wake_.wait(lock, [this] { return stopRequested_ || resetRequested_.load(std::memory_order_acquire) || !queue_.empty() || discontinuity_.load(std::memory_order_acquire) || inputGap_.load(std::memory_order_acquire); });
            if (stopRequested_) { queue_.clear(); break; }
            if (resetRequested_.exchange(false, std::memory_order_acq_rel)) {
                const auto requestReason = resetRequestReason_.load(std::memory_order_acquire);
                droppedAccessUnits_.fetch_add(queue_.size(), std::memory_order_relaxed);
                queue_.clear();
                lock.unlock();
                resetDecoder(requestReason == VideoResetRequestReason::AdaptiveWatchdog
                    ? "adaptive_watchdog" : "user_master_button");
                continue;
            }
            if (inputGap_.exchange(false, std::memory_order_acq_rel)) {
                std::deque<AccessUnit> dropped;
                dropped.swap(queue_);
                droppedAccessUnits_.fetch_add(dropped.size(), std::memory_order_relaxed);
                lock.unlock();
                for (const auto& stale : dropped) cacheParameterSets(stale.bytes, *decoderState_);
                decoderState_->waitingIdr = true;
                decoderState_->injectRecoveryHeaders = true;
                keyframeNeeded_.store(true, std::memory_order_release);
                logLine("video_queue_recovery discarded=%zu action=gate_until_complete_idr reset=0 native_surfaces=retained",
                    dropped.size());
                continue;
            }
            if (discontinuity_.exchange(false, std::memory_order_acq_rel)) {
                lock.unlock();
                if (!resetDecoder("input_discontinuity")) {
                    logLine("decoder recovery reset failed; input remains gated until IDR");
                }
                continue;
            }
            if (queue_.empty()) continue;
            const uint64_t checkNow = sceKernelGetProcessTime();
            if (checkNow >= queue_.front().enqueuedAtUs &&
                checkNow - queue_.front().enqueuedAtUs > VideoQueuePolicy::maxAgeUs) {
                if (checkNow - recoveryRequestUs_ >= 1000000u) {
                    recoveryRequestUs_ = checkNow;
                    keyframeNeeded_.store(true, std::memory_order_release);
                    logLine("video_queue_pressure oldest_ms=%.3f units=%zu action=request_idr_keep_references reset=0",
                        (checkNow - queue_.front().enqueuedAtUs) / 1000.0, queue_.size());
                }
                size_t newestIdr = 0;
                for (size_t i = 1; i < queue_.size(); ++i)
                    if (containsIdr(queue_[i].bytes)) newestIdr = i;
                for (size_t i = 0; i < newestIdr; ++i) {
                    catchupDiscarded.push_back(std::move(queue_.front()));
                    queue_.pop_front();
                }
            }
            ++dequeuedAccessUnits_;
            unit = std::move(queue_.front());
            queue_.pop_front();
            queueDepth = queue_.size();
            const uint64_t dequeuedAtUs = sceKernelGetProcessTime();
            queueWaitUs = dequeuedAtUs >= unit.enqueuedAtUs ? dequeuedAtUs - unit.enqueuedAtUs : 0;
        }
        if (!catchupDiscarded.empty()) {
            // Carry parameter sets across the discarded prefix, then prepend
            // matching SPS/PPS to the IDR. Native pending surfaces remain owned.
            for (const auto& stale : catchupDiscarded)
                cacheParameterSets(stale.bytes, *decoderState_);
            decoderState_->injectRecoveryHeaders = true;
            catchupDroppedAccessUnits_.fetch_add(catchupDiscarded.size(), std::memory_order_relaxed);
            droppedAccessUnits_.fetch_add(catchupDiscarded.size(), std::memory_order_relaxed);
            logLine("video_catchup boundary=idr discarded=%zu action=prepend_parameter_sets reset=0",
                catchupDiscarded.size());
        }
        cpuSampler.sample();
        queueWaitTimeUsWindow_ += queueWaitUs;
        ++queueWaitSamplesWindow_;
        queueWaitMaxUsWindow_ = std::max(queueWaitMaxUsWindow_, queueWaitUs);
        decoderLastCpu_.store(sceKernelGetCurrentCpu(), std::memory_order_relaxed);
        // GPU publication only packs NV12; the CPU conversion backlog rule
        // must not discard decoded frames before the presenter can see them.
        const bool publishOutput = gpuVideoEnabled_.load(std::memory_order_relaxed) ||
                                   queueDepth < kPublishBacklogThreshold;
        if (!decode(unit, publishOutput, queueDepth)) {
            auto* native = decoderState_.get();
            if (native && native->config.pipelineDepth > 1) {
                auto cachedSps = std::move(native->sps);
                auto cachedPps = std::move(native->pps);
                logLine("decoder_pipeline event=fallback from=2 to=1 reason=decode_or_ownership_error");
                decoderState_.reset();
                if (!initializeDecoder(1)) break;
                decoderState_->sps = std::move(cachedSps);
                decoderState_->pps = std::move(cachedPps);
                decoderState_->waitingIdr = true;
                decoderState_->recoveryPending = true;
                keyframeNeeded_.store(true, std::memory_order_release);
            } else {
                // An unsuccessful AU can invalidate future reference pictures.
                discontinuity_.store(true, std::memory_order_release);
            }
            const uint64_t errors = ++failedAccessUnits_;
            if (errors == 1 || errors % 120 == 0)
                logLine("STAGE L6 FAIL event=decode_access_unit errors=%llu rtp_ts=%u bytes=%zu queue_depth=%zu",
                        static_cast<unsigned long long>(errors), unit.rtpTimestamp, unit.bytes.size(), queueDepth);
        }
    }
    decoderState_.reset();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        workerInitOk_ = false;
        queue_.clear();
    }
}

} // namespace xc::ps4
