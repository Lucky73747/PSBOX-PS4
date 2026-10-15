// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 RafaelNGP
// Microsoft account (MSA) OAuth via login.live.com: device-code sign-in and
// refresh. The device-code flow fits a console: the TV shows a short code
// (and a QR code) and the user signs in with their real account on a phone.
#pragma once

#include <cstdint>
#include <string>

namespace xc::auth {

struct MsaConfig {
    // Defaults match the modern consumers device-code flow used by Green-NX.
    std::string clientId = "1f907974-e22b-4810-a9de-d9647380c97e";
    std::string scope = "xboxlive.signin openid profile offline_access";
    std::string deviceCodeUrl = "https://login.microsoftonline.com/consumers/oauth2/v2.0/devicecode";
    std::string tokenUrl = "https://login.microsoftonline.com/consumers/oauth2/v2.0/token";
    bool includeLegacyResponseType = false;
};

struct DeviceCode {
    std::string userCode;
    std::string deviceCode;
    std::string verificationUri;  // e.g. https://www.microsoft.com/link
    std::string verificationUriComplete;
    int intervalSec = 5;
    int expiresInSec = 900;
};

struct MsaTokens {
    std::string accessToken;
    std::string refreshToken;
    std::string userId;
    int64_t expiresAt = 0;  // unix seconds
};

enum class PollResult { Pending, SlowDown, Success, Expired, Denied, Error };

class MsaClient {
public:
    explicit MsaClient(MsaConfig cfg = {}) : cfg_(std::move(cfg)) {}

    bool startDeviceCode(DeviceCode& out, std::string& err) const;
    PollResult pollDeviceCode(const DeviceCode& dc, MsaTokens& out, std::string& err) const;
    // `scope` empty = the configured sign-in scope. `networkError` (optional)
    // tells a transport failure apart from a rejected token.
    bool refresh(const std::string& refreshToken, MsaTokens& out, std::string& err,
                 const std::string& scope = {}, bool* networkError = nullptr) const;

    const MsaConfig& config() const { return cfg_; }

private:
    MsaConfig cfg_;
};

int64_t unixNow();

}  // namespace xc::auth
