#include "game_update_install.hpp"

#include <algorithm>
#include <cctype>

namespace pipensx {
namespace {

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

std::vector<uint8_t> selectUpdateFiles(const TorrentPreview& preview) {
    std::vector<uint8_t> actions;
    actions.reserve(preview.files.size());
    bool matched = false;
    for (const auto& file : preview.files) {
        if (file.package && isUpdateFile(file.path)) {
            actions.push_back(static_cast<uint8_t>(FileAction::Install));
            matched = true;
        } else {
            actions.push_back(static_cast<uint8_t>(FileAction::Skip));
        }
    }
    if (matched)
        return actions;
    actions.clear();
    for (const auto& file : preview.files) {
        actions.push_back(
            static_cast<uint8_t>(file.package ? FileAction::Install
                                              : FileAction::Skip));
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
