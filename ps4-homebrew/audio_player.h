// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

namespace xc::ps4 {

class AudioPlayer {
public:
    AudioPlayer();
    ~AudioPlayer();
    AudioPlayer(const AudioPlayer&) = delete;
    AudioPlayer& operator=(const AudioPlayer&) = delete;

    // Audio is optional: failure must not prevent video streaming.
    bool start();
    bool enqueue(const uint8_t* opus, size_t size, uint32_t rtpTimestamp);
    void setVolume(uint32_t percent);
    uint32_t volume() const;
    void stop();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace xc::ps4
