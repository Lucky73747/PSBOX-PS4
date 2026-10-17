// SPDX-License-Identifier: GPL-3.0-or-later
#include "xcloud_login.h"

#include "xcloud/catalog.h"
#include "net/http.h"

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

    struct BackgroundCatalogScope {
        BackgroundCatalogScope() { xc::net::setBackgroundHttpThread(true); }
        ~BackgroundCatalogScope() { xc::net::setBackgroundHttpThread(false); }
    } catalogScope;
    constexpr size_t kLibraryTitleLimit = 5000;
    // Use the lightweight Game Pass catalog endpoint instead of the full
    // display-catalog hydration, whose image-heavy response can exceed the
    // PS4 SceHttp bridge's 1 MiB response cap.
    auto displayName = [](const xc::xcloud::Title& title) -> const std::string& {
        return title.name.empty() ? title.titleId : title.name;
    };

    auto sortTitles = [&] {
        std::stable_sort(out.titles.begin(), out.titles.end(), [&](const auto& a, const auto& b) {
            const auto& left = displayName(a);
            const auto& right = displayName(b);
            return std::lexicographical_compare(left.begin(), left.end(), right.begin(), right.end(),
                [](unsigned char x, unsigned char y) { return std::tolower(x) < std::tolower(y); });
        });
        std::stable_sort(out.displayTitles.begin(), out.displayTitles.end(), [&](const auto& a, const auto& b) {
            const auto& left = displayName(a);
            const auto& right = displayName(b);
            return std::lexicographical_compare(left.begin(), left.end(), right.begin(), right.end(),
                [](unsigned char x, unsigned char y) { return std::tolower(x) < std::tolower(y); });
        });
    };

    const bool listed = out.client->listTitlesPaged(error, [&](std::vector<xc::xcloud::Title>&& page) {
            std::set<std::string> uniqueIds;
            for (const auto& title : page)
                if (!title.productId.empty()) uniqueIds.insert(title.productId);
            std::vector<std::string> productIds(uniqueIds.begin(), uniqueIds.end());

            // Show the GSSV page immediately. Catalog hydration can require
            // several extra HTTP requests; waiting for it here used to leave
            // the library screen empty while every page was enriched.
            const size_t pageStart = out.titles.size();
            out.titles.insert(out.titles.end(), std::make_move_iterator(page.begin()),
                              std::make_move_iterator(page.end()));
            out.displayTitles.insert(out.displayTitles.end(), out.titles.begin() + pageStart, out.titles.end());
            if (onUpdate) onUpdate(out, false);

            std::map<std::string, xc::xcloud::Product> products;
            std::string catalogError;
            if (xc::xcloud::fetchProducts(productIds, out.market.empty() ? "US" : out.market,
                                          "en-us", products, catalogError, false)) {
                for (size_t i = pageStart; i < out.titles.size(); ++i) {
                    auto& title = out.titles[i];
                    auto found = products.find(title.productId);
                    if (found == products.end()) continue;
                    if (!found->second.title.empty()) title.name = found->second.title;
                    const auto& image = found->second.tileUrl.empty() ? found->second.posterUrl : found->second.tileUrl;
                    if (!image.empty()) title.imageUrl = image;
                }
                for (auto& visible : out.displayTitles) {
                    for (size_t i = pageStart; i < out.titles.size(); ++i) {
                        const auto& title = out.titles[i];
                        if (title.titleId == visible.titleId && title.productId == visible.productId) {
                            visible = title;
                            break;
                        }
                    }
                }
            } else if (out.catalogWarning.empty()) {
                out.catalogWarning = catalogError;
            }
            // Publish again when names/artwork become available. The UI uses
            // this update to refresh covers without losing the focused game.
            if (onUpdate) onUpdate(out, false);
        }, false, kLibraryTitleLimit);
    sortTitles();
    if (!listed) {
        return false;  // Keep any already-published pages visible to the caller.
    }
    if (onUpdate) onUpdate(out, true);
    return true;
}

}  // namespace xc::ps4
