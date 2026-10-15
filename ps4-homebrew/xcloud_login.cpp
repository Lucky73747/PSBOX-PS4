// SPDX-License-Identifier: GPL-3.0-or-later
#include "xcloud_login.h"

#include "xcloud/catalog.h"

#include <algorithm>
#include <cctype>
#include <iterator>
#include <set>
#include <vector>

namespace xc::ps4 {
bool loginXcloud(const xc::auth::XblToken& gssvXsts, XcloudLoginInfo& out, std::string& error,
                 const LibraryUpdateCallback& onUpdate) {
    out.client = std::make_shared<xc::xcloud::GssvClient>("xgpuweb");
    if (!out.client->login(gssvXsts, error)) {
        out.client.reset();
        return false;
    }
    out.market = out.client->session().market;
    out.region = out.client->region().name;
    out.regionCount = out.client->session().regions.size();

    constexpr size_t kLibraryTitleLimit = 5000;
    // Use the lightweight Game Pass catalog endpoint instead of the full
    // display-catalog hydration, whose image-heavy response can exceed the
    // PS4 SceHttp bridge's 1 MiB response cap.
    auto displayName = [](const xc::xcloud::Title& title) -> const std::string& {
        return title.name.empty() ? title.titleId : title.name;
    };

            std::set<std::string> uniqueIds;
            for (const auto& title : page)
                if (!title.productId.empty()) uniqueIds.insert(title.productId);
            std::vector<std::string> productIds(uniqueIds.begin(), uniqueIds.end());

            // Show the GSSV page immediately. Catalog hydration can require
            // several extra HTTP requests; waiting for it here used to leave
            // the library screen empty while every page was enriched.
            out.titles.insert(out.titles.end(), std::make_move_iterator(page.begin()),
                              std::make_move_iterator(page.end()));
            if (onUpdate) onUpdate(out, false);

            std::map<std::string, xc::xcloud::Product> products;
            std::string catalogError;
            if (xc::xcloud::fetchProducts(productIds, out.market.empty() ? "US" : out.market,
                                          "en-us", products, catalogError, false)) {
                    auto found = products.find(title.productId);
                    if (found == products.end()) continue;
                    if (!found->second.title.empty()) title.name = found->second.title;
                    const auto& image = found->second.tileUrl.empty() ? found->second.posterUrl : found->second.tileUrl;
                    if (!image.empty()) title.imageUrl = image;
                }
            } else if (out.catalogWarning.empty()) {
                out.catalogWarning = catalogError;
            }
            // Publish again when names/artwork become available. The UI uses
            // this update to refresh covers without losing the focused game.
            if (onUpdate) onUpdate(out, false);
        return false;  // Keep any already-published pages visible to the caller.
    }
    if (onUpdate) onUpdate(out, true);
    return true;
}

}  // namespace xc::ps4
