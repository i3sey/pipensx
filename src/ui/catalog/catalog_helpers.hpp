#pragma once

#include <algorithm>
#include <cctype>
#include <ctime>
#include <string>
#include <vector>

#include <borealis.hpp>

#include "app/catalog_presentation.hpp"
#include "app/catalog_service.hpp"
#include "ui/i18n.hpp"
#include "app/game_metadata_service.hpp"
#include "ui/common/async_image.hpp"

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
}  // namespace pipensx::ui
