// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
namespace xc::ps4 {
// Reuse only released surfaces, rotating across them instead of repeatedly
// selecting surface zero. No accepted/pending surface can be overwritten.
template<class IsFree>
size_t nextAvailableSurface(size_t count, size_t& next, IsFree isFree) {
    if (!count) return 0;
    for (size_t step = 0; step < count; ++step) {
        const size_t index = (next + step) % count;
        if (isFree(index)) { next = (index + 1) % count; return index; }
    }
    return count;
}
}
