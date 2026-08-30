#pragma once

#include "download_manager.hpp"
#include "nx_file_types.hpp"

#include <string>
#include <vector>

namespace pipensx {
namespace {

inline std::string portSelectionLower(std::string value) {
    for (char& ch : value)
        if (ch >= 'A' && ch <= 'Z')
            ch = static_cast<char>(ch - 'A' + 'a');
    return value;
}

inline std::string torrentLogicalPath(const TorrentPreview& preview,
                                      const TorrentPreview::File& file) {
    return preview.multi ? preview.name + "/" + file.path : file.path;
}

inline std::string portParentPath(const std::string& path) {
    const size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

inline void addPortRoot(std::vector<std::string>& roots,
                        const std::string& root) {
    const std::string folded = portSelectionLower(root);
    for (const std::string& existing : roots)
        if (existing == folded)
            return;
    roots.push_back(folded);
}

inline std::vector<std::string> candidatePortPayloadRoots(
    const TorrentPreview& preview) {
    std::vector<std::string> roots;
    for (const TorrentPreview::File& file : preview.files) {
        if (file.package || file.cartridge ||
            !hasNroExtension(file.path))
            continue;
        addPortRoot(roots, portParentPath(torrentLogicalPath(preview, file)));
    }
    return roots;
}

inline bool pathUnderPortRoot(const std::string& logical,
                              const std::string& root) {
    const std::string folded = portSelectionLower(logical);
    if (root.empty())
        return folded.find('/') == std::string::npos;
    return folded == root || folded.rfind(root + "/", 0) == 0;
}

} // namespace

// Retained for the folder-selection UI and old callers. It describes the
// unambiguous legacy switch/ root, while the unified planner below no longer
// requires that spelling and instead discovers every NRO parent directory.
inline std::string candidatePortRoot(const TorrentPreview& preview) {
    std::vector<std::string> roots;
    for (const TorrentPreview::File& file : preview.files) {
        const std::string logical = torrentLogicalPath(preview, file);
        if (!hasNroExtension(logical))
            continue;
        size_t start = 0;
        while (start < logical.size()) {
            const size_t slash = logical.find('/', start);
            const std::string component = logical.substr(
                start, slash == std::string::npos ? std::string::npos
                                                   : slash - start);
            if (portSelectionLower(component) == "switch") {
                addPortRoot(roots, logical.substr(0, start + component.size()));
                break;
            }
            if (slash == std::string::npos)
                break;
            start = slash + 1;
        }
    }
    return roots.size() == 1 ? roots.front() : std::string();
}

inline bool torrentHasPortArchive(const TorrentPreview& preview) {
    for (const TorrentPreview::File& file : preview.files)
        if (!file.package && !file.cartridge &&
            isPortArchiveName(torrentLogicalPath(preview, file)))
            return true;
    return false;
}

inline bool torrentHasLayeredFsPayload(const TorrentPreview& preview) {
    for (const TorrentPreview::File& file : preview.files)
        if (!file.package && !file.cartridge &&
            isLayeredFsRomfsPath(torrentLogicalPath(preview, file)))
            return true;
    return false;
}

inline bool torrentPortLayoutDetected(const TorrentPreview& preview) {
    return !candidatePortPayloadRoots(preview).empty() ||
           torrentHasPortArchive(preview) ||
           torrentHasLayeredFsPayload(preview);
}

inline std::vector<uint8_t> selectPortPayloadActions(
    const TorrentPreview& preview, const std::string& legacyRoot = {}) {
    std::vector<uint8_t> mask(preview.files.size(),
                              static_cast<uint8_t>(FileAction::Skip));
    std::vector<std::string> roots = candidatePortPayloadRoots(preview);
    if (roots.empty() && !legacyRoot.empty())
        addPortRoot(roots, legacyRoot);
    for (size_t i = 0; i < preview.files.size(); ++i) {
        const TorrentPreview::File& file = preview.files[i];
        if (file.package || file.cartridge)
            continue;
        const std::string logical = torrentLogicalPath(preview, file);
        bool selected = isPortArchiveName(logical) ||
                        isLayeredFsRomfsPath(logical);
        for (const std::string& root : roots)
            selected = selected || pathUnderPortRoot(logical, root);
        if (selected)
            mask[i] = static_cast<uint8_t>(FileAction::Download);
    }
    return mask;
}

// A port is downloaded completely before deployment. Packages deliberately
// use Download here (not Install): SwitchDeployService installs them from the
// settled local files only after the payload receipt has been committed.
inline std::vector<uint8_t> selectPortInstallActions(
    const TorrentPreview& preview) {
    std::vector<uint8_t> mask = selectPortPayloadActions(preview);
    size_t selected = 0;
    for (size_t i = 0; i < preview.files.size(); ++i) {
        const TorrentPreview::File& file = preview.files[i];
        if (file.package) {
            mask[i] = static_cast<uint8_t>(FileAction::Download);
        } else if (file.cartridge) {
            mask[i] = static_cast<uint8_t>(FileAction::Skip);
        }
        if (mask[i] != static_cast<uint8_t>(FileAction::Skip))
            ++selected;
    }
    if (selected == 0) {
        for (uint8_t& action : mask)
            action = static_cast<uint8_t>(FileAction::Download);
    }
    return mask;
}

} // namespace pipensx
