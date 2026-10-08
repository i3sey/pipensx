#pragma once

#include "task_files.hpp"
#include <map>
#include <string>

namespace pipensx {
struct PortLayoutRule {
    bool enabled = true;
    // Empty means automatic. '/' explicitly extracts relative to the SD root.
    std::string directory;
};
using PortLayoutRules = std::map<std::string, PortLayoutRule>;

inline bool normalizePortDirectory(const std::string& input, std::string& relative) {
    if (input.empty() || input.front() != '/' || input.size() > 512 ||
        input.find('\\') != std::string::npos) return false;
    relative = input.substr(1);
    if (!relative.empty() && relative.back() == '/') relative.pop_back();
    if (!relative.empty() && !taskFilePathIsFatCompatible(relative)) return false;
    std::string folded = relative;
    for (char& c : folded) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    return folded != "switch/pipensx" && folded.rfind("switch/pipensx/", 0) != 0;
}

inline bool manualPortMemberDestination(const std::string& directory,
                                         const std::string& source,
                                         std::string& destination) {
    std::string root;
    if (!normalizePortDirectory(directory, root) ||
        !taskFilePathIsFatCompatible(source)) return false;
    destination = root.empty() ? source : root + "/" + source;
    std::string validated;
    return normalizePortDirectory("/" + destination, validated);
}
} // namespace pipensx
