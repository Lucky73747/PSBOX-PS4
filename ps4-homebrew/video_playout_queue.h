// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <vector>
#include <cstdint>
#include <cstring>
#include <cstddef>

namespace xc::ps4 {
// Caller serializes access. Retain two reusable decoded surfaces: dropping
// these is safe for H.264 references, unlike dropping compressed input AUs.
class VideoPlayoutQueue {
public:
    static constexpr size_t capacity = 2;
    void clear(bool resetStats = false) {
        head_ = count_ = 0; primed_ = false;
        if (resetStats) uploaded = replaced = repeated = primeWaits = 0;
        lastSequence = 0;
    }
        if (count_ == capacity) { head_ = (head_ + 1) % capacity; --count_; ++replaced; }
        auto& frame = frames_[(head_ + count_) % capacity];
        ++count_;
    }
        const bool smooth = waitFrames && fps == 60;
        if (smooth != smooth_) { primed_ = false; smooth_ = smooth; }
        if (!count_) { if (primed_) ++repeated; primed_ = false; return false; }
        if (waitFrames && fps == 60 && !primed_ && count_ < 2) { ++primeWaits; return false; }
        primed_ = true;
        // Present the newest complete decoded picture at either refresh rate.
        // Compressed reference pictures are still decoded in order.
        while ((!waitFrames || fps != 60) && count_ > 1) { head_ = (head_ + 1) % capacity; --count_; ++replaced; }
        const auto& frame = frames_[head_];
        if (!destination || bytes < frame.bytes.size()) return false;
        std::memcpy(destination, frame.bytes.data(), frame.bytes.size());
        head_ = (head_ + 1) % capacity; --count_; ++uploaded;
        return true;
    }
        const bool smooth = waitFrames && fps == 60;
        if (smooth != smooth_) { primed_ = false; smooth_ = smooth; }
        if (!count_) { if (primed_) ++repeated; primed_ = false; return false; }
        while ((!waitFrames || fps != 60) && count_ > 1) { head_ = (head_ + 1) % capacity; --count_; ++replaced; }
        if (waitFrames && fps == 60 && !primed_ && count_ < 2) { ++primeWaits; return false; }
        auto& frame = frames_[head_];
        if (bytes < frame.bytes.size()) return false;
        lastSequence = frame.sequence; primed_ = true;
        head_ = (head_ + 1) % capacity; --count_; ++uploaded;
        return true;
    }
    size_t size() const { return count_; }
    uint64_t uploaded = 0, replaced = 0, repeated = 0, primeWaits = 0, lastSequence = 0;
private:
    std::array<Frame, capacity> frames_;
    size_t head_ = 0, count_ = 0;
    bool primed_ = false, smooth_ = false;
};
}
