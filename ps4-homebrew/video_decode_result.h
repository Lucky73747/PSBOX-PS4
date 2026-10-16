// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
namespace xc::ps4 {
enum class DecodeResult { Failure, Deferred, Output, SerialFlush };
// Framebuffer acceptance and bitstream processing are distinct. A successful
// asynchronous call may consume input before accepting an output surface.
inline DecodeResult classifyDecodeResult(uint32_t depth, int32_t rc, int32_t submitRc,
                                        bool accepted, bool valid, bool error) {
    if (rc != 0 || submitRc != 0 || error || (depth == 1 && !accepted))
        return DecodeResult::Failure;
    if (valid) return DecodeResult::Output;
    return depth > 1 ? DecodeResult::Deferred : DecodeResult::SerialFlush;
}
}
