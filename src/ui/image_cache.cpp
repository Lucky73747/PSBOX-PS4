// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 RafaelNGP
#include "ui/image_cache.h"

#include "net/http.h"
#include "util/log.h"

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "stb_image.h"

#define STB_IMAGE_RESIZE_IMPLEMENTATION
#define STB_IMAGE_RESIZE_STATIC
#include "stb_image_resize2.h"

#include <dirent.h>
#include <sys/stat.h>
#include <utime.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <limits>

namespace xc::ui {

bool decodeImage(const std::string& bytes, Image& out) {
    constexpr size_t kMaxEncodedBytes = 24u << 20;
    constexpr uint64_t kMaxDecodedPixels = 4u << 20;
    if (bytes.empty() || bytes.size() > kMaxEncodedBytes ||
        bytes.size() > static_cast<size_t>(std::numeric_limits<int>::max())) return false;
    int w = 0, h = 0, n = 0;
    if (!stbi_info_from_memory(reinterpret_cast<const stbi_uc*>(bytes.data()),
                               static_cast<int>(bytes.size()), &w, &h, &n) ||
        w <= 0 || h <= 0 || w > 8192 || h > 8192 ||
        static_cast<uint64_t>(w) * static_cast<uint64_t>(h) > kMaxDecodedPixels) return false;
    unsigned char* data = stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(bytes.data()),
                                                static_cast<int>(bytes.size()), &w, &h, &n, 4);
    if (!data) return false;
    out.w = w;
    out.h = h;
    out.px.resize(static_cast<size_t>(w) * h);
    std::memcpy(out.px.data(), data, out.px.size() * 4);
    stbi_image_free(data);
    return true;
}

Image coverResize(const Image& src, int w, int h) {
    Image out;
    if (src.w <= 0 || src.h <= 0 || w <= 0 || h <= 0 ||
        w > 8192 || h > 8192 || static_cast<uint64_t>(w) * static_cast<uint64_t>(h) > (4u << 20) ||
        src.px.size() < static_cast<size_t>(src.w) * static_cast<size_t>(src.h)) return out;
    out.w = w;
    out.h = h;
    out.px.resize(static_cast<size_t>(w) * h);
    // Crop the source to the target aspect ratio, centred.
    double scale = std::max(static_cast<double>(w) / src.w, static_cast<double>(h) / src.h);
    int cw = std::min(src.w, static_cast<int>(w / scale + 0.5));
    int ch = std::min(src.h, static_cast<int>(h / scale + 0.5));
    int cx = (src.w - cw) / 2, cy = (src.h - ch) / 2;
    const auto* start = reinterpret_cast<const unsigned char*>(&src.px[static_cast<size_t>(cy) * src.w + cx]);
    stbir_resize_uint8_srgb(start, cw, ch, src.w * 4, reinterpret_cast<unsigned char*>(out.px.data()), w, h, w * 4,
                            STBIR_RGBA);
    return out;
}

namespace {

// store-images.s-microsoft.com resizes on request: fetch close to the size
// we draw instead of the multi-megapixel original.
std::string sizedUrl(std::string url, int w, int h) {
    if (url.rfind("//", 0) == 0) url = "https:" + url;
    if (url.find("store-images.s-microsoft.com") != std::string::npos) {
        url += url.find('?') == std::string::npos ? "?" : "&";
        url += "w=" + std::to_string(w) + "&h=" + std::to_string(h) + "&mode=scale&q=90&format=jpg";
    }
    return url;
}

bool readCacheFile(const std::string& path, std::string& data) {
    constexpr off_t kMaxCacheFileBytes = 24u << 20;
    struct stat st {};
    if (::stat(path.c_str(), &st) != 0) return false;
    if (!S_ISREG(st.st_mode) || st.st_size <= 0 || st.st_size > kMaxCacheFileBytes) {
        errno = EFBIG;
        return false;
    }
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    data.resize(static_cast<size_t>(st.st_size));
    const size_t got = std::fread(data.data(), 1, data.size(), f);
    const bool ok = got == data.size() && std::ferror(f) == 0;
    std::fclose(f);
    if (!ok) {
        data.clear();
        errno = EIO;
    }
    return ok;
}

}  // namespace

ImageCache::ImageCache(std::function<void()> onReady, size_t budgetBytes, std::string diskDir,
                       size_t diskBudgetBytes)
    : onReady_(std::move(onReady)), budget_(budgetBytes), diskDir_(std::move(diskDir)), diskBudget_(diskBudgetBytes) {
    if (!diskDir_.empty()) {
        bool ready = true;
        if (::mkdir(diskDir_.c_str(), 0777) != 0 && errno != EEXIST) {
            XC_LOGW("image cache disk unavailable: mkdir failed errno=%d", errno);
            ready = false;
        }
        struct stat st {};
        if (ready && (::stat(diskDir_.c_str(), &st) != 0 || !S_ISDIR(st.st_mode))) {
            XC_LOGW("image cache disk unavailable: path is not a directory errno=%d", errno);
            ready = false;
        }
        // Artwork is non-sensitive; allow the console's FTP workflow to inspect
        // the cache directory without exposing the private token directory.
        if (ready && ::chmod(diskDir_.c_str(), 0777) != 0) {
            XC_LOGW("image cache directory chmod failed errno=%d", errno);
        }
        if (ready && ::access(diskDir_.c_str(), W_OK | R_OK | X_OK) != 0) {
            XC_LOGW("image cache disk unavailable: directory not accessible errno=%d", errno);
            ready = false;
        }
        if (ready) {
            const std::string probe = diskDir_ + "/.psbox-cache-probe";
            FILE* f = std::fopen(probe.c_str(), "wb");
            bool probeOk = f != nullptr;
            if (f) {
                probeOk = std::fwrite("ok", 1, 2, f) == 2;
                if (std::fclose(f) != 0) probeOk = false;
            }
            if (!probeOk) {
                ::unlink(probe.c_str());
                XC_LOGW("image cache disk unavailable: write probe failed errno=%d", errno);
                ready = false;
            } else {
                ::unlink(probe.c_str());
            }
        }
        diskEnabled_ = ready;
        XC_LOGI("image cache disk path=%s enabled=%d quota_mb=%zu", diskDir_.c_str(),
                diskEnabled_ ? 1 : 0, diskBudget_ >> 20);
        if (diskEnabled_) initialTrimPending_.store(true, std::memory_order_relaxed);
    }
    threads_.resize(3);
    for (auto& t : threads_) {
        if (platform::startThread(t, [this] { loop(); }, 2u << 20)) {
            ++workersStarted_;
        } else {
            XC_LOGW("image cache worker start failed");
        }
    }
    if (workersStarted_ == 0) XC_LOGE("image cache unavailable: no loader workers started");
}

ImageCache::~ImageCache() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
    }
    cv_.notify_all();
    for (auto& t : threads_) t.join();
}

std::shared_ptr<const Image> ImageCache::get(const std::string& url, int w, int h, ImagePriority priority) {
    if (url.empty() || w <= 0 || h <= 0) return nullptr;
    std::string key = url + "#" + std::to_string(w) + "x" + std::to_string(h);
    std::lock_guard<std::mutex> lock(mutex_);
    if (entries_.size() >= 2048) {
        while (entries_.size() >= 1536) {
            auto victim = entries_.end();
            for (auto it = entries_.begin(); it != entries_.end(); ++it) {
                if (it->first == key || it->second.loading) continue;
                if (victim == entries_.end() || it->second.lastUse < victim->second.lastUse) victim = it;
            }
            if (victim == entries_.end()) break;
            if (victim->second.image) bytes_ -= victim->second.image->bytes();
            entries_.erase(victim);
        }
    }
    Entry& e = entries_[key];
    e.lastUse = ++clock_;
    if (e.image) {
        ++memoryHits_;
        return e.image;
    }
    const uint64_t now = platform::nowMs();
    if (!e.loading && now >= e.retryAtMs) {
        if (e.failed) ++retries_;
        constexpr size_t kMaxQueuedJobs = 128;
        if (jobs_.size() >= kMaxQueuedJobs) {
            ++queueDrops_;
            e.retryAtMs = now + 500;
            return nullptr;
        }
        e.loading = true;
        e.failed = false;
        jobs_.push_back({key, url, w, h, priority, ++jobOrder_});
        cv_.notify_one();
    } else if (e.loading) {
        for (auto& job : jobs_) {
            if (job.key != key) continue;
            if (priority > job.priority) {
                job.priority = priority;
                job.order = ++jobOrder_;
            }
            break;
        }
    }
    return nullptr;
}

ImageCache::Stats ImageCache::stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return {bytes_, jobs_.size(), memoryHits_, diskHits_, downloads_, failures_, retries_, queueDrops_,
            workersStarted_, diskEnabled_, diskReadErrors_, diskWriteErrors_};
}

void ImageCache::evict() {
    // Caller holds mutex_.
    while (bytes_ > budget_) {
        auto victim = entries_.end();
        for (auto it = entries_.begin(); it != entries_.end(); ++it)
            if (it->second.image && (victim == entries_.end() || it->second.lastUse < victim->second.lastUse))
                victim = it;
        if (victim == entries_.end()) break;
        bytes_ -= victim->second.image->bytes();
        entries_.erase(victim);
    }
}

std::string ImageCache::diskPath(const std::string& url) const {
    uint64_t h = 1469598103934665603ull;  // FNV-1a of the sized URL
    for (unsigned char ch : url) h = (h ^ ch) * 1099511628211ull;
    char name[24];
    std::snprintf(name, sizeof name, "%016llx", static_cast<unsigned long long>(h));
    return diskDir_ + "/" + name;
}

void ImageCache::trimDisk() {
    std::lock_guard<std::mutex> diskLock(diskMutex_);
    struct File {
        std::string path;
        off_t size;
        time_t mtime;
    };
    std::vector<File> files;
    size_t total = 0;
    if (!diskEnabled_) return;
    if (DIR* d = ::opendir(diskDir_.c_str())) {
        while (dirent* e = ::readdir(d)) {
            if (e->d_name[0] == '.' || std::strstr(e->d_name, ".tmp") != nullptr) continue;
            std::string path = diskDir_ + "/" + e->d_name;
            struct stat st {};
            if (::stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) continue;
            files.push_back({path, st.st_size, st.st_mtime});
            total += static_cast<size_t>(st.st_size);
        }
        ::closedir(d);
    } else {
        XC_LOGW("image cache scan failed errno=%d", errno);
        return;
    }
    if (total <= diskBudget_) {
        XC_LOGI("image cache: %zu files, %zu MB on disk", files.size(), total >> 20);
        return;
    }
    std::sort(files.begin(), files.end(), [](const File& a, const File& b) { return a.mtime < b.mtime; });
    size_t removed = 0;
    for (const auto& f : files) {
        if (total <= diskBudget_ * 3 / 4) break;  // some headroom
        if (::unlink(f.path.c_str()) == 0) {
            total -= static_cast<size_t>(f.size);
            ++removed;
        }
    }
    XC_LOGI("image cache: trimmed %zu files, %zu MB left", removed, total >> 20);
}

void ImageCache::trimDiskAfterWrite() {
    uint32_t writes = diskWritesSinceTrim_.fetch_add(1, std::memory_order_relaxed) + 1;
    if (writes < 16) return;
    uint32_t expected = writes;
    if (diskWritesSinceTrim_.compare_exchange_strong(expected, 0, std::memory_order_relaxed)) trimDisk();
}

void ImageCache::loop() {
    if (initialTrimPending_.exchange(false, std::memory_order_relaxed)) trimDisk();
    auto awaitGameplayPause = [this]() {
#if defined(__ORBIS__)
        bool logged = false;
        while (net::backgroundHttpPaused()) {
            { std::lock_guard<std::mutex> lock(mutex_); if (stop_) return false; }
            if (!logged) { XC_LOGI("background_artwork paused=1 disk_decode_resize=paused"); logged = true; }
            platform::sleepMs(10);
        }
        if (logged) XC_LOGI("background_artwork paused=0 disk_decode_resize=resumed");
#endif
        return true;
    };
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] { return stop_ || !jobs_.empty(); });
            if (stop_) return;
            auto best = std::max_element(jobs_.begin(), jobs_.end(), [](const Job& a, const Job& b) {
                if (a.priority != b.priority) return a.priority < b.priority;
                return a.order > b.order;
            });
            job = std::move(*best);
            jobs_.erase(best);
        }
        if (!awaitGameplayPause()) return;
        net::Request req;
        req.url = sizedUrl(job.url, job.w, job.h);
        req.headers = {{"Accept", "image/*"}};
        Image decoded;
        std::shared_ptr<Image> result;
        std::string cached;
        std::string path = diskEnabled_ ? diskPath(req.url) : std::string();
        net::Response r;
        if (!path.empty()) {
            bool diskRead = false;
            {
                std::lock_guard<std::mutex> diskLock(diskMutex_);
                diskRead = readCacheFile(path, cached);
                if (diskRead) {
                    struct utimbuf times {};
                    times.actime = times.modtime = std::time(nullptr);
                    ::utime(path.c_str(), &times);
                } else if (errno != ENOENT) {
                    std::lock_guard<std::mutex> lock(mutex_);
                    ++diskReadErrors_;
                }
            }
            if (diskRead) {
                if (decodeImage(cached, decoded)) {
                    r.status = 200;  // from disk
                    std::lock_guard<std::mutex> lock(mutex_);
                    ++diskHits_;
                } else {
                    {
                        std::lock_guard<std::mutex> diskLock(diskMutex_);
                        ::unlink(path.c_str());
                    }
                    std::lock_guard<std::mutex> lock(mutex_);
                    ++diskReadErrors_;
                }
            }
        }
        if (!r.ok()) {
            r = net::perform(req);
            if (r.ok()) {
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    ++downloads_;
                }
                if (!decodeImage(r.body, decoded)) decoded = {};
                else if (!path.empty()) {
                    bool written;
                    {
                        std::lock_guard<std::mutex> diskLock(diskMutex_);
                        written = platform::writeFileAtomic(path, r.body);
                    }
                    if (written) trimDiskAfterWrite();
                    else {
                        std::lock_guard<std::mutex> lock(mutex_);
                        ++diskWriteErrors_;
                    }
                }
            }
        }
        if (!awaitGameplayPause()) return;
        if (r.ok() && decoded.w > 0) {
            result = std::make_shared<Image>(decoded.w == job.w && decoded.h == job.h
                                                 ? std::move(decoded)
                                                 : coverResize(decoded, job.w, job.h));
        } else {
            XC_LOGW("image cache load failed status=%d reason=%s", r.status,
                    r.status ? "http_or_invalid_image" : "transport_error");
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            Entry& e = entries_[job.key];
            e.loading = false;
            if (result) {
                e.image = result;
                bytes_ += result->bytes();
                evict();
                e.failureCount = 0;
                e.retryAtMs = 0;
            } else {
                e.failed = true;
                e.failureCount = static_cast<uint8_t>(std::min<unsigned>(e.failureCount + 1, 7));
                const uint64_t backoffMs = std::min<uint64_t>(300000, 2000ull << (e.failureCount - 1));
                e.retryAtMs = platform::nowMs() + backoffMs;
                ++failures_;
            }
        }
        if (result && onReady_) onReady_();
    }
}

}  // namespace xc::ui
