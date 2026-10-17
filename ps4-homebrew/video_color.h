// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

namespace xc::ps4 {

enum class VideoColorMatrix : uint8_t {
    Bt709 = 0,
    Bt601 = 1,
    Bt2020 = 2,
};

// SDR NV12 fallback: BT.709 limited range. SPS/VUI values override these
// defaults when the stream signals a supported matrix or range.
struct VideoColorInfo {
    VideoColorMatrix matrix = VideoColorMatrix::Bt709;
    bool fullRange = false;
    uint8_t signaledMatrixCoefficients = 2; // H.264 value 2 means unspecified.
    bool matrixSignaled = false;
    bool rangeSignaled = false;
};

} // namespace xc::ps4
