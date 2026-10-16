// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
namespace xc::ps4 {
// Absorb short delivery/scheduling bursts without losing H.264 references.
// The age threshold requests catch-up; only units/bytes are hard safety bounds.
struct VideoQueuePolicy {
    static constexpr size_t maxUnits = 18;
    static constexpr size_t maxBytes = 8u * 1024u * 1024u;
    static constexpr uint64_t maxAgeUs = 180000;
    static const char* reject(size_t units, size_t bytes, size_t incoming, uint64_t ageUs) {
        if (units >= maxUnits) return "unit_limit";
        if (incoming > maxBytes || bytes > maxBytes - incoming) return "byte_limit";
        (void)ageUs; // Age alone must not destroy the H.264 reference chain.
        return nullptr;
    }
};
}
