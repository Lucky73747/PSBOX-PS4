// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 RafaelNGP
#include "stream/stream_session.h"

#include "stream/h264_receiver.h"
#include "stream/rtcp_reporter.h"

#include "platform/platform.h"
#include "util/json.h"
#include "util/log.h"

#include <rtc/rtc.hpp>

#if defined(__ORBIS__)
#include <orbis/libkernel.h>
#endif

#include <arpa/inet.h>
#include <algorithm>
#include <cerrno>
#include <netinet/in.h>
#include <sys/socket.h>

#include <atomic>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

namespace xc::stream {

namespace {

// Fixed values the xbox.com web client sends.
constexpr const char* kControlAccessKey = "4BDB3609-C1F1-4195-9B37-FEFF45DA8B8E";
constexpr const char* kHandshakeId = "be0bfc6d-1e83-4c8a-90ed-fa8601c5a179";
constexpr const char* kClientAppInstallId = "c97d7ee0-73b2-4239-bf1d-9d805a338429";
// SystemUiType of the system UIs this client draws itself.
constexpr int kShowVirtualKeyboard = 10;

constexpr const char* kSctpTracePath = "/data/PSBOX/sctp-flow-v50.log";
constexpr size_t kSctpTraceCapacity = 128;
constexpr size_t kSctpTraceLineSize = 768;

struct SctpTraceRecord {
    uint64_t sequence = 0;
    double timeMs = 0.0;
    char line[kSctpTraceLineSize]{};
};

std::mutex gSctpTraceMutex;
std::array<SctpTraceRecord, kSctpTraceCapacity> gSctpTrace{};
size_t gSctpTraceHead = 0;
size_t gSctpTraceCount = 0;
uint64_t gSctpTraceNextSequence = 1;
uint64_t gSctpTracePersistedSequence = 0;
std::atomic<bool> gSctpCommUpSeen{false};
std::atomic<bool> gPeerConnectionSctpCallbackSeen{false};

bool isSctpTraceLine(const char* message) {
    return message && (std::strstr(message, "STAGE L4") != nullptr ||
                       std::strstr(message, "peer connection state") != nullptr ||
                       std::strstr(message, "STAGE L3 OK event=ice_state") != nullptr);
}

void resetSctpFlowTrace() {
    {
        std::lock_guard<std::mutex> lock(gSctpTraceMutex);
        gSctpTraceHead = 0;
        gSctpTraceCount = 0;
        gSctpTraceNextSequence = 1;
        gSctpTracePersistedSequence = 0;
        gSctpTrace.fill({});
    }
    gSctpCommUpSeen = false;
    gPeerConnectionSctpCallbackSeen = false;

}

void appendSctpFlowSnapshot(const char* reason) {
}

void logIceDiagnostic(const char* message) {
#if defined(__ORBIS__)
    if (message && std::strstr(message, "event=sctp_association state=COMM_UP"))
        gSctpCommUpSeen = true;
    if (message && std::strstr(message, "event=peerconnection_sctp_callback phase=begin"))
        gPeerConnectionSctpCallbackSeen = true;

    if (isSctpTraceLine(message)) {
        std::lock_guard<std::mutex> lock(gSctpTraceMutex);
        auto& record = gSctpTrace[gSctpTraceHead];
        record.sequence = gSctpTraceNextSequence++;
        record.timeMs = clockMs();
        std::snprintf(record.line, sizeof(record.line), "%s", message ? message : "(null)");
        gSctpTraceHead = (gSctpTraceHead + 1) % kSctpTraceCapacity;
        gSctpTraceCount = std::min(gSctpTraceCount + 1, kSctpTraceCapacity);
    }

    // V33 socket diagnostics repeatedly dumped sockaddr bytes and flooded klog.
    if (message && std::strstr(message, "ICE_DIAG sockaddr "))
        return;

    char line[1536];
    std::snprintf(line, sizeof(line), "[%s] ICE_DIAG t_ms=%.1f %s\n",
                  kBuildLabel, clockMs(), message ? message : "(null)");
#else
    XC_LOGW("ICE_DIAG %s", message ? message : "(null)");
#endif
}

std::string uuid4() {
    uint8_t b[16];
    platform::randomBytes(b, sizeof b);
    b[6] = static_cast<uint8_t>((b[6] & 0x0f) | 0x40);
    b[8] = static_cast<uint8_t>((b[8] & 0x3f) | 0x80);
    char s[37];
    std::snprintf(s, sizeof s, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", b[0], b[1],
                  b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
    return s;
}

std::vector<std::string> midsInOrder(const std::string& sdp) {
    std::vector<std::string> mids;
    size_t pos = 0;
    while ((pos = sdp.find("a=mid:", pos)) != std::string::npos) {
        pos += 6;
        size_t end = sdp.find_first_of("\r\n", pos);
        mids.push_back(sdp.substr(pos, end - pos));
    }
    return mids;
}

// a=imageattr after every a=fmtp line: the largest picture we accept. The
// xbox.com client adds "recv [x=[0:1920],y=[0:1080],fps=[0:60]]" to limit
// devices that can't do 1440p; announcing 2560x1440 is what allows 1440p.
    if (sdp.find("a=imageattr") != std::string::npos) return sdp;
    std::string out;
    size_t pos = 0;
    while (pos < sdp.size()) {
        size_t eol = sdp.find('\n', pos);
        if (eol == std::string::npos) eol = sdp.size() - 1;
        std::string line = sdp.substr(pos, eol - pos + 1);
        out += line;
        if (line.rfind("a=fmtp:", 0) == 0 && line.find("profile-level-id") != std::string::npos) {
            std::string pt = line.substr(7, line.find(' ') - 7);
            out += "a=imageattr:" + pt + " recv [x=[0:" + std::to_string(w) + "],y=[0:" + std::to_string(h) +
        }
        pos = eol + 1;
    }
    return out;
}

void initRtcLogging() {
    static std::once_flag once;
    std::call_once(once, [] {
        rtc::InitLogger(rtc::LogLevel::Warning, [](rtc::LogLevel level, std::string message) {
            (void)level;
            logIceDiagnostic(message.c_str());
        });
    });
}

}  // namespace

bool decodeTeredo(const std::string& ipv6, std::string& ipv4, int& port) {
    in6_addr a{};
    if (::inet_pton(AF_INET6, ipv6.c_str(), &a) != 1) return false;
    const uint8_t* b = a.s6_addr;
    if (!(b[0] == 0x20 && b[1] == 0x01 && b[2] == 0x00 && b[3] == 0x00)) return false;
    // Bytes 10-11: obfuscated (inverted) port; 12-15: inverted client IPv4.
    port = static_cast<uint16_t>(~((b[10] << 8) | b[11]));
    char s[16];
    std::snprintf(s, sizeof s, "%u.%u.%u.%u", b[12] ^ 0xff, b[13] ^ 0xff, b[14] ^ 0xff, b[15] ^ 0xff);
    ipv4 = s;
    return true;
}

struct StreamSession::Impl {
    xcloud::GssvClient& gssv;
    StreamCallbacks cb;
    StreamOptions opt;

    std::shared_ptr<rtc::PeerConnection> pc;
    std::shared_ptr<rtc::DataChannel> chat, control, input, message;
    std::shared_ptr<rtc::Track> audio, video;

    std::mutex mutex;
    std::condition_variable cv;
    std::vector<rtc::Candidate> localCandidates;
    bool gatheringDone = false;
    bool handshakeDone = false;
    bool failed = false;
    std::atomic<bool> iceConnected{false};
    std::atomic<bool> peerConnectionConnectedCallbackSeen{false};
    std::atomic<unsigned> dataChannelsOpened{0};
    std::atomic<bool> handshakeSendAttempted{false};
    std::atomic<bool> handshakeAckReceived{false};
    std::string failReason;

    std::atomic<bool> open{false};
    std::atomic<bool> closedNotified{false};
    std::atomic<bool> firstVideoAccessUnit{false};
    std::atomic<bool> firstVideoTimeoutLogged{false};
    std::atomic<uint64_t> streamReadyAtMs{0};
    std::atomic<uint32_t> inputSequence{1};
    uint64_t lastKeepaliveMs = 0;
    VideoReceiveStats videoStats;
    std::shared_ptr<RtcpReporter> videoReporter;
    std::atomic<int> otherInputReports{0};

    Impl(xcloud::GssvClient& g, StreamCallbacks c, StreamOptions o) : gssv(g), cb(std::move(c)), opt(o) {}

    double nowMs() const { return stream::clockMs(); }

    void reportProgress(const char* phase) {
        const std::string message = phase ? phase : "transport setup";
        logIceDiagnostic(message.c_str());
        if (cb.progress) cb.progress(message);
    }

    void fail(const std::string& reason, bool recoverable = true) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (!failed) {
                failed = true;
                failReason = reason;
            }
        }
        cv.notify_all();
        open = false;
        if (!closedNotified.exchange(true)) {
            XC_LOGW("stream closed: %s", reason.c_str());
            if (cb.closed) cb.closed(reason, recoverable);
        }
    }

    static bool sendText(const std::shared_ptr<rtc::DataChannel>& ch, const std::string& text) {
        // The web client sends its JSON as binary (TextEncoder) messages.
        if (!ch || !ch->isOpen()) return false;
        try {
            return ch->send(reinterpret_cast<const rtc::byte*>(text.data()), text.size());
        } catch (const std::exception& e) {
            XC_LOGW("send on %s: %s", ch->label().c_str(), e.what());
            return false;
        }
    }

    void noteDataChannelOpen(const char* label) {
        const unsigned opened = ++dataChannelsOpened;
        char line[192];
        std::snprintf(line, sizeof(line),
                      "STAGE L4 OK event=data_channel_open channel=%s opened=%u expected=4",
                      label ? label : "(null)", opened);
        logIceDiagnostic(line);
        if (opened == 4)
            logIceDiagnostic("STAGE L4 OK event=all_data_channels_open count=4");
    }

    static bool sendBinary(const std::shared_ptr<rtc::DataChannel>& ch, const std::vector<uint8_t>& data) {
        if (!ch || !ch->isOpen()) return false;
        try {
            return ch->send(reinterpret_cast<const rtc::byte*>(data.data()), data.size());
        } catch (const std::exception& e) {
            XC_LOGW("send on %s: %s", ch->label().c_str(), e.what());
            return false;
        }
    }

    static std::string asText(const rtc::message_variant& m) {
        if (auto s = std::get_if<std::string>(&m)) return *s;
        const auto& b = std::get<rtc::binary>(m);
        return std::string(reinterpret_cast<const char*>(b.data()), b.size());
    }

    // --- message channel -----------------------------------------------------

    std::string messageEnvelope(const std::string& target, const json::Value& content) {
        json::Value v = json::Value::object();
        v.set("type", "Message");
        v.set("content", content.dump());
        v.set("id", uuid4());
        v.set("target", target);
        v.set("cv", "");
        return v.dump();
    }

    void completeTransaction(const std::string& id, const json::Value& content) {
        json::Value v = json::Value::object();
        v.set("type", "TransactionComplete");
        v.set("content", content.dump());
        v.set("id", id);
        v.set("cv", "");
    }

    void cancelTransaction(const std::string& id) {
        json::Value v = json::Value::object();
        v.set("type", "ReceiverCancel");
        v.set("id", id);
        v.set("cv", "");
        sendText(message, v.dump());
    }

    void sendResolution(const std::string& alias) {
        // The HQ tiers and 1440 need Game Pass Ultimate in a market where
        // they're enabled; elsewhere the service ignores them and keeps what
        // it had (1080 at the start). The plain tier first, then: measured,
        // "720" switches in a few seconds, "720HQ" is ignored in Brazil.
        std::string base = alias == "1440" ? "1080" : alias;
        if (base.size() > 2 && base.compare(base.size() - 2, 2, "HQ") == 0) base.resize(base.size() - 2);
        // The user's own Xbox offers every tier on any plan: just the one asked for.
        if (gssv.isHome()) base = alias;
        auto send = [this](const std::string& a) {
            json::Value res = json::Value::object();
            res.set("message", "userRequestedResolutionUpdate");
            res.set("resolutionAlias", a);
            const bool accepted = sendText(control, res.dump());
            XC_LOGI("requested stream resolution %s accepted=%u control_open=%u", a.c_str(),
                    accepted ? 1u : 0u, control && control->isOpen() ? 1u : 0u);
        };
        send(base);
        if (alias != base) send(alias);
    }

    void sendClientConfig() {
        json::Value ver = json::Value::array();
        ver.push(0);
        ver.push(2);
        ver.push(0);
        json::Value ui = json::Value::object();
        ui.set("version", ver);
        json::Value systemUis = json::Value::array();
        if (cb.textInput) systemUis.push(kShowVirtualKeyboard);
        ui.set("systemUis", systemUis);
        sendText(message, messageEnvelope("/streaming/systemUi/configuration", ui));

        json::Value install = json::Value::object();
        install.set("clientAppInstallId", kClientAppInstallId);
        sendText(message, messageEnvelope("/streaming/properties/clientappinstallidchanged", install));

        json::Value orientation = json::Value::object();
        orientation.set("orientation", 0);
        sendText(message, messageEnvelope("/streaming/characteristics/orientationchanged", orientation));

        json::Value touch = json::Value::object();
        touch.set("touchInputEnabled", false);
        sendText(message, messageEnvelope("/streaming/characteristics/touchinputenabledchanged", touch));

        sendText(message, messageEnvelope("/streaming/characteristics/clientdevicecapabilities", json::Value::object()));

        // The picture's size, as the web client sends its video element's:
        // a 1440p tier needs a 1440p one (the display itself is 4K).
        const bool is720 = opt.resolutionAlias == "720" || opt.resolutionAlias == "720HQ";
        const bool is1440 = opt.resolutionAlias == "1440";
        int w = is1440 ? 2560 : (is720 ? 1280 : 1920);
        int h = is1440 ? 1440 : (is720 ? 720 : 1080);
        json::Value dims = json::Value::object();
        dims.set("horizontal", w);
        dims.set("vertical", h);
        dims.set("preferredWidth", w);
        dims.set("preferredHeight", h);
        dims.set("safeAreaLeft", 0);
        dims.set("safeAreaTop", 0);
        dims.set("safeAreaRight", w);
        dims.set("safeAreaBottom", h);
        dims.set("supportsCustomResolution", true);
        sendText(message, messageEnvelope("/streaming/characteristics/dimensionschanged", dims));
    }

    void sendGamepadChanged(int index, bool added) {
        json::Value v = json::Value::object();
        v.set("message", "gamepadChanged");
        v.set("gamepadIndex", index);
        v.set("wasAdded", added);
        sendText(control, v.dump());
    }

    void onHandshakeAck() {
        handshakeAckReceived = true;
        XC_LOGI("message channel handshake complete");
        json::Value auth = json::Value::object();
        auth.set("message", "authorizationRequest");
        auth.set("accessKey", kControlAccessKey);
        sendText(control, auth.dump());
        // Same sequence as the web client: reset slot 0, then attach the pad.
        sendGamepadChanged(0, true);
        sendGamepadChanged(0, false);
        sendGamepadChanged(0, true);
        if (!opt.resolutionAlias.empty()) sendResolution(opt.resolutionAlias);


        // One touch point, as the web client announces (touch is then turned
        // on only while a touch screen is up: setTouchEnabled()).
        sendBinary(input, clientMetadataReport(0, nowMs(), static_cast<uint8_t>(std::getenv("XC_TOUCH_POINTS") ? std::atoi(std::getenv("XC_TOUCH_POINTS")) : 1)));
        sendClientConfig();
        {
            std::lock_guard<std::mutex> lock(mutex);
            handshakeDone = true;
        }
        open = true;
        cv.notify_all();
        appendSctpFlowSnapshot("HandshakeAck");
    }

    void onMessageChannel(const rtc::message_variant& m) {
        std::string text = asText(m);
        auto j = json::parse(text);
        if (!j) {
            XC_LOGW("message channel: unparsable %s", text.substr(0, 200).c_str());
            return;
        }
        std::string type = (*j)["type"].str();
        if (type == "HandshakeAck") {
            handshakeAckReceived = true;
            char line[128];
            std::snprintf(line, sizeof(line), "STAGE L4 OK event=HandshakeAck_message bytes=%zu",
                          text.size());
            logIceDiagnostic(line);
            onHandshakeAck();
            return;
        }
        if (type == "SenderCancel") {
            XC_LOGI("message channel: the server withdrew %s", (*j)["id"].str().c_str());
            if (cb.textInputCancelled) cb.textInputCancelled((*j)["id"].str());
            return;
        }
        if (type != "Message" && type != "TransactionStart") {
            XC_LOGD("message channel: %s", text.substr(0, 300).c_str());
            return;
        }
        std::string target = (*j)["target"].str();
        std::string id = (*j)["id"].str();
        XC_LOGI("message %s %s", type.c_str(), target.c_str());
        if (target == "/streaming/properties/titleinfo") {
            auto info = json::parse((*j)["content"].str());
            if (info && cb.titleFocus) cb.titleFocus((*info)["focused"].asBool());
        }
        if (target == "/streaming/sessionLifetimeManagement/serverInitiatedDisconnect") {
            std::string body = (*j)["content"].str();
            XC_LOGW("server disconnect: %s", body.substr(0, 300).c_str());
            completeTransaction(id, json::Value(""));
            // A warning shares this message with the real kicks: the session
            // goes on (the xbox.com client only shows a notice).
            auto content = json::parse(body);
            std::string reason = content ? (*content)["reason"].str() : std::string();
            if (reason == "WarningForBeingIdle") {
                if (cb.idleWarning) cb.idleWarning(static_cast<int>((*content)["secondsUntilKick"].asInt(120)));
            } else if (reason == "KickForClosedGame") {
                // The game itself quit (or failed to start) on the server.
                char hr[16];
                std::snprintf(hr, sizeof hr, "0x%08X", static_cast<unsigned>((*content)["hr"].asInt()));
                fail(std::string("the game closed on the server (") + hr + ")", false);
            } else {
                fail(reason.empty() ? "the server ended the session" : "the server ended the session (" + reason + ")",
                     false);
            }
        } else if (target == "/streaming/systemUi/messages/ShowVirtualKeyboard" && cb.textInput) {
            auto content = json::parse((*j)["content"].str());
            TextInputRequest req;
            req.id = id;
            if (content) {
                req.title = (*content)["TitleText"].str();
                req.description = (*content)["DescriptionText"].str();
                req.defaultText = (*content)["DefaultText"].str();
                req.inputScope = static_cast<int>((*content)["InputScope"].asInt());
                req.maxLength = static_cast<int>((*content)["MaxLength"].asInt());
            }
            XC_LOGI("the game asks for text (scope %d, up to %d characters)", req.inputScope, req.maxLength);
            cb.textInput(req);
        } else if (target == "/streaming/systemUi/messages/ShowMessageDialog") {
            // No dialog UI yet: log it and pick the first (default) button.
            auto content = json::parse((*j)["content"].str());
            if (content)
                XC_LOGW("server dialog: %s / %s", (*content)["TitleText"].str().c_str(),
                        (*content)["ContentText"].str().c_str());
            json::Value result = json::Value::object();
            result.set("Result", 0);
            completeTransaction(id, result);
        }
    }

    // --- set-up ---------------------------------------------------------------

    std::shared_ptr<rtc::DataChannel> makeChannel(const char* label, const char* protocol) {
        rtc::DataChannelInit init;
        init.protocol = protocol;
        auto ch = pc->createDataChannel(label, init);
        ch->onOpen([this, label] {
            XC_LOGI("data channel %s open", label);
            noteDataChannelOpen(label);
#if defined(__ORBIS__)
            if (std::strcmp(label, "control") == 0 && handshakeAckReceived.load() && !opt.resolutionAlias.empty())
                sendResolution(opt.resolutionAlias);
#endif
        });
        ch->onClosed([this, label] {
            XC_LOGI("data channel %s closed", label);
            if (open) fail(std::string("data channel ") + label + " closed");
        });
        return ch;
    }

    void createPeer() {
        resetSctpFlowTrace();
        initRtcLogging();
        rtc::Configuration config;
        config.disableAutoNegotiation = true;
        config.enableIceUdpMux = opt.enableIceUdpMux;
        logIceDiagnostic(config.enableIceUdpMux ? "backend=libjuice-udp-mux" : "backend=libjuice-poll");
        pc = std::make_shared<rtc::PeerConnection>(config);
        logIceDiagnostic("peer connection created; registering ICE callbacks");

        pc->onLocalCandidate([this](rtc::Candidate c) {
            const std::string candidate = std::string(c);
            const char* type = candidate.find(" typ srflx") != std::string::npos ? "srflx" :
                               candidate.find(" typ relay") != std::string::npos ? "relay" : "host/other";
            size_t count = 0;
            {
                std::lock_guard<std::mutex> lock(mutex);
                localCandidates.push_back(std::move(c));
                count = localCandidates.size();
            }
            char message[128];
            std::snprintf(message, sizeof(message), "local candidate discovered type=%s count=%zu",
                          type, count);
            logIceDiagnostic(message);
            std::string stage = std::string("STAGE L1 OK event=local_candidate type=") + type +
                                " candidate=" + candidate;
            logIceDiagnostic(stage.c_str());
        });
        pc->onGatheringStateChange([this](rtc::PeerConnection::GatheringState s) {
            logIceDiagnostic(s == rtc::PeerConnection::GatheringState::Complete ?
                             "STAGE L1 OK event=gathering_complete" : "STAGE L1 OK event=gathering_in_progress");
            if (s == rtc::PeerConnection::GatheringState::Complete) {
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    gatheringDone = true;
                }
                cv.notify_all();
            }
        });
        pc->onIceStateChange([this](rtc::PeerConnection::IceState s) {
            const char* name = "unknown";
            switch (s) {
            case rtc::PeerConnection::IceState::New: name = "new"; break;
            case rtc::PeerConnection::IceState::Checking: name = "checking"; break;
            case rtc::PeerConnection::IceState::Connected: name = "connected"; iceConnected = true; break;
            case rtc::PeerConnection::IceState::Completed: name = "completed"; iceConnected = true; break;
            case rtc::PeerConnection::IceState::Failed: name = "failed"; iceConnected = false; break;
            case rtc::PeerConnection::IceState::Disconnected: name = "disconnected"; iceConnected = false; break;
            case rtc::PeerConnection::IceState::Closed: name = "closed"; break;
            }
            char line[160];
            const bool failedState = s == rtc::PeerConnection::IceState::Failed ||
                                     s == rtc::PeerConnection::IceState::Disconnected;
            std::snprintf(line, sizeof(line), "STAGE L3 %s event=ice_state state=%s enum=%d",
                          failedState ? "FAIL" : "OK", name, static_cast<int>(s));
            logIceDiagnostic(line);
            if (s == rtc::PeerConnection::IceState::Connected ||
                s == rtc::PeerConnection::IceState::Completed) {
                rtc::Candidate local;
                rtc::Candidate remote;
                if (pc && pc->getSelectedCandidatePair(&local, &remote)) {
                    std::string pair = std::string("STAGE L3 OK event=selected_pair local=") +
                                       std::string(local) + " remote=" + std::string(remote);
                    logIceDiagnostic(pair.c_str());
                } else {
                    logIceDiagnostic("STAGE L3 OK event=selected_pair unavailable_at_callback=1");
                }
            }
        });
        pc->onStateChange([this](rtc::PeerConnection::State s) {
            XC_LOGI("peer connection state: %d", static_cast<int>(s));
            if (s == rtc::PeerConnection::State::Connected && !peerConnectionConnectedCallbackSeen.exchange(true))
                logIceDiagnostic("STAGE L4 event=peerconnection_state_callback state=CONNECTED");
            char stateMessage[96];
            std::snprintf(stateMessage, sizeof(stateMessage), "peer connection state=%d",
                          static_cast<int>(s));
            logIceDiagnostic(stateMessage);
            if (s == rtc::PeerConnection::State::Failed) {
                logIceDiagnostic("STAGE L3 FAIL event=peer_connection_state_failed");
                fail("WebRTC connection failed");
            }
            if (s == rtc::PeerConnection::State::Disconnected) fail("WebRTC connection lost");
        });

        // Same channels, in the same order, as the web client.
        chat = makeChannel("chat", "chatV1");
        chat->onMessage([](rtc::message_variant m) { XC_LOGI("chat channel: %s", asText(m).c_str()); });
        control = makeChannel("control", "controlV1");
        input = makeChannel("input", "1.0");
        message = makeChannel("message", "messageV1");

        message->onOpen([this] {
            noteDataChannelOpen("message");
            XC_LOGI("data channel message open: sending handshake");
            json::Value hs = json::Value::object();
            hs.set("type", "Handshake");
            hs.set("version", "messageV1");
            hs.set("id", kHandshakeId);
            hs.set("cv", "0");
            const std::string payload = hs.dump();
            handshakeSendAttempted = true;
            const bool acceptedImmediately = sendText(message, payload);
            std::string stage = std::string("STAGE L4 OK event=Handshake_send_attempt bytes=") +
                                std::to_string(payload.size()) +
                                " send_return=" + (acceptedImmediately ? "accepted" : "buffered") +
                                " payload=" + payload;
            logIceDiagnostic(stage.c_str());
        });
        message->onMessage([this](rtc::message_variant m) { onMessageChannel(m); });
        control->onMessage([](rtc::message_variant m) { XC_LOGD("control: %s", asText(m).substr(0, 300).c_str()); });
        input->onMessage([this](rtc::message_variant m) {
            auto b = std::get_if<rtc::binary>(&m);
            if (!b) return;
            auto* d = reinterpret_cast<const uint8_t*>(b->data());
            Vibration v;
            uint32_t w, h;
            if (parseVibration(d, b->size(), v)) {
                if (cb.vibration) cb.vibration(v);
            } else if (parseServerMetadata(d, b->size(), w, h)) {
                XC_LOGI("server video size %ux%u", w, h);
            } else if (b->size() >= 2 && ++otherInputReports <= 5) {
                XC_LOGI("input channel: report type %u, %zu bytes", d[0], b->size());
            }
        });

        // Audio: sendrecv like the browser (the send side carries chat).
        rtc::Description::Audio a("0", rtc::Description::Direction::SendRecv);
        a.addOpusCodec(111, "minptime=10;useinbandfec=1;stereo=1");
        a.addSSRC(1, "audio");
        audio = pc->addTrack(a);

        auto audioDepacketizer = std::make_shared<rtc::OpusRtpDepacketizer>();
        audioDepacketizer->addToChain(std::make_shared<RtcpReporter>(48000, 0));

        auto rtpConfig = std::make_shared<rtc::RtpPacketizationConfig>(
            1, "audio", 111, rtc::OpusRtpPacketizer::DefaultClockRate);
        auto audioPacketizer = std::make_shared<rtc::OpusRtpPacketizer>(rtpConfig);
        audioDepacketizer->addToChain(audioPacketizer);

        audio->setMediaHandler(audioDepacketizer);
        audio->onFrame([this](rtc::binary data, rtc::FrameInfo info) {
            if (cb.audio) cb.audio(reinterpret_cast<const uint8_t*>(data.data()), data.size(), info.timestamp);
        });

        // Video: H.264 only, Main before Constrained Baseline / Baseline.
        rtc::Description::Video v("1", rtc::Description::Direction::RecvOnly);
        // The PS4 Videodec2 adapter is currently configured for the PS4 probe's
        // Baseline path. Other builds retain the original Main-first offer.
        if (!opt.baselineH264Only)
            v.addH264Codec(102, "level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=4d0033");
        v.addH264Codec(104, "level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42e01f");
        v.addH264Codec(106, "level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42001f");
        video = pc->addTrack(v);
        // Our jitter buffer (NACK, whole frames only) in place of
        // H264RtpDepacketizer, which hands over frames with holes in them.
        auto videoReceiver = std::make_shared<H264Receiver>(
            [this] {
                if (!open) return;
            },
            videoStats);
        videoReceiver->setSimulatedLoss(opt.simulatedVideoLoss);
        videoReceiver->addToChain(videoReporter);
        video->setMediaHandler(videoReceiver);
        video->onFrame([this](rtc::binary data, rtc::FrameInfo info) {
            if (!firstVideoAccessUnit.exchange(true)) {
                char line[160];
                std::snprintf(line, sizeof(line), "STAGE L6 OK event=first_depacketized_access_unit bytes=%zu rtp_ts=%u",
                              data.size(), info.timestamp);
                logIceDiagnostic(line);
                appendSctpFlowSnapshot("first_video_access_unit");
            }
            if (cb.video) cb.video(reinterpret_cast<const uint8_t*>(data.data()), data.size(), info.timestamp);
        });
    }

    bool waitFor(const std::function<bool()>& pred, int timeoutMs, const char* label) {
#if defined(__ORBIS__)
        // The PS4 libc++ timed condition-variable path ended in SIGSYS while
        // waiting for the WebRTC handshake (V30). Poll the same predicates
        // under the existing mutex, using the platform monotonic clock, and
        // sleep without holding the mutex so ICE callbacks can make progress.
        const uint64_t startMs = platform::nowMs();
        const uint64_t timeout = timeoutMs > 0 ? static_cast<uint64_t>(timeoutMs) : 0;
        const uint64_t deadlineMs = startMs + timeout;
        char message[128];
        std::snprintf(message, sizeof(message), "wait begin kind=%s timeout_ms=%d",
                      label ? label : "unknown", timeoutMs);
        logIceDiagnostic(message);
        for (;;) {
            int state = 0;  // 0: pending, 1: predicate true, -1: session failed
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (failed) state = -1;
                else if (pred()) state = 1;
            }
            const uint64_t nowMs = platform::nowMs();
            if (state != 0 || nowMs >= deadlineMs) {
                // Recheck once under the lock at the deadline to avoid losing
                // a callback that completed at the same instant.
                if (state == 0) {
                    std::lock_guard<std::mutex> lock(mutex);
                    if (failed) state = -1;
                    else if (pred()) state = 1;
                }
                const char* result = state > 0 ? "complete" : state < 0 ? "failed" : "timeout";
                std::snprintf(message, sizeof(message), "wait end kind=%s result=%s elapsed_ms=%llu",
                              label ? label : "unknown", result,
                              static_cast<unsigned long long>(nowMs - startMs));
                logIceDiagnostic(message);
                return state > 0;
            }
            const uint64_t remainingMs = deadlineMs - nowMs;
            platform::sleepMs(static_cast<uint32_t>(remainingMs < 10 ? remainingMs : 10));
        }
#else
        std::unique_lock<std::mutex> lock(mutex);
        return cv.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&] { return failed || pred(); }) && !failed;
#endif
    }

    bool addRemoteCandidate(const std::string& candidate, const std::string& mid) {
        std::string c = candidate;
        while (!c.empty() && (c.back() == ' ' || c.back() == '\r' || c.back() == '\n')) c.pop_back();
        try {
            pc->addRemoteCandidate(rtc::Candidate(c, mid));
            std::string stage = std::string("STAGE L3 OK event=remote_candidate_added candidate=") + c;
            logIceDiagnostic(stage.c_str());
            return true;
        } catch (const std::exception& e) {
            std::string stage = std::string("STAGE L3 FAIL event=remote_candidate_rejected candidate=") +
                                c + " error=" + e.what();
            logIceDiagnostic(stage.c_str());
            XC_LOGW("ignoring a remote candidate: %s", e.what());
            return false;
        }
    }

    bool start(std::string& err) {
        reportProgress("WebRTC 1/8: creating peer connection and data channels");
        createPeer();
        reportProgress("WebRTC 2/8: creating local offer and gathering ICE candidates");
        pc->setLocalDescription(rtc::Description::Type::Offer);
        reportProgress("WebRTC 3/8: local offer created; waiting for ICE gathering");
        if (!waitFor([this] { return gatheringDone; }, 10000, "ice-gather")) {
            size_t count = 0;
            {
                std::lock_guard<std::mutex> lock(mutex);
                count = localCandidates.size();
            }
            logIceDiagnostic(count ? "STAGE L1 TIMEOUT event=gathering_complete candidates_available=1" :
                                    "STAGE L1 FAIL event=gathering_complete candidates=0");
            char message[128];
            std::snprintf(message, sizeof(message), "ICE gathering timeout candidates=%zu",
                          count);
            reportProgress(message);
            XC_LOGW("ICE gathering did not complete; continuing with %zu candidates", count);
        }

        auto local = pc->localDescription();
        if (!local) {
            err = "no local SDP offer";
            return false;
        }
        std::string offer = std::string(*local);
        reportProgress("WebRTC 4/8: local offer ready; contacting cloud signaling");
#if defined(__ORBIS__)
        if (opt.resolutionAlias == "720" || opt.resolutionAlias == "720HQ")
#endif
        XC_LOGD("local offer:\n%s", offer.c_str());
        if (!gssv.sendSdpOffer(offer, err)) return false;
        reportProgress("WebRTC 5/8: offer sent; waiting for cloud answer");

        std::string answer;
        for (int i = 0; i < 60 && answer.empty(); ++i) {
            if (!gssv.pollSdpAnswer(answer, err)) return false;
            if (answer.empty()) platform::sleepMs(500);
        }
        if (answer.empty()) {
            err = "timed out waiting for the SDP answer";
            return false;
        }
        reportProgress("WebRTC 6/8: cloud answer received; applying remote description");
        XC_LOGD("remote answer:\n%s", answer.c_str());
        for (size_t p = answer.find("a=rtpmap:"); p != std::string::npos; p = answer.find("a=rtpmap:", p + 1))
            XC_LOGI("answer %s", answer.substr(p, answer.find_first_of("\r\n", p) - p).c_str());
        try {
            pc->setRemoteDescription(rtc::Description(answer, rtc::Description::Type::Answer));
        } catch (const std::exception& e) {
            err = std::string("remote SDP rejected: ") + e.what();
            return false;
        }
        reportProgress("WebRTC 7/8: exchanging ICE candidates");

        // Local candidates -> gssv, with their m-line index in our offer.
        std::vector<std::string> mids = midsInOrder(offer);
        std::vector<xcloud::IceCandidate> mine;
        {
            std::lock_guard<std::mutex> lock(mutex);
            for (const auto& c : localCandidates) {
                xcloud::IceCandidate ic;
                ic.candidate = std::string(c);  // "candidate:...", as a browser sends
                ic.sdpMid = c.mid();
                for (size_t i = 0; i < mids.size(); ++i)
                    if (mids[i] == ic.sdpMid) ic.sdpMLineIndex = static_cast<int>(i);
                mine.push_back(std::move(ic));
            }
        }
        XC_LOGI("sending %zu local ICE candidates", mine.size());
        if (mine.empty()) logIceDiagnostic("STAGE L1 FAIL event=publish_local_candidates count=0");
        for (const auto& candidate : mine) {
            std::string exact = std::string("STAGE L1 OK event=signaled_local_candidate candidate=") +
                                candidate.candidate;
            logIceDiagnostic(exact.c_str());
        }
        {
            char message[128];
            std::snprintf(message, sizeof(message), "sending local ICE candidates count=%zu", mine.size());
            reportProgress(message);
        }
        if (!gssv.sendIceCandidates(mine, err)) return false;
        reportProgress("WebRTC 8/8: local candidates sent; waiting for cloud candidates");

        std::vector<xcloud::IceCandidate> theirs;
        for (int i = 0; i < 30 && theirs.empty(); ++i) {
            std::string pollDiagnostic;
            if (!gssv.pollIceCandidates(theirs, err, &pollDiagnostic)) {
                if (!pollDiagnostic.empty()) logIceDiagnostic(pollDiagnostic.c_str());
                return false;
            }
            if (!pollDiagnostic.empty()) {
                char message[256];
                std::snprintf(message, sizeof(message), "ICE poll attempt=%d %s", i + 1,
                              pollDiagnostic.c_str());
                logIceDiagnostic(message);
            }
            if (theirs.empty()) platform::sleepMs(1000);
        }
        if (theirs.empty()) {
            err = "the server sent no ICE candidates";
            logIceDiagnostic("remote ICE polling ended with zero candidates");
            return false;
        }
        std::vector<std::string> remoteMids = midsInOrder(answer);
        std::string bundleMid = remoteMids.empty() ? "0" : remoteMids.front();
        size_t remoteAdded = 0;
        size_t remoteRejected = 0;
        for (size_t remoteIndex = 0; remoteIndex < theirs.size(); ++remoteIndex) {
            const auto& c = theirs[remoteIndex];
            if (c.candidate.find("end-of-candidates") != std::string::npos) continue;
            char candidateBegin[512];
            std::snprintf(candidateBegin, sizeof(candidateBegin),
                          "STAGE L3 BEGIN event=remote_candidate index=%zu candidate=%s",
                          remoteIndex, c.candidate.c_str());
            logIceDiagnostic(candidateBegin);
            std::string mid = c.sdpMid.empty() ? bundleMid : c.sdpMid;
            // The address field of "candidate:<f> <c> <proto> <prio> <addr> <port> ...".
            std::vector<std::string> parts;
            size_t p = 0;
            while (p < c.candidate.size()) {
                size_t e = c.candidate.find(' ', p);
                if (e == std::string::npos) e = c.candidate.size();
                parts.push_back(c.candidate.substr(p, e - p));
                p = e + 1;
            }
            std::string ipv4;
            int port = 0;
            if (parts.size() > 4 && decodeTeredo(parts[4], ipv4, port)) {
                // The console's public address: not in the log.
                XC_LOGI("teredo candidate -> port %d (and 9002)", port);
                remoteAdded += addRemoteCandidate("a=candidate:10 1 UDP 1 " + ipv4 + " 9002 typ host", mid);
                remoteAdded += addRemoteCandidate("a=candidate:11 1 UDP 1 " + ipv4 + " " + std::to_string(port) + " typ host", mid);
            }
            if (addRemoteCandidate(c.candidate, mid)) ++remoteAdded;
            else ++remoteRejected;
        }
        char candidateMessage[160];
        std::snprintf(candidateMessage, sizeof(candidateMessage),
                      "remote ICE candidates received=%zu added=%zu rejected=%zu",
                      theirs.size(), remoteAdded, remoteRejected);
        logIceDiagnostic(candidateMessage);

        char handshakeWaitLine[160];
        std::snprintf(handshakeWaitLine, sizeof(handshakeWaitLine),
                      "STAGE L4 BEGIN event=sctp_channels_handshake_wait timeout_ms=%d",
                      opt.connectTimeoutMs);
        logIceDiagnostic(handshakeWaitLine);
        if (!waitFor([this] { return handshakeDone; }, opt.connectTimeoutMs, "handshake")) {
            {
                std::lock_guard<std::mutex> lock(mutex);
                err = failed ? failReason : "timed out connecting the stream";
            }
            if (!iceConnected.load()) {
                logIceDiagnostic("STAGE L3 TIMEOUT event=ice_or_datachannel_handshake ice_connected=0");
            } else if (!gSctpCommUpSeen.load()) {
                char line[320];
                std::snprintf(line, sizeof(line),
                              "STAGE L4 TIMEOUT event=sctp_association peerconnection_state=%d comm_up_seen=0 sctp_callback_seen=%u state_callback_seen=%u channels_open=%u",
                              pc ? static_cast<int>(pc->state()) : -1,
                              gPeerConnectionSctpCallbackSeen.load() ? 1u : 0u,
                              peerConnectionConnectedCallbackSeen.load() ? 1u : 0u,
                              dataChannelsOpened.load());
                logIceDiagnostic(line);
            } else if (!gPeerConnectionSctpCallbackSeen.load()) {
                char line[320];
                std::snprintf(line, sizeof(line),
                              "STAGE L4 TIMEOUT event=peerconnection_sctp_callback peerconnection_state=%d comm_up_seen=1 sctp_callback_seen=0 state_callback_seen=%u channels_open=%u",
                              pc ? static_cast<int>(pc->state()) : -1,
                              peerConnectionConnectedCallbackSeen.load() ? 1u : 0u,
                              dataChannelsOpened.load());
                logIceDiagnostic(line);
            } else if (dataChannelsOpened.load() < 4) {
                char line[256];
                std::snprintf(line, sizeof(line),
                              "STAGE L4 TIMEOUT event=data_channel_open opened=%u expected=4 peerconnection_state=%d comm_up_seen=1 sctp_callback_seen=1 state_callback_seen=%u",
                              dataChannelsOpened.load(), pc ? static_cast<int>(pc->state()) : -1,
                              peerConnectionConnectedCallbackSeen.load() ? 1u : 0u);
                logIceDiagnostic(line);
            } else if (!handshakeSendAttempted.load()) {
                logIceDiagnostic("STAGE L4 TIMEOUT event=Handshake_send channel=message_opened");
            } else if (!handshakeAckReceived.load()) {
                logIceDiagnostic("STAGE L4 TIMEOUT event=HandshakeAck handshake_send_attempted=1");
            } else {
                logIceDiagnostic("STAGE L4 TIMEOUT event=handshake_complete_state_unpublished");
            }
            logIceDiagnostic("peer connection handshake timed out");
            appendSctpFlowSnapshot("handshake_timeout");
            return false;
        }
        streamReadyAtMs = platform::nowMs();
        logIceDiagnostic("STAGE L6 BEGIN event=waiting_for_first_media timeout_ms=10000");
        lastKeepaliveMs = lastKeyframeMs = platform::nowMs();
        return true;
    }

    void requestKeyframe() {
        json::Value v = json::Value::object();
        v.set("message", "videoKeyframeRequested");
        v.set("ifrRequested", true);
        sendText(control, v.dump());
    }

    void tick() {
        if (!open) return;
        const uint64_t readyAt = streamReadyAtMs.load();
            !firstVideoTimeoutLogged.exchange(true))
            logIceDiagnostic("STAGE L6 TIMEOUT event=first_depacketized_access_unit timeout_ms=10000");
            lastKeepaliveMs = now;
            std::string err;
            if (!gssv.keepalive(err)) XC_LOGW("%s", err.c_str());
        }
            lastKeyframeMs = now;
            requestKeyframe();
        }
    }

    void close() {
        open = false;
        if (pc) {
            try {
                pc->close();
            } catch (const std::exception& e) {
                XC_LOGW("closing peer connection: %s", e.what());
            }
        }
    }
};

StreamSession::StreamSession(xcloud::GssvClient& gssv, StreamCallbacks callbacks, StreamOptions options)
    : impl_(std::make_unique<Impl>(gssv, std::move(callbacks), options)) {}

StreamSession::~StreamSession() {
    impl_->closedNotified = true;  // no callback while tearing down
    impl_->close();
}

bool StreamSession::start(std::string& err) {
    // libdatachannel reports set-up failures (e.g. no usable socket) by throwing.
    try {
        return impl_->start(err);
    } catch (const std::exception& e) {
        err = std::string("WebRTC: ") + e.what();
    } catch (...) {
        err = "WebRTC: unknown error";
    }
    impl_->close();
    return false;
}
bool StreamSession::isOpen() const { return impl_->open; }

bool StreamSession::sendGamepad(const GamepadFrame& frame) {
    if (!impl_->open) return false;
}

void StreamSession::sendAudio(const uint8_t* opus, size_t size, uint32_t timestamp) {
    if (!impl_ || !impl_->open || !impl_->audio || !opus || size == 0) return;
    try {
        rtc::FrameInfo info(timestamp);
        impl_->audio->sendFrame(reinterpret_cast<const rtc::byte*>(opus), size, info);
    } catch (const std::exception&) {
    }
}

void StreamSession::setTouchEnabled(bool on) {
    if (!impl_->open) return;
    json::Value touch = json::Value::object();
    touch.set("touchInputEnabled", on);
    Impl::sendText(impl_->message, impl_->messageEnvelope("/streaming/characteristics/touchinputenabledchanged", touch));
}

void StreamSession::setGamepadConnected(int index, bool connected) {
    if (!impl_->open) return;
    XC_LOGI("gamepad %d %s", index, connected ? "attached" : "detached");
    impl_->sendGamepadChanged(index, connected);
}

void StreamSession::requestKeyframe() {
    try {
        impl_->requestKeyframe();
    } catch (const std::exception& e) {
        XC_LOGW("keyframe request: %s", e.what());
    }
}
void StreamSession::reportFrame(const FrameMetadata& frame) {
    if (!impl_->open) return;
    Impl::sendBinary(impl_->input, metadataReport(impl_->inputSequence++, impl_->nowMs(), {frame}));
}

double StreamSession::clockMs() const { return stream::clockMs(); }

void StreamSession::simulateDrop() {
    XC_LOGW("test: dropping the connection");
    if (impl_->pc) impl_->pc->close();
}

double clockMs() {
    static const auto epoch = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - epoch).count();
}

void StreamSession::completeTextInput(const std::string& id, const std::string& text) {
    json::Value v = json::Value::object();
    v.set("Text", text);
    impl_->completeTransaction(id, v);
}

void StreamSession::cancelTextInput(const std::string& id) { impl_->cancelTransaction(id); }

void StreamSession::requestResolution(const std::string& alias) { impl_->sendResolution(alias); }
void StreamSession::setMaximumBitrate(uint32_t bitsPerSecond) {
    if (impl_->videoReporter) impl_->videoReporter->setMaximumBitrate(bitsPerSecond);
}

void StreamSession::tick() { impl_->tick(); }
void StreamSession::close() { impl_->close(); }
int StreamSession::rttMs() const {
    try {
        if (impl_->pc)
            if (auto rtt = impl_->pc->rtt()) return static_cast<int>(rtt->count());
    } catch (const std::exception&) {
    }
    return -1;
}

const VideoReceiveStats& StreamSession::videoStats() const {
    if (impl_->videoReporter) {
        impl_->videoStats.estimate = impl_->videoReporter->estimate();
    }
    return impl_->videoStats;
}

}  // namespace xc::stream
