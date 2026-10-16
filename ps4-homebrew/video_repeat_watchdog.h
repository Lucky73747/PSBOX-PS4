// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
namespace xc::ps4 {
class VideoRepeatWatchdog {
public:
    struct Decision { bool reset = false; uint32_t threshold = 0; uint64_t stalledMs = 0; double score = 0; };
    Decision evaluate(bool enabled, bool known, uint64_t sequence, uint32_t consecutive,
                      uint32_t targetFps, uint32_t decodedFps, uint64_t nowMs) {
        Decision out;
        if (!known || !sequence) return out;
        if (sequence != sequence_) {
            sequence_ = sequence;
            firstRepeatedAt_ = consecutive > 1 ? nowMs : 0;
            fired_ = false;
        } else if (consecutive > 1 && !firstRepeatedAt_) {
            firstRepeatedAt_ = nowMs;
        }
        const uint32_t fps = targetFps == 30 ? 30 : 60;
        // Wait for at least 0.75 s of confirmed repeated scanouts, with a floor
        // of six flips and a ceiling of one second for low/odd display rates.
        out.threshold = static_cast<uint32_t>((fps * 3 + 3) / 4);
        if (out.threshold < 6) out.threshold = 6;
        const uint64_t requiredMs = (out.threshold * 1000ull + fps - 1) / fps;
        out.stalledMs = firstRepeatedAt_ && nowMs >= firstRepeatedAt_ ? nowMs - firstRepeatedAt_ : 0;
        const double repeatScore = out.threshold ? static_cast<double>(consecutive) / out.threshold : 0.0;
        const double timeScore = requiredMs ? static_cast<double>(out.stalledMs) / requiredMs : 0.0;
        const double flowScore = decodedFps == 0 ? 1.0 : 0.5;
        out.score = 0.45 * repeatScore + 0.45 * timeScore + 0.10 * flowScore;
        if (!enabled || fired_ || nowMs < resetAt_ || nowMs - resetAt_ < 2000) return out;
        if (consecutive >= out.threshold && out.stalledMs >= requiredMs && out.score >= 1.0) {
            fired_ = true;
            resetAt_ = nowMs;
            out.reset = true;
        }
        return out;
    }
private:
    uint64_t sequence_ = 0, resetAt_ = 0, firstRepeatedAt_ = 0;
    bool fired_ = false;
};
}
