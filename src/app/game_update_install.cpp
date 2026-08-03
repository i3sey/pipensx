#include "game_update_install.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>

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

} // namespace

std::vector<size_t> updateVersionMatches(const TorrentPreview& preview,
                                         const std::string& latestVersion) {
    std::vector<size_t> matches;
    uint64_t wanted = 0;
    if (!parseDecimal(latestVersion, wanted) || wanted == 0)
        return matches;
    for (size_t i = 0; i < preview.files.size(); ++i) {
        uint64_t tag = 0;
        if (preview.files[i].package &&
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
                                       const std::string& latestVersion) {
    const std::vector<size_t> matches =
        updateVersionMatches(preview, latestVersion);
    if (!matches.empty())
        return selectFiles(preview, matches);

    std::vector<size_t> marked;
    for (size_t i = 0; i < preview.files.size(); ++i) {
        if (preview.files[i].package && isUpdateFile(preview.files[i].path))
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

    // Nothing marked: install every package. Already-installed content keys
    // short-circuit at commit, so the worst case is re-downloading.
    std::vector<size_t> all;
    for (size_t i = 0; i < preview.files.size(); ++i) {
        if (preview.files[i].package)
            all.push_back(i);
    }
    return selectFiles(preview, all);
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

namespace {

// One-line file label keeping both ends of the path: deep directories
// distinguish duplicate file names (a mod folder and the release root may
// share one). The byte caps roll back to UTF-8 code point boundaries so a
// truncated Cyrillic name never ends in a partial character.
std::string fileChoiceLabel(const std::string& path) {
    constexpr size_t kMax = 60;
    if (path.size() <= kMax)
        return path;
    const size_t head = utf8TruncateBoundary(path, 18);
    const size_t tailStart = utf8TruncateBoundary(path, path.size() - (kMax - 21));
    return path.substr(0, head) + "..." + path.substr(tailStart);
}

} // namespace

UpdateFileChoicePage updateFileChoicePage(const TorrentPreview& preview,
                                          const std::vector<size_t>& matches,
                                          size_t start,
                                          std::vector<uint8_t> initialPeers) {
    UpdateFileChoicePage page;
    for (size_t i = start; i < matches.size() && page.files.size() < 2; ++i) {
        UpdateFileChoicePage::FileButton button;
        button.index = matches[i];
        button.label = fileChoiceLabel(preview.files[button.index].path);
        button.peers = initialPeers;
        page.files.push_back(std::move(button));
    }
    page.nextStart = start + page.files.size();
    page.remaining = matches.size() - page.nextStart;
    if (page.remaining > 0)
        page.morePeers = std::move(initialPeers);
    return page;
}

} // namespace pipensx
