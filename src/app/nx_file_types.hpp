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

// Supported port payload archives. Releases use both the conventional
// switch.zip/switch.7z names and game-specific names, so classification is by
// supported extension; the post-download probe confirms that the archive
// actually contains an NRO payload before anything is deployed.
inline bool isPortArchiveName(const std::string& path) {
    if (hasFileExtension(path, ".zip"))
        return true;
    if (path.size() < 3)
        return false;
    const size_t base = path.size() - 3;
    return path[base] == '.' && path[base + 1] == '7' &&
           static_cast<char>(std::tolower(
               static_cast<unsigned char>(path[base + 2]))) == 'z';
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

inline bool isHexTitleId(const std::string& value) {
    if (value.size() != 16)
        return false;
    for (const unsigned char ch : value)
        if (!std::isxdigit(ch))
            return false;
    return true;
}

// Recognizes a safe Atmosphere LayeredFS RomFS member at any release prefix:
// [prefix/]atmosphere/contents/<16-hex title id>/romfs/<member>. Executable
// patches under exefs are deliberately excluded; silently deploying those has
// a materially larger security and compatibility surface than game data.
inline bool isLayeredFsRomfsPath(const std::string& path,
                                 size_t* atmosphereOffset = nullptr,
                                 std::string* titleId = nullptr) {
    size_t start = 0;
    std::string parts[4];
    size_t partStarts[4] {};
    size_t count = 0;
    while (start < path.size()) {
        const size_t slash = path.find_first_of("/\\", start);
        const std::string component = path.substr(
            start, slash == std::string::npos ? std::string::npos
                                               : slash - start);
        if (component.empty())
            return false;
        if (count < 4) {
            parts[count] = component;
            partStarts[count] = start;
            ++count;
        } else {
            parts[0] = std::move(parts[1]);
            parts[1] = std::move(parts[2]);
            parts[2] = std::move(parts[3]);
            parts[3] = component;
            partStarts[0] = partStarts[1];
            partStarts[1] = partStarts[2];
            partStarts[2] = partStarts[3];
            partStarts[3] = start;
        }
        if (count == 4) {
            auto equal = [](const std::string& value, const char* expected) {
                size_t i = 0;
                for (; expected[i] != '\0'; ++i) {
                    if (i >= value.size() ||
                        static_cast<char>(std::tolower(
                            static_cast<unsigned char>(value[i]))) !=
                            expected[i])
                        return false;
                }
                return i == value.size();
            };
            if (equal(parts[0], "atmosphere") &&
                equal(parts[1], "contents") && isHexTitleId(parts[2]) &&
                equal(parts[3], "romfs") && slash != std::string::npos &&
                slash + 1 < path.size()) {
                if (atmosphereOffset)
                    *atmosphereOffset = partStarts[0];
                if (titleId)
                    *titleId = parts[2];
                return true;
            }
        }
        if (slash == std::string::npos)
            break;
        start = slash + 1;
    }
    return false;
}

inline bool isPortPayloadName(const std::string& path) {
    return isPortArchiveName(path) || pathContainsSwitchComponent(path) ||
           isLayeredFsRomfsPath(path);
}

} // namespace pipensx
