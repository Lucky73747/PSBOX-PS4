// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "auth/xbox_live.h"
#include "xcloud/gssv.h"

#include <memory>
#include <functional>
#include <string>
#include <vector>

namespace xc::ps4 {

struct XcloudLoginInfo {
    std::string market;
    std::string region;
    size_t regionCount = 0;
    std::vector<xc::xcloud::Title> titles;
    // Small PS4 display subset, maintained incrementally while pages arrive.
    std::vector<xc::xcloud::Title> displayTitles;
    std::shared_ptr<xc::xcloud::GssvClient> client;
    std::string catalogWarning;
};

// Performs the xgpuweb GSSV login and returns display-safe metadata only.
// The GSSV token is validated but never returned or persisted.
using LibraryUpdateCallback = std::function<void(const XcloudLoginInfo&, bool complete)>;
bool loginXcloud(const xc::auth::XblToken& gssvXsts, XcloudLoginInfo& out, std::string& error,
                 const LibraryUpdateCallback& onUpdate = {});

}  // namespace xc::ps4
