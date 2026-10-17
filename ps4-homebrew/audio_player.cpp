// SPDX-License-Identifier: GPL-3.0-or-later
#include "audio_player.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include "util/log.h"
#include <opus.h>

#include <array>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>

namespace xc::ps4 {
namespace {
constexpr int kSampleRate = 48000;
constexpr int kChannels = 2;
constexpr int kMaxOpusSamples = 5760;  // Opus maximum packet duration: 120 ms at 48 kHz.
constexpr size_t kMaxPacketBytes = 1500;
constexpr size_t kQueueCapacity = 64;
constexpr uint32_t kSoftwareQueueMaxSamples = kSampleRate / 10;  // 100 ms.
constexpr int kOutputQueueMaxMs = 100;
constexpr uint64_t kMetricsIntervalMs = 2000;
}

struct AudioPlayer::Impl {
    struct Packet {
        std::array<uint8_t, kMaxPacketBytes> bytes{};
        size_t size = 0;
        uint32_t samples = 0;
        uint32_t timestamp = 0;
    };

    std::array<Packet, kQueueCapacity> queue{};
    size_t readIndex = 0;
    size_t writeIndex = 0;
    size_t queued = 0;
    uint32_t queuedSamples = 0;
    std::mutex mutex;
    std::condition_variable changed;
    std::atomic<bool> running{false};
    std::atomic<uint32_t> volumePercent{100};
    std::thread worker;
    SDL_AudioStream* stream = nullptr;
    OpusDecoder* decoder = nullptr;
    bool audioSubsystem = false;
    std::atomic<uint64_t> received{0}, bytes{0}, queueOverflow{0}, invalidPackets{0};
    std::atomic<uint64_t> decoded{0}, decodeErrors{0}, pcmSubmitted{0}, pcmDropped{0};
    std::atomic<uint64_t> queueEmptyWaits{0};

    void run() {
        std::array<float, static_cast<size_t>(kMaxOpusSamples) * kChannels> pcm{};
        uint64_t lastReport = SDL_GetTicks();
        uint64_t prevReceived = 0, prevBytes = 0, prevDecoded = 0, prevSubmitted = 0;
        uint64_t prevDropped = 0, prevOverflow = 0, prevErrors = 0;
        while (running.load(std::memory_order_acquire)) {
            Packet packet;
            {
                std::unique_lock<std::mutex> lock(mutex);
                if (queued == 0) {
                    queueEmptyWaits.fetch_add(1, std::memory_order_relaxed);
                    changed.wait(lock, [&] {
                        return !running.load(std::memory_order_acquire) || queued != 0;
                    });
                }
                if (!running.load(std::memory_order_acquire)) break;
                packet = queue[readIndex];
                readIndex = (readIndex + 1) % kQueueCapacity;
                --queued;
                queuedSamples -= packet.samples;
            }

            if (packet.size != 0) {
                const int samples = opus_decode_float(decoder, packet.bytes.data(),
                    static_cast<opus_int32>(packet.size), pcm.data(), kMaxOpusSamples, 0);
                if (samples < 0) {
                    decodeErrors.fetch_add(1, std::memory_order_relaxed);
                } else {
                    const uint32_t volume = volumePercent.load(std::memory_order_relaxed);
                    if (volume < 100) {
                        const float gain = static_cast<float>(volume) / 100.0f;
                        const size_t sampleCount = static_cast<size_t>(samples) * kChannels;
                        for (size_t i = 0; i < sampleCount; ++i) pcm[i] *= gain;
                    }
                    decoded.fetch_add(1, std::memory_order_relaxed);
                    const int outputBytes = samples * kChannels * static_cast<int>(sizeof(float));
                    const int available = SDL_GetAudioStreamAvailable(stream);
                    const int maxQueuedBytes = kSampleRate * kChannels * static_cast<int>(sizeof(float)) *
                                               kOutputQueueMaxMs / 1000;
                    if (available < 0 || available > maxQueuedBytes) {
                        // Drop this decoded chunk if the device queue has become stale.
                        pcmDropped.fetch_add(static_cast<uint64_t>(samples), std::memory_order_relaxed);
                    } else if (outputBytes > maxQueuedBytes - available) {
                        pcmDropped.fetch_add(static_cast<uint64_t>(samples), std::memory_order_relaxed);
                    } else if (!SDL_PutAudioStreamData(stream, pcm.data(), outputBytes)) {
                        decodeErrors.fetch_add(1, std::memory_order_relaxed);
                    } else {
                        pcmSubmitted.fetch_add(static_cast<uint64_t>(samples), std::memory_order_relaxed);
                    }
                }
            }

            const uint64_t now = SDL_GetTicks();
            if (now - lastReport >= kMetricsIntervalMs) {
                const uint64_t rx = received.load(std::memory_order_relaxed);
                const uint64_t byteCount = bytes.load(std::memory_order_relaxed);
                const uint64_t dec = decoded.load(std::memory_order_relaxed);
                const uint64_t sub = pcmSubmitted.load(std::memory_order_relaxed);
                const uint64_t drop = pcmDropped.load(std::memory_order_relaxed);
                const uint64_t over = queueOverflow.load(std::memory_order_relaxed);
                const uint64_t err = decodeErrors.load(std::memory_order_relaxed);
                const uint64_t bad = invalidPackets.load(std::memory_order_relaxed);
                size_t depth;
                uint32_t softwareQueueSamples;
                { std::lock_guard<std::mutex> lock(mutex); depth = queued; softwareQueueSamples = queuedSamples; }
                const int deviceBytes = SDL_GetAudioStreamAvailable(stream);
                const unsigned queuedMs = deviceBytes > 0
                    ? static_cast<unsigned>(deviceBytes * 1000 / (kSampleRate * kChannels * sizeof(float))) : 0;
                XC_LOGI("audio_metrics window_ms=%llu rx_packets=%llu rx_bytes=%llu decoded_packets=%llu submitted_pcm_ms=%llu queue_packets=%zu software_queue_ms=%u queue_limit_packets=%zu device_queue_ms=%u queue_overflow=%llu invalid_packets=%llu pcm_dropped_ms=%llu decode_errors=%llu queue_empty_waits=%llu rtp_timestamp=%u",
                    static_cast<unsigned long long>(now - lastReport),
                    static_cast<unsigned long long>(rx - prevReceived),
                    static_cast<unsigned long long>(byteCount - prevBytes),
                    static_cast<unsigned long long>(dec - prevDecoded),
                    static_cast<unsigned long long>((sub - prevSubmitted) * 1000 / kSampleRate),
                    depth, softwareQueueSamples * 1000u / kSampleRate, kQueueCapacity, queuedMs,
                    static_cast<unsigned long long>(over - prevOverflow),
                    static_cast<unsigned long long>(bad),
                    static_cast<unsigned long long>((drop - prevDropped) * 1000 / kSampleRate),
                    static_cast<unsigned long long>(err - prevErrors),
                    static_cast<unsigned long long>(queueEmptyWaits.exchange(0, std::memory_order_relaxed)),
                    packet.timestamp);
                prevReceived = rx; prevBytes = byteCount; prevDecoded = dec; prevSubmitted = sub;
                prevDropped = drop; prevOverflow = over; prevErrors = err;
                lastReport = now;
            }
        }
    }
};

AudioPlayer::AudioPlayer() : impl_(new Impl) {}
AudioPlayer::~AudioPlayer() { stop(); }

bool AudioPlayer::start() {
    auto& p = *impl_;
    if (p.running.load(std::memory_order_acquire)) return true;
    int opusError = OPUS_OK;
    p.decoder = opus_decoder_create(kSampleRate, kChannels, &opusError);
    if (!p.decoder || opusError != OPUS_OK) {
        XC_LOGW("audio_start failed stage=opus_decoder error=%d", opusError);
        if (p.decoder) opus_decoder_destroy(p.decoder);
        p.decoder = nullptr;
        return false;
    }
    // This homebrew supplies its own main(), without SDL's SDL_main wrapper.
    SDL_SetMainReady();
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        XC_LOGW("audio_start failed stage=sdl_init error=%s", SDL_GetError());
        opus_decoder_destroy(p.decoder); p.decoder = nullptr;
        return false;
    }
    p.audioSubsystem = true;
    const SDL_AudioSpec spec{SDL_AUDIO_F32, kChannels, kSampleRate};
    p.stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (!p.stream) {
        XC_LOGW("audio_start failed stage=sdl_open error=%s", SDL_GetError());
        SDL_QuitSubSystem(SDL_INIT_AUDIO); p.audioSubsystem = false;
        opus_decoder_destroy(p.decoder); p.decoder = nullptr;
        return false;
    }
    p.running.store(true, std::memory_order_release);
    try {
        p.worker = std::thread([&p] { p.run(); });
    } catch (...) {
        p.running.store(false, std::memory_order_release);
        SDL_DestroyAudioStream(p.stream); p.stream = nullptr;
        SDL_QuitSubSystem(SDL_INIT_AUDIO); p.audioSubsystem = false;
        opus_decoder_destroy(p.decoder); p.decoder = nullptr;
        XC_LOGW("audio_start failed stage=worker_create");
        return false;
    }
    if (!SDL_ResumeAudioStreamDevice(p.stream)) {
        XC_LOGW("audio_start failed stage=sdl_resume error=%s", SDL_GetError());
        stop();
        return false;
    }
    XC_LOGI("audio_start ready decoder=libopus rate_hz=48000 channels=2 format=f32 driver=%s queue_packets=%zu device_queue_cap_ms=%d",
            SDL_GetCurrentAudioDriver() ? SDL_GetCurrentAudioDriver() : "unknown",
            kQueueCapacity, kOutputQueueMaxMs);
    return true;
}

bool AudioPlayer::enqueue(const uint8_t* opus, size_t size, uint32_t timestamp) {
    auto& p = *impl_;
    if (!p.running.load(std::memory_order_acquire)) return false;
    p.received.fetch_add(1, std::memory_order_relaxed);
    p.bytes.fetch_add(size, std::memory_order_relaxed);
    const int packetSamples = opus && size > 0 && size <= kMaxPacketBytes
        ? opus_packet_get_nb_samples(opus, static_cast<opus_int32>(size), kSampleRate) : OPUS_BAD_ARG;
    if (packetSamples <= 0 || static_cast<uint32_t>(packetSamples) > kSoftwareQueueMaxSamples) {
        p.invalidPackets.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    std::lock_guard<std::mutex> lock(p.mutex);
    if (!p.running.load(std::memory_order_acquire)) return false;
    if (p.queued == kQueueCapacity || p.queuedSamples + static_cast<uint32_t>(packetSamples) > kSoftwareQueueMaxSamples) {
        p.queueOverflow.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    auto& slot = p.queue[p.writeIndex];
    std::memcpy(slot.bytes.data(), opus, size);
    slot.size = size;
    slot.samples = static_cast<uint32_t>(packetSamples);
    slot.timestamp = timestamp;
    p.writeIndex = (p.writeIndex + 1) % kQueueCapacity;
    ++p.queued;
    p.queuedSamples += static_cast<uint32_t>(packetSamples);
    p.changed.notify_one();
    return true;
}

void AudioPlayer::setVolume(uint32_t percent) {
    impl_->volumePercent.store(std::min(percent, 100u), std::memory_order_relaxed);
}

uint32_t AudioPlayer::volume() const {
    return impl_->volumePercent.load(std::memory_order_relaxed);
}

void AudioPlayer::stop() {
    if (!impl_) return;
    auto& p = *impl_;
    {
        // Change the wait predicate while holding its mutex so a stop
        // notification cannot be lost between the predicate check and wait.
        std::lock_guard<std::mutex> lock(p.mutex);
        p.running.store(false, std::memory_order_release);
    }
    p.changed.notify_all();
    if (p.worker.joinable()) p.worker.join();
    if (p.stream) {
        (void)SDL_PauseAudioStreamDevice(p.stream);
        SDL_DestroyAudioStream(p.stream);
        p.stream = nullptr;
    }
    if (p.audioSubsystem) {
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        p.audioSubsystem = false;
    }
    if (p.decoder) {
        opus_decoder_destroy(p.decoder);
        p.decoder = nullptr;
    }
    std::lock_guard<std::mutex> lock(p.mutex);
    p.readIndex = p.writeIndex = p.queued = 0;
    p.queuedSamples = 0;
    XC_LOGI("audio_stop complete");
}

}  // namespace xc::ps4
