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

} // namespace

inline std::string candidatePortRoot(const TorrentPreview& preview) {
    std::vector<std::string> roots;
    for (const TorrentPreview::File& file : preview.files) {
        const std::string logical = torrentLogicalPath(preview, file);
        const std::string folded = portSelectionLower(logical);
        if (folded.size() < 4 ||
            folded.compare(folded.size() - 4, 4, ".nro") != 0)
            continue;
        size_t start = 0;
        while (start < logical.size()) {
            const size_t slash = logical.find('/', start);
            const std::string component = logical.substr(
                start, slash == std::string::npos ? std::string::npos
                                                   : slash - start);
            if (portSelectionLower(component) == "switch") {
                const std::string root = portSelectionLower(
                    logical.substr(0, start + component.size()));
                bool seen = false;
                for (const std::string& existing : roots) {
                    if (existing == root) {
                        seen = true;
                        break;
                    }
                }
                if (!seen)
                    roots.push_back(root);
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
    for (const TorrentPreview::File& file : preview.files) {
        if (isPortArchiveName(torrentLogicalPath(preview, file)))
            return true;
    }
    return false;
}

inline bool torrentPortLayoutDetected(const TorrentPreview& preview) {
    return !candidatePortRoot(preview).empty() || torrentHasPortArchive(preview);
}

inline std::vector<uint8_t> selectPortPayloadActions(
    const TorrentPreview& preview, const std::string& root) {
    std::vector<uint8_t> mask(preview.files.size(),
                              static_cast<uint8_t>(FileAction::Skip));
    const std::string prefix =
        root.empty() ? std::string() : portSelectionLower(root) + "/";
    for (size_t i = 0; i < preview.files.size(); ++i) {
        const TorrentPreview::File& file = preview.files[i];
        const std::string logical = torrentLogicalPath(preview, file);
        const std::string folded = portSelectionLower(logical);
        const bool underRoot =
            !prefix.empty() && folded.rfind(prefix, 0) == 0 &&
            !file.package && !file.cartridge;
        const bool portArchive =
            isPortArchiveName(logical) && !file.package && !file.cartridge;
        if (underRoot || portArchive)
            mask[i] = static_cast<uint8_t>(FileAction::Download);
    }
    return mask;
}

inline std::vector<uint8_t> selectPortInstallActions(
    const TorrentPreview& preview) {
    std::vector<uint8_t> mask(preview.files.size(),
                              static_cast<uint8_t>(FileAction::Skip));
    const std::string root = candidatePortRoot(preview);
    const std::string prefix =
        root.empty() ? std::string() : portSelectionLower(root) + "/";
    size_t selected = 0;
    for (size_t i = 0; i < preview.files.size(); ++i) {
        const TorrentPreview::File& file = preview.files[i];
        const std::string logical = torrentLogicalPath(preview, file);
        const std::string folded = portSelectionLower(logical);
        if (file.package) {
            mask[i] = static_cast<uint8_t>(FileAction::Install);
            ++selected;
            continue;
        }
        if (file.cartridge)
            continue;
        const bool underRoot =
            !prefix.empty() && folded.rfind(prefix, 0) == 0;
        if (underRoot || isPortArchiveName(logical)) {
            mask[i] = static_cast<uint8_t>(FileAction::Download);
            ++selected;
        }
    }
    if (selected == 0) {
        for (uint8_t& action : mask)
            action = static_cast<uint8_t>(FileAction::Download);
    }
    return mask;
}

} // namespace pipensx
