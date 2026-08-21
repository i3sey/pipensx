#include "game_update_install.hpp"
#include "installed_title_service.hpp"
#include "nx_file_types.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <unordered_set>

namespace pipensx {
namespace {

// Strict decimal parse ("131072"); rejects signs, whitespace and overflow —
// strtoull would happily turn "1.2.3" into 1 and match a [v1] package.
bool parseDecimal(const std::string& text, uint64_t& out) {
    if (text.empty())
        return false;
    uint64_t value = 0;
    for (unsigned char c : text) {
        if (c < '0' || c > '9')
            return false;
        const uint64_t digit = static_cast<uint64_t>(c - '0');
        if (value > (UINT64_MAX - digit) / 10)
            return false;
        value = value * 10 + digit;
    }
    out = value;
    return true;
}

// First "[vN]" numeric tag in a file name
// ("Minecraft [0100D71004694800][v10092544].nsp" -> 10092544). Returns false
// when the name carries no numeric [vN] tag.
bool fileVersionTag(const std::string& path, uint64_t& value) {
    std::string lower = path;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) {
                       return static_cast<char>(std::tolower(c));
                   });
    for (size_t i = 0; i + 2 < lower.size(); ++i) {
        if (lower[i] == '[' && lower[i + 1] == 'v' &&
            lower[i + 2] >= '0' && lower[i + 2] <= '9') {
            char* end = nullptr;
            const unsigned long long v =
                strtoull(lower.c_str() + i + 2, &end, 10);
            if (end != lower.c_str() + i + 2) {
                value = static_cast<uint64_t>(v);
                return true;
            }
        }
    }
    return false;
}

bool isUpdateFile(const std::string& path) {
    std::string lower = path;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) {
                       return static_cast<char>(std::tolower(c));
                   });
    if (lower.find("update") != std::string::npos ||
        lower.find("patch") != std::string::npos ||
        lower.find("upd") != std::string::npos)
        return true;
    // "[vN]" with a non-zero N — release bundles tag the bundled update
    // version in the file name, and the base package usually carries "[v0]".
    for (size_t i = 0; i + 2 < lower.size(); ++i) {
        if (lower[i] == '[' && lower[i + 1] == 'v' &&
            lower[i + 2] >= '1' && lower[i + 2] <= '9')
            return true;
    }
    return false;
}

// Every 16-hex title id embedded in a file path, uppercased.
std::vector<std::string> titleIdsInPath(const std::string& path) {
    std::vector<std::string> ids;
    for (size_t i = 0; i + 16 <= path.size(); ++i) {
        std::string candidate = path.substr(i, 16);
        uint64_t parsed = 0;
        if (InstalledTitleService::parseTitleId(candidate, parsed)) {
            ids.push_back(InstalledTitleService::formatTitleId(parsed));
            i += 15;
        }
    }
    return ids;
}

std::string normalizeNxBaseTitleId(const std::string& titleId) {
    uint64_t parsed = 0;
    if (!InstalledTitleService::parseTitleId(titleId, parsed))
        return {};
    return InstalledTitleService::formatTitleId(
        InstalledTitleService::nxBaseApplicationId(parsed));
}

// Scene releases tag the Patch package with …800, not the base …000.
// DLC (low 12 bits >= 0x1000) shares the base id and must not match here.
bool pathHasBaseOrPatchTitleId(const std::string& path,
                               const std::string& titleId) {
    if (titleId.empty())
        return true;
    const std::string wanted = normalizeNxBaseTitleId(titleId);
    if (wanted.empty())
        return false;
    for (const std::string& id : titleIdsInPath(path)) {
        uint64_t parsed = 0;
        if (!InstalledTitleService::parseTitleId(id, parsed))
            continue;
        if (normalizeNxBaseTitleId(id) != wanted)
            continue;
        const uint64_t low = parsed & 0x1FFFULL;
        if (low == 0 || low == 0x800ULL)
            return true;
    }
    return false;
}

// Base packages keep the application id in the file name (…000), not …800.
bool pathHasTitleId(const std::string& path, const std::string& titleId) {
    if (titleId.empty())
        return true;
    std::string lowerPath = path;
    std::string lowerId = titleId;
    std::transform(lowerPath.begin(), lowerPath.end(), lowerPath.begin(),
                   [](unsigned char c) {
                       return static_cast<char>(std::tolower(c));
                   });
    std::transform(lowerId.begin(), lowerId.end(), lowerId.begin(),
                   [](unsigned char c) {
                       return static_cast<char>(std::tolower(c));
                   });
    return lowerPath.find(lowerId) != std::string::npos;
}

bool isBasePackageFile(const TorrentPreview::File& file,
                       const std::string& titleId) {
    if (!file.package || !pathHasTitleId(file.path, titleId) ||
        isUpdateFile(file.path))
        return false;
    uint64_t tag = 0;
    return !fileVersionTag(file.path, tag) || tag == 0;
}

bool isLikelyModPackage(const std::string& path) {
    std::string lower = path;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) {
                       return static_cast<char>(std::tolower(c));
                   });
    return lower.find("mod") != std::string::npos ||
           lower.find("exefs") != std::string::npos ||
           lower.find("romfs") != std::string::npos;
}

std::vector<size_t> smartUpdateMatches(const TorrentPreview& preview,
                                       const std::string& latestVersion,
                                       const std::string& titleId) {
    std::vector<size_t> matches = updateVersionMatches(preview, latestVersion,
                                                       titleId);
    matches.erase(std::remove_if(matches.begin(), matches.end(),
        [&preview](size_t i) {
            return i >= preview.files.size() ||
                   isLikelyModPackage(preview.files[i].path);
        }), matches.end());
    return matches;
}

std::string bundledUpdateVersion(const TorrentPreview& preview,
                                 const std::string& titleId) {
    uint64_t best = 0;
    bool have = false;
    for (const auto& file : preview.files) {
        if (!file.package || !pathHasBaseOrPatchTitleId(file.path, titleId))
            continue;
        uint64_t tag = 0;
        if (!fileVersionTag(file.path, tag) || tag == 0)
            continue;
        if (!have || tag > best) {
            best = tag;
            have = true;
        }
    }
    if (!have)
        return {};
    return std::to_string(best);
}

std::unordered_set<std::string> normalizedSet(
    const std::vector<std::string>& values) {
    std::unordered_set<std::string> out;
    out.reserve(values.size());
    for (const std::string& value : values) {
        uint64_t parsed = 0;
        if (InstalledTitleService::parseTitleId(value, parsed))
            out.insert(InstalledTitleService::formatTitleId(parsed));
    }
    return out;
}

} // namespace

std::vector<size_t> updateVersionMatches(const TorrentPreview& preview,
                                         const std::string& latestVersion,
                                         const std::string& titleId) {
    std::vector<size_t> matches;
    uint64_t wanted = 0;
    if (!parseDecimal(latestVersion, wanted) || wanted == 0)
        return matches;
    for (size_t i = 0; i < preview.files.size(); ++i) {
        uint64_t tag = 0;
        if (preview.files[i].package &&
            pathHasBaseOrPatchTitleId(preview.files[i].path, titleId) &&
            fileVersionTag(preview.files[i].path, tag) && tag == wanted)
            matches.push_back(i);
    }
    return matches;
}

std::vector<uint8_t> selectFiles(const TorrentPreview& preview,
                                 const std::vector<size_t>& picks) {
    std::vector<uint8_t> actions(
        preview.files.size(), static_cast<uint8_t>(FileAction::Skip));
    for (const size_t i : picks) {
        if (i < actions.size())
            actions[i] = static_cast<uint8_t>(FileAction::Install);
    }
    return actions;
}

std::vector<uint8_t> selectUpdateFiles(const TorrentPreview& preview,
                                       const std::string& latestVersion,
                                       const std::string& titleId) {
    const std::vector<size_t> matches =
        updateVersionMatches(preview, latestVersion, titleId);
    if (!matches.empty())
        return selectFiles(preview, matches);

    std::vector<size_t> marked;
    for (size_t i = 0; i < preview.files.size(); ++i) {
        if (preview.files[i].package &&
            pathHasBaseOrPatchTitleId(preview.files[i].path, titleId) &&
            isUpdateFile(preview.files[i].path))
            marked.push_back(i);
    }
    if (!marked.empty()) {
        // No exact tag: install only the highest-tagged marked package, so a
        // stray marker cannot drag unrelated packages along.
        uint64_t bestTag = 0;
        bool haveBest = false;
        std::vector<size_t> best;
        for (const size_t i : marked) {
            uint64_t tag = 0;
            const bool hasTag = fileVersionTag(preview.files[i].path, tag);
            if (hasTag) {
                if (!haveBest || tag > bestTag) {
                    bestTag = tag;
                    haveBest = true;
                    best = {i};
                } else if (tag == bestTag) {
                    best.push_back(i);
                }
            } else if (!haveBest) {
                best.push_back(i);
            }
        }
        return selectFiles(preview, best.empty() ? marked : best);
    }

    // Nothing identifiable: leave everything Skip so the chooser opens with
    // no preselection (Continue stays disabled until the user picks).
    return selectFiles(preview, {});
}

std::vector<uint8_t> selectSmartInstallFiles(
    const TorrentPreview& preview,
    bool titleInstalled,
    const std::string& installedVersion,
    const std::string& latestVersion,
    const std::string& titleId,
    const std::vector<std::string>& installedDlcIds) {
    if (preview.files.empty())
        return {};

    std::vector<uint8_t> actions(
        preview.files.size(), static_cast<uint8_t>(FileAction::Skip));

    std::string latest = latestVersion;
    uint64_t latestValue = 0;
    if (!parseDecimal(latest, latestValue) || latestValue == 0) {
        latest = bundledUpdateVersion(preview, titleId);
        latestValue = 0;
        parseDecimal(latest, latestValue);
    }

    // Base + exact update (the smart-install behaviour from #28).
    if (titleInstalled) {
        uint64_t installed = 0;
        if (parseDecimal(installedVersion, installed) &&
            latestValue > installed) {
            const std::vector<size_t> updates =
                smartUpdateMatches(preview, latest, titleId);
            for (const size_t i : updates)
                if (i < actions.size())
                    actions[i] = static_cast<uint8_t>(FileAction::Install);
        }
    } else {
        for (size_t i = 0; i < preview.files.size(); ++i)
            if (isBasePackageFile(preview.files[i], titleId))
                actions[i] = static_cast<uint8_t>(FileAction::Install);
        const std::vector<size_t> updates =
            smartUpdateMatches(preview, latest, titleId);
        for (const size_t i : updates)
            if (i < actions.size())
                actions[i] = static_cast<uint8_t>(FileAction::Install);
    }

    // Smart DLC (#29): AddOnContent packages for the selected title that are
    // not already installed. A DLC title id sets bit 12 (…1000) and carries its
    // index in the low 12 bits, so it normalises onto the base title.
    const std::string wantedBase = normalizeNxBaseTitleId(titleId);
    if (!wantedBase.empty()) {
        const std::unordered_set<std::string> installedDlc =
            normalizedSet(installedDlcIds);
        for (size_t i = 0; i < preview.files.size(); ++i) {
            if (!preview.files[i].package ||
                actions[i] == static_cast<uint8_t>(FileAction::Install))
                continue;
            for (const std::string& id : titleIdsInPath(preview.files[i].path)) {
                if (normalizeNxBaseTitleId(id) != wantedBase)
                    continue;
                uint64_t parsed = 0;
                InstalledTitleService::parseTitleId(id, parsed);
                if ((parsed & 0x1FFFULL) >= 0x1000ULL &&
                    installedDlc.count(id) == 0) {
                    actions[i] = static_cast<uint8_t>(FileAction::Install);
                    break;
                }
            }
        }
    }

    for (size_t i = 0; i < preview.files.size(); ++i) {
        if (actions[i] == static_cast<uint8_t>(FileAction::Install))
            continue;
        if (preview.files[i].package || preview.files[i].cartridge)
            continue;
        if (isLayeredFsPayloadPath(preview.files[i].path))
            actions[i] = static_cast<uint8_t>(FileAction::Download);
    }

    return actions;
}

std::string updateMagnetFor(const std::string& infoHash,
                            const CatalogEntry* entry) {
    if (entry && !entry->magnetUri.empty())
        return entry->magnetUri;
    // The metadata index is RuTracker-derived, and MagnetResolver only
    // accepts RuTracker trackers, so the fallback carries the canonical
    // mirror (resolveToFile bakes all mirrors into the announce list).
    return "magnet:?xt=urn:btih:" + infoHash +
           "&tr=http://bt.t-ru.org/ann?magnet";
}

bool torrentHasLayeredFsFiles(const TorrentPreview& preview) {
    for (const TorrentPreview::File& file : preview.files) {
        if (!file.package && !file.cartridge &&
            isLayeredFsPayloadPath(file.path))
            return true;
    }
    return false;
}

} // namespace pipensx
