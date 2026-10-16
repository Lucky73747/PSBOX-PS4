// SPDX-License-Identifier: GPL-3.0-or-later
// PS4 device-code sign-in milestone for the Xbox Cloud homebrew.
#include "auth/msa.h"
#include "auth/xbox_live.h"
#include "gpu_wsi.h"
#include "network.h"
#include "net/http.h"
#include "xcloud_login.h"
#include "ui/brand.h"
#include "ui/canvas.h"
#include "ui/font.h"
#include "qrcodegen.h"
#include "xcloud/gssv.h"
#include "ui/app_ui.h"
#include "ui/image_cache.h"
#include "video_decoder.h"
#include "video_repeat_watchdog.h"
#include "stream/stream_session.h"
#include "platform/platform.h"
#include "util/json.h"
#include "util/log.h"

#include <orbis/libkernel.h>
#include <orbis/Sysmodule.h>
#include <orbis/Pad.h>
#include <orbis/UserService.h>
#include <SDL3/SDL.h>

#include <pthread.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdarg>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <condition_variable>
#include <vector>

namespace {
constexpr const char* kLogDirectory = "/data/PSBOX";
constexpr const char* kAppLogPath = "/data/PSBOX/xcloud.log";

void logLine(const char* fmt, ...) {
    char body[1536];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);
    char line[1664];
    std::snprintf(line, sizeof(line), "[%s] %s\n", kBuild, body);
    XC_LOGI("%s", line);
}

struct StreamProfile {
    uint32_t frameRate = 60;
    bool automaticVideoReset = false;
    uint32_t lastQueueMax = 0;
    uint32_t lastDecodeAvgUs = 0;
    uint32_t lastConvertAvgUs = 0;
    uint32_t lastSkipPermille = 0;
};

constexpr const char* kStreamProfilePath = "/data/PSBOX/stream_profile.json";
constexpr uint32_t k1080Bitrate = 25000000;
constexpr std::array<uint32_t, 10> kSelectableBitrates = {
    3000000, 4000000, 6000000, 8000000, 10000000,
    12000000, 15000000, 20000000, 25000000, 30000000
};

uint32_t normalizeBitrate(uint64_t bitrate) {
    uint32_t best = kSelectableBitrates.front();
    uint64_t distance = bitrate > best ? bitrate - best : best - bitrate;
    for (uint32_t candidate : kSelectableBitrates) {
        const uint64_t nextDistance = bitrate > candidate ? bitrate - candidate : candidate - bitrate;
        if (nextDistance < distance) {
            best = candidate;
            distance = nextDistance;
        }
    }
    return best;
}

const char* tierLabel(const std::string& tier) { return tier == "1080" ? "1080p" : "720p"; }

StreamProfile loadStreamProfile() {
    StreamProfile profile;
    std::string text;
    if (!xc::platform::readFile(kStreamProfilePath, text)) {
        return profile;
    }
    std::string error;
    auto parsed = xc::json::parse(text, &error);
        ((*parsed)["tier"].str() != "720" && (*parsed)["tier"].str() != "1080")) {
        return profile;
    }
    profile.tier = (*parsed)["tier"].str();
    profile.frameRate = (*parsed)["frame_rate"].asInt() == 30 ? 30 : 60;
    profile.rcasSharpness = static_cast<uint32_t>(std::clamp<int64_t>((*parsed)["fsr_rcas_sharpness"].asInt(2), 0, 4));
    profile.automaticVideoReset = (*parsed)["automatic_video_reset"].asBool(false);
    const int64_t storedBitrate = (*parsed)["max_bitrate"].asInt();
    profile.maxBitrate = storedBitrate > 0
        ? normalizeBitrate(static_cast<uint64_t>(storedBitrate))
        : (profile.tier == "720" ? k720Bitrate : k1080Bitrate);
    profile.lastQueueMax = static_cast<uint32_t>(std::clamp<int64_t>((*parsed)["queue_max"].asInt(), 0, 18));
    profile.lastDecodeAvgUs = static_cast<uint32_t>(std::clamp<int64_t>((*parsed)["decode_avg_us"].asInt(), 0, 1000000));
    profile.lastConvertAvgUs = static_cast<uint32_t>(std::clamp<int64_t>((*parsed)["convert_avg_us"].asInt(), 0, 1000000));
    profile.lastSkipPermille = static_cast<uint32_t>(std::clamp<int64_t>((*parsed)["skip_permille"].asInt(), 0, 1000));
            profile.lastDecodeAvgUs / 1000.0, profile.lastConvertAvgUs / 1000.0,
            profile.lastSkipPermille / 10.0);
    return profile;
}

bool saveStreamProfile(const StreamProfile& profile) {
    xc::json::Value value = xc::json::Value::object();
    value.set("tier", profile.tier);
    value.set("frame_rate", static_cast<int64_t>(profile.frameRate));
    value.set("automatic_video_reset", profile.automaticVideoReset);
    value.set("gpu_upscaler", static_cast<int64_t>(profile.upscaler));
    value.set("frame_wait", static_cast<int64_t>(profile.frameWait));
    value.set("fsr_rcas_sharpness", static_cast<int64_t>(profile.rcasSharpness));
    value.set("max_bitrate", static_cast<int64_t>(profile.maxBitrate));
    value.set("queue_max", static_cast<int64_t>(profile.lastQueueMax));
    value.set("decode_avg_us", static_cast<int64_t>(profile.lastDecodeAvgUs));
    value.set("convert_avg_us", static_cast<int64_t>(profile.lastConvertAvgUs));
    value.set("skip_permille", static_cast<int64_t>(profile.lastSkipPermille));
    if (!xc::platform::writeFileAtomic(kStreamProfilePath, value.dump())) {
        logLine("stream profile cache write failed path=%s", kStreamProfilePath);
        return false;
    }
            profile.lastSkipPermille / 10.0);
    return true;
}

enum class LoginStage {
    Starting, CheckingSaved, Refreshing, ShowingCode, XboxLive, Xcloud, Library,
};

const char* loginStageName(LoginStage stage) {
    switch (stage) {
    case LoginStage::Starting: return "starting";
    case LoginStage::CheckingSaved: return "checking-saved-account";
    case LoginStage::Refreshing: return "refreshing-account";
    case LoginStage::ShowingCode: return "device-code";
    case LoginStage::XboxLive: return "xbox-live";
    case LoginStage::Xcloud: return "xcloud-login";
    case LoginStage::Library: return "library";
    case LoginStage::SessionStarting: return "session-starting";
    case LoginStage::SessionProvisioning: return "session-provisioning";
    case LoginStage::SessionReady: return "session-ready";
    case LoginStage::Streaming: return "streaming";
    case LoginStage::Failed: return "failed";
    case LoginStage::Expired: return "expired";
    case LoginStage::Denied: return "denied";
    }
    return "unknown";
}

}

constexpr const char* kTransferScope =
    "service::http://Passport.NET/purpose::PURPOSE_XBOX_CLOUD_CONSOLE_TRANSFER_TOKEN";

class Controller {
public:
    bool initialize() {
        const int32_t userModule = static_cast<int32_t>(sceSysmoduleLoadModuleInternal(
            ORBIS_SYSMODULE_INTERNAL_USER_SERVICE));
        const int32_t padModule = static_cast<int32_t>(sceSysmoduleLoadModuleInternal(
            ORBIS_SYSMODULE_INTERNAL_PAD));
        if (userModule < 0 || padModule < 0) {
            logLine("scePad module load failed user=0x%x pad=0x%x",
                    static_cast<unsigned>(userModule), static_cast<unsigned>(padModule));
            return false;
        }
        const int32_t userInit = sceUserServiceInitialize(nullptr);
        logLine("sceUserServiceInitialize rc=0x%x", static_cast<unsigned>(userInit));
        const int32_t padInit = scePadInit();
        if (padInit < 0) {
            logLine("scePadInit failed rc=0x%x", static_cast<unsigned>(padInit));
            return false;
        }
        padInitialized_ = true;
        pump();
        logLine("scePad input initialized user_module=0x%x pad_module=0x%x handle=%d",
                static_cast<unsigned>(userModule), static_cast<unsigned>(padModule), handle_);
        return true;
    }

    ~Controller() {
        if (handle_ >= 0 && ownsHandle_) scePadClose(handle_);
    }

    void pump() {
        std::lock_guard<std::mutex> pumpLock(pumpMutex_);
        if (!padInitialized_) return;
        if (handle_ < 0) {
            const uint64_t now = sceKernelGetProcessTime();
            if (now >= nextOpenAt_) openPad(now);
            return;
        }

        OrbisPadData next{};
        const int rc = scePadReadState(handle_, &next);
        if (rc < 0) {
            logLine("scePadReadState failed rc=0x%x; retrying", static_cast<unsigned>(rc));
            closePad();
            nextOpenAt_ = sceKernelGetProcessTime() + 1000000;
            {
                std::lock_guard<std::mutex> stateLock(stateMutex_);
                state_ = {};
            }
            return;
        }
        if (!next.connected) {
            bool wasConnected = false;
            {
                std::lock_guard<std::mutex> stateLock(stateMutex_);
                wasConnected = state_.connected;
                state_ = {};
            }
            if (wasConnected) logLine("scePad controller disconnected");
            connectedLogged_ = false;
            return;
        }

        OrbisPadData previous{};
        {
            std::lock_guard<std::mutex> stateLock(stateMutex_);
            previous = state_;
            state_ = next;
        }
        const uint32_t previousButtons = previous.buttons;
        const int oldLX = axisBucket(previous.leftStick.x);
        const int oldLY = axisBucket(previous.leftStick.y);
        const int newLX = axisBucket(next.leftStick.x);
        const int newLY = axisBucket(next.leftStick.y);
        if (!connectedLogged_) {
            logLine("scePad controller connected handle=%d", handle_);
            connectedLogged_ = true;
        }
        if (next.buttons != previousButtons) {
            logLine("scePad buttons=0x%08x", static_cast<unsigned>(next.buttons));
        }
        if ((newLX != oldLX || newLY != oldLY) && (newLX || newLY || oldLX || oldLY)) {
            logLine("scePad left_stick x=%u y=%u", next.leftStick.x, next.leftStick.y);
        }
    }

    bool button(SDL_GamepadButton button) const {
        uint32_t mask = 0;
        switch (button) {
        case SDL_GAMEPAD_BUTTON_SOUTH: mask = ORBIS_PAD_BUTTON_CROSS; break;
        case SDL_GAMEPAD_BUTTON_EAST: mask = ORBIS_PAD_BUTTON_CIRCLE; break;
        case SDL_GAMEPAD_BUTTON_WEST: mask = ORBIS_PAD_BUTTON_SQUARE; break;
        case SDL_GAMEPAD_BUTTON_NORTH: mask = ORBIS_PAD_BUTTON_TRIANGLE; break;
        case SDL_GAMEPAD_BUTTON_START: mask = ORBIS_PAD_BUTTON_OPTIONS; break;
        case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: mask = ORBIS_PAD_BUTTON_L1; break;
        case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: mask = ORBIS_PAD_BUTTON_R1; break;
        case SDL_GAMEPAD_BUTTON_LEFT_STICK: mask = ORBIS_PAD_BUTTON_L3; break;
        case SDL_GAMEPAD_BUTTON_RIGHT_STICK: mask = ORBIS_PAD_BUTTON_R3; break;
        case SDL_GAMEPAD_BUTTON_DPAD_UP: mask = ORBIS_PAD_BUTTON_UP; break;
        case SDL_GAMEPAD_BUTTON_DPAD_DOWN: mask = ORBIS_PAD_BUTTON_DOWN; break;
        case SDL_GAMEPAD_BUTTON_DPAD_LEFT: mask = ORBIS_PAD_BUTTON_LEFT; break;
        case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: mask = ORBIS_PAD_BUTTON_RIGHT; break;
        case SDL_GAMEPAD_BUTTON_TOUCHPAD: mask = ORBIS_PAD_BUTTON_TOUCH_PAD; break;
        default: return false;
        }
        std::lock_guard<std::mutex> lock(stateMutex_);
        return state_.connected && (state_.buttons & mask) != 0;
    }
    float axis(SDL_GamepadAxis axis) const {
        std::lock_guard<std::mutex> lock(stateMutex_);
        if (!state_.connected) return 0.0f;
        uint8_t value = 128;
        switch (axis) {
        case SDL_GAMEPAD_AXIS_LEFTX: value = state_.leftStick.x; break;
        case SDL_GAMEPAD_AXIS_LEFTY: value = state_.leftStick.y; break;
        case SDL_GAMEPAD_AXIS_RIGHTX: value = state_.rightStick.x; break;
        case SDL_GAMEPAD_AXIS_RIGHTY: value = state_.rightStick.y; break;
        case SDL_GAMEPAD_AXIS_LEFT_TRIGGER:
            return static_cast<float>(state_.analogButtons.l2) / 255.0f;
        case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER:
            return static_cast<float>(state_.analogButtons.r2) / 255.0f;
        default: return 0.0f;
        }
        if (value >= 126 && value <= 130) return 0.0f;
        return value < 128 ? (static_cast<float>(value) - 128.0f) / 128.0f
                           : (static_cast<float>(value) - 128.0f) / 127.0f;
    }

private:
    static int axisBucket(uint8_t value) {
        if (value < 72) return -1;
        if (value > 184) return 1;
        return 0;
    }

    void openPad(uint64_t now) {
        nextOpenAt_ = now + 1000000;
        int32_t user = -1;
        const int userRc = sceUserServiceGetInitialUser(&user);
        if (userRc < 0) {
            if (!openErrorLogged_) logLine("sceUserServiceGetInitialUser failed rc=0x%x", static_cast<unsigned>(userRc));
            openErrorLogged_ = true;
            return;
        }
        handle_ = scePadOpen(user, ORBIS_PAD_PORT_TYPE_STANDARD, 0, nullptr);
        ownsHandle_ = handle_ >= 0;
        if (handle_ < 0) handle_ = scePadGetHandle(user, ORBIS_PAD_PORT_TYPE_STANDARD, 0);
        if (handle_ < 0) {
            if (!openErrorLogged_) logLine("scePadOpen/GetHandle failed user=%d rc=0x%x", user, static_cast<unsigned>(handle_));
            openErrorLogged_ = true;
            return;
        }
        openErrorLogged_ = false;
        logLine("scePad opened user=%d handle=%d owned=%d", user, handle_, ownsHandle_ ? 1 : 0);
    }

    void closePad() {
        if (handle_ >= 0 && ownsHandle_) scePadClose(handle_);
        handle_ = -1;
        ownsHandle_ = false;
        connectedLogged_ = false;
    }

    OrbisPadData state_{};
    mutable std::mutex stateMutex_;
    std::mutex pumpMutex_;
    int32_t handle_ = -1;
    uint64_t nextOpenAt_ = 0;
    bool padInitialized_ = false;
    bool ownsHandle_ = false;
    bool connectedLogged_ = false;
    bool openErrorLogged_ = false;
};

constexpr const char* kTokenDirectory = "/data/XCHM00001";
constexpr const char* kRefreshTokenPath = "/data/XCHM00001/msa_refresh_token.txt";

bool readRefreshToken(std::string& token, int& errorCode) {
    token.clear();
    FILE* file = std::fopen(kRefreshTokenPath, "rb");
    if (!file) {
        errorCode = errno;
        return false;
    }
    char buffer[4096];
    const size_t count = std::fread(buffer, 1, sizeof(buffer), file);
    const bool tooLarge = count == sizeof(buffer) && std::fgetc(file) != EOF;
    const bool ok = !tooLarge && std::ferror(file) == 0;
    errorCode = ok ? 0 : (tooLarge ? EFBIG : EIO);
    std::fclose(file);
    if (!ok) {
        std::memset(buffer, 0, sizeof(buffer));
        return false;
    }
    token.assign(buffer, count);
    std::memset(buffer, 0, sizeof(buffer));
    while (!token.empty() && (token.back() == '\n' || token.back() == '\r')) token.pop_back();
    return !token.empty();
}

bool saveRefreshToken(const std::string& token, int& errorCode) {
    if (token.empty()) {
        errorCode = EINVAL;
        return false;
    }
    if (mkdir(kTokenDirectory, 0700) != 0 && errno != EEXIST) {
        errorCode = errno;
        return false;
    }
    if (chmod(kTokenDirectory, 0700) != 0) {
        errorCode = errno;
        return false;
    }
    errorCode = 0;
    const std::string temporary = std::string(kRefreshTokenPath) + ".tmp";
    FILE* file = std::fopen(temporary.c_str(), "wb");
    if (!file) {
        errorCode = errno;
        return false;
    }
    bool ok = std::fwrite(token.data(), 1, token.size(), file) == token.size();
    if (!ok) errorCode = errno ? errno : EIO;
    ok = (std::fflush(file) == 0) && ok;
    if (!ok && errorCode == 0) errorCode = errno ? errno : EIO;
    ok = (std::fclose(file) == 0) && ok;
    if (!ok && errorCode == 0) errorCode = errno ? errno : EIO;
    if (ok && chmod(temporary.c_str(), 0600) != 0) {
        ok = false;
        errorCode = errno;
    }
    if (ok && std::rename(temporary.c_str(), kRefreshTokenPath) != 0) {
        ok = false;
        errorCode = errno;
    }
    if (!ok) std::remove(temporary.c_str());
    else errorCode = 0;
    return ok;
}

void clearRefreshTokenCache() {
    std::remove(kRefreshTokenPath);
}

struct LoginState {
    std::mutex mutex;
    bool gpuVideoActive = false;
    std::atomic<uint32_t> streamWorkerStep{0};
    std::atomic<uint64_t> streamWorkerStepAtMs{0};
    LoginStage stage = LoginStage::Starting;
    std::string verificationUri;
    std::string verificationUriComplete;
    std::string userCode;
    std::string error;
    std::string market;
    std::string region;
    std::string gamertag;
    bool libraryLoading = false;
    size_t libraryLoaded = 0;
    uint64_t libraryRevision = 0;
    std::shared_ptr<xc::xcloud::GssvClient> gssv;
    std::string refreshToken;
    std::string sessionMessage;
    bool stopSessionRequested = false;
    bool streamTransportLost = false;
    uint32_t streamRembEstimate = 0;
    uint32_t streamReceiveRate = 0;
    int streamRttMs = -1;
    uint32_t streamWidth = 0, streamHeight = 0;
    uint32_t streamNewVideoFps = 0, streamPublishedFps = 0;
    uint32_t streamQueueDepth = 0, streamQueuePeak = 0;
    uint32_t streamQueueWaitAvgUs = 0, streamQueueWaitMaxUs = 0;
    uint32_t streamDecodeAvgUs = 0, streamConvertAvgUs = 0;
    uint64_t streamStatsGeneration = 0;
    bool streamSettingsOpen = false;
    bool streamFsrSettingsOpen = false;
    uint8_t streamSettingsRow = 0;
    uint32_t streamFrameRate = 60, streamDraftFrameRate = 60;
    bool streamAutoReset = false, streamDraftAutoReset = false;
    uint32_t streamUpscaler = 1, streamDraftUpscaler = 1;
    uint32_t streamFrameWait = 0, streamDraftFrameWait = 0;
    size_t selectedTitle = 0;
    int expiresInSec = 0;
    uint64_t generation = 1;
};

struct LoginSnapshot {
    LoginStage stage;
    std::string verificationUri;
    std::string verificationUriComplete;
    std::string userCode;
    std::string error;
    std::string market;
    std::string region;
    std::string gamertag;
    std::string sessionMessage;
    size_t selectedTitle;
    int expiresInSec;
    uint64_t generation;
    bool libraryLoading;
    size_t libraryLoaded;
    uint64_t libraryRevision;
    bool streamTransportLost;
    std::string streamTier;
    uint32_t streamBitrateCap;
    uint32_t streamRembEstimate;
    uint32_t streamReceiveRate;
    int streamRttMs;
    uint32_t streamWidth, streamHeight;
    uint32_t streamNewVideoFps, streamPublishedFps;
    uint32_t streamQueueDepth, streamQueuePeak;
    uint32_t streamQueueWaitAvgUs, streamQueueWaitMaxUs;
    uint32_t streamDecodeAvgUs, streamConvertAvgUs;
    uint64_t streamStatsGeneration;
    bool streamSettingsOpen;
    bool streamFsrSettingsOpen = false;
    uint8_t streamSettingsRow;
    std::string streamDraftTier;
    uint32_t streamDraftBitrate;
    uint32_t streamFrameRate = 60, streamDraftFrameRate = 60;
    bool streamAutoReset = false, streamDraftAutoReset = false;
    uint32_t streamUpscaler = 1, streamDraftUpscaler = 1;
    uint32_t streamFrameWait = 0, streamDraftFrameWait = 0;
};

    std::lock_guard<std::mutex> lock(state.mutex);
    return {state.stage, state.verificationUri, state.verificationUriComplete, state.userCode, state.error,
            state.expiresInSec, state.generation, state.libraryLoading, state.libraryLoaded,
            state.streamBitrateCap, state.streamRembEstimate, state.streamReceiveRate,
            state.streamRttMs, state.streamWidth, state.streamHeight,
            state.streamNewVideoFps, state.streamPublishedFps, state.streamQueueDepth,
            state.streamQueuePeak, state.streamQueueWaitAvgUs, state.streamQueueWaitMaxUs,
            state.streamDecodeAvgUs, state.streamConvertAvgUs, state.streamStatsGeneration,
            state.streamFrameRate, state.streamDraftFrameRate,
            state.streamAutoReset, state.streamDraftAutoReset, state.streamUpscaler, state.streamDraftUpscaler,
}

void setStage(LoginState& state, LoginStage stage, const std::string& error = {}) {
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        state.stage = stage;
        state.error = error;
        ++state.generation;
    }
    logLine("login progress stage=%s error_present=%d", loginStageName(stage), error.empty() ? 0 : 1);
}

void wipe(std::string& s) {
    volatile char* p = s.empty() ? nullptr : &s[0];
    for (size_t i = 0; i < s.size(); ++i) p[i] = 0;
    s.clear();
}

void wipeToken(xc::auth::XblToken& token) {
    wipe(token.token);
    wipe(token.userHash);
    wipe(token.gamertag);
    wipe(token.xuid);
    wipe(token.notAfter);
}

bool finishXboxLogin(LoginState& state, xc::auth::MsaTokens& msaTokens) {
    std::string refreshToken = msaTokens.refreshToken;
    setStage(state, LoginStage::XboxLive);
    logLine("xbox live user authentication started");

    xc::auth::XblToken userToken;
    std::string error;
    const bool userOk = xc::auth::xblUserToken(msaTokens.accessToken, "d=", userToken, error);
    wipe(msaTokens.accessToken);
    wipe(msaTokens.refreshToken);
    wipe(msaTokens.userId);
    if (!userOk) {
        logLine("xbox live user authentication failed: %s", error.c_str());
        setStage(state, LoginStage::Failed, error);
        wipeToken(userToken);
        return false;
    }
    const std::string gamertag = userToken.gamertag;
    logLine("xbox live user authentication complete");

    xc::auth::XblToken gssvXsts;
    if (!xc::auth::xstsToken(userToken, xc::auth::rp::kGssv, gssvXsts, error)) {
        logLine("xsts authorization for xcloud failed: %s", error.c_str());
        setStage(state, LoginStage::Failed, error);
        wipeToken(userToken);
        wipeToken(gssvXsts);
        return false;
    }
    wipeToken(userToken);
    logLine("xsts authorization for xcloud complete");

    setStage(state, LoginStage::Xcloud);
    logLine("xcloud GSSV login started offering=xgpuweb");
    xc::ps4::XcloudLoginInfo info;
    bool libraryPublished = false;
    auto publishLibrary = [&](const xc::ps4::XcloudLoginInfo& update, bool complete) {
        // Pause catalog pagination at a batch boundary while the media session
        // owns the connection/CPU budget. Never wait holding the UI mutex.
        bool paused = false;
        for (;;) {
            bool mediaActive;
            {
                std::lock_guard<std::mutex> lock(state.mutex);
                mediaActive = state.stage == LoginStage::SessionStarting ||
            }
            if (!mediaActive) break;
            if (!paused) { logLine("library background pagination paused for active stream"); paused = true; }
            sceKernelUsleep(100000);
        }
        if (paused) logLine("library background pagination resumed");
        size_t count = update.titles.size();
        {
            std::lock_guard<std::mutex> lock(state.mutex);
            if (state.stage == LoginStage::Xcloud || state.stage == LoginStage::Library)
                state.stage = LoginStage::Library;
            state.error.clear();
            state.market = update.market;
            state.region = update.region;
            state.gamertag = gamertag;
            state.gssv = update.client;
            if (state.refreshToken.empty()) state.refreshToken = refreshToken;
            if (!libraryPublished) {
                state.selectedTitle = 0;
                state.sessionMessage.clear();
            }
            state.libraryLoading = !complete;
            ++state.libraryRevision;
            ++state.generation;
        }
        libraryPublished = true;
    };
    if (!xc::ps4::loginXcloud(gssvXsts, info, error, publishLibrary)) {
        logLine("xcloud GSSV login failed: %s", error.c_str());
        if (libraryPublished) {
            std::lock_guard<std::mutex> lock(state.mutex);
            state.libraryLoading = false;
            if (state.stage == LoginStage::Library) state.error = error;
            ++state.generation;
                    state.libraryLoaded);
        } else {
            setStage(state, LoginStage::Failed, error);
        }
        wipeToken(gssvXsts);
        wipe(refreshToken);
        return libraryPublished;
    }
    wipeToken(gssvXsts);
    wipe(refreshToken);
            info.market.c_str(), info.region.c_str(), info.regionCount, info.titles.size(),
    if (!info.catalogWarning.empty())
        logLine("catalog artwork/name lookup warning: %s", info.catalogWarning.c_str());
    return true;
}

void updateSession(LoginState& state, LoginStage stage, const std::string& message,
                   const std::string& error = {}) {
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        state.stage = stage;
        state.sessionMessage = message;
        state.error = error;
        ++state.generation;
    }
    logLine("session progress stage=%s detail=%s error_present=%d",
            loginStageName(stage), message.empty() ? "(none)" : message.c_str(), error.empty() ? 0 : 1);
}

struct SessionJob {
    LoginState* state;
    std::shared_ptr<xc::xcloud::GssvClient> gssv;
    std::string titleId;
    std::string refreshToken;
    std::string titleName;
    xc::ps4::VideoDecoder* video = nullptr;
    Controller* controller = nullptr;
};

xc::stream::GamepadFrame readStreamGamepad(const Controller& controller);

void* sessionWorker(void* opaque) {
    struct ResumeBackgroundOnExit {
        ~ResumeBackgroundOnExit() { xc::net::pauseBackgroundHttp(false); }
    } resumeBackgroundOnExit;
    std::unique_ptr<SessionJob> job(static_cast<SessionJob*>(opaque));
    auto& state = *job->state;
    std::string error;
    StreamProfile streamProfile = loadStreamProfile();
    logLine("session setup started title=%s title_id=%s", job->titleName.c_str(), job->titleId.c_str());
    if (job->gssv) {
        job->gssv->setResolution(streamProfile.tier == "1080"
            ? xc::xcloud::Resolution::P1080 : xc::xcloud::Resolution::P720);
        logLine("cloud session profile requested=%s bitrate_cap=%u resolution_policy=user_selected_persistent",
                tierLabel(streamProfile.tier), streamProfile.maxBitrate);
    }
    if (!job->gssv || !job->gssv->startSession(job->titleId, "en-US", error)) {
        updateSession(state, LoginStage::Failed, {}, error.empty() ? "Could not start the cloud session." : error);
        logLine("session start failed: %s", error.c_str());
        wipe(job->refreshToken);
        return nullptr;
    }
    updateSession(state, LoginStage::SessionProvisioning, "Request accepted; checking cloud capacity...");

    bool connected = false;
    int pollErrors = 0;
    std::string lastRawState;
    for (int attempt = 0; attempt < 600; ++attempt) {
        xc::xcloud::SessionStatus status;
        error.clear();
        if (!job->gssv->sessionState(status, error)) {
            if (++pollErrors >= 3) {
                updateSession(state, LoginStage::Failed, {}, error);
                logLine("session status failed after retries: %s", error.c_str());
                job->gssv->stopSession();
                wipe(job->refreshToken);
                return nullptr;
            }
            sceKernelUsleep(1000000);
            continue;
        }
        pollErrors = 0;
        if (status.raw != lastRawState) {
            logLine("session state=%s", status.raw.c_str());
            lastRawState = status.raw;
            updateSession(state, LoginStage::SessionProvisioning,
                          "Cloud service state: " + status.raw);
        }
        if (status.state == xc::xcloud::SessionState::Failed) {
            const std::string message = status.errorCode + " " + status.errorMessage;
            updateSession(state, LoginStage::Failed, {}, message.empty() ? "The cloud session failed." : message);
            job->gssv->stopSession();
            wipe(job->refreshToken);
            return nullptr;
        }
        if (status.state == xc::xcloud::SessionState::WaitingForResources) {
            if (attempt % 15 == 0) {
                char message[128];
                const int wait = job->gssv->waitTimeSeconds();
                if (wait >= 0) std::snprintf(message, sizeof(message), "Waiting in cloud queue (estimate: %d sec)", wait);
                else std::snprintf(message, sizeof(message), "Waiting for cloud capacity...");
                updateSession(state, LoginStage::SessionProvisioning, message);
            }
        } else if (status.state == xc::xcloud::SessionState::ReadyToConnect && !connected) {
            updateSession(state, LoginStage::SessionProvisioning, "Cloud host ready; completing console authorization...");
            xc::auth::MsaConfig config;
            xc::auth::MsaClient msa(std::move(config));
            xc::auth::MsaTokens transfer;
            if (!msa.refresh(job->refreshToken, transfer, error, kTransferScope)) {
                updateSession(state, LoginStage::Failed, {}, "Console-transfer authorization failed: " + error);
                logLine("console-transfer token request failed: %s", error.c_str());
                job->gssv->stopSession();
                wipe(job->refreshToken);
                return nullptr;
            }
            int cacheError = 0;
            if (!transfer.refreshToken.empty() && saveRefreshToken(transfer.refreshToken, cacheError)) {
                wipe(job->refreshToken);
                job->refreshToken = transfer.refreshToken;
                std::lock_guard<std::mutex> lock(state.mutex);
                wipe(state.refreshToken);
                state.refreshToken = transfer.refreshToken;
            }
            if (!job->gssv->connect(transfer.accessToken, error)) {
                wipe(transfer.accessToken);
                wipe(transfer.refreshToken);
                updateSession(state, LoginStage::Failed, {}, error);
                logLine("session connect failed: %s", error.c_str());
                job->gssv->stopSession();
                wipe(job->refreshToken);
                return nullptr;
            }
            wipe(transfer.accessToken);
            wipe(transfer.refreshToken);
            connected = true;
            logLine("session connect authorization complete");
        } else if (status.state == xc::xcloud::SessionState::Provisioned) {
            updateSession(state, LoginStage::SessionProvisioning,
                          "Session ready; starting H.264 transport and PS4 decoder...");
            logLine("session provisioned; retaining session and starting WebRTC video");

            updateSession(state, LoginStage::SessionProvisioning,
                          "Initializing the PS4 H.264 decoder...");
            logLine("H.264 decoder initialization begin");
            if (!job->video || !job->video->start()) {
                const std::string message = "PS4 Videodec2 initialization failed.";
                logLine("video decoder initialization failed");
                job->gssv->stopSession();
                updateSession(state, LoginStage::Failed, {}, message);
                wipe(job->refreshToken);
                return nullptr;
            }
            logLine("H.264 decoder initialization complete");

            xc::stream::StreamCallbacks callbacks;
            callbacks.progress = [&state](const std::string& detail) {
                bool streamUiActive = false;
                {
                    std::lock_guard<std::mutex> lock(state.mutex);
                    streamUiActive = state.stage == LoginStage::Streaming;
                }
                if (!streamUiActive)
                    updateSession(state, LoginStage::SessionProvisioning,
                logLine("xcloud transport progress: %s", detail.c_str());
            };
            callbacks.video = [video = job->video](const uint8_t* data, size_t size, uint32_t timestamp) {
                (void)video->pushAccessUnit(data, size, timestamp);
            };
            callbacks.closed = [&state](const std::string& reason, bool recoverable) {
                bool userRequested = false;
                {
                    std::lock_guard<std::mutex> lock(state.mutex);
                    userRequested = state.stopSessionRequested;
                    if (!userRequested && state.stage == LoginStage::Streaming) {
                        state.streamTransportLost = true;
                        state.sessionMessage = recoverable
                            ? "Video paused: connection lost. The last frame is held on screen."
                            : "Video transport ended. The last frame is held on screen.";
                        ++state.generation;
                    }
                }
                logLine("WebRTC stream closed recoverable=%d user_cancel=%d reason=%s; keeping app in stream UI",
                        recoverable ? 1 : 0, userRequested ? 1 : 0, reason.c_str());
            };
            xc::stream::StreamOptions streamOptions;
            streamOptions.baselineH264Only = true;
            streamOptions.enableIceUdpMux = true;
            streamOptions.resolutionAlias = streamProfile.tier;
            streamOptions.maxBitrate = streamProfile.maxBitrate;
            streamOptions.maxFrameRate = streamProfile.frameRate;
            // Recovery already requests IDR on loss/discontinuity. Periodic
            // unconditional IDRs add bursts even when the picture is healthy.
            streamOptions.keyframeIntervalSec = 0;
            logLine("stream media config resolution=%sp remb_cap=%u decoder_max=1920x1088 adaptive=queue+decode+convert",
                    streamProfile.tier.c_str(), streamProfile.maxBitrate);
            xc::stream::StreamSession stream(*job->gssv, std::move(callbacks), std::move(streamOptions));
            updateSession(state, LoginStage::SessionProvisioning,
                          "Negotiating H.264 video with the cloud console...");
            error.clear();
            if (!stream.start(error)) {
                logLine("WebRTC start failed: %s", error.c_str());
                stream.close();
                job->video->stop();
                job->gssv->stopSession();
                updateSession(state, LoginStage::Failed, {}, "Video transport failed: " + error);
                wipe(job->refreshToken);
                return nullptr;
            }

            updateSession(state, LoginStage::Streaming,
            {
                std::lock_guard<std::mutex> lock(state.mutex);
                state.streamTransportLost = false;
                state.streamFrameRate = streamProfile.frameRate;
                state.streamTier = streamProfile.tier;
                state.streamBitrateCap = streamProfile.maxBitrate;
                state.streamSettingsOpen = false;
                state.streamAutoReset = streamProfile.automaticVideoReset;
                state.streamUpscaler = streamProfile.upscaler;
                state.streamRcasSharpness = streamProfile.rcasSharpness;
                state.streamFrameWait = streamProfile.frameWait;
                state.streamFsrSettingsOpen = false;
            }
            logLine("WebRTC connected; waiting for H.264 pictures and controller reports");
            logLine("video_scaler policy=vulkan_nv12_preferred cpu_fallback=available actual_path_in_stream_metrics=1");
            bool requestedStop = false;
            uint64_t nextGamepadAt = sceKernelGetProcessTime() / 1000u;

            uint64_t nextPerformanceCheckAt = nextGamepadAt;
            uint64_t nextVideoFlowLogAt = nextGamepadAt;
            uint64_t lastVideoDrops = 0;
            xc::stream::GamepadFrame lastGamepad{};
            bool firstGamepadReport = true;
            bool previousTouchpad = false;
            bool previousCircle = false, previousCross = false, previousTriangle = false;
            bool settingsOpen = false, settingsOpenLatched = false;
            uint64_t optionsHeldSince = 0;
            uint64_t nextSettingsRepeatAt = 0;
            uint8_t settingsRow = 0;
            std::string draftTier = streamProfile.tier;
            uint32_t draftBitrate = streamProfile.maxBitrate;
            uint32_t draftFps = streamProfile.frameRate;
            bool draftAutoReset = streamProfile.automaticVideoReset;
            uint32_t draftUpscaler = streamProfile.upscaler;
            uint32_t draftFrameWait = streamProfile.frameWait;
            uint32_t draftRcasSharpness = streamProfile.rcasSharpness;
            bool fsrSettingsOpen = false;
            StreamProfile settingsOriginal = streamProfile;
            uint64_t nextHudStatsAt = nextGamepadAt;
            uint64_t decoderResetHeldSince = 0;
            bool decoderResetLatched = false;
            uint64_t xboxGuideUntil = 0;
            bool transportLostNotified = false;
            uint64_t lastPerformanceGeneration = 0;
            uint64_t performanceWindows = 0;
            uint32_t pressureWindows = 0;
            StreamProfile profileToSave = streamProfile;
            xc::ps4::VideoPerformanceSnapshot lastVideoPerformance{};
            std::mutex sendStateMutex;
            std::condition_variable sendStateChanged;
            xc::stream::GamepadFrame latestGamepad{};
            uint64_t latestGamepadSampleAtMs = 0;
            uint64_t latestGamepadGeneration = 0;
            std::atomic<bool> inputMuted{false};
            std::atomic<uint64_t> guidePulseUntil{0};
            std::atomic<bool> stopInputPoll{false};
            std::atomic<bool> stopGamepadSender{false};
            std::atomic<bool> stopGamepadMetrics{false};
            std::atomic<uint64_t> gamepadAttempts{0}, gamepadAccepted{0}, gamepadRejected{0};
            std::atomic<uint64_t> gamepadSendDurationUsMax{0};
            std::atomic<uint64_t> gamepadInputAgeMaxMs{0};
            std::atomic<uint64_t> gamepadSendStartedAtMs{0};
            std::atomic<uint64_t> controllerPollCalls{0};
            std::atomic<bool> maintenanceStop{false}, pendingKeyframe{false};
            std::atomic<int> pendingResolution{0}, cachedRtt{-1};
            std::atomic<uint32_t> maintenancePhase{0};
            std::atomic<uint64_t> maintenanceAtMs{0};
            std::mutex profileWriteMutex;
            bool profileWritePending = false;
            StreamProfile pendingProfile = streamProfile;
            auto queueProfileWrite = [&](const StreamProfile& value) {
                std::lock_guard<std::mutex> lock(profileWriteMutex);
                pendingProfile = value; profileWritePending = true;
            };
            std::thread maintenance([&]() {
                uint64_t nextTick = 0, lastKeyframe = 0;
                auto phase = [&](uint32_t value) {
                    maintenanceAtMs.store(sceKernelGetProcessTime() / 1000u);
                    maintenancePhase.store(value);
                };
                while (!maintenanceStop.load()) {
                    const uint64_t now = sceKernelGetProcessTime() / 1000u;
                    const int resolution = pendingResolution.exchange(0);
                    if (resolution) { phase(1); stream.requestResolution(resolution == 1080 ? "1080" : "720"); }
                    if (pendingKeyframe.load() && now - lastKeyframe >= 1000) {
                        pendingKeyframe.store(false); lastKeyframe = now;
                        phase(2); stream.requestKeyframe();
                        logLine("video_recovery action=request_keyframe coalesced=1 decoder_reset=0");
                    }
                    if (now >= nextTick) {
                        nextTick = now + 1000;
                        phase(3); stream.tick();
                        phase(4); cachedRtt.store(stream.rttMs());
                    }
                    bool write = false; StreamProfile profile;
                    {
                        std::lock_guard<std::mutex> lock(profileWriteMutex);
                        if (profileWritePending) { profile = pendingProfile; profileWritePending = false; write = true; }
                    }
                    if (write) { phase(5); saveStreamProfile(profile); }
                    phase(0); sceKernelUsleep(10000);
                }
            });
            std::mutex gamepadIntervalMutex;
            std::array<uint64_t, 512> gamepadIntervalsUs{};
            size_t gamepadIntervalCount = 0;
            auto workerStep = [&state](uint32_t step) {
                state.streamWorkerStepAtMs.store(sceKernelGetProcessTime() / 1000u, std::memory_order_relaxed);
                state.streamWorkerStep.store(step, std::memory_order_release);
            };
            std::thread inputPoller([&]() {
                while (!stopInputPoll.load(std::memory_order_acquire)) {
                    if (job->controller) job->controller->pump();
                    ++controllerPollCalls;
                    auto sampled = job->controller ? readStreamGamepad(*job->controller) : xc::stream::GamepadFrame{};
                    const uint64_t sampledAt = sceKernelGetProcessTime() / 1000u;
                    if (sampledAt < guidePulseUntil.load(std::memory_order_relaxed))
                        sampled.buttons |= xc::stream::kNexus;
                    if (job->controller && job->controller->button(SDL_GAMEPAD_BUTTON_LEFT_STICK) &&
                        job->controller->button(SDL_GAMEPAD_BUTTON_RIGHT_STICK))
                        sampled.buttons &= ~(xc::stream::kLeftThumb | xc::stream::kRightThumb);
                    if (inputMuted.load(std::memory_order_acquire)) sampled = {};
                    {
                        std::lock_guard<std::mutex> lock(sendStateMutex);
                        latestGamepad = sampled;
                        latestGamepadSampleAtMs = sampledAt;
                        ++latestGamepadGeneration;
                    }
                    sendStateChanged.notify_one();
                    sceKernelUsleep(4000);
                }
            });
            std::thread gamepadSender([&]() {
                uint64_t sentGeneration = 0;
                uint64_t previousCallStartUs = 0;
                while (!stopGamepadSender.load(std::memory_order_acquire)) {
                    xc::stream::GamepadFrame frame{};
                    uint64_t sampleAtMs = 0, generation = 0;
                    {
                        std::unique_lock<std::mutex> lock(sendStateMutex);
                        sendStateChanged.wait(lock, [&]() {
                            return stopGamepadSender.load(std::memory_order_acquire) ||
                                   latestGamepadGeneration != sentGeneration;
                        });
                        if (stopGamepadSender.load(std::memory_order_acquire)) break;
                        frame = latestGamepad;
                        sampleAtMs = latestGamepadSampleAtMs;
                        generation = latestGamepadGeneration;
                    }
                    const uint64_t nowMs = sceKernelGetProcessTime() / 1000u;
                    if (stream.isOpen()) {
                        const uint64_t callStartUs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now().time_since_epoch()).count());
                        if (previousCallStartUs) {
                            std::lock_guard<std::mutex> lock(gamepadIntervalMutex);
                            if (gamepadIntervalCount < gamepadIntervalsUs.size())
                                gamepadIntervalsUs[gamepadIntervalCount++] = callStartUs - previousCallStartUs;
                        }
                        previousCallStartUs = callStartUs;
                        ++gamepadAttempts;
                        gamepadSendStartedAtMs.store(nowMs, std::memory_order_release);
                        const uint64_t inputAgeMs = sampleAtMs && nowMs >= sampleAtMs ? nowMs - sampleAtMs : 0;
                        uint64_t oldAge = gamepadInputAgeMaxMs.load();
                        while (inputAgeMs > oldAge && !gamepadInputAgeMaxMs.compare_exchange_weak(oldAge, inputAgeMs)) {}
                        const auto begin = std::chrono::steady_clock::now();
                        const bool accepted = stream.sendGamepad(frame);
                        gamepadSendStartedAtMs.store(0, std::memory_order_release);
                        const uint64_t elapsedUs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now() - begin).count());
                        uint64_t oldMax = gamepadSendDurationUsMax.load();
                        while (elapsedUs > oldMax && !gamepadSendDurationUsMax.compare_exchange_weak(oldMax, elapsedUs)) {}
                        if (accepted) ++gamepadAccepted; else ++gamepadRejected;
                    }
                    sentGeneration = generation;
                }
            });
            std::thread gamepadMetrics([&]() {
                uint64_t windowStart = sceKernelGetProcessTime() / 1000u;
                uint64_t lastAttempts = 0, lastAccepted = 0, lastRejected = 0;
                while (!stopGamepadMetrics.load(std::memory_order_acquire)) {
                    sceKernelUsleep(100000);
                    const uint64_t nowMs = sceKernelGetProcessTime() / 1000u;
                    const uint64_t elapsedMs = nowMs - windowStart;
                    if (elapsedMs < 1000) continue;
                    const uint64_t attempts = gamepadAttempts.load();
                    const uint64_t accepted = gamepadAccepted.load();
                    const uint64_t rejected = gamepadRejected.load();
                    const uint64_t attemptDelta = attempts - lastAttempts;
                    const uint64_t acceptedDelta = accepted - lastAccepted;
                    const uint64_t rejectedDelta = rejected - lastRejected;
                    const uint64_t polls = controllerPollCalls.exchange(0);
                    std::vector<uint64_t> intervals;
                    {
                        std::lock_guard<std::mutex> lock(gamepadIntervalMutex);
                        intervals.assign(gamepadIntervalsUs.begin(), gamepadIntervalsUs.begin() + gamepadIntervalCount);
                        gamepadIntervalCount = 0;
                    }
                    std::sort(intervals.begin(), intervals.end());
                    const uint64_t intervalMinUs = intervals.empty() ? 0 : intervals.front();
                    const size_t p99Index = intervals.empty() ? 0 : (intervals.size() * 99 + 99) / 100 - 1;
                    const uint64_t intervalP99Us = intervals.empty() ? 0 : intervals[p99Index];
                    const uint64_t sendStarted = gamepadSendStartedAtMs.load(std::memory_order_acquire);
                    const uint64_t inFlightMs = sendStarted && nowMs >= sendStarted ? nowMs - sendStarted : 0;
                    logLine("GAMEPAD TX rate window_ms=%llu input_poll_calls=%llu input_poll_hz=%.2f calls=%llu accepted=%llu coalesced_or_failed=%llu calls_per_sec=%.2f accepted_per_sec=%.2f intervals_n=%zu interval_min_ms=%.3f interval_p99_ms=%.3f max_send_ms=%.2f input_age_max_ms=%llu in_flight_ms=%llu policy=latest_sample_no_backlog local_acceptance_only=1",
                        static_cast<unsigned long long>(elapsedMs), static_cast<unsigned long long>(polls),
                        elapsedMs ? polls * 1000.0 / elapsedMs : 0.0,
                        static_cast<unsigned long long>(attemptDelta), static_cast<unsigned long long>(acceptedDelta),
                        static_cast<unsigned long long>(rejectedDelta),
                        elapsedMs ? attemptDelta * 1000.0 / elapsedMs : 0.0,
                        elapsedMs ? acceptedDelta * 1000.0 / elapsedMs : 0.0,
                        intervals.size(), intervalMinUs / 1000.0, intervalP99Us / 1000.0,
                        gamepadSendDurationUsMax.exchange(0) / 1000.0,
                        static_cast<unsigned long long>(gamepadInputAgeMaxMs.exchange(0)),
                        static_cast<unsigned long long>(inFlightMs));
                    const auto decoder = job->video->performanceSnapshot();
                    const auto renderer = xc::ps4::presentationHeartbeat();
                    const auto& media = stream.videoStats();
                    const uint64_t phaseAt = maintenanceAtMs.load();
                    logLine("pipeline_health renderer_stage=%u renderer_age_ms=%llu decoder_stage=%u decoder_stage_age_ms=%llu decoder_input_age_ms=%llu decoder_output_age_ms=%llu decoder_queue=%u snapshot_busy=%u rtp_age_ms=%llu rtp_packets=%llu maintenance_stage=%u maintenance_age_ms=%llu",
                        renderer.stage, static_cast<unsigned long long>(renderer.ageMs), decoder.stage,
                        static_cast<unsigned long long>(decoder.stageAgeMs),
                        static_cast<unsigned long long>(decoder.lastInputAgeMs), static_cast<unsigned long long>(decoder.lastOutputAgeMs),
                        decoder.queuedAccessUnits, decoder.snapshotBusy, static_cast<unsigned long long>(media.lastRtpAgeMs.load()),
                        static_cast<unsigned long long>(media.packets.load()), maintenancePhase.load(),
                        static_cast<unsigned long long>(phaseAt && nowMs >= phaseAt ? nowMs-phaseAt : 0));
                    windowStart = nowMs;
                    lastAttempts = attempts;
                    lastAccepted = accepted;
                    lastRejected = rejected;
                }
            });
            while (true) {
                workerStep(1);
                {
                    std::lock_guard<std::mutex> lock(state.mutex);
                    requestedStop = state.stopSessionRequested;
                }
                if (requestedStop) break;
                const uint64_t now = sceKernelGetProcessTime() / 1000u;
                const bool streamOpen = stream.isOpen();
                    workerStep(2);
                    workerStep(3);
                    const bool options = job->controller && job->controller->button(SDL_GAMEPAD_BUTTON_START);
                    const bool circle = job->controller && job->controller->button(SDL_GAMEPAD_BUTTON_EAST);
                    const bool touchpad = job->controller && job->controller->button(SDL_GAMEPAD_BUTTON_TOUCHPAD);
                    const bool resetChord = job->controller &&
                        job->controller->button(SDL_GAMEPAD_BUTTON_LEFT_STICK) &&
                        job->controller->button(SDL_GAMEPAD_BUTTON_RIGHT_STICK);
                    if (resetChord) {
                        if (!decoderResetHeldSince) decoderResetHeldSince = now;
                        if (!decoderResetLatched && now - decoderResetHeldSince >= 1000) {
                            decoderResetLatched = true;
                            job->video->requestReset(xc::ps4::VideoResetRequestReason::UserMasterButton);
                            logLine("decoder_reset request=user_master_button chord=L3+R3 hold_ms=1000");
                        }
                    } else {
                        decoderResetHeldSince = 0;
                        decoderResetLatched = false;
                    }
                    if (options) {
                        if (!optionsHeldSince) optionsHeldSince = now;
                        if (!settingsOpen && !settingsOpenLatched && now - optionsHeldSince >= 1000) {
                            settingsOpen = true;
                            inputMuted.store(true, std::memory_order_release);
                            settingsOpenLatched = true;
                            settingsRow = 0;
                            settingsOriginal = streamProfile;
                            draftTier = streamProfile.tier;
                            draftBitrate = streamProfile.maxBitrate;
                            draftFps = streamProfile.frameRate;
                            draftAutoReset = streamProfile.automaticVideoReset;
                            draftUpscaler = streamProfile.upscaler;
                            draftFrameWait = streamProfile.frameWait;
                            draftRcasSharpness = streamProfile.rcasSharpness;
                            fsrSettingsOpen = false;
                            nextSettingsRepeatAt = 0;
                            {
                                std::lock_guard<std::mutex> lock(state.mutex);
                                state.streamSettingsOpen = true;
                                state.streamSettingsRow = settingsRow;
                                state.streamDraftTier = draftTier;
                                state.streamDraftBitrate = draftBitrate;
                                state.streamDraftFrameRate = draftFps;
                                state.streamDraftAutoReset = draftAutoReset;
                                state.streamDraftUpscaler = draftUpscaler;
                                state.streamFsrSettingsOpen = false;
                                state.streamDraftRcasSharpness = draftRcasSharpness;
                            state.streamDraftFrameWait = draftFrameWait;
                                ++state.generation;
                            }
                            logLine("stream_settings action=open hold_ms=1000 tier=%s bitrate_cap=%u controls=dpad_cross_circle",
                                    tierLabel(streamProfile.tier), streamProfile.maxBitrate);
                        }
                    } else {
                        optionsHeldSince = 0;
                        settingsOpenLatched = false;
                    }

                    const bool wasSettingsOpen = settingsOpen;
                    const bool cross = job->controller && job->controller->button(SDL_GAMEPAD_BUTTON_SOUTH);
                    const bool triangle = job->controller && job->controller->button(SDL_GAMEPAD_BUTTON_NORTH);
                    const bool dpadUp = job->controller && job->controller->button(SDL_GAMEPAD_BUTTON_DPAD_UP);
                    const bool dpadDown = job->controller && job->controller->button(SDL_GAMEPAD_BUTTON_DPAD_DOWN);
                    const bool dpadLeft = job->controller && job->controller->button(SDL_GAMEPAD_BUTTON_DPAD_LEFT);
                    const bool dpadRight = job->controller && job->controller->button(SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
                    if (settingsOpen) {
                        auto updateLiveSettings = [&](const StreamProfile& value) {
                            if (streamProfile.tier != value.tier)
                                pendingResolution.store(value.tier == "1080" ? 1080 : 720);
                            if (streamProfile.maxBitrate != value.maxBitrate)
                                stream.setMaximumBitrate(value.maxBitrate);
                            streamProfile = value;
                            std::lock_guard<std::mutex> lock(state.mutex);
                            state.streamFrameRate = value.frameRate;
                            state.streamAutoReset = value.automaticVideoReset;
                            state.streamUpscaler = value.upscaler;
                            state.streamRcasSharpness = value.rcasSharpness;
                            state.streamFrameWait = value.frameWait;
                            state.streamTier = value.tier;
                            state.streamBitrateCap = value.maxBitrate;
                            ++state.generation;
                        };
                            fsrSettingsOpen = !fsrSettingsOpen;
                            std::lock_guard<std::mutex> lock(state.mutex);
                            state.streamFsrSettingsOpen = fsrSettingsOpen;
                            ++state.generation;
                            logLine("stream_settings action=%s submenu=fsr rcas_sharpness_level=%u", fsrSettingsOpen ? "open" : "close", draftRcasSharpness);
                        } else if (fsrSettingsOpen && circle && !previousCircle) {
                            fsrSettingsOpen = false;
                            std::lock_guard<std::mutex> lock(state.mutex);
                            state.streamFsrSettingsOpen = false;
                            ++state.generation;
                        } else if (circle && !previousCircle) {
                            updateLiveSettings(settingsOriginal);
                            settingsOpen = false;
                            inputMuted.store(false, std::memory_order_release);
                            {
                                std::lock_guard<std::mutex> lock(state.mutex);
                                state.streamSettingsOpen = false;
                                ++state.generation;
                            }
                            logLine("stream_settings action=cancel tier=%s bitrate_cap=%u",
                                    tierLabel(streamProfile.tier), streamProfile.maxBitrate);
                        } else if (cross && !previousCross) {
                            const bool resolutionChanged = streamProfile.tier != draftTier;
                            streamProfile.tier = draftTier;
                            streamProfile.maxBitrate = draftBitrate;
                            streamProfile.frameRate = draftFps;
                            streamProfile.automaticVideoReset = draftAutoReset;
                            streamProfile.upscaler = draftUpscaler;
                            streamProfile.frameWait = draftFrameWait;
                            streamProfile.rcasSharpness = draftRcasSharpness;
                            logLine("stream_fps local_target=%u server_sdp_applies=next_session received_60fps_must_still_decode_all_references=1", draftFps);
                            if (resolutionChanged)
                                pendingResolution.store(streamProfile.tier == "1080" ? 1080 : 720);
                            stream.setMaximumBitrate(streamProfile.maxBitrate);
                            profileToSave = streamProfile;
                            pressureWindows = 0;
                            queueProfileWrite(profileToSave);
                            {
                                std::lock_guard<std::mutex> lock(state.mutex);
                                state.streamFrameRate = streamProfile.frameRate;
                                state.streamAutoReset = streamProfile.automaticVideoReset;
                                state.streamUpscaler = streamProfile.upscaler;
                                state.streamRcasSharpness = streamProfile.rcasSharpness;
                state.streamFrameWait = streamProfile.frameWait;
                                state.streamFsrSettingsOpen = false;
                                state.streamTier = streamProfile.tier;
                                state.streamBitrateCap = streamProfile.maxBitrate;
                                state.streamSettingsOpen = false;
                                ++state.generation;
                            }
                            settingsOpen = false;
                            inputMuted.store(false, std::memory_order_release);
                                    tierLabel(streamProfile.tier), streamProfile.maxBitrate,
                            else if (settingsRow == 2) draftFps = draftFps == 60 ? 30 : 60;
                            else if (settingsRow == 3) draftAutoReset = !draftAutoReset;
                            else if (settingsRow == 5) draftFrameWait = draftFrameWait ? 0 : 1;
                            else if (settingsRow == 0) draftTier = draftTier == "720" ? "1080" : "720";
                                size_t index = 0;
                                while (index + 1 < kSelectableBitrates.size() &&
                                       kSelectableBitrates[index] != draftBitrate) ++index;
                                if (dpadLeft) index = (index + kSelectableBitrates.size() - 1) % kSelectableBitrates.size();
                                else index = (index + 1) % kSelectableBitrates.size();
                                draftBitrate = kSelectableBitrates[index];
                            }
                            if (!fsrSettingsOpen && (dpadLeft || dpadRight)) {
                                StreamProfile preview = streamProfile;
                                preview.tier = draftTier;
                                preview.maxBitrate = draftBitrate;
                                preview.frameRate = draftFps;
                                preview.automaticVideoReset = draftAutoReset;
                                preview.frameWait = draftFrameWait;
                                preview.upscaler = draftUpscaler;
                                updateLiveSettings(preview);
                                    settingsRow, tierLabel(draftTier), draftBitrate, draftFps,
                            }
                            nextSettingsRepeatAt = now + 220;
                            std::lock_guard<std::mutex> lock(state.mutex);
                            state.streamSettingsRow = settingsRow;
                            state.streamDraftTier = draftTier;
                            state.streamDraftBitrate = draftBitrate;
                            state.streamDraftFrameRate = draftFps;
                            state.streamDraftAutoReset = draftAutoReset;
                            state.streamDraftUpscaler = draftUpscaler;
                            state.streamDraftRcasSharpness = draftRcasSharpness;
                            state.streamDraftFrameWait = draftFrameWait;
                            state.streamFsrSettingsOpen = fsrSettingsOpen;
                            ++state.generation;
                        }
                    }

                    if (touchpad && !previousTouchpad) {
                        xboxGuideUntil = now + 250;
                        guidePulseUntil.store(xboxGuideUntil, std::memory_order_release);
                    }
                    previousTouchpad = touchpad;
                    previousCircle = circle;
                    previousCross = cross;
                    previousTriangle = triangle;
                    xc::stream::GamepadFrame gamepad = job->controller
                        ? readStreamGamepad(*job->controller) : xc::stream::GamepadFrame{};
                    if (settingsOpen || wasSettingsOpen) gamepad = {};
                    if (streamOpen) {
                        const auto axisBucket = [](float value) { return static_cast<int>(value * 8.0f); };
                        const bool axesChanged = axisBucket(gamepad.leftX) != axisBucket(lastGamepad.leftX) ||
                                                 axisBucket(gamepad.leftY) != axisBucket(lastGamepad.leftY) ||
                                                 axisBucket(gamepad.rightX) != axisBucket(lastGamepad.rightX) ||
                                                 axisBucket(gamepad.rightY) != axisBucket(lastGamepad.rightY) ||
                                                 axisBucket(gamepad.leftTrigger) != axisBucket(lastGamepad.leftTrigger) ||
                                                 axisBucket(gamepad.rightTrigger) != axisBucket(lastGamepad.rightTrigger);
                        if (firstGamepadReport || gamepad.buttons != lastGamepad.buttons || axesChanged) {
                            logLine("GAMEPAD state queued buttons=0x%04x left=(%.2f,%.2f) right=(%.2f,%.2f) triggers=(%.2f,%.2f)",
                                    gamepad.buttons, gamepad.leftX, gamepad.leftY,
                                    gamepad.rightX, gamepad.rightY,
                                    gamepad.leftTrigger, gamepad.rightTrigger);
                            firstGamepadReport = false;
                        }
                        lastGamepad = gamepad;
                    }
                    nextGamepadAt = now + 16;
                }
                if (!streamOpen) {
                    if (!transportLostNotified) {
                        {
                            std::lock_guard<std::mutex> lock(state.mutex);
                            if (!state.streamTransportLost) {
                                state.streamTransportLost = true;
                                ++state.generation;
                            }
                        }
                        logLine("stream transport closed; holding stream UI and last frame until explicit user cancel");
                        transportLostNotified = true;
                    }
                    sceKernelUsleep(50000);
                    continue;
                }
                if (job->video->takeKeyframeRequest()) {
                    pendingKeyframe.store(true);
                    logLine("video recovery action=request_keyframe reason=decoder_reset");
                }
                if (now >= nextPerformanceCheckAt) {
                    nextPerformanceCheckAt = now + 250;
                    workerStep(6);
                    const auto performance = job->video->performanceSnapshot();
                    if (performance.lastOutputAgeMs > 750 && performance.lastInputAgeMs > 250)
                        pendingKeyframe.store(true);
                    if (now >= nextVideoFlowLogAt) {
                        nextVideoFlowLogAt = now + 2000;
                        logLine("video_flow received=%llu accepted=%llu dequeued=%llu queued=%u decoded=%llu published=%llu errors=%llu empty=%llu dropped=%llu catchup_dropped=%llu input_age_ms=%llu output_age_ms=%llu cpu=%d,%d,%d,%d,%d",
                            static_cast<unsigned long long>(performance.receivedAccessUnits),
                            static_cast<unsigned long long>(performance.acceptedAccessUnits),
                            static_cast<unsigned long long>(performance.dequeuedAccessUnits), performance.queuedAccessUnits,
                            static_cast<unsigned long long>(performance.totalDecoded),
                            static_cast<unsigned long long>(performance.totalPublished),
                            static_cast<unsigned long long>(performance.failedAccessUnits),
                            static_cast<unsigned long long>(performance.emptyOutputs),
                            static_cast<unsigned long long>(performance.droppedAccessUnits),
                            static_cast<unsigned long long>(performance.catchupDroppedAccessUnits),
                            static_cast<unsigned long long>(performance.lastInputAgeMs),
                            static_cast<unsigned long long>(performance.lastOutputAgeMs), performance.decoderCpu,
                            performance.conversionCallerCpu, performance.converterCpus[0],
                            performance.converterCpus[1], performance.converterCpus[2]);
                        logLine("video_playout queued=%u uploaded=%llu decoded_replaced=%llu underflow=%llu prime_waits=%llu last_uploaded_sequence=%llu policy=latest_decoded_no_prefill",
                            performance.playoutDepth, static_cast<unsigned long long>(performance.playoutUploaded),
                            static_cast<unsigned long long>(performance.playoutReplaced),
                            static_cast<unsigned long long>(performance.playoutRepeated),
                            static_cast<unsigned long long>(performance.playoutPrimeWaits),
                            static_cast<unsigned long long>(performance.playoutSequence));
                        const uint64_t unsafeDrops = performance.droppedAccessUnits >= performance.catchupDroppedAccessUnits
                            ? performance.droppedAccessUnits - performance.catchupDroppedAccessUnits : 0;
                        if (unsafeDrops > lastVideoDrops) {
                            pendingKeyframe.store(true);
                            logLine("video recovery action=request_keyframe reason=decoder_input_queue_drop");
                        }
                        lastVideoDrops = unsafeDrops;
                    }

                    if (now >= nextHudStatsAt) {
                        nextHudStatsAt = now + 1000;
                        const auto& network = stream.videoStats();
                        const int rttMs = cachedRtt.load();
                        {
                            std::lock_guard<std::mutex> lock(state.mutex);
                            state.streamFrameRate = streamProfile.frameRate;
                state.streamTier = streamProfile.tier;
                            state.streamBitrateCap = streamProfile.maxBitrate;
                            state.streamRembEstimate = network.rembLastRequestedBitrate.load();
                            state.streamReceiveRate = network.receiveRate.load();
                            state.streamRttMs = rttMs;
                            state.streamWidth = performance.width;
                            state.streamHeight = performance.height;
                            state.streamNewVideoFps = performance.decodedFps;
                            state.streamPublishedFps = performance.publishedFps;
                            state.streamQueueDepth = performance.queuedAccessUnits;
                            state.streamQueuePeak = performance.maxQueueDepth;
                            state.streamQueueWaitAvgUs = performance.queueWaitAvgUs;
                            state.streamQueueWaitMaxUs = performance.queueWaitMaxUs;
                            state.streamDecodeAvgUs = performance.decodeAvgUs;
                            state.streamConvertAvgUs = performance.convertAvgUs;
                            ++state.streamStatsGeneration;
                        }
                        const uint64_t steadyNowMs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now().time_since_epoch()).count());
                        const uint64_t lastRembAtMs = network.rembLastRequestAtMs.load();
                        const uint64_t rembAgeMs = lastRembAtMs && steadyNowMs >= lastRembAtMs
                            ? steadyNowMs - lastRembAtMs : 0;
                        logLine("stream_metrics requested_tier=%s actual=%ux%u decoded_fps=%u published_fps=%u scaler=%s aspect=fit queue=%u peak=%u queue_wait_avg_ms=%.3f queue_wait_max_ms=%.3f decode_ms=%.3f convert_ms=%.3f rtt_ms=%d rtp_receive_mbps=%.2f remb_requested_mbps=%.2f remb_cap_mbps=%.2f remb_packets_requested=%llu remb_last_age_ms=%llu receive_above_cap_samples=%llu loss=%llu recovered=%llu",
                            tierLabel(streamProfile.tier), performance.width, performance.height,
                            performance.decodedFps, performance.publishedFps,
                            performance.queuedAccessUnits, performance.maxQueueDepth,
                            performance.queueWaitAvgUs / 1000.0, performance.queueWaitMaxUs / 1000.0,
                            performance.decodeAvgUs / 1000.0, performance.convertAvgUs / 1000.0,
                            rttMs, network.receiveRate.load() / 1000000.0,
                            network.rembLastRequestedBitrate.load() / 1000000.0,
                            streamProfile.maxBitrate / 1000000.0,
                            static_cast<unsigned long long>(network.rembReportsRequested.load()),
                            static_cast<unsigned long long>(rembAgeMs),
                            static_cast<unsigned long long>(network.receiveRateAboveCapSamples.load()),
                            static_cast<unsigned long long>(network.lost.load()),
                            static_cast<unsigned long long>(network.recovered.load()));
                    }

                    if (performance.generation == lastPerformanceGeneration) {
                        workerStep(7);
                sceKernelUsleep(4000);
                        continue;
                    }
                    lastPerformanceGeneration = performance.generation;
                    lastVideoPerformance = performance;
                    ++performanceWindows;
                    const uint32_t skipPermille = performance.decoded
                        ? performance.skipped * 1000u / performance.decoded : 0;
                    const uint64_t combinedUs = static_cast<uint64_t>(performance.decodeAvgUs) +
                                                performance.convertAvgUs;
                    const bool pressure = performance.maxQueueDepth >= 4 || skipPermille >= 200 ||
                                          combinedUs >= 23000;
                    const auto& network = stream.videoStats();
                    logLine("stream_adapt window=%llu tier=%s queue_max=%u skipped=%u/%u window_ms=%u decode_fps=%u published_fps=%u decode_ms=%.2f convert_ms=%.2f cpu_decoder=%d cpu_convert=%d,%d,%d loss=%llu recovered=%llu remb=%u receive_bps=%u rtt_ms=%d",
                            static_cast<unsigned long long>(performance.generation),
                            streamProfile.tier.c_str(), performance.maxQueueDepth,
                            performance.skipped, performance.decoded,
                            performance.windowDurationMs, performance.decodedFps, performance.publishedFps,
                            performance.decodeAvgUs / 1000.0, performance.convertAvgUs / 1000.0,
                            performance.decoderCpu, performance.converterCpus[0],
                            performance.converterCpus[1], performance.converterCpus[2],
                            static_cast<unsigned long long>(network.lost.load()),
                            static_cast<unsigned long long>(network.recovered.load()),
                            network.estimate.load(), network.receiveRate.load(), cachedRtt.load());

                    pressureWindows = pressure ? pressureWindows + 1 : 0;
                    if (pressure) {
                        logLine("stream_adapt action=keep_user_resolution selected=%s pressure_window=%u queue_max=%u skipped=%u/%u decode_ms=%.2f convert_ms=%.2f actual=%ux%u",
                                tierLabel(streamProfile.tier), pressureWindows,
                                performance.maxQueueDepth, performance.skipped, performance.decoded,
                                performance.decodeAvgUs / 1000.0, performance.convertAvgUs / 1000.0,
                                performance.width, performance.height);
                    }
                    profileToSave.lastQueueMax = performance.maxQueueDepth;
                    profileToSave.lastDecodeAvgUs = performance.decodeAvgUs;
                    profileToSave.lastConvertAvgUs = performance.convertAvgUs;
                    profileToSave.lastSkipPermille = skipPermille;
                }
                sceKernelUsleep(4000);
            }
            stopInputPoll.store(true, std::memory_order_release);
            stopGamepadSender.store(true, std::memory_order_release);
            sendStateChanged.notify_all();
            maintenanceStop.store(true);
            stream.close();
            if (maintenance.joinable()) maintenance.join();
            if (gamepadSender.joinable()) gamepadSender.join();
            stopGamepadMetrics.store(true);
            if (gamepadMetrics.joinable()) gamepadMetrics.join();
            if (inputPoller.joinable()) inputPoller.join();
            if (performanceWindows != 0) saveStreamProfile(profileToSave);
            job->video->stop();
            job->gssv->stopSession();
            if (requestedStop) {
                std::lock_guard<std::mutex> lock(state.mutex);
                state.stage = LoginStage::Library;
                state.sessionMessage.clear();
                state.error.clear();
                state.stopSessionRequested = false;
                state.streamTransportLost = false;
                state.streamSettingsOpen = false;
                ++state.generation;
                logLine("cloud stream stopped by user");
            } else {
                logLine("cloud stream ended");
            }
            wipe(job->refreshToken);
            return nullptr;
        }
        sceKernelUsleep(1000000);
    }
    job->gssv->stopSession();
    updateSession(state, LoginStage::Failed, {}, "Timed out while waiting for cloud session provisioning.");
    logLine("session provisioning timed out");
    wipe(job->refreshToken);
    return nullptr;
}

void* signInWorker(void* opaque) {
    auto& state = *static_cast<LoginState*>(opaque);
    xc::auth::MsaConfig config;
    xc::auth::MsaClient msa(std::move(config));
    xc::auth::MsaTokens tokens;
    xc::auth::DeviceCode deviceCode;
    std::string error;

    setStage(state, LoginStage::CheckingSaved);
    std::string cachedRefreshToken;
    int cacheError = 0;
    if (readRefreshToken(cachedRefreshToken, cacheError)) {
        logLine("saved Microsoft account found; refreshing session");
        setStage(state, LoginStage::Refreshing);
        bool networkError = false;
        if (msa.refresh(cachedRefreshToken, tokens, error, {}, &networkError)) {
            logLine("saved Microsoft session refreshed");
            if (!saveRefreshToken(tokens.refreshToken, cacheError))
                logLine("warning: refreshed account cache could not be updated errno=%d", cacheError);
            wipe(cachedRefreshToken);
            finishXboxLogin(state, tokens);
            return nullptr;
        }
        wipe(cachedRefreshToken);
        if (networkError) {
            logLine("saved Microsoft session refresh failed due to network/service error");
            setStage(state, LoginStage::Failed,
                     "Could not reach Microsoft to refresh the saved account. Check the connection and retry.");
            return nullptr;
        }
        logLine("saved Microsoft session was rejected; requesting sign-in again");
        clearRefreshTokenCache();
        error.clear();
    } else {
        if (cacheError != ENOENT)
            logLine("saved account cache could not be read errno=%d", cacheError);
        else
            logLine("no saved Microsoft account; requesting device code");
    }

    setStage(state, LoginStage::Starting);
    if (!msa.startDeviceCode(deviceCode, error)) {
        logLine("msa device-code request failed: %s", error.c_str());
        setStage(state, LoginStage::Failed, error);
        return nullptr;
    }

    {
        std::lock_guard<std::mutex> lock(state.mutex);
        state.verificationUri = deviceCode.verificationUri;
        state.verificationUriComplete = deviceCode.verificationUriComplete;
        state.userCode = deviceCode.userCode;
        state.expiresInSec = deviceCode.expiresInSec;
        state.stage = LoginStage::ShowingCode;
        ++state.generation;
    }
    // The user code and device code are credentials: show the user code on TV,
    // but never write either value to klog or stdout.
    logLine("msa device code issued; expires_sec=%d codes=redacted qr=%s",
            deviceCode.expiresInSec,
            deviceCode.verificationUriComplete.empty() ? "unavailable" : "ready");

    int interval = std::max(1, deviceCode.intervalSec);
    const uint64_t deadlineUs = sceKernelGetProcessTime() +
        static_cast<uint64_t>(std::max(1, deviceCode.expiresInSec)) * 1000000ull;
    while (sceKernelGetProcessTime() < deadlineUs) {
        sceKernelUsleep(static_cast<uint64_t>(interval) * 1000000ull);
        error.clear();
        switch (msa.pollDeviceCode(deviceCode, tokens, error)) {
            case xc::auth::PollResult::Success:
                wipe(deviceCode.deviceCode);
                logLine("msa authorization complete; continuing with xbox live and xcloud");
                if (!tokens.refreshToken.empty()) {
                    if (saveRefreshToken(tokens.refreshToken, cacheError))
                        logLine("Microsoft account refresh token saved in app data");
                    else
                        logLine("warning: could not save Microsoft account refresh token errno=%d", cacheError);
                } else {
                    logLine("warning: Microsoft did not return a refresh token; next launch may request sign-in");
                }
                finishXboxLogin(state, tokens);
                return nullptr;
            case xc::auth::PollResult::Pending:
                break;
            case xc::auth::PollResult::SlowDown:
                interval += 5;
                logLine("msa requested slower polling; interval_sec=%d", interval);
                break;
            case xc::auth::PollResult::Expired:
                wipe(deviceCode.deviceCode);
                setStage(state, LoginStage::Expired, "The sign-in code expired. Restart the app to try again.");
                logLine("msa device code expired");
                return nullptr;
            case xc::auth::PollResult::Denied:
                wipe(deviceCode.deviceCode);
                setStage(state, LoginStage::Denied, "Sign-in was declined on the other device.");
                logLine("msa authorization denied");
                return nullptr;
            case xc::auth::PollResult::Error:
                wipe(deviceCode.deviceCode);
                setStage(state, LoginStage::Failed, error);
                logLine("msa authorization failed: %s", error.c_str());
                return nullptr;
        }
    }
    wipe(deviceCode.deviceCode);
    setStage(state, LoginStage::Expired, "The sign-in code expired. Restart the app to try again.");
    logLine("msa device-code deadline reached");
    return nullptr;
}

struct DrawContext {
    xc::ui::Canvas* canvas;
    xc::ui::Fonts* fonts;
    LoginState* state;
    Controller* controller;
    xc::ui::AppUi* ps5Ui = nullptr;
    xc::ps4::VideoDecoder* video = nullptr;
    LoginStage previousStage = LoginStage::Starting;
    uint32_t previousButtons = 0;
    bool previousL2 = false;
    bool previousR2 = false;
    uint64_t lastGeneration = 0;
    uint64_t lastHudPerformanceGeneration = UINT64_MAX;
    uint64_t lastPresentationGeneration = UINT64_MAX;
    uint64_t lastStreamStatsGeneration = UINT64_MAX;
    uint64_t lastFreezeDiagPresentationGeneration = UINT64_MAX;
    uint64_t repeatEpisodeSequence = 0;
    uint64_t repeatEpisodeStartedAtMs = 0;
    bool repeatEpisodeActive = false;
    bool noUploadEpisodeActive = false;
    bool lastHudTransportLost = false;
    std::chrono::steady_clock::time_point canvasCopyWindowStartedAt{};
    uint64_t canvasCopySumUs = 0;
    uint32_t canvasCopySamples = 0;
    uint32_t canvasCopyMaxUs = 0;
    uint32_t canvasCopyAvgUs = 0;
    size_t ps5UiTitleCount = 0;
    uint64_t ps5UiLibraryRevision = 0;
    struct Repeater {
        bool update(bool held, uint64_t now) {
            if (!held) { next = 0; return false; }
            if (!next) { next = now + 350; return true; }
            if (now >= next) { next = now + 110; return true; }
            return false;
        }
        uint64_t next = 0;
    } up, down, left, right, dpadLeft, dpadRight;
    bool ps5UiReady = false;
    bool ps5UiLoading = false;
    xc::ps4::VideoRepeatWatchdog repeatWatchdog;
};

xc::stream::GamepadFrame readStreamGamepad(const Controller& controller) {
    using B = SDL_GamepadButton;
    using A = SDL_GamepadAxis;
    xc::stream::GamepadFrame frame;
    auto set = [&](bool down, uint16_t bit) {
        if (down) frame.buttons |= bit;
    };
    set(controller.button(B::SDL_GAMEPAD_BUTTON_SOUTH), xc::stream::kA);
    set(controller.button(B::SDL_GAMEPAD_BUTTON_EAST), xc::stream::kB);
    set(controller.button(B::SDL_GAMEPAD_BUTTON_WEST), xc::stream::kX);
    set(controller.button(B::SDL_GAMEPAD_BUTTON_NORTH), xc::stream::kY);
    set(controller.button(B::SDL_GAMEPAD_BUTTON_DPAD_UP), xc::stream::kDPadUp);
    set(controller.button(B::SDL_GAMEPAD_BUTTON_DPAD_DOWN), xc::stream::kDPadDown);
    set(controller.button(B::SDL_GAMEPAD_BUTTON_DPAD_LEFT), xc::stream::kDPadLeft);
    set(controller.button(B::SDL_GAMEPAD_BUTTON_DPAD_RIGHT), xc::stream::kDPadRight);
    set(controller.button(B::SDL_GAMEPAD_BUTTON_LEFT_SHOULDER), xc::stream::kLeftShoulder);
    set(controller.button(B::SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER), xc::stream::kRightShoulder);
    set(controller.button(B::SDL_GAMEPAD_BUTTON_LEFT_STICK), xc::stream::kLeftThumb);
    set(controller.button(B::SDL_GAMEPAD_BUTTON_RIGHT_STICK), xc::stream::kRightThumb);
    set(controller.button(B::SDL_GAMEPAD_BUTTON_START), xc::stream::kMenu);
    // The PS4 touchpad click is reserved for a short Xbox Guide/Nexus pulse.
    // Mapping it to View made Guide inaccessible when the chord was sampled
    // between slow UI refreshes.
    frame.leftX = controller.axis(A::SDL_GAMEPAD_AXIS_LEFTX);
    frame.leftY = -controller.axis(A::SDL_GAMEPAD_AXIS_LEFTY);
    frame.rightX = controller.axis(A::SDL_GAMEPAD_AXIS_RIGHTX);
    frame.rightY = -controller.axis(A::SDL_GAMEPAD_AXIS_RIGHTY);
    frame.leftTrigger = controller.axis(A::SDL_GAMEPAD_AXIS_LEFT_TRIGGER);
    frame.rightTrigger = controller.axis(A::SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
    return frame;
}

xc::ui::NavInput readNavInput(DrawContext& context, uint64_t now) {
    using B = SDL_GamepadButton;
    constexpr uint32_t kCross = 1u << 0, kCircle = 1u << 1, kSquare = 1u << 2;
    constexpr uint32_t kTriangle = 1u << 3, kStart = 1u << 4, kL1 = 1u << 5;
    constexpr uint32_t kR1 = 1u << 6, kR3 = 1u << 7, kTouchpad = 1u << 8;
    uint32_t current = 0;
    auto held = [&](B button, uint32_t bit) {
        const bool down = context.controller->button(button);
        if (down) current |= bit;
        return down;
    };
    const bool cross = held(B::SDL_GAMEPAD_BUTTON_SOUTH, kCross);
    const bool circle = held(B::SDL_GAMEPAD_BUTTON_EAST, kCircle);
    const bool square = held(B::SDL_GAMEPAD_BUTTON_WEST, kSquare);
    const bool triangle = held(B::SDL_GAMEPAD_BUTTON_NORTH, kTriangle);
    const bool start = held(B::SDL_GAMEPAD_BUTTON_START, kStart);
    const bool l1 = held(B::SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, kL1);
    const bool r1 = held(B::SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, kR1);
    const bool r3 = held(B::SDL_GAMEPAD_BUTTON_RIGHT_STICK, kR3);
    const bool touchpad = held(B::SDL_GAMEPAD_BUTTON_TOUCHPAD, kTouchpad);
    auto pressed = [&](bool down, uint32_t bit) { return down && !(context.previousButtons & bit); };

    xc::ui::NavInput nav;
    nav.nowMs = now;
    const bool dpadUp = context.controller->button(B::SDL_GAMEPAD_BUTTON_DPAD_UP);
    const bool dpadDown = context.controller->button(B::SDL_GAMEPAD_BUTTON_DPAD_DOWN);
    const bool dpadLeft = context.controller->button(B::SDL_GAMEPAD_BUTTON_DPAD_LEFT);
    const bool dpadRight = context.controller->button(B::SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
    nav.up = context.up.update(dpadUp || context.controller->axis(SDL_GAMEPAD_AXIS_LEFTY) < -0.6f, now);
    nav.down = context.down.update(dpadDown || context.controller->axis(SDL_GAMEPAD_AXIS_LEFTY) > 0.6f, now);
    nav.left = context.left.update(dpadLeft || context.controller->axis(SDL_GAMEPAD_AXIS_LEFTX) < -0.6f, now);
    nav.right = context.right.update(dpadRight || context.controller->axis(SDL_GAMEPAD_AXIS_LEFTX) > 0.6f, now);
    nav.accept = pressed(cross, kCross);
    nav.back = pressed(circle, kCircle);
    nav.options = pressed(start, kStart);
    nav.square = pressed(square, kSquare);
    nav.triangle = pressed(triangle, kTriangle);
    nav.l1 = pressed(l1, kL1);
    nav.r1 = pressed(r1, kR1);
    nav.r3 = pressed(r3, kR3);
    nav.touchpad = touchpad;
    const float l2 = context.controller->axis(SDL_GAMEPAD_AXIS_LEFT_TRIGGER);
    const float r2 = context.controller->axis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
    nav.l2 = l2 > 0.5f && !context.previousL2;
    nav.r2 = r2 > 0.5f && !context.previousR2;
    nav.l2Analog = l2;
    nav.r2Analog = r2;
    context.previousL2 = l2 > 0.5f;
    context.previousR2 = r2 > 0.5f;
    nav.stickX = context.controller->axis(SDL_GAMEPAD_AXIS_LEFTX);
    nav.stickY = context.controller->axis(SDL_GAMEPAD_AXIS_LEFTY);
    nav.dpadLeft = context.dpadLeft.update(dpadLeft, now);
    nav.dpadRight = context.dpadRight.update(dpadRight, now);
    context.previousButtons = current;
    return nav;
}

    std::vector<xc::ui::GameTile> tiles;
    for (const auto& title : titles) {
        xc::ui::GameTile tile;
        tile.productId = title.productId.empty() ? title.titleId : title.productId;
        tile.titleId = title.titleId;
        tile.name = title.name.empty() ? title.titleId : title.name;
        tile.tileUrl = title.imageUrl;
        tile.heroUrl = title.imageUrl;
        tile.playable = true;
        tile.freeInStore = title.isFreeInStore;
        tiles.push_back(std::move(tile));
    }
    return tiles;
}

void loadPs5LibraryUi(xc::ui::AppUi& appUi, const LoginSnapshot& state) {
    appUi.setPrefs({}, xc::ui::LibrarySort::Recent);
    appUi.setLoading(state.libraryLoading, state.libraryLoaded, 0);
    appUi.setProfile(state.gamertag, {});
    appUi.showTab(xc::ui::Tab::Library);
}

void updatePs5LibraryUi(xc::ui::AppUi& appUi, const LoginSnapshot& state) {
    appUi.setLoading(state.libraryLoading, state.libraryLoaded, 0);
}

char titleInitial(const xc::xcloud::Title& title) {
    const std::string& name = title.name.empty() ? title.titleId : title.name;
    for (unsigned char ch : name) {
        if (std::isalnum(ch)) return static_cast<char>(std::toupper(ch));
    }
    return '#';
}

size_t jumpToAdjacentInitial(const std::vector<xc::xcloud::Title>& titles, size_t current, int direction) {
    if (titles.empty()) return 0;
    const char initial = titleInitial(titles[current]);
    for (size_t step = 1; step < titles.size(); ++step) {
        const size_t candidate = direction > 0
            ? (current + step) % titles.size()
            : (current + titles.size() - (step % titles.size())) % titles.size();
        if (titleInitial(titles[candidate]) != initial) return candidate;
    }
    return current;
}

    using namespace xc::ui;
    static const std::vector<xc::xcloud::Title> kNoTitles;
    const bool inLibrary = s.stage == LoginStage::Library || s.stage == LoginStage::SessionStarting ||
                           s.stage == LoginStage::SessionProvisioning || s.stage == LoginStage::SessionReady;
    fonts.semibold.draw(canvas, inLibrary ? "Choose a cloud game" : "Sign in with your Microsoft account",
                        150, 415, 34, rgba(238, 238, 238));

    switch (s.stage) {
        case LoginStage::Starting:
            fonts.regular.draw(canvas, "Requesting a secure sign-in code...", 150, 510, 27, rgba(190, 210, 190));
            break;
        case LoginStage::CheckingSaved:
            fonts.regular.draw(canvas, "Checking for a saved Microsoft account...", 150, 510, 27,
                               rgba(190, 210, 190));
            break;
        case LoginStage::Refreshing:
            fonts.regular.draw(canvas, "Refreshing your saved Microsoft sign-in...", 150, 510, 27,
                               rgba(190, 210, 190));
            break;
        case LoginStage::ShowingCode:
            fonts.regular.draw(canvas, "On your phone or computer, open:", 150, 500, 26, rgba(205, 215, 205));
            fonts.semibold.draw(canvas, s.verificationUri.c_str(), 150, 550, 34, rgba(140, 235, 145));
            fonts.regular.draw(canvas, "Enter this code:", 150, 640, 25, rgba(205, 215, 205));
            fonts.bold.draw(canvas, s.userCode.c_str(), 150, 690, 66, rgba(255, 255, 255));
            fonts.regular.draw(canvas, "Waiting for approval. Keep this screen open.", 150, 785, 24, rgba(190, 200, 190));
            if (!s.verificationUriComplete.empty()) {
                constexpr int maxVersion = 10;
                constexpr int quietZone = 4;
                uint8_t qr[qrcodegen_BUFFER_LEN_FOR_VERSION(maxVersion)]{};
                uint8_t scratch[qrcodegen_BUFFER_LEN_FOR_VERSION(maxVersion)]{};
                if (qrcodegen_encodeText(s.verificationUriComplete.c_str(), scratch, qr,
                                         qrcodegen_Ecc_MEDIUM, qrcodegen_VERSION_MIN,
                                         maxVersion, qrcodegen_Mask_AUTO, true)) {
                    const int qrSize = qrcodegen_getSize(qr);
                    const int modules = qrSize + quietZone * 2;
                    const int scale = 7; // integer scale keeps modules camera-readable at 1080p
                    const int side = modules * scale;
                    const int x0 = 1425;
                    const int y0 = 385;
                    canvas.fillRect({x0, y0, side, side}, rgba(255, 255, 255));
                    for (int y = 0; y < qrSize; ++y) {
                        int run = 0;
                        for (int x = 0; x <= qrSize; ++x) {
                            if (x < qrSize && qrcodegen_getModule(qr, x, y)) {
                                ++run;
                                continue;
                            }
                            if (run) {
                                canvas.fillRect({x0 + (quietZone + x - run) * scale,
                                                 y0 + (quietZone + y) * scale,
                                                 run * scale, scale}, rgba(8, 12, 9));
                                run = 0;
                            }
                        }
                    }
                    fonts.regular.draw(canvas, "Scan with your phone", x0, y0 + side + 22,
                                       23, rgba(210, 220, 210));
                }
            }
            break;
        case LoginStage::XboxLive:
                               rgba(205, 215, 205));
            break;
        case LoginStage::Xcloud:
                                rgba(140, 235, 145));
            fonts.regular.draw(canvas, "Checking xCloud access and available region.", 150, 590, 25,
                               rgba(205, 215, 205));
            break;
        case LoginStage::Library: {
            const std::string details = "Account library  |  " + s.market + "  |  " + s.region +
                "  |  " + std::to_string(titles.size()) + (s.libraryLoading ? "+ games (loading)" : " games");
            fonts.regular.draw(canvas, details.c_str(), 150, 470, 21, rgba(190, 205, 190));
            canvas.fillRect({145, 515, 850, 365}, rgba(12, 20, 14, 225), 16);
            canvas.strokeRect({145, 515, 850, 365}, rgba(71, 118, 73, 255), 2, 16);
            if (titles.empty()) {
                                    180, 555, 26, rgba(235, 225, 210));
            } else {
                constexpr size_t visibleRows = 6;
                const size_t first = s.selectedTitle >= visibleRows ? s.selectedTitle - visibleRows + 1 : 0;
                const size_t last = std::min(titles.size(), first + visibleRows);
                for (size_t i = first; i < last; ++i) {
                    const int y = 535 + static_cast<int>(i - first) * 59;
                    if (i == s.selectedTitle)
                        canvas.fillRect({160, y, 820, 52}, rgba(30, 89, 37, 245), 9);
                    const auto& title = titles[i];
                    const std::string name = title.name.empty() ? title.titleId : title.name;
                    fonts.semibold.draw(canvas, name.c_str(), 180, y + 8, 23,
                                        i == s.selectedTitle ? rgba(255, 255, 255) : rgba(215, 225, 215));
                }
                const auto& selected = titles[std::min(s.selectedTitle, titles.size() - 1)];
                const std::string name = selected.name.empty() ? selected.titleId : selected.name;
                const std::string letter = std::string("Letter: ") + titleInitial(selected) +
                    "    " + std::to_string(s.selectedTitle + 1) + "/" + std::to_string(titles.size());
                fonts.semibold.draw(canvas, letter.c_str(), 1060, 500, 23, rgba(145, 225, 150));
                fonts.bold.draw(canvas, name.c_str(), 1060, 545, 34, rgba(245, 255, 245));
                fonts.regular.draw(canvas, selected.hasEntitlement ? "Available on this account" :
                                   (selected.viaSubscription ? "Game Pass title" : "Account/store title"),
                                   1060, 605, 23, rgba(145, 225, 150));
                fonts.regular.draw(canvas, "Press X to request a cloud session.", 1060, 675, 22,
                                   rgba(205, 215, 205));
            }
            break;
        }
        case LoginStage::SessionStarting:
        case LoginStage::SessionProvisioning:
        case LoginStage::SessionReady: {
            if (!titles.empty()) {
                const auto& selected = titles[std::min(s.selectedTitle, titles.size() - 1)];
                const std::string name = selected.name.empty() ? selected.titleId : selected.name;
                fonts.regular.draw(canvas, name.c_str(), 150, 590, 27, rgba(235, 240, 235));
            }
            fonts.regular.draw(canvas, s.sessionMessage.c_str(), 150, 650, 23, rgba(205, 215, 205));
            if (s.stage == LoginStage::SessionProvisioning)
                fonts.regular.draw(canvas, "Detailed transport steps are also recorded in the PS4 klog.",
                                   150, 710, 21, rgba(165, 190, 165));
            if (s.stage == LoginStage::SessionReady)
                fonts.regular.draw(canvas, "Press Circle to return to the library.", 150, 715, 22,
                                   rgba(190, 200, 190));
            break;
        }
        case LoginStage::Streaming:
                                150, 535, 34, s.streamTransportLost ? rgba(255, 205, 116) : rgba(140, 235, 145));
            fonts.regular.draw(canvas, s.sessionMessage.c_str(), 150, 590, 23, rgba(205, 215, 205));
            fonts.regular.draw(canvas, s.streamTransportLost
                                   ? "The last decoded picture stays visible until you cancel."
                                   : "Waiting for the first H.264 picture...",
                               150, 650, 22,
                               rgba(190, 200, 190));
            break;
        case LoginStage::Expired:
        case LoginStage::Denied:
        case LoginStage::Failed:
            fonts.semibold.draw(canvas, s.generation && !titles.empty() ? "Cloud session did not complete" :
                                "Sign-in did not complete", 150, 535, 34, rgba(255, 190, 120));
            fonts.regular.draw(canvas, s.error.c_str(), 150, 590, 23, rgba(230, 220, 205));
            break;
    }

    canvas.fillRect({150, 900, 760, 72}, rgba(24, 32, 25, 230), 14);
    canvas.strokeRect({150, 900, 760, 72}, rgba(71, 118, 73, 255), 2, 14);
    const char* footer = s.stage == LoginStage::Library ? "Up/Down: browse    L1/R1: previous/next letter    X: request session" :
                         s.stage == LoginStage::SessionReady ? "Circle: return to library" :
                         s.stage == LoginStage::Failed && !titles.empty() ? "Circle: return to library" :
                         s.stage == LoginStage::SessionProvisioning || s.stage == LoginStage::SessionStarting
                             ? "Keep the app open while the cloud session is prepared" :
    fonts.regular.draw(canvas, footer, 180, 923, 21,
                       rgba(190, 205, 190));
}

}

bool beginCloudSession(LoginState& state, const xc::ui::GameTile& game,
                      xc::ps4::VideoDecoder* video, Controller* controller) {
    std::unique_lock<std::mutex> lock(state.mutex);
    if (state.stage != LoginStage::Library || !state.gssv || state.refreshToken.empty() || game.titleId.empty())
        return false;
    auto* job = new SessionJob{&state, state.gssv, game.titleId, state.refreshToken, game.name, video, controller};
    state.stage = LoginStage::SessionStarting;
    state.stopSessionRequested = false;
    state.sessionMessage = "Contacting the cloud session service...";
    state.error.clear();
            state.selectedTitle = i;
            break;
        }
    }
    ++state.generation;
    lock.unlock();
    const bool backgroundReleased = xc::net::pauseBackgroundHttp(true);
    if (!backgroundReleased) {
        lock.lock();
        state.stage = LoginStage::Library;
        state.error = "Background catalog/artwork HTTP did not release the shared HTTP context.";
        ++state.generation;
        lock.unlock();
        xc::net::pauseBackgroundHttp(false);
        delete job;
        logLine("cloud session blocked reason=background_http_lease_timeout");
        return false;
    }
    pthread_t worker{};
    const int rc = pthread_create(&worker, nullptr, sessionWorker, job);
    if (rc == 0) {
        pthread_detach(worker);
        logLine("cloud session worker started title=%s", game.titleId.c_str());
        return true;
    }
    delete job;
    lock.lock();
    state.stage = LoginStage::Library;
    state.error = "Could not start the cloud session worker.";
    ++state.generation;
    lock.unlock();
    xc::net::pauseBackgroundHttp(false);
    logLine("cloud session worker failed pthread_create=%d", rc);
    return false;
}

xc::ps4::CanvasFrameUpdateResult refreshCanvas(uint32_t*, void* opaque,
                                               const xc::ps4::PresentationStats& presentation,
                                               bool gpuVideoEnabled) {
    auto& context = *static_cast<DrawContext*>(opaque);
    if (context.video) context.video->setGpuVideoEnabled(gpuVideoEnabled);
    {
        std::lock_guard<std::mutex> lock(context.state->mutex);
        context.state->gpuVideoActive = gpuVideoEnabled;
    }
    const uint64_t now = sceKernelGetProcessTime() / 1000u;
    static bool settingsWasOpen = false;
    static bool fsrWasOpen = false;
    static uint64_t settingsAnimAt = 0;
    static uint64_t pageAnimAt = 0;
    if (state.streamSettingsOpen != settingsWasOpen) {
        settingsWasOpen = state.streamSettingsOpen;
        settingsAnimAt = now;
    }
    if (state.streamFsrSettingsOpen != fsrWasOpen) {
        fsrWasOpen = state.streamFsrSettingsOpen;
        pageAnimAt = now;
    }
    const auto animProgress = [now](uint64_t start) {
    };
    const float settingsEase = animProgress(settingsAnimAt);
    const bool settingsShouldDraw = state.streamSettingsOpen ||
        (!state.streamSettingsOpen && settingsAnimationActive && settingsAnimAt != 0);
    const float settingsVisibility = state.streamSettingsOpen ? settingsEase : 1.0f - settingsEase;
    const float pageEase = animProgress(pageAnimAt);
    const float settingsPageProgress = pageAnimationActive
        ? (state.streamFsrSettingsOpen ? pageEase : 1.0f - pageEase)
        : (state.streamFsrSettingsOpen ? 1.0f : 0.0f);
    if (state.stage != LoginStage::Streaming) {
        context.controller->pump();
    }
    const auto nav = state.stage == LoginStage::Streaming
        ? xc::ui::NavInput{} : readNavInput(context, now);

    context.canvas->enableGpuUi(gpuVideoEnabled && state.stage == LoginStage::Streaming &&
                                context.video && context.video->hasFrame());
    if (context.video) { context.video->setPresentationFps(state.streamFrameRate); context.video->setPlayoutDelayFrames(state.streamFrameWait); }
    const bool enteringStream = context.previousStage != LoginStage::Streaming &&
                                state.stage == LoginStage::Streaming;
        context.ps5Ui->showHome();
        context.ps5Ui->showTab(xc::ui::Tab::Library);
        context.ps5Ui->invalidate();
        context.ps5UiReady = false;
    }
    context.previousStage = state.stage;

    if (state.stage == LoginStage::Streaming) {
        if (context.video && presentation.generation != context.lastFreezeDiagPresentationGeneration) {
            context.lastFreezeDiagPresentationGeneration = presentation.generation;
            const auto video = context.video->performanceSnapshot();
            if (presentation.scanoutValid && presentation.newVideoFps == 0 && video.lastOutputAgeMs >= 750) {
                if (!context.noUploadEpisodeActive) {
                    context.noUploadEpisodeActive = true;
                    logLine("video_freeze_diagnosis event=no_new_video_upload scanout_fps=%u uploaded_video_fps=%u input_age_ms=%llu output_age_ms=%llu received_aus=%llu decoded_total=%llu published_total=%llu queue=%u playout_depth=%u rtp_receive_mbps=%.3f remb_advertised_mbps=%.3f remb_cap_mbps=%.3f rtt_ms=%d action=observe_no_decoder_reset",
                        presentation.scanoutFps, presentation.newVideoFps,
                        static_cast<unsigned long long>(video.lastInputAgeMs),
                        static_cast<unsigned long long>(video.lastOutputAgeMs),
                        static_cast<unsigned long long>(video.receivedAccessUnits),
                        static_cast<unsigned long long>(video.totalDecoded),
                        static_cast<unsigned long long>(video.totalPublished), video.queuedAccessUnits,
                        video.playoutDepth, state.streamReceiveRate / 1000000.0,
                        state.streamRembEstimate / 1000000.0, state.streamBitrateCap / 1000000.0,
                        state.streamRttMs);
                }
            } else if (context.noUploadEpisodeActive &&
                       (presentation.newVideoFps > 0 || video.lastOutputAgeMs < 400)) {
                context.noUploadEpisodeActive = false;
                logLine("video_freeze_diagnosis event=new_video_upload_resumed output_age_ms=%llu uploaded_video_fps=%u confirmed_unique_video_frames=%llu",
                    static_cast<unsigned long long>(video.lastOutputAgeMs), presentation.newVideoFps,
                    static_cast<unsigned long long>(presentation.confirmedUniqueVideoFrames));
            }
        }
        if (presentation.confirmedVideoKnown && presentation.repeatedVideoFlips >= 6) {
            if (!context.repeatEpisodeActive || context.repeatEpisodeSequence != presentation.confirmedVideoSequence) {
                context.repeatEpisodeActive = true;
                context.repeatEpisodeSequence = presentation.confirmedVideoSequence;
                context.repeatEpisodeStartedAtMs = now;
                const auto video = context.video ? context.video->performanceSnapshot() : xc::ps4::VideoPerformanceSnapshot{};
                logLine("video_freeze_diagnosis event=confirmed_frame_repeat sequence=%llu same_frame_flips=%u scanout_fps=%u uploaded_video_fps=%u input_age_ms=%llu output_age_ms=%llu received_aus=%llu decoded_total=%llu published_total=%llu queue=%u playout_depth=%u unique_scanout_frames=%llu repeated_scanouts_total=%llu rtp_receive_mbps=%.3f remb_advertised_mbps=%.3f remb_cap_mbps=%.3f rtt_ms=%d action=observe_no_decoder_reset",
                    static_cast<unsigned long long>(presentation.confirmedVideoSequence), presentation.repeatedVideoFlips,
                    presentation.scanoutFps, presentation.newVideoFps,
                    static_cast<unsigned long long>(video.lastInputAgeMs),
                    static_cast<unsigned long long>(video.lastOutputAgeMs),
                    static_cast<unsigned long long>(video.receivedAccessUnits),
                    static_cast<unsigned long long>(video.totalDecoded),
                    static_cast<unsigned long long>(video.totalPublished), video.queuedAccessUnits,
                    video.playoutDepth, static_cast<unsigned long long>(presentation.confirmedUniqueVideoFrames),
                    static_cast<unsigned long long>(presentation.confirmedRepeatedVideoFlips),
                    state.streamReceiveRate / 1000000.0, state.streamRembEstimate / 1000000.0,
                    state.streamBitrateCap / 1000000.0, state.streamRttMs);
            }
        } else if (context.repeatEpisodeActive && presentation.confirmedVideoKnown &&
                   presentation.confirmedVideoSequence != context.repeatEpisodeSequence) {
            logLine("video_freeze_diagnosis event=confirmed_frame_changed previous_sequence=%llu next_sequence=%llu observed_repeat_ms=%llu unique_scanout_frames=%llu repeated_scanouts_total=%llu",
                static_cast<unsigned long long>(context.repeatEpisodeSequence),
                static_cast<unsigned long long>(presentation.confirmedVideoSequence),
                static_cast<unsigned long long>(now - context.repeatEpisodeStartedAtMs),
                static_cast<unsigned long long>(presentation.confirmedUniqueVideoFrames),
                static_cast<unsigned long long>(presentation.confirmedRepeatedVideoFlips));
            context.repeatEpisodeActive = false;
        }
        const auto resetDecision = context.repeatWatchdog.evaluate(state.streamAutoReset,
                presentation.confirmedVideoKnown, presentation.confirmedVideoSequence,
                presentation.repeatedVideoFlips, state.streamFrameRate, state.streamNewVideoFps, now);
        if (context.video && resetDecision.reset) {
            context.video->requestReset(xc::ps4::VideoResetRequestReason::AdaptiveWatchdog);
            logLine("repeat_watchdog mode=adaptive enabled=1 confirmed_sequence=%llu same_video_flips=%u threshold=%u stalled_ms=%llu score=%.3f decoded_fps=%u action=request_decoder_reset_wait_idr cooldown_ms=2000 once_per_stuck_sequence=1",
                    static_cast<unsigned long long>(presentation.confirmedVideoSequence), presentation.repeatedVideoFlips,
                    resetDecision.threshold, static_cast<unsigned long long>(resetDecision.stalledMs),
                    resetDecision.score, state.streamNewVideoFps);
        }
        const uint64_t stepAt = context.state->streamWorkerStepAtMs.load(std::memory_order_relaxed);
        const uint32_t step = context.state->streamWorkerStep.load(std::memory_order_acquire);
        const uint64_t stepAge = stepAt && now >= stepAt ? now - stepAt : 0;
        static const char* stepNames[] = {"idle", "state_lock", "pad_pump", "input", "gamepad_send", "stream_tick_keepalive", "performance", "sleep"};
        if (presentation.generation != context.lastPresentationGeneration)
            logLine("stream_worker step=%s age_ms=%llu stalled=%u",
                    step < 8 ? stepNames[step] : "unknown", static_cast<unsigned long long>(stepAge), stepAge > 1000 ? 1u : 0u);
        if (gpuVideoEnabled && enteringStream) context.canvas->clearTransparent();
        const auto copyStart = std::chrono::steady_clock::now();
        const bool newFrame = !gpuVideoEnabled && context.video && context.video->copyLatestToCanvas(
        const uint32_t copyUs = static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - copyStart).count());
        const auto nowSteady = std::chrono::steady_clock::now();
        if (context.canvasCopyWindowStartedAt.time_since_epoch().count() == 0)
            context.canvasCopyWindowStartedAt = nowSteady;
        if (newFrame) {
            context.canvasCopySumUs += copyUs;
            ++context.canvasCopySamples;
            context.canvasCopyMaxUs = std::max(context.canvasCopyMaxUs, copyUs);
        }
        if (std::chrono::duration_cast<std::chrono::milliseconds>(
                nowSteady - context.canvasCopyWindowStartedAt).count() >= 2000) {
            if (context.canvasCopySamples) {
                context.canvasCopyAvgUs = static_cast<uint32_t>(context.canvasCopySumUs / context.canvasCopySamples);
                logLine("canvas_copy_timing window_ms=2000 new_frames=%u avg_ms=%.3f max_ms=%.3f",
                        context.canvasCopySamples, context.canvasCopyAvgUs / 1000.0,
                        context.canvasCopyMaxUs / 1000.0);
            }
            context.canvasCopyWindowStartedAt = nowSteady;
            context.canvasCopySumUs = 0;
            context.canvasCopySamples = 0;
            context.canvasCopyMaxUs = 0;
        }
        if (context.video && context.video->hasFrame()) {
            const auto performance = context.video->performanceSnapshot();
            const bool videoStalled = performance.lastOutputAgeMs > 750;
                                    state.generation != context.lastGeneration ||
            if (gpuVideoEnabled) context.canvas->clearTransparent();
            char title[128];
            std::snprintf(title, sizeof(title), "%s  |  requested %s  |  %s",
                tierLabel(state.streamTier),
                performance.width ? "VIDEO ACTIVE" : "VIDEO STARTING");
            char stats[192];
            // Show measured rates without clamping; bursts can exceed 60 in a short window.
            std::snprintf(stats, sizeof(stats), "Decode %u/s | Published %u/s | Uploaded %u/s | Unique scanout %u/s | Flips %u/s",
                state.streamNewVideoFps, state.streamPublishedFps, presentation.newVideoFps,
                presentation.confirmedUniqueVideoFps, presentation.scanoutFps);
            char detail[192];
            char rtt[32];
            if (state.streamRttMs >= 0) std::snprintf(rtt, sizeof(rtt), "%d ms", state.streamRttMs);
            else std::snprintf(rtt, sizeof(rtt), "unknown");
            std::snprintf(detail, sizeof(detail), "Received %.2f Mbps | REMB requested %.2f Mbps | Cap %.0f Mbps | RTT %s",
                state.streamReceiveRate / 1000000.0, state.streamRembEstimate / 1000000.0,
                state.streamBitrateCap / 1000000.0, rtt);
            char pipeline[224];
            std::snprintf(pipeline, sizeof(pipeline), "Queue %u / peak %u   Queue wait %.2f / %.2f ms   Decode %.2f ms   Convert %.2f ms",
                state.streamQueueDepth, state.streamQueuePeak,
                state.streamQueueWaitAvgUs / 1000.0, state.streamQueueWaitMaxUs / 1000.0,
                state.streamDecodeAvgUs / 1000.0, state.streamConvertAvgUs / 1000.0);
            char presentationLine[224];
            std::snprintf(presentationLine, sizeof(presentationLine),
                "Canvas copy %.2f ms   Update %.2f   Upload %.2f   Acquire %.2f   Encode+submit %.2f ms",
                context.canvasCopyAvgUs / 1000.0, presentation.updateAvgUs / 1000.0,
                presentation.uploadAvgUs / 1000.0, presentation.acquireAvgUs / 1000.0,
                presentation.encodeSubmitAvgUs / 1000.0);
            char waitLine[192];
            std::snprintf(waitLine, sizeof(waitLine), "VideoOut flips %s%u/s (includes repeats)   Cadence %.2f / %.2f ms avg/max",
                presentation.scanoutValid ? "" : "unavailable ", presentation.scanoutFps,
                presentation.frameIntervalAvgUs / 1000.0, presentation.frameIntervalMaxUs / 1000.0);
            char cpu[192];
            std::snprintf(cpu, sizeof(cpu), "Session worker: %s age %llu ms | decoder CPU %d | input warning above 1000 ms",
                step < 8 ? stepNames[step] : "unknown", static_cast<unsigned long long>(stepAge), performance.decoderCpu);
            const char* controls = state.streamTransportLost
            context.lastHudPerformanceGeneration = performance.generation;
            context.lastPresentationGeneration = presentation.generation;
            context.lastStreamStatsGeneration = state.streamStatsGeneration;
            context.lastHudTransportLost = state.streamTransportLost;
            context.lastGeneration = state.generation;
        }
            context.lastGeneration = state.generation;
        }
    }

    if (state.stage == LoginStage::Library && context.ps5Ui) {
        if (!context.ps5UiReady) {
            loadPs5LibraryUi(*context.ps5Ui, state);
            context.ps5UiReady = true;
            context.ps5UiLoading = state.libraryLoading;
        } else {
            if (titleCount != context.ps5UiTitleCount ||
                updatePs5LibraryUi(*context.ps5Ui, state);
                context.ps5UiTitleCount = titleCount;
                context.ps5UiLoading = state.libraryLoading;
                        state.libraryLoading ? 1 : 0);
            } else if (context.ps5UiLoading != state.libraryLoading) {
                context.ps5Ui->setLoading(state.libraryLoading, state.libraryLoaded, 0);
                context.ps5UiLoading = state.libraryLoading;
            }
        }
        const auto event = context.ps5Ui->handle(nav);
        if (event.action == xc::ui::Action::Play) {
            beginCloudSession(*context.state, event.game, context.video, context.controller);
        }
        if (context.ps5Ui->needsRedraw(now)) {
            context.ps5Ui->render(*context.canvas, now);
            context.lastGeneration = state.generation;
            return {true, false};
        }
        return {false, false};
    }

    if (state.stage == LoginStage::SessionReady ||
        if (nav.back) {
            std::lock_guard<std::mutex> lock(context.state->mutex);
            if (context.state->stage == LoginStage::SessionReady ||
                context.state->stage = LoginStage::Library;
                context.state->error.clear();
                context.state->sessionMessage.clear();
                ++context.state->generation;
            }
            if (context.ps5Ui) {
                context.ps5Ui->showHome();
                context.ps5Ui->showTab(xc::ui::Tab::Library);
            }
            return {true, false};
        }
    }

    if (state.generation == context.lastGeneration) return {};
    context.lastGeneration = state.generation;
    logLine("login canvas state changed stage=%d qr=%s manual_code=visible",
            static_cast<int>(state.stage),
            state.verificationUriComplete.empty() ? "unavailable" : "ready");
    return {true, false};
}

bool uploadLatestNv12(void* opaque, uint8_t* destination, size_t capacity,
    auto& context = *static_cast<DrawContext*>(opaque);
    uint32_t frameWidth = 0, frameHeight = 0;
    *width = frameWidth;
    *height = frameHeight;
    return true;
}
}

int main() {
    struct stat logDirectoryStat{};
    if (::stat(kLogDirectory, &logDirectoryStat) != 0 && ::mkdir(kLogDirectory, 0777) != 0 && errno != EEXIST) {
        char message[160];
        std::snprintf(message, sizeof(message), "cannot create log directory %s errno=%d\n", kLogDirectory, errno);
        sceKernelDebugOutText(0, message);
    }
    if (::stat(kLogDirectory, &logDirectoryStat) == 0 && S_ISDIR(logDirectoryStat.st_mode)) {
        xc::log::setFile(kAppLogPath);
        const bool async = xc::log::startAsync([](const char* line) { sceKernelDebugOutText(0, line); });
        if (!async) sceKernelDebugOutText(0, "async diagnostic logger creation failed; synchronous fallback active\n");

    } else {
        sceKernelDebugOutText(0, "log directory /data/PSBOX is unavailable\n");
    }
    logLine("log_file=%s", kAppLogPath);
    xc::ps4::setVideoDiagnosticSink([](const char* message) {
        logLine("[XCLOUD-PS4-VIDEO] %s", message);
    });

    if (setenv("ORBIS_SUBMIT_DONE", "1", 1) != 0)
        logLine("diagnostic setup failed: setenv ORBIS_SUBMIT_DONE");
    else
        logLine("diagnostic mode: ORBIS_SUBMIT_DONE=1 before Vulkan initialization");
    logLine("startup begin; mode=msa-cache+xbl+xcloud+account-library+webrtc+h264; offering=xgpuweb");

    const auto network = xc::ps4::initializeNetworkAndCheckInternet();
    logLine("network stack=%s ip=%s internet=%s http_status=%d failed_step=%s error=0x%08x",
            network.stackReady ? "ready" : "failed", network.ipAddress,
            network.internetReachable ? "reachable" : "not-confirmed", network.httpStatus,
            network.failedStep, static_cast<unsigned>(network.errorCode));

    xc::ui::Fonts fonts;
    if (!fonts.load("/app0/assets/fonts")) {
        logLine("font load failed; refusing to pretend the UI is ready");
        for (;;) sceKernelUsleep(1000000);
    }

    LoginState state;
    xc::ps4::VideoDecoder videoDecoder;
    Controller controller;
    const bool controllerReady = controller.initialize();
    if (!controllerReady) logLine("controller navigation unavailable; sign-in and network UI remain active");
    pthread_t worker{};
    const int threadRc = pthread_create(&worker, nullptr, signInWorker, &state);
    if (threadRc != 0) {
        logLine("could not start MSA worker; pthread_create=%d", threadRc);
        setStage(state, LoginStage::Failed, "Could not start the sign-in network worker.");
    } else {
        logLine("MSA sign-in worker started");
    }

    if (mkdir("/data/XCHM00001", 0700) != 0 && errno != EEXIST)
        logLine("image cache directory setup failed errno=%d", errno);
    xc::ui::Canvas canvas(1920, 1080);
    xc::ui::AppUi* appUiPtr = nullptr;
    xc::ui::ImageCache images([&] { if (appUiPtr) appUiPtr->invalidate(); },
    xc::ui::AppUi appUi(fonts, images);
    appUiPtr = &appUi;
    DrawContext draw{&canvas, &fonts, &state, &controller, &appUi, &videoDecoder};
    drawLoginScreen(canvas, fonts, initialState);
    draw.lastGeneration = initialState.generation;
    logLine("canvas ready 1920x1080 RGBA; entering Mesa Orbis WSI/VideoOut");
    xc::ps4::presentCanvas(canvas.data(), canvas.width(), canvas.height(), refreshCanvas,
                           uploadLatestNv12, &draw);
}
