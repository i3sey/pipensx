#pragma once

#include <cctype>
#include <string>

namespace pipensx {

// Case-insensitive test that `name` ends with the 4-character extension
// `ext4` (e.g. ".nsp"). Shared by every Switch file-name classifier so the
// recognized extensions live in exactly one place.
inline bool hasFileExtension(const std::string& name, const char* ext4) {
    if (name.size() < 4)
        return false;
    const size_t base = name.size() - 4;
    for (int i = 0; i < 4; ++i)
        if (static_cast<char>(std::tolower(
                static_cast<unsigned char>(name[base + i]))) != ext4[i])
            return false;
    return true;
}

// Installable NSP/NSZ package.
inline bool isPackageName(const std::string& name) {
    return hasFileExtension(name, ".nsp") || hasFileExtension(name, ".nsz");
}

// XCI/XCZ cartridge dump.
inline bool isCartridgeName(const std::string& name) {
    return hasFileExtension(name, ".xci") || hasFileExtension(name, ".xcz");
}

// Zstd-compressed installable package (.nsz). Used for space estimation,
// where a compressed package has an unknown expanded size.
inline bool isCompressedName(const std::string& name) {
    return hasFileExtension(name, ".nsz");
}

} // namespace pipensx
