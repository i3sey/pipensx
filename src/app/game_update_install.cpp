#include "game_update_install.hpp"
#include "installed_title_service.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <map>
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
        if (!file.package || !pathHasBaseOrPatchTitleId(file.path, titleId) ||
            isLikelyModPackage(file.path))
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

// Prefer a non-mod path, then the larger file, then the earlier index.
size_t pickBestPackage(const TorrentPreview& preview,
                       const std::vector<size_t>& candidates) {
    size_t best = preview.files.size();
    uint64_t bestLen = 0;
    bool bestMod = true;
    for (const size_t i : candidates) {
        if (i >= preview.files.size())
            continue;
        const bool mod = isLikelyModPackage(preview.files[i].path);
        const uint64_t len = preview.files[i].length;
        const bool better =
            best == preview.files.size() ||
            (mod != bestMod && !mod) ||
            (mod == bestMod && len > bestLen) ||
            (mod == bestMod && len == bestLen && i < best);
        if (better) {
            best = i;
            bestLen = len;
            bestMod = mod;
        }
    }
    return best;
}

std::string addOnContentId(const std::string& path,
                           const std::string& wantedBase) {
    if (wantedBase.empty())
        return {};
    for (const std::string& id : titleIdsInPath(path)) {
        if (normalizeNxBaseTitleId(id) != wantedBase)
            continue;
        uint64_t parsed = 0;
        if (!InstalledTitleService::parseTitleId(id, parsed))
            continue;
        if ((parsed & 0x1FFFULL) >= 0x1000ULL)
            return id;
    }
    return {};
}

bool fileInstalling(const std::vector<uint8_t>& actions, size_t i) {
    return i < actions.size() &&
           actions[i] == static_cast<uint8_t>(FileAction::Install);
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

    // Prefer the highest [vN] actually in this torrent so a stale catalog
    // latestVersion cannot pin an older bundled patch (or miss a newer one).
    std::string latest = bundledUpdateVersion(preview, titleId);
    uint64_t latestValue = 0;
    if (!parseDecimal(latest, latestValue) || latestValue == 0) {
        latest = latestVersion;
        latestValue = 0;
        parseDecimal(latest, latestValue);
    }

    if (!titleInstalled) {
        std::vector<size_t> bases;
        for (size_t i = 0; i < preview.files.size(); ++i)
            if (isBasePackageFile(preview.files[i], titleId))
                bases.push_back(i);
        const size_t base = pickBestPackage(preview, bases);
        if (base < actions.size())
            actions[base] = static_cast<uint8_t>(FileAction::Install);
    }

    uint64_t installed = 0;
    const bool haveInstalled = parseDecimal(installedVersion, installed);
    const bool wantUpdate =
        latestValue > 0 &&
        (!titleInstalled || (haveInstalled && latestValue > installed));
    if (wantUpdate) {
        std::vector<size_t> updates =
            smartUpdateMatches(preview, latest, titleId);
        if (updates.empty())
            updates = updateVersionMatches(preview, latest, titleId);
        const size_t picked = pickBestPackage(preview, updates);
        if (picked < actions.size())
            actions[picked] = static_cast<uint8_t>(FileAction::Install);
    }

    // Unique AddOnContent per title id, largest file wins on duplicates.
    const std::string wantedBase = normalizeNxBaseTitleId(titleId);
    if (!wantedBase.empty()) {
        const std::unordered_set<std::string> installedDlc =
            normalizedSet(installedDlcIds);
        std::map<std::string, size_t> bestDlc;
        for (size_t i = 0; i < preview.files.size(); ++i) {
            if (!preview.files[i].package)
                continue;
            const std::string id =
                addOnContentId(preview.files[i].path, wantedBase);
            if (id.empty() || installedDlc.count(id) != 0)
                continue;
            const auto found = bestDlc.find(id);
            if (found == bestDlc.end()) {
                bestDlc.emplace(id, i);
                continue;
            }
            const size_t current = found->second;
            const size_t picked =
                pickBestPackage(preview, {current, i});
            found->second = picked;
        }
        for (const auto& entry : bestDlc)
            if (entry.second < actions.size())
                actions[entry.second] = static_cast<uint8_t>(FileAction::Install);
    }

    return actions;
}

std::vector<uint8_t> overlappingSelectionConflicts(
    const TorrentPreview& preview, const std::vector<uint8_t>& actions) {
    std::vector<uint8_t> conflicts(preview.files.size(), 0);
    std::map<std::string, std::vector<size_t>> bases;
    std::map<std::string, std::vector<size_t>> patches;
    std::map<std::string, std::vector<size_t>> dlc;
    for (size_t i = 0; i < preview.files.size(); ++i) {
        if (!preview.files[i].package || !fileInstalling(actions, i))
            continue;
        std::string baseId;
        std::string dlcId;
        for (const std::string& id : titleIdsInPath(preview.files[i].path)) {
            uint64_t parsed = 0;
            if (!InstalledTitleService::parseTitleId(id, parsed))
                continue;
            const uint64_t low = parsed & 0x1FFFULL;
            if (low >= 0x1000ULL)
                dlcId = id;
            else if (low == 0 || low == 0x800ULL)
                baseId = normalizeNxBaseTitleId(id);
        }
        if (!dlcId.empty()) {
            dlc[dlcId].push_back(i);
            continue;
        }
        if (baseId.empty())
            continue;
        if (isBasePackageFile(preview.files[i], baseId))
            bases[baseId].push_back(i);
        else
            patches[baseId].push_back(i);
    }
    auto mark = [&conflicts](const std::vector<size_t>& group) {
        if (group.size() < 2)
            return;
        for (const size_t i : group)
            if (i < conflicts.size())
                conflicts[i] = 1;
    };
    for (const auto& entry : bases)
        mark(entry.second);
    for (const auto& entry : patches)
        mark(entry.second);
    for (const auto& entry : dlc)
        mark(entry.second);
    return conflicts;
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

UpdatePreflight describeUpdatePreflight(const InstalledTitle& installed,
                                         const std::string& latestVersion,
                                         bool titleInstalled) {
    UpdatePreflight pre;
    pre.titleInstalled = titleInstalled;
    pre.installedXyz = formatTitleVersion(installed.version);
    pre.targetXyz = formatTitleVersion(latestVersion);
    pre.displayVersion = installed.displayVersion;
    pre.hasMods = installed.hasLayeredFsMods;
    return pre;
}

std::string formatRequiredHosVersion(uint32_t requiredSystemVersion) {
    if (requiredSystemVersion == 0)
        return {};
    const unsigned major =
        static_cast<unsigned>((requiredSystemVersion >> 26) & 0x3f);
    const unsigned minor =
        static_cast<unsigned>((requiredSystemVersion >> 20) & 0x3f);
    const unsigned micro =
        static_cast<unsigned>((requiredSystemVersion >> 16) & 0xfu);
    return std::to_string(major) + "." + std::to_string(minor) + "." +
           std::to_string(micro);
}

uint32_t makeRequiredSystemVersion(unsigned major, unsigned minor,
                                   unsigned micro) {
    return ((static_cast<uint32_t>(major) & 0x3fu) << 26) |
           ((static_cast<uint32_t>(minor) & 0x3fu) << 20) |
           ((static_cast<uint32_t>(micro) & 0xfu) << 16);
}

bool updateRequiresNewerHos(uint32_t requiredSystemVersion,
                             unsigned hosMajor, unsigned hosMinor,
                             unsigned hosMicro) {
    if (requiredSystemVersion == 0)
        return false;
    const unsigned needMajor =
        static_cast<unsigned>((requiredSystemVersion >> 26) & 0x3f);
    const unsigned needMinor =
        static_cast<unsigned>((requiredSystemVersion >> 20) & 0x3f);
    const unsigned needMicro =
        static_cast<unsigned>((requiredSystemVersion >> 16) & 0xfu);
    if (needMajor != hosMajor)
        return needMajor > hosMajor;
    if (needMinor != hosMinor)
        return needMinor > hosMinor;
    return needMicro > hosMicro;
}

UpdateFailureHint classifyUpdateFailure(const std::string& installError,
                                        bool hasMods,
                                        uint32_t requiredSystemVersion,
                                        unsigned hosMajor, unsigned hosMinor,
                                        unsigned hosMicro) {
    std::string lower = installError;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) {
                       return static_cast<char>(std::tolower(c));
                   });
    // "0x00000291" (ticket) contains "0291"; "0x291" is the short form.
    if (lower.find("0291") != std::string::npos ||
        lower.find("0x291") != std::string::npos ||
        lower.find("sys-patch") != std::string::npos ||
        lower.find("syspatch") != std::string::npos ||
        lower.find("sigpatch") != std::string::npos ||
        lower.find("sig-patch") != std::string::npos ||
        lower.find("signature patch") != std::string::npos)
        return UpdateFailureHint::SigPatches;
    if (updateRequiresNewerHos(requiredSystemVersion, hosMajor, hosMinor,
                               hosMicro))
        return UpdateFailureHint::NeedsNewHos;
    if (hasMods)
        return UpdateFailureHint::ModConflict;
    return UpdateFailureHint::None;
}

std::string formatUpdateFailureHint(UpdateFailureHint hint,
                                    const std::string& targetXyz,
                                    uint32_t requiredSystemVersion) {
    switch (hint) {
    case UpdateFailureHint::ModConflict: {
        std::string text =
            "If the game closes on launch after this update, remove the "
            "mods in atmosphere/contents/ for this title (or update the "
            "mods to match the new version) and try again.";
        if (!targetXyz.empty())
            text = "Update v" + targetXyz + " may conflict with the "
                     "installed mods. " + text;
        return text;
    }
    case UpdateFailureHint::NeedsNewHos: {
        const std::string need =
            formatRequiredHosVersion(requiredSystemVersion);
        if (!need.empty() && !targetXyz.empty())
            return "Update v" + targetXyz + " requires system " + need +
                   ". Update the firmware before launching it.";
        if (!need.empty())
            return "This update requires system " + need +
                   ". Update the firmware before launching it.";
        return "This update needs newer firmware. Update the firmware "
               "before launching it.";
    }
    case UpdateFailureHint::SigPatches:
        return "Title ticket import failed (0x291). Update sys-patch / "
               "sigpatches for this firmware, then reinstall the update.";
    case UpdateFailureHint::None:
    default:
        return {};
    }
}

} // namespace pipensx
