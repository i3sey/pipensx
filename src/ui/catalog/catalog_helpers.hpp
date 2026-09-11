#pragma once

#include <algorithm>
#include <cctype>
#include <ctime>
#include <numeric>
#include <string>
#include <vector>

#include <borealis.hpp>

#include "app/app_settings.hpp"
#include "app/catalog_presentation.hpp"
#include "app/catalog_service.hpp"
#include "app/curl_https.hpp"
#include "ui/i18n.hpp"
#include "app/game_metadata_service.hpp"
#include "ui/common/async_image.hpp"
#include "ui/common/busy_pulse.hpp"
#include "ui/theme.hpp"

namespace pipensx::ui {

// ---------------------------------------------------------------------------
// Shared catalog helpers (used by both the list and the detail page)
// ---------------------------------------------------------------------------

inline std::string catalogLower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) {
                       return static_cast<char>(std::tolower(c));
                   });
    return value;
}

inline std::string formatCatalogRefreshError(const std::string& error) {
    if (isSslCertificateErrorMessage(error))
        return tr("pipensx/debrid/ssl_cert_hint");
    return error;
}

inline std::string classifyResolveFailure(const std::string& error) {
    std::string lower = catalogLower(error);
    if (lower.find("not registered") != std::string::npos ||
        lower.find("stale") != std::string::npos)
        return tr("pipensx/health/stale");
    // Reachable-peer failures come before the metadata one: not getting a
    // connection open is a network problem, and "no metadata" would send the
    // reporter after the catalog entry instead of their Wi-Fi.
    if (lower.find("could not connect") != std::string::npos)
        return tr("pipensx/health/network_blocked");
    if (lower.find("metadata") != std::string::npos)
        return tr("pipensx/health/no_metadata");
    if (lower.find("no usable peers") != std::string::npos ||
        lower.find("no peers") != std::string::npos)
        return tr("pipensx/health/no_peers");
    return tr("pipensx/health/resolve_failed");
}

inline std::string badgeForCatalogHealth(const CatalogEntry& entry) {
    switch (entry.health) {
        case pipensx::CatalogHealth::Ok:
            return entry.metadataOk ? tr("pipensx/health/fresh")
                                    : tr("pipensx/health/checked");
        case pipensx::CatalogHealth::NoPeers:
            return tr("pipensx/health/no_peers");
        case pipensx::CatalogHealth::MetadataTimeout:
            return tr("pipensx/health/no_metadata");
        case pipensx::CatalogHealth::TrackerNotRegistered:
        case pipensx::CatalogHealth::Dead:
            return tr("pipensx/health/dead");
        case pipensx::CatalogHealth::Replaced:
            return tr("pipensx/health/replaced");
        case pipensx::CatalogHealth::Unknown:
            break;
    }
    return entry.catalogGeneratedAt || entry.sourceUpdatedAt
         ? tr("pipensx/health/unchecked") : std::string();
}

inline std::string joinStrings(const std::vector<std::string>& values,
                        const char* separator) {
    std::string out;
    for (const std::string& value : values) {
        if (value.empty())
            continue;
        if (!out.empty())
            out += separator;
        out += value;
    }
    return out;
}

// Menu order of the player-mode filter; PlayerFilter::Any is offered
// unconditionally and lives outside this table.
struct PlayerModeOption {
    PlayerFilter filter;
    uint8_t bit;
    const char* key;
};

inline const std::vector<PlayerModeOption>& playerModeOptions() {
    static const std::vector<PlayerModeOption> options = {
        {PlayerFilter::Splitscreen, kPlayerModeSplit,
         "pipensx/catalog/players_split"},
        {PlayerFilter::LocalCoop, kPlayerModeCoop,
         "pipensx/catalog/players_coop"},
        {PlayerFilter::Lan, kPlayerModeLan, "pipensx/catalog/players_lan"},
        {PlayerFilter::Online, kPlayerModeOnline,
         "pipensx/catalog/players_online"},
    };
    return options;
}

inline std::string playerFilterLabel(PlayerFilter filter) {
    if (filter == PlayerFilter::Any)
        return tr("pipensx/catalog/players_any");
    for (const PlayerModeOption& option : playerModeOptions())
        if (option.filter == filter)
            return tr(option.key);
    return {};
}

// Detail-page fact: "up to 4 - split screen, local co-op". Empty when the
// index knows neither a player count nor a mode, so the row disappears.
inline std::string playersFact(const GameMetadata* metadata) {
    if (!metadata)
        return {};
    std::string count;
    if (metadata->players >= 2)
        count = tr("pipensx/detail/players_up_to",
                   std::to_string(metadata->players));
    else if (metadata->players == 1)
        count = tr("pipensx/detail/players_single");
    std::vector<std::string> modes;
    for (const PlayerModeOption& option : playerModeOptions())
        if (metadata->modes & option.bit)
            modes.push_back(tr(option.key));
    const std::string joined = joinStrings(modes, ", ");
    if (count.empty())
        return joined;
    if (joined.empty())
        return count;
    return count + " • " + joined;
}

inline std::string shortDescription(const std::string& value) {
    if (value.size() <= 900)
        return value;
    return value.substr(0, 900) + "...";
}

inline std::string formatEpochDateUtc(int64_t epochSec) {
    if (epochSec <= 0)
        return {};
    const time_t raw = static_cast<time_t>(epochSec);
    std::tm tm {};
#if defined(_WIN32)
    if (gmtime_s(&tm, &raw) != 0)
        return {};
#else
    if (!gmtime_r(&raw, &tm))
        return {};
#endif
    char buf[16];
    if (strftime(buf, sizeof(buf), "%Y-%m-%d", &tm) == 0)
        return {};
    return buf;
}

// Append a freshly created async image to a box (banner / screenshot on the
// detail page). Reuses loadImageInto for the disk-cached fetch.
inline void appendAsyncImage(brls::Box* parent, GameMetadataService* service,
                      const std::string& url, float height) {
    if (!service || url.empty())
        return;
    auto* image = new AsyncRgbaImage();
    image->setHeight(height);
    image->setMarginBottom(12);
    image->setAlignSelf(brls::AlignSelf::CENTER);
    image->setScalingType(brls::ImageScalingType::FIT);
    image->setClipsToBounds(false);  // no letterbox edge bands
    loadImageInto(image, service, url);
    parent->addView(image);
}

// Popularity ranking used by the Home hero and the catalog Popular sort.
// Indices are into `visible` (itself a list of snapshot indices). Peer count
// wins; with no peers at all, a freshness+size rank sum is the fallback.
inline std::vector<int> catalogPopularityOrder(
    const std::vector<CatalogEntry>& all, const std::vector<int>& visible,
    bool& usedFallback) {
    std::vector<int> order(visible.size());
    std::iota(order.begin(), order.end(), 0);
    auto entry = [&](int visIndex) -> const CatalogEntry& {
        return all[static_cast<size_t>(
            visible[static_cast<size_t>(visIndex)])];
    };
    usedFallback = std::none_of(visible.begin(), visible.end(),
        [&all](int snapshotIndex) {
            return all[static_cast<size_t>(snapshotIndex)].peerCount > 0;
        });
    if (!usedFallback) {
        std::stable_sort(order.begin(), order.end(),
            [&](int left, int right) {
                const auto& l = entry(left);
                const auto& r = entry(right);
                if (l.peerCount != r.peerCount)
                    return l.peerCount > r.peerCount;
                return l.publishedAt > r.publishedAt;
            });
        return order;
    }
    std::vector<size_t> score(visible.size(), 0);
    std::vector<int> ranked = order;
    std::stable_sort(ranked.begin(), ranked.end(),
        [&](int left, int right) {
            return entry(left).publishedAt > entry(right).publishedAt;
        });
    for (size_t pos = 0; pos < ranked.size(); ++pos)
        score[static_cast<size_t>(ranked[pos])] += pos;
    std::stable_sort(ranked.begin(), ranked.end(),
        [&](int left, int right) {
            return entry(left).size > entry(right).size;
        });
    for (size_t pos = 0; pos < ranked.size(); ++pos)
        score[static_cast<size_t>(ranked[pos])] += pos;
    std::stable_sort(order.begin(), order.end(),
        [&](int left, int right) {
            if (score[static_cast<size_t>(left)] !=
                score[static_cast<size_t>(right)])
                return score[static_cast<size_t>(left)] <
                       score[static_cast<size_t>(right)];
            return entry(left).publishedAt > entry(right).publishedAt;
        });
    return order;
}

inline std::string catalogFeaturedImageUrl(const CatalogEntry& entry,
                                           const GameMetadata* metadata) {
    if (metadata && !metadata->bannerUrl.empty())
        return metadata->bannerUrl;
    if (metadata && !metadata->screenshots.empty())
        return metadata->screenshots.front();
    if (!entry.screenshots.empty())
        return entry.screenshots.front();
    return {};
}

// Catalog refresh freshness badge (catalog header, left of the release count).
class CatalogFreshnessBadge : public brls::Box {
public:
    CatalogFreshnessBadge() : brls::Box(brls::Axis::ROW) {
        setFocusable(false);
        setAlignItems(brls::AlignItems::CENTER);
        setShrink(0.0f);
        busyDot_ = new brls::Label();
        busyDot_->setText("●");
        busyDot_->setFontSize(theme::kFontCaption);
        busyDot_->setTextColor(theme::warning());
        busyDot_->setMarginRight(6);
        busyDot_->setFocusable(false);
        busyDot_->setShrink(0.0f);
        busyDot_->setVisibility(brls::Visibility::GONE);
        addView(busyDot_);
        label_ = new brls::Label();
        label_->setFontSize(theme::kFontCaption);
        label_->setSingleLine(true);
        label_->setFocusable(false);
        label_->setShrink(0.0f);
        addView(label_);
    }

    void update(bool busy, bool refreshInFlight, AppSettings* settings,
                CatalogService* catalog) {
        const uint64_t wallSec =
            settings ? settings->get().lastCatalogRefreshWallSec : 0;
        const int64_t snapshot = catalog ? catalog->snapshotEpochSec() : 0;
        const bool hasEntries = catalog && !catalog->entries().empty();
        const CatalogFreshness state = resolveCatalogFreshness(
            busy || refreshInFlight, wallSec, snapshot, hasEntries,
            wallSec != 0 &&
                isLocalToday(static_cast<int64_t>(wallSec)));
        switch (state.kind) {
            case CatalogFreshness::Kind::Updating:
                label_->setText(tr("pipensx/catalog/freshness_updating"));
                label_->setTextColor(theme::warning());
                return;
            case CatalogFreshness::Kind::Never:
                label_->setText(tr("pipensx/catalog/freshness_never"));
                label_->setTextColor(theme::error());
                return;
            case CatalogFreshness::Kind::Ok:
                label_->setText(tr("pipensx/catalog/freshness_ok",
                                    formatEpochDateUtc(state.epochSec)));
                label_->setTextColor(theme::success());
                return;
            case CatalogFreshness::Kind::Stale:
                label_->setText(tr("pipensx/catalog/freshness_stale",
                                    formatEpochDateUtc(state.epochSec)));
                label_->setTextColor(theme::error());
                return;
        }
    }

    void setBusy(bool busy) {
        if (busy) {
            busyDot_->setVisibility(brls::Visibility::VISIBLE);
            startBusyPulse(busyDot_);
        } else {
            stopBusyPulse(busyDot_);
            busyDot_->setVisibility(brls::Visibility::GONE);
        }
    }

private:
    brls::Label* busyDot_ = nullptr;
    brls::Label* label_ = nullptr;
};

}  // namespace pipensx::ui
