// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 RafaelNGP
// RTCP receiver side of a track: RFC 3550 receiver reports (loss, jitter,
// LSR/DLSR for the sender's RTT) and, for video, REMB bandwidth estimates.
//
// The xCloud server negotiates goog-remb for congestion control. REMB here is
// receiver feedback requested from the server, not an acknowledgment or a
// guaranteed hard cap on the incoming media rate.
// libdatachannel's RtcpReceivingSession reports a highest sequence number of
// 0, no loss and no jitter, and sends REMB only if asked: the server then
// shows the "slow connection" icon and keeps the bitrate low.
#pragma once

#include "stream/video_stats.h"
#include <rtc/rtc.hpp>

#include <atomic>
#include <cstdint>

namespace xc::stream {

// SSRC of our RTCP packets (receiver reports, NACK, PLI, REMB).
constexpr uint32_t kReceiverSsrc = 0x50534278;  // "PSBx"

class RtcpReporter final : public rtc::MediaHandler {
public:
    // `clockRate`: RTP clock of the track (90000 video, 48000 Opus).
    // `maxBitrate`: bits/s offered with REMB; 0 = no REMB (audio).
    RtcpReporter(uint32_t clockRate, uint32_t maxBitrate, VideoReceiveStats* videoStats = nullptr);

    void incoming(rtc::message_vector& messages, const rtc::message_callback& send) override;

    // Last locally advertised REMB request and measured receive rate, bits/s.
    uint32_t estimate() const { return estimate_; }
    uint32_t receiveRate() const { return receiveRate_; }
    uint64_t lastRtpAtMs() const { return lastRtpAtMs_.load(std::memory_order_relaxed); }
    uint64_t rembReportsRequested() const { return rembReportsRequested_.load(std::memory_order_relaxed); }
    uint64_t rembLastRequestAtMs() const { return rembLastRequestAtMs_.load(std::memory_order_relaxed); }
    uint32_t rembLastRequestedBitrate() const { return rembLastRequestedBitrate_.load(std::memory_order_relaxed); }
    uint64_t receiveRateAboveCapSamples() const { return receiveRateAboveCapSamples_.load(std::memory_order_relaxed); }
    // Safe to update from the app thread; incoming()/report() remain single-writer.
    void setMaximumBitrate(uint32_t bitsPerSecond);

private:
    void onRtp(const uint8_t* d, size_t n, uint64_t nowUs);
    void onRtcp(const uint8_t* d, size_t n, uint64_t nowUs);
    void report(uint64_t nowUs, const rtc::message_callback& send);

    const uint32_t clockRate_;
    VideoReceiveStats* videoStats_ = nullptr;
    std::atomic<uint32_t> maxBitrate_{0};
    uint32_t mediaSsrc_ = 0;
    bool started_ = false;
    uint16_t maxSeq_ = 0;
    uint32_t cycles_ = 0, baseSeq_ = 0;
    uint32_t received_ = 0, expectedPrior_ = 0, receivedPrior_ = 0;
    double jitter_ = 0;
    int64_t lastTransit_ = 0;
    uint32_t lastTimestamp_ = 0;
    bool haveTransit_ = false;
    uint32_t lastSr_ = 0;          // middle 32 bits of the last SR's NTP time
    uint64_t lastSrArrivalUs_ = 0;
    uint64_t lastReportUs_ = 0;
    uint64_t bytesSinceReport_ = 0;
    std::atomic<uint32_t> estimate_{0}, receiveRate_{0};
    std::atomic<uint64_t> lastRtpAtMs_{0};
    std::atomic<uint64_t> rembReportsRequested_{0}, rembLastRequestAtMs_{0}, receiveRateAboveCapSamples_{0};
    std::atomic<uint32_t> rembLastRequestedBitrate_{0};
};

}  // namespace xc::stream
