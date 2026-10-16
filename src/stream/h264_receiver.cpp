// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 RafaelNGP
#include "stream/h264_receiver.h"

#include "stream/rtcp_reporter.h"

#include "platform/platform.h"
#include "util/log.h"

#include <algorithm>

namespace xc::stream {

namespace {

// How long a hole may wait for its retransmission (a couple of RTTs), how
// often a NACK is repeated, and how many times.
constexpr uint64_t kWaitMs = 150;
constexpr uint64_t kRenackMs = 40;
constexpr int kMaxNacks = 3;
constexpr size_t kMaxBuffered = 4000;      // packets (about 4 MB)

constexpr uint8_t kStartCode[] = {0, 0, 0, 1};

// NAL types that begin a decodable access unit after loss.
bool isKeyNal(uint8_t type) { return type == 5; }

// Parameter-only AUs may initialize the decoder, but never end IDR recovery.
bool parameterOnly(const std::vector<uint8_t>& bytes) {
    bool found = false;
    for (size_t i=0; i+4<bytes.size(); ++i) {
        if (bytes[i] || bytes[i+1] || bytes[i+2] || bytes[i+3] != 1) continue;
        const auto type = bytes[i+4] & 0x1f;
        if (type >= 1 && type <= 5) return false;
        if (type == 7 || type == 8) found = true;
    }
    return found;
}

void put16(std::vector<uint8_t>& b, uint16_t v) {
    b.push_back(static_cast<uint8_t>(v >> 8));
    b.push_back(static_cast<uint8_t>(v));
}
void put32(std::vector<uint8_t>& b, uint32_t v) {
    for (int s = 24; s >= 0; s -= 8) b.push_back(static_cast<uint8_t>(v >> s));
}

const std::byte* asBytes(const std::vector<uint8_t>& v) { return reinterpret_cast<const std::byte*>(v.data()); }

}  // namespace

H264Receiver::H264Receiver(std::function<void()> onKeyframeNeeded, VideoReceiveStats& stats)
    : onKeyframeNeeded_(std::move(onKeyframeNeeded)), stats_(stats) {}

int64_t H264Receiver::unwrap(uint16_t seq) {
    if (highest_ < 0) return seq;
    int64_t base = highest_ & ~int64_t(0xFFFF);
    int64_t candidate = base | seq;
    // Pick the value closest to the highest seen (handles both wrap directions).
    if (candidate - highest_ > 0x8000) candidate -= 0x10000;
    else if (highest_ - candidate > 0x8000) candidate += 0x10000;
    return candidate;
}

void H264Receiver::sendNack(const std::vector<int64_t>& seqs, const rtc::message_callback& send) {
    if (seqs.empty() || !mediaSsrc_) return;
    // Generic NACK (RFC 4585 6.2.1): FCI = PID + bitmask of the next 16.
    std::vector<std::pair<uint16_t, uint16_t>> fci;
    for (size_t i = 0; i < seqs.size();) {
        int64_t pid = seqs[i];
        uint16_t blp = 0;
        size_t j = i + 1;
        while (j < seqs.size() && seqs[j] - pid <= 16) {
            blp |= static_cast<uint16_t>(1u << (seqs[j] - pid - 1));
            ++j;
        }
        fci.emplace_back(static_cast<uint16_t>(pid), blp);
        i = j;
    }
    std::vector<uint8_t> b;
    b.push_back(0x81);  // V=2, FMT=1
    b.push_back(205);   // RTPFB
    put16(b, static_cast<uint16_t>(2 + fci.size()));
    put32(b, kReceiverSsrc);
    put32(b, mediaSsrc_);
    for (auto [pid, blp] : fci) {
        put16(b, pid);
        put16(b, blp);
    }
    send(rtc::make_message(asBytes(b), asBytes(b) + b.size(), rtc::Message::Control));
    ++stats_.nacks;
    if (simulatedLoss_ > 0)
        for (int64_t s : seqs) {
            auto h = held_.find(static_cast<uint16_t>(s));
            if (h == held_.end()) continue;
            redeliver_.emplace_back(platform::nowMs() + 30, std::move(h->second));
            held_.erase(h);
        }
}

void H264Receiver::sendPli(const rtc::message_callback& send) {
    if (!mediaSsrc_) return;
    std::vector<uint8_t> b;
    b.push_back(0x81);  // V=2, FMT=1
    b.push_back(206);   // PSFB
    put16(b, 2);
    put32(b, kReceiverSsrc);
    put32(b, mediaSsrc_);
    send(rtc::make_message(asBytes(b), asBytes(b) + b.size(), rtc::Message::Control));
}

bool H264Receiver::requestKeyframe(const rtc::message_callback& send) {
    if (!mediaSsrc_.load(std::memory_order_relaxed)) return false;
    sendPli(send);
    ++stats_.keyframeRequests;
    return true;
}

void H264Receiver::askKeyframe(const rtc::message_callback& send) {
    waitingKeyframe_ = true;
    uint64_t now = platform::nowMs();
    if (now - lastKeyframeRequestMs_ < 300) return;  // one request per burst
    lastKeyframeRequestMs_ = now;
    ++stats_.keyframeRequests;
    sendPli(send);
    if (onKeyframeNeeded_) onKeyframeNeeded_();
}

void H264Receiver::noteGap(int64_t from, int64_t to, uint64_t now, const rtc::message_callback& send) {
    if (to - from > 512) {  // a huge jump: not worth NACKing, resynchronise
        stats_.lost += static_cast<uint64_t>(to - from);
        return;
    }
    std::vector<int64_t> seqs;
    for (int64_t s = from; s < to; ++s) {
        missing_[s] = Missing{now, now, 1};
        seqs.push_back(s);
    }
    stats_.lost += seqs.size();
    sendNack(seqs, send);
}

void H264Receiver::resendNacks(uint64_t now, const rtc::message_callback& send) {
    std::vector<int64_t> seqs;
    for (auto& [s, m] : missing_)
        if (m.nacks < kMaxNacks && now - m.lastNackMs >= kRenackMs) {
            m.lastNackMs = now;
            ++m.nacks;
            seqs.push_back(s);
        }
    sendNack(seqs, send);
}

bool H264Receiver::assemble(int64_t first, int64_t last, std::vector<uint8_t>& out, bool& keyframe) const {
    out.clear();
    keyframe = false;
    bool fragmented = false; uint8_t fragmentType = 0;
    const auto timestamp = packets_.at(first).timestamp;
    for (int64_t s = first; s <= last; ++s) {
        auto it = packets_.find(s);
        if (it == packets_.end()) return false;
        const auto& p = it->second.payload;
        if (p.empty()) return false;
        uint8_t type = p[0] & 0x1F;
        if (type >= 1 && type <= 23) {  // single NAL unit
            if (fragmented) return false;
            out.insert(out.end(), kStartCode, kStartCode + 4);
            out.insert(out.end(), p.begin(), p.end());
            keyframe |= isKeyNal(type);
        } else if (type == 24) {  // STAP-A
            if (fragmented) return false;
            size_t i = 1;
            while (i + 2 <= p.size()) {
                size_t n = (static_cast<size_t>(p[i]) << 8) | p[i + 1];
                i += 2;
                if (n == 0 || i + n > p.size() || !(p[i] & 0x1f) || (p[i] & 0x1f) > 23) return false;
                out.insert(out.end(), kStartCode, kStartCode + 4);
                out.insert(out.end(), p.begin() + static_cast<long>(i), p.begin() + static_cast<long>(i + n));
                keyframe |= isKeyNal(p[i] & 0x1F);
                i += n;
            }
            if (i != p.size()) return false;
        } else if (type == 28 && p.size() > 2) {  // FU-A
            bool start = (p[1] & 0x80) != 0;
            const bool end = (p[1] & 0x40) != 0;
            const uint8_t nal = p[1] & 0x1f;
            if (!nal || nal > 23 || (start && (fragmented || end)) || (!start && (!fragmented || nal != fragmentType))) return false;
            if (start) {
                fragmented = true; fragmentType = nal;
                out.insert(out.end(), kStartCode, kStartCode + 4);
                out.push_back(static_cast<uint8_t>((p[0] & 0xE0) | (p[1] & 0x1F)));
                keyframe |= isKeyNal(p[1] & 0x1F);
            }
            out.insert(out.end(), p.begin() + 2, p.end());
            if (end) fragmented = false;
        } else return false;
    }
    return !out.empty() && !fragmented;
}

bool H264Receiver::resync(rtc::message_vector& out) {
    for (auto it = packets_.begin(); it != packets_.end(); ++it) {
        auto prev = packets_.find(it->first - 1);
        if (prev == packets_.end()) {
            // A complete IDR/SPS or FU-A start can be a safe first boundary
            // even when there is no predecessor (startup or a new SSRC).
            const auto& payload = it->second.payload;
            if (payload.empty()) continue;
            const uint8_t type = payload[0] & 0x1f;
            // For IDR slices, first_mb_in_slice must be zero (the first
            // unsigned Exp-Golomb bit is 1). A later slice is not an AU start.
            bool stapStart = false;
            if (type == 24) {
                size_t offset = 1;
                while (offset + 2 <= payload.size()) {
                    size_t length = (size_t(payload[offset]) << 8) | payload[offset+1]; offset += 2;
                    if (!length || offset + length > payload.size()) break;
                    const auto nal = payload[offset] & 0x1f;
                    if (nal == 7 || (nal == 5 && length >= 2 && (payload[offset+1] & 0x80))) stapStart = true;
                    offset += length;
                }
            }
            const bool start = stapStart || type == 7 ||
                (type == 5 && payload.size() >= 2 && (payload[1] & 0x80)) ||
                (type == 28 && payload.size() >= 3 && (payload[1] & 0x80) &&
                 (((payload[1] & 0x1f) == 7) ||
                  ((payload[1] & 0x1f) == 5 && (payload[2] & 0x80))));
            if (!start) continue;
        } else if (!prev->second.marker && prev->second.timestamp == it->second.timestamp) continue;
            // Skip past the hole and inspect later frame boundaries.
            continue;
        }
        std::vector<uint8_t> frame;
        bool key = false;
            nextSeq_ = it->first;
            packets_.erase(packets_.begin(), it);  // older junk
            for (auto e = missing_.begin(); e != missing_.end() && e->first < nextSeq_;) e = missing_.erase(e);
            (void)out;
            return true;
        }
    }
    return false;
}

void H264Receiver::drain(uint64_t now, rtc::message_vector& out, const rtc::message_callback& send) {
    for (;;) {
        if (nextSeq_ < 0) {
            if (!resync(out)) return;
        }
        if (hole >= 0) {
            if (hole > highest_) return;  // just not here yet
            auto m = missing_.find(hole);
            uint64_t since = m != missing_.end() ? m->second.sinceMs : now;
            if (m == missing_.end()) missing_[hole] = Missing{now, now, 0};
            if (now - since < kWaitMs) return;  // give the retransmission time
            // Lost for good: drop what we have of this frame and resync on
            // the next key frame.
            ++stats_.framesDropped;
            for (auto e = packets_.begin(); e != packets_.end() && e->first <= hole;) e = packets_.erase(e);
            for (auto e = missing_.begin(); e != missing_.end() && e->first <= hole;) e = missing_.erase(e);
            nextSeq_ = -1;
            askKeyframe(send);
            continue;
        }
        std::vector<uint8_t> frame;
        bool key = false;
        if (waitingKeyframe_ && !key && !parameterOnly(frame)) {
            ++stats_.framesDropped;  // would decode against lost references
            continue;
        }
        if (key) waitingKeyframe_ = false;
        stats_.lastAccessUnitAtMs.store(now);
        if (key) stats_.lastKeyframeAtMs.store(now);
        ++stats_.framesOut;
        out.push_back(rtc::make_message(asBytes(frame), asBytes(frame) + frame.size(), std::make_shared<rtc::FrameInfo>(ts)));
    }
}

void H264Receiver::accept(const uint8_t* d, size_t n, uint64_t now, const rtc::message_callback& send) {
    if (n < 12 || (d[0] >> 6) != 2) return;
    uint16_t rawSeq = static_cast<uint16_t>((d[2] << 8) | d[3]);
    if (simulatedLoss_ > 0) {
        lossState_ = lossState_ * 1103515245u + 12345u;
        if ((lossState_ >> 16) % 100 < static_cast<uint32_t>(simulatedLoss_) && !held_.count(rawSeq)) {
            held_[rawSeq].assign(d, d + n);
            if (held_.size() > 2000) held_.erase(held_.begin());
            return;
        }
    }
    size_t off = 12 + 4u * (d[0] & 0x0F);
    if (d[0] & 0x10) {  // header extension
        if (off + 4 > n) return;
        off += 4 + 4u * ((static_cast<size_t>(d[off + 2]) << 8) | d[off + 3]);
    }
    size_t end = n;
    if (d[0] & 0x20) {  // padding
        if (d[n - 1] > n) return;
        end -= d[n - 1];
    }
    if (off > end) return;
    const uint32_t source = (uint32_t(d[8]) << 24) | (uint32_t(d[9]) << 16) | (uint32_t(d[10]) << 8) | d[11];
    const uint32_t previousSource = mediaSsrc_.exchange(source, std::memory_order_relaxed);
    if (previousSource && previousSource != source) {
        // Sequence numbers belong to one SSRC. Comparing a new source with
        // the previous highest sequence can reject fresh frames indefinitely.
        packets_.clear(); missing_.clear(); highest_ = nextSeq_ = -1;
        waitingKeyframe_ = true; lastKeyframeRequestMs_ = 0;
        XC_LOGI("h264_source_changed old_ssrc=%08x new_ssrc=%08x action=clear_jitter_wait_idr", previousSource, source);
        askKeyframe(send);
    }
    ++stats_.packets;

    int64_t seq = unwrap(rawSeq);
    if (nextSeq_ >= 0 && seq < nextSeq_) return;  // too late or duplicate
    if (packets_.count(seq)) return;
    if (missing_.erase(seq)) ++stats_.recovered;

    Packet p;
    p.timestamp = (uint32_t(d[4]) << 24) | (uint32_t(d[5]) << 16) | (uint32_t(d[6]) << 8) | d[7];
    p.marker = (d[1] & 0x80) != 0;
    p.payload.assign(d + off, d + end);
    packets_.emplace(seq, std::move(p));

    if (highest_ >= 0 && seq > highest_ + 1) noteGap(highest_ + 1, seq, now, send);
    if (seq > highest_) highest_ = seq;
}

void H264Receiver::incoming(rtc::message_vector& messages, const rtc::message_callback& send) {
    rtc::message_vector out;
    uint64_t now = platform::nowMs();
    for (auto it = redeliver_.begin(); it != redeliver_.end();) {
        if (it->first > now) {
            ++it;
            continue;
        }
        std::vector<uint8_t> packet = std::move(it->second);
        it = redeliver_.erase(it);
        accept(packet.data(), packet.size(), now, send);
    }
    for (auto& message : messages) {
        if (message->type == rtc::Message::Control) continue;  // RTCP: handled down the chain
        accept(reinterpret_cast<const uint8_t*>(message->data()), message->size(), now, send);
    }
    resendNacks(now, send);
    drain(now, out, send);
    // The first PLI can be lost along with the recovery frame. Repeat it while
    // waiting for a key frame; askKeyframe rate-limits requests to 300 ms.
    if (nextSeq_ < 0 && !packets_.empty()) askKeyframe(send);
    if (packets_.size() > kMaxBuffered) {  // stuck: start over
        packets_.clear();
        missing_.clear();
        nextSeq_ = -1;
        askKeyframe(send);
    }
    stats_.bufferedPackets.store(packets_.size());
    stats_.missingPackets.store(missing_.size());
    stats_.waitingKeyframe.store(waitingKeyframe_);
    messages.swap(out);
}

}  // namespace xc::stream
