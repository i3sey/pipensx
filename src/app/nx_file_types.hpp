#pragma once

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

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

// Port payload archive sitting next to NSP forwarders: switch.7z / switch.zip.
// "switch.7z" is 9 chars — do not gate on length or it is silently dropped.
inline bool isPortArchiveName(const std::string& path) {
    const size_t slash = path.find_last_of("/\\");
    const std::string base =
        slash == std::string::npos ? path : path.substr(slash + 1);
    std::string lower = base;
    for (char& ch : lower)
        if (ch >= 'A' && ch <= 'Z')
            ch = static_cast<char>(ch - 'A' + 'a');
    return lower == "switch.7z" || lower == "switch.zip";
}

inline bool hasNroExtension(const std::string& path) {
    return hasFileExtension(path, ".nro");
}

inline bool isSwitchPathComponent(const std::string& value) {
    const char expected[] = "switch";
    if (value.size() != 6)
        return false;
    for (size_t i = 0; i < 6; ++i)
        if (static_cast<char>(std::tolower(
                static_cast<unsigned char>(value[i]))) != expected[i])
            return false;
    return true;
}

inline bool pathContainsSwitchComponent(const std::string& path) {
    size_t start = 0;
    while (start < path.size()) {
        const size_t slash = path.find_first_of("/\\", start);
        const std::string component = path.substr(
            start, slash == std::string::npos ? std::string::npos
                                               : slash - start);
        if (isSwitchPathComponent(component))
            return true;
        if (slash == std::string::npos)
            break;
        start = slash + 1;
    }
    return false;
}

inline bool isPortPayloadName(const std::string& path) {
    return isPortArchiveName(path) || pathContainsSwitchComponent(path);
}

inline bool asciiPathComponentEquals(const std::string& value,
                                     const char* expected) {
    size_t i = 0;
    for (; expected[i] != '\0'; ++i) {
        if (i >= value.size())
            return false;
        const char c = static_cast<char>(std::tolower(
            static_cast<unsigned char>(value[i])));
        if (c != expected[i])
            return false;
    }
    return i == value.size();
}

// Atmosphere's own folder, plus the `atmopshere` typo used by some releases.
inline bool isAtmosphereFolderName(const std::string& value) {
    return asciiPathComponentEquals(value, "atmosphere") ||
           asciiPathComponentEquals(value, "atmopshere");
}

inline bool parseNxTitleIdComponent(const std::string& name, uint64_t& id) {
    if (name.size() != 16)
        return false;
    uint64_t value = 0;
    for (char c : name) {
        int digit = -1;
        if (c >= '0' && c <= '9')
            digit = c - '0';
        else if (c >= 'a' && c <= 'f')
            digit = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F')
            digit = c - 'A' + 10;
        if (digit < 0)
            return false;
        value = (value << 4) | static_cast<uint64_t>(digit);
    }
    id = value;
    return true;
}

// Atmosphere sysmodules live at 010000000000xxxx. Game LayeredFS must not
// be allowed to overwrite those contents folders.
inline bool isNxSysmoduleTitleId(uint64_t applicationId) {
    return (applicationId & ~0xFFFFULL) == 0x0100000000000000ULL;
}

inline std::string formatNxTitleIdUpper(uint64_t applicationId) {
    char text[17];
    std::snprintf(text, sizeof(text), "%016llX",
                  static_cast<unsigned long long>(applicationId));
    return text;
}

// Loose LayeredFS file: .../{atmosphere|atmopshere}/contents/<tid>/{romfs|exefs}/...
// Destination is always spelled `atmosphere/contents/<TID>/romfs|exefs/...`.
// Sysmodule title ids return false and set rejectedSysmodule.
inline bool layeredFsDeployRelative(const std::string& path,
                                    std::string& destinationRelative,
                                    bool* rejectedSysmodule = nullptr) {
    if (rejectedSysmodule)
        *rejectedSysmodule = false;
    std::vector<std::string> parts;
    size_t start = 0;
    while (start <= path.size()) {
        const size_t slash = path.find_first_of("/\\", start);
        parts.push_back(path.substr(
            start, slash == std::string::npos ? std::string::npos
                                              : slash - start));
        if (slash == std::string::npos)
            break;
        start = slash + 1;
    }
    for (size_t i = 0; i + 4 < parts.size(); ++i) {
        if (!isAtmosphereFolderName(parts[i]) ||
            !asciiPathComponentEquals(parts[i + 1], "contents"))
            continue;
        uint64_t titleId = 0;
        if (!parseNxTitleIdComponent(parts[i + 2], titleId))
            continue;
        if (isNxSysmoduleTitleId(titleId)) {
            if (rejectedSysmodule)
                *rejectedSysmodule = true;
            continue;
        }
        const bool romfs = asciiPathComponentEquals(parts[i + 3], "romfs");
        const bool exefs = asciiPathComponentEquals(parts[i + 3], "exefs");
        if (!romfs && !exefs)
            continue;
        destinationRelative = "atmosphere/contents/";
        destinationRelative += formatNxTitleIdUpper(titleId);
        destinationRelative += romfs ? "/romfs" : "/exefs";
        for (size_t j = i + 4; j < parts.size(); ++j) {
            destinationRelative += '/';
            destinationRelative += parts[j];
        }
        return true;
    }
    return false;
}

inline bool isLayeredFsPayloadPath(const std::string& path) {
    std::string unused;
    return layeredFsDeployRelative(path, unused);
}

} // namespace pipensx
