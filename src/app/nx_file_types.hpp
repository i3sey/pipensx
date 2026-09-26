#pragma once

#include <cctype>
#include <cstdint>
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
// supported extension; the post-download probe confirms an NRO payload, an
// Atmosphere LayeredFS tree, or both before anything is deployed.
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

inline bool layeredFsComponentEqual(const std::string& value,
                                   const char* expected) {
    size_t i = 0;
    for (; expected[i] != '\0'; ++i) {
        if (i >= value.size() ||
            static_cast<char>(std::tolower(
                static_cast<unsigned char>(value[i]))) != expected[i])
            return false;
    }
    return i == value.size();
}

// `leaf` is "romfs" or "exefs". A match is a member under one of:
//   [prefix/]atmosphere/contents/<16-hex>/leaf/<member>
//   [prefix/]contents/<16-hex>/leaf/<member>
//   [prefix/]titles/<16-hex>/leaf/<member>
// `destination`, when set, is always the SD-relative canonical path
// atmosphere/contents/<title id>/leaf/<member>. `sourceOffset` is the start
// of the recognized root in `path` (useful for the atmosphere spelling,
// where a suffix slice still begins with "atmosphere").
inline bool locateLayeredFsMember(const std::string& path, const char* leaf,
                                  size_t* sourceOffset, std::string* titleId,
                                  std::string* destination) {
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
        auto publish = [&](size_t rootIndex, size_t tidIndex,
                           size_t memberStart) {
            if (sourceOffset)
                *sourceOffset = partStarts[rootIndex];
            if (titleId)
                *titleId = parts[tidIndex];
            if (destination) {
                std::string member = path.substr(memberStart);
                for (char& ch : member)
                    if (ch == '\\')
                        ch = '/';
                *destination = "atmosphere/contents/" + parts[tidIndex] +
                               "/" + leaf + "/" + member;
            }
            return true;
        };
        if (count == 4) {
            if (layeredFsComponentEqual(parts[0], "atmosphere") &&
                layeredFsComponentEqual(parts[1], "contents") &&
                isHexTitleId(parts[2]) &&
                layeredFsComponentEqual(parts[3], leaf) &&
                slash != std::string::npos && slash + 1 < path.size())
                return publish(0, 2, slash + 1);
            if ((layeredFsComponentEqual(parts[0], "contents") ||
                 layeredFsComponentEqual(parts[0], "titles")) &&
                isHexTitleId(parts[1]) &&
                layeredFsComponentEqual(parts[2], leaf))
                return publish(0, 1, partStarts[3]);
        }
        if (slash == std::string::npos)
            break;
        start = slash + 1;
    }
    return false;
}

// RomFS game data. Also accepts a release that omitted atmosphere/ or used
// the old titles/<tid>/ root; the SD destination is still
// atmosphere/contents/<tid>/romfs/<member>. Executable exefs patches are not
// romfs: they stay out of one-tap and are opted in from the file picker.
inline bool isLayeredFsRomfsPath(const std::string& path,
                                 size_t* atmosphereOffset = nullptr,
                                 std::string* titleId = nullptr,
                                 std::string* destination = nullptr) {
    return locateLayeredFsMember(path, "romfs", atmosphereOffset, titleId,
                                 destination);
}

// Code patch under exefs. Same roots as romfs, canonical destination
// atmosphere/contents/<tid>/exefs/<member>. Not a one-tap extra.
inline bool isLayeredFsExefsPath(const std::string& path,
                                std::string* destination = nullptr,
                                std::string* titleId = nullptr) {
    return locateLayeredFsMember(path, "exefs", nullptr, titleId,
                                 destination);
}

inline bool isPortPayloadName(const std::string& path) {
    return isPortArchiveName(path) || pathContainsSwitchComponent(path) ||
           isLayeredFsRomfsPath(path);
}

// Name-only extra classification. Zip/7z still need a post-download probe to
// tell an NRO port archive from a LayeredFS rusifikator; junk never deploys.
enum class SwitchPathKind {
    Package,
    Cartridge,
    Nro,
    LayeredFsRomfs,
    Archive,
    Junk,
};

inline SwitchPathKind classifySwitchPath(const std::string& path) {
    if (isPackageName(path))
        return SwitchPathKind::Package;
    if (isCartridgeName(path))
        return SwitchPathKind::Cartridge;
    if (hasNroExtension(path))
        return SwitchPathKind::Nro;
    if (isLayeredFsRomfsPath(path))
        return SwitchPathKind::LayeredFsRomfs;
    if (isPortArchiveName(path))
        return SwitchPathKind::Archive;
    return SwitchPathKind::Junk;
}

inline bool isDeployableExtraPath(const std::string& path) {
    const SwitchPathKind kind = classifySwitchPath(path);
    return kind == SwitchPathKind::LayeredFsRomfs ||
           kind == SwitchPathKind::Archive;
}

// Extras the picker can mark Install: unpack/copy after the download,
// rather than leaving the file in pipensx/downloads. Readme/nfo stay
// Download-or-Skip only.
inline bool extraAcceptsInstallAction(const std::string& path) {
    const SwitchPathKind kind = classifySwitchPath(path);
    return kind == SwitchPathKind::Nro ||
           kind == SwitchPathKind::LayeredFsRomfs ||
           kind == SwitchPathKind::Archive ||
           isLayeredFsExefsPath(path);
}

inline bool isRarName(const std::string& name) {
    return hasFileExtension(name, ".rar");
}

inline bool isCompressedArchiveName(const std::string& name) {
    return isPortArchiveName(name) || isRarName(name);
}

// Patch NSPs are tagged [vN] with N>0, named update/patch, or carry the
// …800 title id. Lower rank installs first so a dump that lists the patch
// before the [v0] base still commits the application package first.
inline bool pathLooksLikeUpdatePackage(const std::string& path) {
    std::string lower;
    lower.resize(path.size());
    for (size_t i = 0; i < path.size(); ++i)
        lower[i] = static_cast<char>(
            std::tolower(static_cast<unsigned char>(path[i])));
    if (lower.find("update") != std::string::npos ||
        lower.find("patch") != std::string::npos)
        return true;
    for (size_t i = 0; i + 2 < lower.size(); ++i) {
        if (lower[i] == '[' && lower[i + 1] == 'v' &&
            lower[i + 2] >= '1' && lower[i + 2] <= '9')
            return true;
    }
    for (size_t i = 0; i + 16 <= path.size(); ++i) {
        uint64_t id = 0;
        bool hex = true;
        for (size_t j = 0; j < 16; ++j) {
            const unsigned char ch = static_cast<unsigned char>(path[i + j]);
            unsigned digit = 0;
            if (ch >= '0' && ch <= '9')
                digit = ch - '0';
            else if (ch >= 'a' && ch <= 'f')
                digit = 10 + (ch - 'a');
            else if (ch >= 'A' && ch <= 'F')
                digit = 10 + (ch - 'A');
            else {
                hex = false;
                break;
            }
            id = (id << 4) | digit;
        }
        if (hex && (id & 0xFFFULL) == 0x800ULL)
            return true;
    }
    return false;
}

inline int packageInstallRank(const std::string& path) {
    if (!isPackageName(path))
        return 2;
    return pathLooksLikeUpdatePackage(path) ? 1 : 0;
}

} // namespace pipensx
