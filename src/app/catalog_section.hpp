#pragma once
#include <algorithm>
#include <cctype>
#include <string>

namespace pipensx {
inline bool isPortTitle(const std::string& titleLower) {
    if (titleLower.find("[nro") != std::string::npos ||
        titleLower.find(".nro") != std::string::npos) return true;
    if (titleLower.find("хоумбрю") != std::string::npos ||
        titleLower.find("homebrew") != std::string::npos) return true;
    if (titleLower.find("(порт") != std::string::npos ||
        titleLower.find(" порт") != std::string::npos ||
        titleLower.find("порт)") != std::string::npos ||
        titleLower.find("порты") != std::string::npos) return true;
    return false;
}

// Shared by the native catalog and the remote companion.
inline bool catalogTitleIsGame(std::string title, const std::string& titleId,
                               const std::string& metadataTitleId = {},
                               const std::string& packageType = {}) {
    std::transform(title.begin(), title.end(), title.begin(),
                   [](unsigned char c) { return char(std::tolower(c)); });
    std::string pkg = packageType;
    std::transform(pkg.begin(), pkg.end(), pkg.begin(),
                   [](unsigned char c) { return char(std::tolower(c)); });
    if (pkg == "nro") return false;
    if (isPortTitle(title)) return false;
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
