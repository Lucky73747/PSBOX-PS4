// SPDX-License-Identifier: GPL-3.0-or-later
#include "xcloud_login.h"

#include "xcloud/catalog.h"
#include "ui/strings.h"
#include "net/http.h"
#include "platform/platform.h"
#include "util/json.h"
#include "util/log.h"
#include <ctime>
#include <map>

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

    // Cache public product metadata only. Entitlements are always taken from
    // this authenticated session, never restored from another account's cache.
    constexpr const char* cachePath = "/data/PSBOX/catalog-metadata-v1.json";
    const std::string market = out.market.empty() ? "US" : out.market;
    const std::string language = xc::ui::catalogLanguage();
    struct Metadata { std::string name, image; int64_t fetchedAt = 0; };
    std::map<std::string, Metadata> metadata;
    std::string cached;
    if (platform::readFile(cachePath, cached) && cached.size() <= 8 * 1024 * 1024) {
        auto j = json::parse(cached);
        if (j && (*j)["version"].asInt() == 1 && (*j)["market"].str() == market &&
            (*j)["language"].str() == language) {
            for (const auto& [id, item] : (*j)["products"].members()) {
                if (metadata.size() >= kLibraryTitleLimit) break;
                const auto name = item["name"].str(), image = item["image"].str();
                if (id.size() > 128 || name.empty() || name.size() > 1024 || image.size() > 4096) continue;
                metadata.emplace(id, Metadata{name, image, item["at"].asInt()});
            }
        }
    }
    cached.clear();
    const uint64_t started = platform::nowMs();
    size_t cacheHits = 0, metadataRequests = 0;
    bool metadataDirty = false;
    std::vector<std::string> pending;
    std::set<std::string> queued;
    const int64_t now = static_cast<int64_t>(std::time(nullptr));
    auto publish = [&](bool complete) {
        out.displayTitles = out.titles;
        if (onUpdate) onUpdate(out, complete);
    };
    auto applyMetadata = [&] {
        for (auto& title : out.titles) {
            const auto found = metadata.find(title.productId);
            if (found == metadata.end()) continue;
            title.name = found->second.name;
            title.imageUrl = found->second.image;
        }
    };
    auto saveMetadata = [&] {
        if (!metadataDirty) return;
        json::Value root = json::Value::object(), entries = json::Value::object();
        std::set<std::string> saved;
        // Bound storage to IDs present in this live catalogue. No account identifiers.
        for (const auto& title : out.titles) {
            auto it = metadata.find(title.productId);
            if (it == metadata.end() || !saved.insert(it->first).second) continue;
            json::Value item = json::Value::object();
            item.set("name", it->second.name); item.set("image", it->second.image);
            item.set("at", it->second.fetchedAt); entries.set(it->first, std::move(item));
        }
        root.set("version", 1); root.set("market", market); root.set("language", language);
        root.set("products", std::move(entries));
        if (!platform::writeFileAtomic(cachePath, root.dump())) XC_LOGW("catalog_metadata cache_write_failed");
        else metadataDirty = false;
    };
    auto hydrate = [&](const std::vector<std::string>& ids) {
        if (ids.empty()) return;
        std::map<std::string, xc::xcloud::Product> products;
        std::string catalogError;
        ++metadataRequests;
        const bool ok = xc::xcloud::fetchProducts(ids, market, language, products, catalogError, false);
        // fetchProducts may already have useful results even when a later request fails.
        for (const auto& [id, product] : products) {
            if (product.title.empty()) continue;
            metadataDirty = true;
            metadata[id] = {product.title, product.tileUrl.empty() ? product.posterUrl : product.tileUrl,
                            static_cast<int64_t>(std::time(nullptr))};
        }
        if (!ok && out.catalogWarning.empty()) out.catalogWarning = catalogError;
        applyMetadata();
        publish(false);
    };
    bool firstPage = true;
    const bool listed = out.client->listTitlesPaged(error, [&](std::vector<xc::xcloud::Title>&& page) {
        for (auto& title : page) {
            auto found = metadata.find(title.productId);
            if (found != metadata.end()) {
                title.name = found->second.name; title.imageUrl = found->second.image;
                ++cacheHits;
            }
            const bool fresh = found != metadata.end() && !found->second.image.empty() && now >= found->second.fetchedAt &&
                now - found->second.fetchedAt < 7 * 24 * 60 * 60;
            if (!fresh && !title.productId.empty() && queued.insert(title.productId).second)
                pending.push_back(title.productId);
        }
        out.titles.insert(out.titles.end(), std::make_move_iterator(page.begin()), std::make_move_iterator(page.end()));
        publish(false); // Also preserves the existing pause-at-Play boundary.
        if (firstPage) {
            firstPage = false;
            XC_LOGI("catalog_metadata first_page_ms=%llu cached_titles=%zu",
                    static_cast<unsigned long long>(platform::nowMs() - started), cacheHits);
            // One light request gets useful names/covers on screen on a cold cache.
            // Do not block every subsequent page behind full catalogue hydration.
            const size_t count = std::min<size_t>(60, pending.size());
            if (count) {
                hydrate(std::vector<std::string>(pending.begin(), pending.begin() + count));
                pending.erase(pending.begin(), pending.begin() + count);
            }
        }
    }, false, kLibraryTitleLimit);
    // Fill missing/stale metadata in bounded batches after all title pages are visible.
    // No extra HTTP worker competes with the stream or artwork workers.
    size_t batches = 0;
    if (listed) {
        for (size_t i = 0; i < pending.size(); i += 60) {
            publish(false); // Pause before starting another HTTP request if Play was pressed.
            const size_t end = std::min(pending.size(), i + 60);
            hydrate(std::vector<std::string>(pending.begin() + i, pending.begin() + end));
            if (++batches % 5 == 0) saveMetadata();
        }
    }
    if (!out.titles.empty()) saveMetadata();
    XC_LOGI("catalog_metadata complete=%u titles=%zu cache_hits=%zu requests=%zu elapsed_ms=%llu",
            listed ? 1u : 0u, out.titles.size(), cacheHits, metadataRequests,
            static_cast<unsigned long long>(platform::nowMs() - started));
    sortTitles();
    if (!listed) {
        return false;  // Keep any already-published pages visible to the caller.
    }
    if (onUpdate) onUpdate(out, true);
    return true;
}

}  // namespace xc::ps4
