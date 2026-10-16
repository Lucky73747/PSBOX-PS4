// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 RafaelNGP
#include "util/log.h"
#ifdef __PS4__
#include "../../ps4-homebrew/native_thread_cpu.h"
#endif
#include <atomic>
#include <algorithm>
#include <cstdint>
#include <chrono>
#include <thread>
#include <functional>
#include <condition_variable>
#include <cstdio>
#include <ctime>
#include <deque>
#include <mutex>
#include <pthread.h>
#include <string>
#include <utility>

namespace xc::log {
namespace {
struct Entry {
    Level level; std::time_t time; std::string text;
    uint64_t monoUs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
    size_t thread = std::hash<std::thread::id>{}(std::this_thread::get_id());
};
const char* tag(Level l) {
    switch (l) {
        case Level::Debug: return "D"; case Level::Info: return "I";
        case Level::Warn: return "W"; case Level::Error: return "E";
    }
    return "?";
}
#ifdef __PS4__
void cpuDiagnostic(const char* fmt, ...) {
    char message[768]; va_list ap; va_start(ap,fmt);
    std::vsnprintf(message,sizeof(message),fmt,ap); va_end(ap);
    write(Level::Info,"%s",message);
}
#endif
struct Logger {
    std::atomic<Level> level{Level::Info};
    std::mutex queueMutex, outputMutex;
    std::condition_variable ready, drained;
    std::deque<Entry> queue;
    FILE* file = nullptr;
    pthread_t worker{};
    bool async = false, stopping = false, writing = false;
    uint64_t dropped = 0;
    void (*debugSink)(const char*) = nullptr;
    void output(const Entry& e) {
        std::tm tm{}; char ts[32];
        localtime_r(&e.time, &tm);
        std::strftime(ts, sizeof(ts), "%H:%M:%S", &tm);
        char line[2300];
        std::snprintf(line, sizeof(line), "%s [%s] t_us=%llu thread=%zx %s\n", ts, tag(e.level),
                      static_cast<unsigned long long>(e.monoUs), e.thread, e.text.c_str());
        // File is authoritative; avoid duplicating every hot-path line through two kernel sinks.
        if (debugSink) { if (e.level >= Level::Warn) debugSink(line); }
        else std::fputs(line, stderr);
        if (file) std::fputs(line, file);
    }
    void run() {
#ifdef __PS4__
        xc::ps4::NativeThreadCpuSampler<cpuDiagnostic> loggerCpuSampler("diagnostic_logger");
        loggerCpuSampler.start();
#endif
        for (;;) {
            std::deque<Entry> batch; uint64_t lost = 0;
            {
                std::unique_lock<std::mutex> lock(queueMutex);
                ready.wait(lock, [&] { return stopping || !queue.empty(); });
                if (stopping && queue.empty()) break;
                const size_t count = std::min<size_t>(32, queue.size());
                for (size_t i=0; i<count; ++i) { batch.push_back(std::move(queue.front())); queue.pop_front(); }
                writing = true; lost = dropped; dropped = 0;
            }
            {
                std::lock_guard<std::mutex> lock(outputMutex);
                if (lost) output({Level::Warn, std::time(nullptr), "async_log dropped="+std::to_string(lost)+" diagnostic_lines_only=1"});
                for (const auto& e : batch) output(e);
                if (file) std::fflush(file);
                std::fflush(stderr);
            }
            {
                std::lock_guard<std::mutex> lock(queueMutex);
                writing = false; if (queue.empty()) drained.notify_all();
            }
#ifdef __PS4__
            loggerCpuSampler.sample();
#endif
        }
    }
    static void* entry(void* p) { static_cast<Logger*>(p)->run(); return nullptr; }
    ~Logger() {
        { std::lock_guard<std::mutex> lock(queueMutex); stopping = true; }
        ready.notify_one();
        if (async) pthread_join(worker, nullptr);
        if (file) std::fclose(file);
    }
};
Logger& logger() { static Logger state; return state; }
}
void setLevel(Level level) { logger().level.store(level, std::memory_order_relaxed); }
void setFile(const char* path) {
    auto& s=logger(); std::lock_guard<std::mutex> lock(s.outputMutex);
    if (s.file) std::fclose(s.file);
    s.file=path ? std::fopen(path,"a") : nullptr;
}
bool startAsync(void (*debugSink)(const char*)) {
    auto& s=logger(); std::lock_guard<std::mutex> lock(s.queueMutex);
    if (s.async) return true;
    s.debugSink=debugSink;
    if (pthread_create(&s.worker,nullptr,Logger::entry,&s)!=0) return false;
    s.async=true; return true;
}
void flush() {
    auto& s=logger();
    { std::unique_lock<std::mutex> lock(s.queueMutex); if(s.async) s.drained.wait(lock,[&]{return s.queue.empty()&&!s.writing;}); }
    std::lock_guard<std::mutex> lock(s.outputMutex);
    if(s.file) std::fflush(s.file);
}
void write(Level level, const char* fmt, ...) {
    auto& s=logger(); if(level<s.level.load(std::memory_order_relaxed)) return;
    char message[2048]; va_list ap; va_start(ap,fmt); std::vsnprintf(message,sizeof(message),fmt,ap); va_end(ap);
    std::string text(message);
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
    for (char& c : text) if (c == '\n' || c == '\r') c = ' ';
    Entry e{level,std::time(nullptr),std::move(text)};
    {
        std::lock_guard<std::mutex> lock(s.queueMutex);
        if(s.async) {
            // Bound memory and never let diagnostic I/O stall media threads.
            if(s.queue.size()>=512) { ++s.dropped; return; }
            s.queue.push_back(std::move(e)); s.ready.notify_one(); return;
        }
    }
    std::lock_guard<std::mutex> lock(s.outputMutex); s.output(e); if(s.file) std::fflush(s.file);
}
} // namespace xc::log
