#pragma once
#include <algorithm>
#include <cctype>
#include <string>

namespace pipensx {
// Shared by the native catalog and the remote companion.
inline bool catalogTitleIsGame(std::string title, const std::string& titleId,
                               const std::string& metadataTitleId = {}) {
    std::transform(title.begin(), title.end(), title.begin(),
                   [](unsigned char c) { return char(std::tolower(c)); });
    if (title.find("[nro") != std::string::npos ||
        title.find(".nro") != std::string::npos) return false;
    const auto validId = [](const std::string& id) {
        return id.size() == 16 && std::all_of(id.begin(), id.end(),
            [](unsigned char c) { return std::isxdigit(c) != 0; });
    };
    if (validId(titleId) || validId(metadataTitleId)) return true;
    for (const char* marker : {"[nsp", "[nsz", "[xci", "[xcz", ".nsp",
                              ".nsz", ".xci", ".xcz", "/nsp", "/nsz", "/xci", "/xcz"})
        if (title.find(marker) != std::string::npos) return true;
    return false;
}
}
