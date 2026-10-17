// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 RafaelNGP
// Box art and hero images: downloaded and decoded on background threads,
// scaled to exactly the size they are drawn at ("cover": fill and crop), and
// kept in memory up to a byte budget (least recently used dropped first).
// With a disk directory, downloads are also kept there (as fetched, already
// sized by the image server) so the next launch doesn't download them again;
// the directory is trimmed to a size limit, oldest files first.
#pragma once

#include "platform/platform.h"
#include "ui/canvas.h"

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace xc::ui {

enum class ImagePriority : uint8_t { Prefetch = 0, Hero = 1, Visible = 2, Focused = 3 };

// Decodes PNG/JPEG bytes. Returns false on unsupported data.
bool decodeImage(const std::string& bytes, Image& out);
// Scales `src` to fill w x h, cropping the excess around the centre.
Image coverResize(const Image& src, int w, int h);

class ImageCache {
public:
    struct Stats {
        size_t residentBytes = 0;
        size_t queued = 0;
        uint64_t memoryHits = 0;
        uint64_t diskHits = 0;
        uint64_t downloads = 0;
        uint64_t failures = 0;
        uint64_t retries = 0;
        uint64_t queueDrops = 0;
        uint32_t workersStarted = 0;
        bool diskEnabled = false;
        uint64_t diskReadErrors = 0;
        uint64_t diskWriteErrors = 0;
    };

    // `onReady` is called (from a loader thread) whenever an image arrives.
    explicit ImageCache(std::function<void()> onReady, size_t budgetBytes = 160u << 20, std::string diskDir = {},
                        size_t diskBudgetBytes = 256u << 20);
    ~ImageCache();

    // The image scaled to w x h, or nullptr while it loads (or if it failed).
    std::shared_ptr<const Image> get(const std::string& url, int w, int h,
                                    ImagePriority priority = ImagePriority::Visible);
    Stats stats() const;

private:
    struct Entry {
        std::shared_ptr<const Image> image;
        bool loading = false, failed = false;
        uint64_t lastUse = 0;
        uint64_t retryAtMs = 0;
        uint8_t failureCount = 0;
    };
    struct Job {
        std::string key, url;
        int w, h;
        ImagePriority priority;
        uint64_t order;
    };
    void loop();
    void evict();
    std::string diskPath(const std::string& url) const;
    void trimDisk();
    void trimDiskAfterWrite();

    std::function<void()> onReady_;
    size_t budget_;
    std::string diskDir_;
    size_t diskBudget_;
    bool diskEnabled_ = false;
    mutable std::mutex mutex_;
    mutable std::mutex diskMutex_;
    std::condition_variable cv_;
    std::map<std::string, Entry> entries_;
    std::deque<Job> jobs_;  // newest first: what is on screen now wins
    size_t bytes_ = 0;
    uint64_t clock_ = 0;
    uint64_t jobOrder_ = 0;
    uint64_t memoryHits_ = 0, diskHits_ = 0, downloads_ = 0, failures_ = 0, retries_ = 0, queueDrops_ = 0;
    uint64_t diskReadErrors_ = 0, diskWriteErrors_ = 0;
    uint32_t workersStarted_ = 0;
    std::atomic<bool> initialTrimPending_{false};
    std::atomic<uint32_t> diskWritesSinceTrim_{0};
    bool stop_ = false;
    std::vector<platform::Thread> threads_;
};

}  // namespace xc::ui
