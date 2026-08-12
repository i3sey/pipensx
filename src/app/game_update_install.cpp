#include "game_update_install.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
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

// Release names put the 16-hex title id next to [vN]. Empty titleId skips
// the check so older call sites / tests keep matching on version alone.
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

std::vector<std::string> titleIdsInPath(const std::string& path) {
    std::vector<std::string> ids;
    for (size_t i = 0; i + 16 <= path.size(); ++i) {
        std::string candidate = path.substr(i, 16);
        uint64_t parsed = 0;
        if (parseNxTitleId(candidate, parsed)) {
            ids.push_back(formatNxTitleId(parsed));
            i += 15;
        }
    }
    return ids;
}

std::unordered_set<std::string> normalizedSet(
    const std::vector<std::string>& values) {
    std::unordered_set<std::string> out;
    out.reserve(values.size());
    for (const std::string& value : values) {
        uint64_t parsed = 0;
        if (parseNxTitleId(value, parsed))
            out.insert(formatNxTitleId(parsed));
    }
    return out;
}

} // namespace

bool parseNxTitleId(const std::string& titleId, uint64_t& value) {
    if (titleId.size() != 16)
        return false;
    uint64_t parsed = 0;
    for (char c : titleId) {
        int digit = -1;
        if (c >= '0' && c <= '9')
            digit = c - '0';
        else if (c >= 'a' && c <= 'f')
            digit = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F')
            digit = c - 'A' + 10;
        if (digit < 0)
            return false;
        parsed = (parsed << 4) | static_cast<uint64_t>(digit);
    }
    value = parsed;
    return true;
}

std::string formatNxTitleId(uint64_t value) {
    char text[17];
    std::snprintf(text, sizeof(text), "%016llX",
                  static_cast<unsigned long long>(value));
    return text;
}

std::string normalizeNxBaseTitleId(const std::string& titleId) {
    uint64_t parsed = 0;
    if (!parseNxTitleId(titleId, parsed))
        return {};
    return formatNxTitleId(parsed & ~0x1FFFULL);
}

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
            pathHasTitleId(preview.files[i].path, titleId) &&
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
            pathHasTitleId(preview.files[i].path, titleId) &&
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
    const std::string& titleId,
    const std::vector<std::string>& installedTitleIds,
    const std::vector<std::string>& installedDlcIds) {
    std::vector<uint8_t> actions(
        preview.files.size(), static_cast<uint8_t>(FileAction::Skip));
    const std::string wantedBase = normalizeNxBaseTitleId(titleId);
    if (wantedBase.empty())
        return actions;
    const std::unordered_set<std::string> installedTitles =
        normalizedSet(installedTitleIds);
    const std::unordered_set<std::string> installedDlc =
        normalizedSet(installedDlcIds);

    for (size_t i = 0; i < preview.files.size(); ++i) {
        const TorrentPreview::File& file = preview.files[i];
        if (!file.package)
            continue;
        bool install = false;
        for (const std::string& id : titleIdsInPath(file.path)) {
            const std::string candidateBase = normalizeNxBaseTitleId(id);
            if (candidateBase != wantedBase)
                continue;
            uint64_t parsed = 0;
            parseNxTitleId(id, parsed);
            const uint64_t low = parsed & 0x1FFFULL;
            const bool dlc = low >= 0x1000ULL;
            const bool basePackage = low == 0;
            const bool update = low == 0x800ULL;
            if (dlc && installedDlc.count(id) == 0) {
                install = true;
                break;
            }
            if (update) {
                install = true;
                break;
            }
            if (basePackage && installedTitles.count(wantedBase) == 0) {
                install = true;
                break;
            }
        }
        if (install)
            actions[i] = static_cast<uint8_t>(FileAction::Install);
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

} // namespace pipensx
