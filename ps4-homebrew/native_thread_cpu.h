// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <orbis/libkernel.h>
#include <orbis/_types/kernel.h>
#include <ctime>
#include <cstdint>
#include <cstring>
namespace xc::ps4 {
// Bypass the SDK clock_gettime shim: its CPU clocks fall back to wall time.
// Validate the native per-thread clock across a sleep before reporting CPU use.
template<auto Log>
class NativeThreadCpuSampler {
    const char* role_;
    using ReadClock = int32_t (*)(clockid_t, struct timespec*);
    ReadClock readClock_ = nullptr;
    clockid_t clock_ = 0;
    uint64_t lastWall_ = 0, lastCpu_ = 0;
    bool read(uint64_t& value) const {
        struct timespec ts{};
        if (!readClock_ || readClock_(clock_, &ts) != 0 || ts.tv_sec < 0 ||
            ts.tv_nsec < 0 || ts.tv_nsec >= 1000000000) return false;
        value = static_cast<uint64_t>(ts.tv_sec) * 1000000u + ts.tv_nsec / 1000u;
        return true;
    }
public:
    explicit NativeThreadCpuSampler(const char* role) : role_(role) {}
    void start() {
        const int32_t rc = scePthreadGetcpuclockid(scePthreadSelf(), &clock_);
        if (rc != 0) {
            Log("native_thread_cpu role=%s available=0 reason=get_thread_clock rc=0x%08x", role_, static_cast<unsigned>(rc));
            return;
        }
        OrbisKernelModule modules[256]{};
        size_t count = 0;
        if (sceKernelGetModuleList(modules, 256, &count) != 0 || count > 256) {
            Log("native_thread_cpu role=%s available=0 reason=module_list", role_);
            return;
        }
        for (size_t i = 0; i < count; ++i) {
            OrbisKernelModuleInfo info{}; info.size = sizeof(info);
            if (sceKernelGetModuleInfo(modules[i], &info) != 0 || !std::strstr(info.name, "libkernel")) continue;
            void* symbol = nullptr;
            if (sceKernelDlsym(modules[i], "sceKernelClockGettime", &symbol) == 0)
                readClock_ = reinterpret_cast<ReadClock>(symbol);
            if (readClock_) break;
        }
        uint64_t before = 0, after = 0;
        const uint64_t wall = sceKernelGetProcessTime();
        const bool validBefore = read(before);
        sceKernelUsleep(2000);
        const uint64_t elapsed = sceKernelGetProcessTime() - wall;
        if (!validBefore || !read(after) || after < before ||
            (after - before) * 100u >= elapsed * 80u) {
            readClock_ = nullptr;
            Log("native_thread_cpu role=%s available=0 reason=native_clock_unverified sdk_wall_fallback=excluded", role_);
            return;
        }
        lastCpu_ = after; lastWall_ = sceKernelGetProcessTime();
        Log("native_thread_cpu role=%s available=1 source=native_thread_clock sleep_check=passed scope=caller_only internal_workers=excluded tid=%d affinity=inherited", role_,
            scePthreadGetthreadid());
    }
    void sample() {
        if (!readClock_) return;
        const uint64_t now = sceKernelGetProcessTime();
        if (now - lastWall_ < 2000000u) return;
        uint64_t cpu = 0;
        if (!read(cpu) || cpu < lastCpu_) {
            readClock_ = nullptr;
            Log("native_thread_cpu role=%s available=0 reason=read_failed", role_);
            return;
        }
        const double percent = (cpu - lastCpu_) * 100.0 / (now - lastWall_);
        if (percent > 105.0) {
            readClock_ = nullptr;
            Log("native_thread_cpu role=%s available=0 reason=invalid_cpu_time_delta", role_);
            return;
        }
        Log("native_thread_cpu role=%s available=1 wall_ms=%.3f cpu_ms=%.3f caller_percent=%.2f cpu_id=%d scope=caller_only internal_workers=excluded", role_,
            (now - lastWall_) / 1000.0, (cpu - lastCpu_) / 1000.0, percent, sceKernelGetCurrentCpu());
        lastCpu_ = cpu; lastWall_ = now;
    }
};


}
