// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <string>

namespace xc::ps4 {

struct NetworkStatus {
    bool stackReady = false;
    bool hasIpAddress = false;
    bool internetReachable = false;
    char ipAddress[16] = "unavailable";
    char failedStep[48] = "none";
    int32_t errorCode = 0;
    int32_t httpStatus = 0;
};

// Initializes the Orbis network modules, records the configured IPv4 address,
// then performs a small unauthenticated HTTP connectivity check.
NetworkStatus initializeNetworkAndCheckInternet();

// Handle of the initialized SceHttp context. The PS4 implementation of
// xc::net::perform uses this context for the existing MSA device-code flow.
int32_t httpContextId();

} // namespace xc::ps4
