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
    // An NRO sitting at the torrent root has an empty parent. Treat that
    // the same way archive mapping treats an empty extract root: every
    // non-package file belongs to the payload (`Game.nro` plus `data/…`).
    if (root.empty())
        return true;
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

inline bool torrentHasPackageFiles(const TorrentPreview& preview) {
    for (const TorrentPreview::File& file : preview.files)
        if (file.package)
            return true;
    return false;
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

inline bool torrentHasInstallableExtras(const TorrentPreview& preview) {
    for (const TorrentPreview::File& file : preview.files)
        if (!file.package && !file.cartridge &&
            extraAcceptsInstallAction(torrentLogicalPath(preview, file)))
            return true;
    return false;
}

// True homebrew ports have an NRO (and maybe a zip). A retail dump plus a
// rusifikator zip or LayeredFS tree is still a game torrent — stream-install
// the NSP and leave extras optional. Zip/LayeredFS-only torrents stay ports.
inline bool torrentPortLayoutDetected(const TorrentPreview& preview) {
    if (!candidatePortPayloadRoots(preview).empty())
        return true;
    if (torrentHasPackageFiles(preview))
        return false;
    return torrentHasPortArchive(preview) ||
           torrentHasLayeredFsPayload(preview);
}

// Detail-card one-tap, after the file list is known. The catalog tab is not
// consulted. NSP/NSZ keeps smart StreamInstall even when an NRO is in the
// torrent or "[NRO]" is in the title. No packages, and an NRO or a
// zip/LayeredFS tree, is a port.
inline bool cardOneTapUsesPortInstall(const TorrentPreview& preview) {
    if (torrentHasPackageFiles(preview))
        return false;
    return torrentPortLayoutDetected(preview);
}

inline bool pathIsExefsPatch(const TorrentPreview& preview,
                            const TorrentPreview::File& file) {
    if (file.package || file.cartridge)
        return false;
    return isLayeredFsExefsPath(file.path) ||
           isLayeredFsExefsPath(torrentLogicalPath(preview, file));
}

inline bool torrentHasExefsPatches(const TorrentPreview& preview) {
    for (const TorrentPreview::File& file : preview.files)
        if (pathIsExefsPatch(preview, file))
            return true;
    return false;
}

// Picker opt-in. Turns every exefs member to Download, or back to Skip when
// they are already all selected. Other rows are left alone. One-tap never
// calls this.
inline void toggleExefsPatchActions(const TorrentPreview& preview,
                                   std::vector<uint8_t>& actions) {
    bool any = false;
    bool enable = false;
    for (size_t i = 0; i < preview.files.size(); ++i) {
        if (!pathIsExefsPatch(preview, preview.files[i]))
            continue;
        any = true;
        const uint8_t action = i < actions.size()
            ? actions[i]
            : static_cast<uint8_t>(FileAction::Skip);
        if (action != static_cast<uint8_t>(FileAction::Download))
            enable = true;
    }
    if (!any)
        return;
    if (actions.size() < preview.files.size())
        actions.resize(preview.files.size(),
                       static_cast<uint8_t>(FileAction::Skip));
    const uint8_t next = static_cast<uint8_t>(
        enable ? FileAction::Download : FileAction::Skip);
    for (size_t i = 0; i < preview.files.size(); ++i)
        if (pathIsExefsPatch(preview, preview.files[i]))
            actions[i] = next;
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

// True when the picker selection is an NRO (or zip/LayeredFS-only) port
// transaction. Retail NSP + zip/LayeredFS stays StreamInstall: packages are
// streamed and extras deploy afterwards.
inline bool selectionIsPortTransaction(const TorrentPreview& preview,
                                       const std::vector<uint8_t>& actions,
                                       size_t* packageCount = nullptr) {
    size_t packages = 0;
    bool payload = false;
    const bool retailPackages = torrentHasPackageFiles(preview);
    for (size_t i = 0; i < actions.size() && i < preview.files.size(); ++i) {
        if (actions[i] == static_cast<uint8_t>(FileAction::Skip))
            continue;
        const TorrentPreview::File& file = preview.files[i];
        if (file.package) {
            ++packages;
            continue;
        }
        if (file.cartridge)
            continue;
        if (hasNroExtension(file.path) ||
            (!retailPackages && isDeployableExtraPath(file.path)))
            payload = true;
    }
    if (packageCount)
        *packageCount = packages;
    return payload;
}

} // namespace pipensx
