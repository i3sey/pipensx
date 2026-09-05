#include "port_archive.hpp"

#include "nx_file_types.hpp"
#include "task_files.hpp"

extern "C" {
#include "../../vendor/lzma/7z.h"
#include "../../vendor/lzma/7zAlloc.h"
#include "../../vendor/lzma/7zCrc.h"
#include "../../vendor/lzma/7zFile.h"
#include "../../vendor/lzma/Bra.h"
#include "../../vendor/lzma/CpuArch.h"
#include "../../vendor/lzma/Delta.h"
#include "../../vendor/lzma/Lzma2Dec.h"
#include "../../vendor/lzma/LzmaDec.h"
}

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>
#include <zlib.h>

#include <sys/stat.h>
#include <unistd.h>

namespace pipensx {
namespace {

constexpr size_t kCopyBufferBytes = 256 * 1024;
constexpr size_t kLookBufBytes = 1u << 18;
constexpr size_t kStreamOutChunkBytes = 1u << 20;
// Prefer streaming once the solid folder exceeds this — keeps huge port
// archives off the heap while still using SzArEx_Extract for tiny ones.
constexpr uint64_t kSolidStreamPreferBytes = 1ull * 1024 * 1024;

constexpr UInt32 kMethodCopy = 0;
constexpr UInt32 kMethodLzma2 = 0x21;
constexpr UInt32 kMethodLzma = 0x30101;
constexpr UInt32 kMethodDelta = 3;
constexpr UInt32 kMethodArm64 = 0xa;
constexpr UInt32 kMethodRiscv = 0xb;
constexpr UInt32 kMethodBcj = 0x3030103;
constexpr UInt32 kMethodPpc = 0x3030205;
constexpr UInt32 kMethodIa64 = 0x3030401;
constexpr UInt32 kMethodArm = 0x3030501;
constexpr UInt32 kMethodArmt = 0x3030701;
constexpr UInt32 kMethodSparc = 0x3030805;
constexpr size_t kMaxProbeFiles = 16384;

std::string lowerAscii(std::string value) {
    for (char& ch : value)
        if (ch >= 'A' && ch <= 'Z')
            ch = static_cast<char>(ch - 'A' + 'a');
    return value;
}

std::string basenameOf(const std::string& path) {
    const size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

bool endsWithCi(const std::string& value, const char* suffix) {
    const size_t n = std::strlen(suffix);
    if (value.size() < n)
        return false;
    for (size_t i = 0; i < n; ++i) {
        const char a = static_cast<char>(
            std::tolower(static_cast<unsigned char>(value[value.size() - n + i])));
        if (a != suffix[i])
            return false;
    }
    return true;
}

struct RawArchiveFile {
    std::string path;
    uint64_t size = 0;
};

std::string normalizeArchivePath(std::string path) {
    for (char& ch : path)
        if (ch == '\\')
            ch = '/';
    while (!path.empty() && path.front() == '/')
        path.erase(path.begin());
    return taskFilePathIsFatCompatible(path) ? path : std::string();
}

std::string parentPathOf(const std::string& path) {
    const size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

std::string basenamePath(const std::string& path) {
    const size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

bool pathInside(const std::string& path, const std::string& root) {
    if (root.empty())
        return true;
    const std::string foldedPath = lowerAscii(path);
    const std::string foldedRoot = lowerAscii(root);
    return foldedPath == foldedRoot ||
           foldedPath.rfind(foldedRoot + "/", 0) == 0;
}

// Build the one authoritative archive mapping. Each directory that directly
// contains an NRO becomes a top-level /switch application directory; files
// are assigned to the nearest such ancestor, so nested helper applications do
// not get copied twice. A legacy switch/ root remains a fallback for mixed
// releases where the NRO is loose and an archive contains only extra data.
bool buildArchiveMapping(const std::vector<RawArchiveFile>& raw,
                         PortArchiveProbe& out) {
    std::vector<std::string> roots;
    for (const RawArchiveFile& file : raw) {
        const std::string normalized = normalizeArchivePath(file.path);
        if (normalized.empty() || !hasNroExtension(normalized))
            continue;
        const std::string root = parentPathOf(normalized);
        const std::string folded = lowerAscii(root);
        bool seen = false;
        for (const std::string& existing : roots)
            seen = seen || lowerAscii(existing) == folded;
        if (!seen)
            roots.push_back(root);
    }
    auto legacyDestination = [](const std::string& source) {
        size_t start = 0;
        while (start < source.size()) {
            const size_t slash = source.find('/', start);
            const size_t end = slash == std::string::npos
                ? source.size() : slash;
            if (lowerAscii(source.substr(start, end - start)) == "switch") {
                if (slash == std::string::npos)
                    return std::string();
                return source.substr(slash + 1);
            }
            if (slash == std::string::npos)
                break;
            start = slash + 1;
        }
        return std::string();
    };
    std::vector<std::string> foldedDestinations;
    for (const RawArchiveFile& file : raw) {
        const std::string source = normalizeArchivePath(file.path);
        if (source.empty())
            continue;
        std::string destination;
        if (roots.empty()) {
            destination = legacyDestination(source);
        } else {
            const std::string* selected = nullptr;
            for (const std::string& root : roots) {
                if (!pathInside(source, root))
                    continue;
                if (!selected || root.size() > selected->size())
                    selected = &root;
            }
            if (!selected)
                continue;
            if (selected->empty()) {
                destination = source;
            } else {
                destination = basenamePath(*selected);
                destination += source.substr(selected->size());
            }
        }
        if (destination.empty() ||
            !taskFilePathIsFatCompatible(destination))
            continue;
        const std::string top = destination.substr(0, destination.find('/'));
        if (lowerAscii(top) == "pipensx") {
            out.error = "Writing inside the pipensx application directory is forbidden.";
            return false;
        }
        const std::string folded = lowerAscii(destination);
        if (std::find(foldedDestinations.begin(), foldedDestinations.end(),
                      folded) != foldedDestinations.end()) {
            out.error = "Archive destination paths collide on FAT.";
            return false;
        }
        if (out.files.size() >= kMaxProbeFiles) {
            out.error = "The archive contains too many port files.";
            return false;
        }
        foldedDestinations.push_back(folded);
        out.sourceFiles.push_back(source);
        out.files.push_back(destination);
        ++out.switchFiles;
        out.unpackBytes += file.size;
    }
    if (out.files.empty()) {
        out.error = roots.empty()
            ? "The archive has neither an NRO payload nor a switch directory."
            : "The archive has no files in an NRO application directory.";
        return false;
    }
    return true;
}

std::string archiveDestination(const PortArchiveProbe& probe,
                               const std::string& memberPath) {
    const std::string normalized = normalizeArchivePath(memberPath);
    for (size_t i = 0; i < probe.sourceFiles.size() &&
                       i < probe.files.size(); ++i)
        if (probe.sourceFiles[i] == normalized)
            return probe.files[i];
    return {};
}

// One mkdir level where EEXIST only counts when the name is already a
// directory — a regular file holding a directory's name must fail here with
// ENOTDIR, not deeper with a cryptic ENOENT (B2).
bool mkdirOne(char* path) {
    struct stat st {};
    if (mkdir(path, 0755) == 0)
        return true;
    if (errno != EEXIST)
        return false;
    if (stat(path, &st) != 0)
        return false;
    if (!S_ISDIR(st.st_mode))
        errno = ENOTDIR;
    return S_ISDIR(st.st_mode) != 0;
}

bool mkdirs(const std::string& path) {
    if (path.empty() || path.size() >= 1024)
        return false;
    char buffer[1024];
    std::snprintf(buffer, sizeof(buffer), "%s", path.c_str());
    for (char* p = buffer + 1; *p; ++p) {
        if (*p != '/')
            continue;
        *p = '\0';
        const bool ok = mkdirOne(buffer);
        *p = '/';
        if (!ok)
            return false;
    }
    return mkdirOne(buffer);
}

bool writeBytes(const std::string& path, const uint8_t* data, size_t size,
                const std::atomic<bool>& cancelled,
                const std::function<void(uint64_t)>& progress,
                std::string& error) {
    struct stat existing {};
    if (lstat(path.c_str(), &existing) == 0 || errno != ENOENT) {
        error = "An extracted destination file already exists.";
        return false;
    }
    const std::string parent = path.substr(0, path.find_last_of('/'));
    if (!parent.empty() && !mkdirs(parent)) {
        error = "Unable to create destination directories.";
        return false;
    }
    const std::string temporary = path + ".pipensx-part";
    std::remove(temporary.c_str());
    std::FILE* out = std::fopen(temporary.c_str(), "wb");
    if (!out) {
        error = "Unable to create extracted file.";
        return false;
    }
    size_t written = 0;
    while (written < size) {
        if (cancelled.load(std::memory_order_relaxed)) {
            std::fclose(out);
            std::remove(temporary.c_str());
            error = "Cancelled.";
            return false;
        }
        const size_t chunk =
            std::min(kCopyBufferBytes, size - written);
        if (std::fwrite(data + written, 1, chunk, out) != chunk) {
            std::fclose(out);
            std::remove(temporary.c_str());
            error = "Unable to write extracted file.";
            return false;
        }
        written += chunk;
        if (progress)
            progress(chunk);
    }
    if (std::fflush(out) != 0 || std::fclose(out) != 0) {
        std::remove(temporary.c_str());
        error = "Unable to flush extracted file.";
        return false;
    }
    if (lstat(path.c_str(), &existing) == 0 || errno != ENOENT) {
        std::remove(temporary.c_str());
        error = "An extracted destination file appeared during extraction.";
        return false;
    }
    if (std::rename(temporary.c_str(), path.c_str()) != 0) {
        std::remove(temporary.c_str());
        error = "Unable to commit extracted file.";
        return false;
    }
    return true;
}

uint32_t readU32le(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

uint16_t readU16le(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

bool extractZip(const std::string& archivePath, const std::string& targetRoot,
                const PortArchiveProbe& probe,
                const std::atomic<bool>& cancelled,
                const std::function<void(uint64_t)>& progress,
                const std::function<void(const std::string&)>& currentFile,
                std::string& error) {
    std::FILE* file = std::fopen(archivePath.c_str(), "rb");
    if (!file) {
        error = "Unable to open ZIP archive.";
        return false;
    }
    std::vector<uint8_t> local(30);
    std::vector<uint8_t> name;
    std::vector<uint8_t> extra;
    std::vector<uint8_t> compressed;
    std::vector<uint8_t> uncompressed;
    while (!cancelled.load(std::memory_order_relaxed)) {
        if (std::fread(local.data(), 1, 30, file) != 30) {
            if (std::feof(file))
                break;
            std::fclose(file);
            error = "Truncated ZIP local header.";
            return false;
        }
        const uint32_t sig = readU32le(local.data());
        if (sig == 0x02014b50 || sig == 0x06054b50)
            break; // central directory / end
        if (sig != 0x04034b50) {
            std::fclose(file);
            error = "Invalid ZIP local header.";
            return false;
        }
        const uint16_t method = readU16le(local.data() + 8);
        const uint32_t compSize = readU32le(local.data() + 18);
        const uint32_t uncompSize = readU32le(local.data() + 22);
        const uint16_t nameLen = readU16le(local.data() + 26);
        const uint16_t extraLen = readU16le(local.data() + 28);
        name.resize(nameLen);
        extra.resize(extraLen);
        if ((nameLen &&
             std::fread(name.data(), 1, nameLen, file) != nameLen) ||
            (extraLen &&
             std::fread(extra.data(), 1, extraLen, file) != extraLen)) {
            std::fclose(file);
            error = "Truncated ZIP entry header.";
            return false;
        }
        const std::string member(reinterpret_cast<char*>(name.data()), nameLen);
        const bool isDir = !member.empty() && member.back() == '/';
        const std::string relative =
            isDir ? std::string() : archiveDestination(probe, member);
        if (compSize > 0) {
            compressed.resize(compSize);
            if (std::fread(compressed.data(), 1, compSize, file) != compSize) {
                std::fclose(file);
                error = "Truncated ZIP entry data.";
                return false;
            }
        } else {
            compressed.clear();
        }
        if (relative.empty())
            continue;
        if (currentFile)
            currentFile(relative);
        if (method == 0) {
            if (compSize != uncompSize) {
                std::fclose(file);
                error = "ZIP stored size mismatch.";
                return false;
            }
            if (!writeBytes(targetRoot + "/" + relative, compressed.data(),
                            compressed.size(), cancelled, progress, error)) {
                std::fclose(file);
                return false;
            }
        } else if (method == 8) {
            uncompressed.resize(uncompSize);
            z_stream stream {};
            stream.next_in = compressed.data();
            stream.avail_in = static_cast<uInt>(compressed.size());
            stream.next_out = uncompressed.data();
            stream.avail_out = static_cast<uInt>(uncompressed.size());
            if (inflateInit2(&stream, -MAX_WBITS) != Z_OK) {
                std::fclose(file);
                error = "Unable to start ZIP inflate.";
                return false;
            }
            const int zrc = inflate(&stream, Z_FINISH);
            inflateEnd(&stream);
            if (zrc != Z_STREAM_END || stream.total_out != uncompSize) {
                std::fclose(file);
                error = "ZIP inflate failed.";
                return false;
            }
            if (!writeBytes(targetRoot + "/" + relative, uncompressed.data(),
                            uncompressed.size(), cancelled, progress, error)) {
                std::fclose(file);
                return false;
            }
        } else {
            std::fclose(file);
            error = "Unsupported ZIP compression method.";
            return false;
        }
    }
    std::fclose(file);
    if (cancelled.load(std::memory_order_relaxed)) {
        error = "Cancelled.";
        return false;
    }
    return true;
}

// Header-only ZIP member walk (no decompression): fills unpackBytes,
// switchFiles and the switch-relative member list. Mirrors extractZip's
// member handling so the two can never disagree about what lands in
// targetRoot.
bool probeZip(const std::string& archivePath, PortArchiveProbe& out) {
    std::FILE* file = std::fopen(archivePath.c_str(), "rb");
    if (!file) {
        out.error = "Unable to open ZIP archive.";
        return false;
    }
    std::vector<uint8_t> local(30);
    std::vector<uint8_t> name;
    std::vector<RawArchiveFile> raw;
    while (true) {
        if (std::fread(local.data(), 1, 30, file) != 30) {
            if (std::feof(file))
                break;
            std::fclose(file);
            out.error = "Truncated ZIP local header.";
            return false;
        }
        const uint32_t sig = readU32le(local.data());
        if (sig == 0x02014b50 || sig == 0x06054b50)
            break; // central directory / end
        if (sig != 0x04034b50) {
            std::fclose(file);
            out.error = "Invalid ZIP local header.";
            return false;
        }
        const uint32_t compSize = readU32le(local.data() + 18);
        const uint32_t uncompSize = readU32le(local.data() + 22);
        const uint16_t nameLen = readU16le(local.data() + 26);
        const uint16_t extraLen = readU16le(local.data() + 28);
        name.resize(nameLen);
        if (nameLen && std::fread(name.data(), 1, nameLen, file) != nameLen) {
            std::fclose(file);
            out.error = "Truncated ZIP entry header.";
            return false;
        }
        const uint64_t skip = static_cast<uint64_t>(extraLen) + compSize;
        if (skip && std::fseek(file, static_cast<long>(skip), SEEK_CUR) != 0) {
            std::fclose(file);
            out.error = "Truncated ZIP entry data.";
            return false;
        }
        const std::string member(reinterpret_cast<char*>(name.data()), nameLen);
        const bool isDir = !member.empty() && member.back() == '/';
        if (!isDir)
            raw.push_back({member, uncompSize});
    }
    std::fclose(file);
    return buildArchiveMapping(raw, out);
}

std::string utf16ToUtf8(const UInt16* src, size_t len) {
    std::string out;
    out.reserve(len);
    for (size_t i = 0; i < len; ++i) {
        const UInt32 c = src[i];
        if (c == 0)
            break;
        if (c < 0x80)
            out.push_back(static_cast<char>(c));
        else if (c < 0x800) {
            out.push_back(static_cast<char>(0xc0 | (c >> 6)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3f)));
        } else {
            out.push_back(static_cast<char>(0xe0 | (c >> 12)));
            out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3f)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3f)));
        }
    }
    return out;
}

struct SwitchOutRange {
    uint64_t start = 0;
    uint64_t end = 0;
    std::string relative;
    std::string absolute;
};

struct FolderByteSink {
    std::vector<SwitchOutRange> ranges;
    size_t nextRange = 0;
    uint64_t pos = 0;
    std::FILE* out = nullptr;
    size_t active = static_cast<size_t>(-1);
    const std::atomic<bool>* cancelled = nullptr;
    std::function<void(uint64_t)> progress;
    std::function<void(const std::string&)> currentFile;
    std::string error;

    ~FolderByteSink() { closeOut(); }

    void closeOut() {
        if (out) {
            std::fflush(out);
            std::fclose(out);
            out = nullptr;
        }
        active = static_cast<size_t>(-1);
    }

    bool openRange(size_t index) {
        closeOut();
        const SwitchOutRange& range = ranges[index];
        if (currentFile)
            currentFile(range.relative);
        const std::string parent =
            range.absolute.substr(0, range.absolute.find_last_of('/'));
        if (!parent.empty() && !mkdirs(parent)) {
            error = "Unable to create destination directories.";
            return false;
        }
        struct stat existing {};
        if (lstat(range.absolute.c_str(), &existing) == 0 || errno != ENOENT) {
            error = "An extracted destination file already exists.";
            return false;
        }
        const std::string temporary = range.absolute + ".pipensx-part";
        std::remove(temporary.c_str());
        out = std::fopen(temporary.c_str(), "wb");
        if (!out) {
            error = "Unable to create extracted file.";
            return false;
        }
        active = index;
        return true;
    }

    bool commitActive() {
        if (active == static_cast<size_t>(-1) || !out)
            return true;
        const std::string temporary =
            ranges[active].absolute + ".pipensx-part";
        if (std::fflush(out) != 0) {
            std::fclose(out);
            out = nullptr;
            std::remove(temporary.c_str());
            error = "Unable to flush extracted file.";
            return false;
        }
        std::fclose(out);
        out = nullptr;
        struct stat existing {};
        if (lstat(ranges[active].absolute.c_str(), &existing) == 0 ||
            errno != ENOENT) {
            std::remove(temporary.c_str());
            error = "An extracted destination file appeared during extraction.";
            return false;
        }
        if (std::rename(temporary.c_str(), ranges[active].absolute.c_str()) !=
            0) {
            std::remove(temporary.c_str());
            error = "Unable to commit extracted file.";
            return false;
        }
        active = static_cast<size_t>(-1);
        return true;
    }

    bool write(const Byte* data, size_t size) {
        size_t offset = 0;
        while (offset < size) {
            if (cancelled && cancelled->load(std::memory_order_relaxed)) {
                error = "Cancelled.";
                return false;
            }
            while (nextRange < ranges.size() && pos >= ranges[nextRange].end) {
                if (!commitActive())
                    return false;
                ++nextRange;
            }
            if (nextRange >= ranges.size() || pos < ranges[nextRange].start) {
                const uint64_t skipTo = nextRange >= ranges.size()
                    ? pos + static_cast<uint64_t>(size - offset)
                    : ranges[nextRange].start;
                const size_t gap = static_cast<size_t>(std::min<uint64_t>(
                    skipTo - pos, static_cast<uint64_t>(size - offset)));
                pos += gap;
                offset += gap;
                continue;
            }
            if (active != nextRange && !openRange(nextRange))
                return false;
            const size_t chunk = static_cast<size_t>(std::min<uint64_t>(
                ranges[nextRange].end - pos,
                static_cast<uint64_t>(size - offset)));
            if (std::fwrite(data + offset, 1, chunk, out) != chunk) {
                error = "Unable to write extracted file.";
                return false;
            }
            pos += chunk;
            offset += chunk;
            if (progress)
                progress(chunk);
            if (pos >= ranges[nextRange].end && !commitActive())
                return false;
        }
        return true;
    }

    bool finish() { return commitActive(); }
};

bool folderStreamable(const CSzFolder& folder) {
    if (folder.NumCoders < 1 || folder.NumCoders > 2)
        return false;
    const UInt32 mainId = static_cast<UInt32>(folder.Coders[0].MethodID);
    if (mainId != kMethodCopy && mainId != kMethodLzma &&
        mainId != kMethodLzma2)
        return false;
    if (folder.NumCoders == 1)
        return folder.NumPackStreams == 1 && folder.NumBonds == 0;
    if (folder.NumPackStreams != 1 || folder.NumBonds != 1)
        return false;
    switch (static_cast<UInt32>(folder.Coders[1].MethodID)) {
        case kMethodDelta:
        case kMethodBcj:
        case kMethodPpc:
        case kMethodIa64:
        case kMethodArm:
        case kMethodArmt:
        case kMethodSparc:
        case kMethodArm64:
        case kMethodRiscv:
            return true;
        default:
            return false;
    }
}

bool streamFolderDecode(const CSzArEx& db, UInt32 folderIndex,
                        ILookInStreamPtr inStream, FolderByteSink& sink,
                        ISzAllocPtr alloc, std::string& error) {
    CSzFolder folder;
    CSzData sd;
    const Byte* data =
        db.db.CodersData + db.db.FoCodersOffsets[folderIndex];
    sd.Data = data;
    sd.Size = db.db.FoCodersOffsets[folderIndex + 1] -
              db.db.FoCodersOffsets[folderIndex];
    if (SzGetNextFolderItem(&folder, &sd) != SZ_OK || sd.Size != 0) {
        error = "Unable to parse a 7z folder.";
        return false;
    }
    if (!folderStreamable(folder)) {
        error = "Unsupported 7z compression for streaming extract.";
        return false;
    }

    const UInt64* packBase =
        db.db.PackPositions + db.db.FoStartPackStreamIndex[folderIndex];
    const UInt64 packOffset = packBase[0];
    const UInt64 packSize = packBase[1] - packBase[0];
    if (LookInStream_SeekTo(inStream, db.dataPos + packOffset) != SZ_OK) {
        error = "Unable to seek inside the 7z archive.";
        return false;
    }

    const CSzCoderInfo& mainCoder = folder.Coders[0];
    const bool hasFilter = folder.NumCoders == 2;
    const UInt32 filterId =
        hasFilter ? static_cast<UInt32>(folder.Coders[1].MethodID) : 0;
    UInt32 filterPc = 0;
    if (hasFilter && filterId == kMethodArm64 &&
        folder.Coders[1].PropsSize == 4)
        filterPc = GetUi32(data + folder.Coders[1].PropsOffset);
    unsigned deltaDistance = 1;
    Byte deltaState[DELTA_STATE_SIZE];
    if (hasFilter && filterId == kMethodDelta) {
        if (folder.Coders[1].PropsSize != 1) {
            error = "Unsupported delta filter.";
            return false;
        }
        deltaDistance =
            static_cast<unsigned>(data[folder.Coders[1].PropsOffset]) + 1;
        Delta_Init(deltaState);
    }
    UInt32 bcjState = Z7_BRANCH_CONV_ST_X86_STATE_INIT_VAL;

    auto emitFiltered = [&](Byte* buf, size_t size) -> bool {
        if (!hasFilter)
            return sink.write(buf, size);
        if (filterId == kMethodDelta) {
            Delta_Decode(deltaState, deltaDistance, buf, size);
            return sink.write(buf, size);
        }
        if (filterId == kMethodBcj) {
            z7_BranchConvSt_X86_Dec(buf, size, filterPc, &bcjState);
            filterPc += static_cast<UInt32>(size);
            return sink.write(buf, size);
        }
        // Branch filters want alignment; keep a tiny carry for the tail.
        return false; // handled below with carry buffer for aligned filters
    };

    std::vector<Byte> carry;
    auto emitAlignedFilter = [&](Byte* buf, size_t size) -> bool {
        if (!hasFilter || filterId == kMethodDelta || filterId == kMethodBcj)
            return emitFiltered(buf, size);
        carry.insert(carry.end(), buf, buf + size);
        size_t align = 1;
        if (filterId == kMethodArm64 || filterId == kMethodArm ||
            filterId == kMethodPpc || filterId == kMethodSparc)
            align = 4;
        else if (filterId == kMethodArmt || filterId == kMethodRiscv)
            align = 2;
        else if (filterId == kMethodIa64)
            align = 16;
        const size_t usable = carry.size() - (carry.size() % align);
        if (usable == 0)
            return true;
        if (filterId == kMethodArm64)
            z7_BranchConv_ARM64_Dec(carry.data(), usable, filterPc);
        else if (filterId == kMethodArm)
            z7_BranchConv_ARM_Dec(carry.data(), usable, filterPc);
        else if (filterId == kMethodArmt)
            z7_BranchConv_ARMT_Dec(carry.data(), usable, filterPc);
        else if (filterId == kMethodPpc)
            z7_BranchConv_PPC_Dec(carry.data(), usable, filterPc);
        else if (filterId == kMethodIa64)
            z7_BranchConv_IA64_Dec(carry.data(), usable, filterPc);
        else if (filterId == kMethodSparc)
            z7_BranchConv_SPARC_Dec(carry.data(), usable, filterPc);
        else if (filterId == kMethodRiscv)
            z7_BranchConv_RISCV_Dec(carry.data(), usable, filterPc);
        else {
            error = "Unsupported 7z branch filter.";
            return false;
        }
        filterPc += static_cast<UInt32>(usable);
        if (!sink.write(carry.data(), usable))
            return false;
        carry.erase(carry.begin(),
                    carry.begin() + static_cast<std::ptrdiff_t>(usable));
        return true;
    };

    auto flushCarry = [&]() -> bool {
        if (carry.empty())
            return true;
        return sink.write(carry.data(), carry.size());
    };

    UInt64 inRemaining = packSize;
    const UInt64 unpackTotal = SzAr_GetFolderUnpackSize(&db.db, folderIndex);

    if (mainCoder.MethodID == kMethodCopy) {
        if (packSize != unpackTotal) {
            error = "7z copy folder size mismatch.";
            return false;
        }
        std::vector<Byte> buf(kStreamOutChunkBytes);
        while (inRemaining > 0) {
            if (sink.cancelled &&
                sink.cancelled->load(std::memory_order_relaxed)) {
                error = "Cancelled.";
                return false;
            }
            size_t look = static_cast<size_t>(
                std::min<UInt64>(inRemaining, buf.size()));
            const void* inBuf = nullptr;
            size_t got = look;
            if (ILookInStream_Look(inStream, &inBuf, &got) != SZ_OK ||
                got == 0) {
                error = "Truncated 7z pack stream.";
                return false;
            }
            std::memcpy(buf.data(), inBuf, got);
            if (ILookInStream_Skip(inStream, got) != SZ_OK) {
                error = "Unable to read 7z pack stream.";
                return false;
            }
            inRemaining -= got;
            if (!emitAlignedFilter(buf.data(), got)) {
                if (error.empty())
                    error = sink.error;
                return false;
            }
        }
        if (!flushCarry() || !sink.finish()) {
            error = sink.error.empty() ? "Unable to finish extract."
                                       : sink.error;
            return false;
        }
        return true;
    }

    if (mainCoder.MethodID == kMethodLzma2) {
        if (mainCoder.PropsSize != 1) {
            error = "Invalid LZMA2 properties.";
            return false;
        }
        CLzma2Dec state;
        Lzma2Dec_CONSTRUCT(&state)
        if (Lzma2Dec_Allocate(&state, data[mainCoder.PropsOffset], alloc) !=
            SZ_OK) {
            error = "Unable to allocate LZMA2 decoder.";
            return false;
        }
        Lzma2Dec_Init(&state);
        std::vector<Byte> outBuf(kStreamOutChunkBytes);
        UInt64 unpacked = 0;
        bool ok = true;
        while (ok && unpacked < unpackTotal) {
            if (sink.cancelled &&
                sink.cancelled->load(std::memory_order_relaxed)) {
                error = "Cancelled.";
                ok = false;
                break;
            }
            const void* inBuf = nullptr;
            size_t look = 1 << 18;
            if (look > inRemaining)
                look = static_cast<size_t>(inRemaining);
            SRes lookRes = ILookInStream_Look(inStream, &inBuf, &look);
            if (lookRes != SZ_OK) {
                error = "Unable to read 7z pack stream.";
                ok = false;
                break;
            }
            SizeT inProcessed = static_cast<SizeT>(look);
            SizeT outProcessed = static_cast<SizeT>(
                std::min<UInt64>(outBuf.size(), unpackTotal - unpacked));
            ELzmaStatus status = LZMA_STATUS_NOT_SPECIFIED;
            const ELzmaFinishMode finish = unpacked + outProcessed == unpackTotal
                ? LZMA_FINISH_END
                : LZMA_FINISH_ANY;
            const SRes dr = Lzma2Dec_DecodeToBuf(
                &state, outBuf.data(), &outProcessed,
                static_cast<const Byte*>(inBuf), &inProcessed, finish,
                &status);
            if (dr != SZ_OK) {
                error = "LZMA2 decode failed.";
                ok = false;
                break;
            }
            if (inProcessed > 0) {
                if (ILookInStream_Skip(inStream, inProcessed) != SZ_OK) {
                    error = "Unable to read 7z pack stream.";
                    ok = false;
                    break;
                }
                inRemaining -= inProcessed;
            }
            if (outProcessed > 0) {
                if (!emitAlignedFilter(outBuf.data(), outProcessed)) {
                    if (error.empty())
                        error = sink.error;
                    ok = false;
                    break;
                }
                unpacked += outProcessed;
            }
            if (inProcessed == 0 && outProcessed == 0) {
                if (status == LZMA_STATUS_FINISHED_WITH_MARK ||
                    status == LZMA_STATUS_MAYBE_FINISHED_WITHOUT_MARK)
                    break;
                error = "LZMA2 decode stalled.";
                ok = false;
                break;
            }
        }
        Lzma2Dec_Free(&state, alloc);
        if (!ok)
            return false;
        if (unpacked != unpackTotal) {
            error = "LZMA2 output size mismatch.";
            return false;
        }
        if (!flushCarry() || !sink.finish()) {
            error = sink.error.empty() ? "Unable to finish extract."
                                       : sink.error;
            return false;
        }
        return true;
    }

    if (mainCoder.MethodID == kMethodLzma) {
        CLzmaDec state;
        LzmaDec_CONSTRUCT(&state)
        if (LzmaDec_Allocate(&state, data + mainCoder.PropsOffset,
                             mainCoder.PropsSize, alloc) != SZ_OK) {
            error = "Unable to allocate LZMA decoder.";
            return false;
        }
        LzmaDec_Init(&state);
        std::vector<Byte> outBuf(kStreamOutChunkBytes);
        UInt64 unpacked = 0;
        bool ok = true;
        while (ok && unpacked < unpackTotal) {
            if (sink.cancelled &&
                sink.cancelled->load(std::memory_order_relaxed)) {
                error = "Cancelled.";
                ok = false;
                break;
            }
            const void* inBuf = nullptr;
            size_t look = 1 << 18;
            if (look > inRemaining)
                look = static_cast<size_t>(inRemaining);
            if (ILookInStream_Look(inStream, &inBuf, &look) != SZ_OK) {
                error = "Unable to read 7z pack stream.";
                ok = false;
                break;
            }
            SizeT inProcessed = static_cast<SizeT>(look);
            SizeT outProcessed = static_cast<SizeT>(
                std::min<UInt64>(outBuf.size(), unpackTotal - unpacked));
            ELzmaStatus status = LZMA_STATUS_NOT_SPECIFIED;
            const ELzmaFinishMode finish = unpacked + outProcessed == unpackTotal
                ? LZMA_FINISH_END
                : LZMA_FINISH_ANY;
            const SRes dr = LzmaDec_DecodeToBuf(
                &state, outBuf.data(), &outProcessed,
                static_cast<const Byte*>(inBuf), &inProcessed, finish,
                &status);
            if (dr != SZ_OK) {
                error = "LZMA decode failed.";
                ok = false;
                break;
            }
            if (inProcessed > 0) {
                if (ILookInStream_Skip(inStream, inProcessed) != SZ_OK) {
                    error = "Unable to read 7z pack stream.";
                    ok = false;
                    break;
                }
                inRemaining -= inProcessed;
            }
            if (outProcessed > 0) {
                if (!emitAlignedFilter(outBuf.data(), outProcessed)) {
                    if (error.empty())
                        error = sink.error;
                    ok = false;
                    break;
                }
                unpacked += outProcessed;
            }
            if (inProcessed == 0 && outProcessed == 0) {
                if (status == LZMA_STATUS_FINISHED_WITH_MARK ||
                    status == LZMA_STATUS_MAYBE_FINISHED_WITHOUT_MARK)
                    break;
                error = "LZMA decode stalled.";
                ok = false;
                break;
            }
        }
        LzmaDec_Free(&state, alloc);
        if (!ok)
            return false;
        if (unpacked != unpackTotal) {
            error = "LZMA output size mismatch.";
            return false;
        }
        if (!flushCarry() || !sink.finish()) {
            error = sink.error.empty() ? "Unable to finish extract."
                                       : sink.error;
            return false;
        }
        return true;
    }

    error = "Unsupported 7z method for streaming extract.";
    return false;
}

bool extract7zRam(CSzArEx& db, ILookInStreamPtr inStream,
                  const std::string& targetRoot,
                  const PortArchiveProbe& probe,
                  const std::atomic<bool>& cancelled,
                  const std::function<void(uint64_t)>& progress,
                  const std::function<void(const std::string&)>& currentFile,
                  ISzAllocPtr allocMain, ISzAllocPtr allocTemp,
                  std::string& error) {
    UInt32 blockIndex = 0xFFFFFFFF;
    Byte* outBuffer = nullptr;
    size_t outBufferSize = 0;
    std::vector<UInt16> nameBuf;
    bool ok = true;
    size_t extracted = 0;
    for (UInt32 i = 0; i < db.NumFiles; ++i) {
        if (cancelled.load(std::memory_order_relaxed)) {
            error = "Cancelled.";
            ok = false;
            break;
        }
        if (SzArEx_IsDir(&db, i))
            continue;
        const size_t nameLen = SzArEx_GetFileNameUtf16(&db, i, nullptr);
        nameBuf.resize(nameLen);
        SzArEx_GetFileNameUtf16(&db, i, nameBuf.data());
        const std::string member = utf16ToUtf8(nameBuf.data(), nameLen);
        const std::string relative = archiveDestination(probe, member);
        if (relative.empty())
            continue;
        if (currentFile)
            currentFile(relative);
        size_t offset = 0;
        size_t outSizeProcessed = 0;
        const SRes res =
            SzArEx_Extract(&db, inStream, i, &blockIndex, &outBuffer,
                           &outBufferSize, &offset, &outSizeProcessed,
                           allocMain, allocTemp);
        if (res != SZ_OK) {
            error = res == SZ_ERROR_MEM
                ? "Not enough free RAM to extract this 7z solid block."
                : "Unable to extract a 7z entry.";
            ok = false;
            break;
        }
        if (!writeBytes(targetRoot + "/" + relative, outBuffer + offset,
                        outSizeProcessed, cancelled, progress, error)) {
            ok = false;
            break;
        }
        ++extracted;
    }
    ISzAlloc_Free(allocMain, outBuffer);
    if (ok && extracted == 0) {
        error = "The archive has no switch/ files to extract.";
        return false;
    }
    return ok;
}

bool extract7z(const std::string& archivePath, const std::string& targetRoot,
               const PortArchiveProbe& probe,
               const std::atomic<bool>& cancelled,
               const std::function<void(uint64_t)>& progress,
               const std::function<void(const std::string&)>& currentFile,
               std::string& error) {
    CFileInStream archiveStream;
    CLookToRead2 lookStream;
    CSzArEx db;
    ISzAlloc allocImp = {SzAlloc, SzFree};
    ISzAlloc allocTempImp = {SzAlloc, SzFree};
    if (InFile_Open(&archiveStream.file, archivePath.c_str()) != 0) {
        error = "Unable to open 7z archive.";
        return false;
    }
    FileInStream_CreateVTable(&archiveStream);
    archiveStream.wres = 0;
    LookToRead2_CreateVTable(&lookStream, False);
    lookStream.buf = nullptr;
    lookStream.buf = static_cast<Byte*>(ISzAlloc_Alloc(&allocImp, kLookBufBytes));
    if (!lookStream.buf) {
        File_Close(&archiveStream.file);
        error = "Out of memory opening 7z archive.";
        return false;
    }
    lookStream.bufSize = kLookBufBytes;
    lookStream.realStream = &archiveStream.vt;
    LookToRead2_INIT(&lookStream)
    CrcGenerateTable();
    SzArEx_Init(&db);
    SRes res = SzArEx_Open(&db, &lookStream.vt, &allocImp, &allocTempImp);
    if (res != SZ_OK) {
        SzArEx_Free(&db, &allocImp);
        ISzAlloc_Free(&allocImp, lookStream.buf);
        File_Close(&archiveStream.file);
        error = "Unable to parse 7z archive.";
        return false;
    }

    uint64_t maxSolid = 0;
    for (UInt32 folder = 0; folder < db.db.NumFolders; ++folder) {
        const UInt64 unpack = SzAr_GetFolderUnpackSize(&db.db, folder);
        if (unpack > maxSolid)
            maxSolid = unpack;
    }
    const bool preferStream = maxSolid >= kSolidStreamPreferBytes;

    struct WantedFile {
        UInt32 folderIndex = 0;
        SwitchOutRange range;
    };
    std::vector<WantedFile> files;
    std::vector<UInt16> nameBuf;
    for (UInt32 i = 0; i < db.NumFiles; ++i) {
        if (SzArEx_IsDir(&db, i))
            continue;
        const size_t nameLen = SzArEx_GetFileNameUtf16(&db, i, nullptr);
        nameBuf.resize(nameLen);
        SzArEx_GetFileNameUtf16(&db, i, nameBuf.data());
        const std::string member = utf16ToUtf8(nameBuf.data(), nameLen);
        const std::string relative = archiveDestination(probe, member);
        if (relative.empty())
            continue;
        const UInt32 folderIndex = db.FileToFolder[i];
        if (folderIndex == static_cast<UInt32>(-1))
            continue;
        WantedFile file;
        file.folderIndex = folderIndex;
        file.range.start = db.UnpackPositions[i] -
                           db.UnpackPositions[db.FolderToFile[folderIndex]];
        file.range.end = file.range.start + SzArEx_GetFileSize(&db, i);
        file.range.relative = relative;
        file.range.absolute = targetRoot + "/" + relative;
        files.push_back(std::move(file));
    }

    bool ok = true;
    if (files.empty()) {
        error = "The archive has no switch/ files to extract.";
        ok = false;
    } else if (!preferStream) {
        ok = extract7zRam(db, &lookStream.vt, targetRoot, probe, cancelled,
                          progress, currentFile, &allocImp, &allocTempImp,
                          error);
    } else {
        std::sort(files.begin(), files.end(),
                  [](const WantedFile& a, const WantedFile& b) {
                      if (a.folderIndex != b.folderIndex)
                          return a.folderIndex < b.folderIndex;
                      return a.range.start < b.range.start;
                  });
        size_t index = 0;
        while (ok && index < files.size()) {
            const UInt32 folderIndex = files[index].folderIndex;
            FolderByteSink sink;
            sink.cancelled = &cancelled;
            sink.progress = progress;
            sink.currentFile = currentFile;
            while (index < files.size() &&
                   files[index].folderIndex == folderIndex) {
                sink.ranges.push_back(files[index].range);
                ++index;
            }
            CSzFolder folder;
            CSzData sd;
            const Byte* coderData =
                db.db.CodersData + db.db.FoCodersOffsets[folderIndex];
            sd.Data = coderData;
            sd.Size = db.db.FoCodersOffsets[folderIndex + 1] -
                      db.db.FoCodersOffsets[folderIndex];
            if (SzGetNextFolderItem(&folder, &sd) != SZ_OK ||
                !folderStreamable(folder)) {
                Byte* outBuffer = nullptr;
                size_t outBufferSize = 0;
                UInt32 blockIndex = 0xFFFFFFFF;
                for (UInt32 i = 0; i < db.NumFiles && ok; ++i) {
                    if (SzArEx_IsDir(&db, i) ||
                        db.FileToFolder[i] != folderIndex)
                        continue;
                    const size_t nameLen =
                        SzArEx_GetFileNameUtf16(&db, i, nullptr);
                    nameBuf.resize(nameLen);
                    SzArEx_GetFileNameUtf16(&db, i, nameBuf.data());
                    const std::string member =
                        utf16ToUtf8(nameBuf.data(), nameLen);
                    const std::string relative =
                        archiveDestination(probe, member);
                    if (relative.empty())
                        continue;
                    if (currentFile)
                        currentFile(relative);
                    size_t offset = 0;
                    size_t outSizeProcessed = 0;
                    res = SzArEx_Extract(&db, &lookStream.vt, i, &blockIndex,
                                         &outBuffer, &outBufferSize, &offset,
                                         &outSizeProcessed, &allocImp,
                                         &allocTempImp);
                    if (res != SZ_OK) {
                        error = res == SZ_ERROR_MEM
                            ? "Not enough free RAM to extract this 7z solid "
                              "block."
                            : "Unable to extract a 7z entry.";
                        ok = false;
                        break;
                    }
                    if (!writeBytes(targetRoot + "/" + relative,
                                    outBuffer + offset, outSizeProcessed,
                                    cancelled, progress, error)) {
                        ok = false;
                        break;
                    }
                }
                ISzAlloc_Free(&allocImp, outBuffer);
                continue;
            }
            if (!streamFolderDecode(db, folderIndex, &lookStream.vt, sink,
                                    &allocImp, error))
                ok = false;
        }
    }

    SzArEx_Free(&db, &allocImp);
    ISzAlloc_Free(&allocImp, lookStream.buf);
    File_Close(&archiveStream.file);
    return ok;
}

bool probe7z(const std::string& archivePath, PortArchiveProbe& out) {
    CFileInStream archiveStream;
    CLookToRead2 lookStream;
    CSzArEx db;
    ISzAlloc allocImp = {SzAlloc, SzFree};
    ISzAlloc allocTempImp = {SzAlloc, SzFree};
    if (InFile_Open(&archiveStream.file, archivePath.c_str()) != 0) {
        out.error = "Unable to open 7z archive.";
        return false;
    }
    FileInStream_CreateVTable(&archiveStream);
    archiveStream.wres = 0;
    LookToRead2_CreateVTable(&lookStream, False);
    lookStream.buf = static_cast<Byte*>(ISzAlloc_Alloc(&allocImp, kLookBufBytes));
    if (!lookStream.buf) {
        File_Close(&archiveStream.file);
        out.error = "Out of memory opening 7z archive.";
        return false;
    }
    lookStream.bufSize = kLookBufBytes;
    lookStream.realStream = &archiveStream.vt;
    LookToRead2_INIT(&lookStream)
    CrcGenerateTable();
    SzArEx_Init(&db);
    const SRes res = SzArEx_Open(&db, &lookStream.vt, &allocImp, &allocTempImp);
    if (res != SZ_OK) {
        SzArEx_Free(&db, &allocImp);
        ISzAlloc_Free(&allocImp, lookStream.buf);
        File_Close(&archiveStream.file);
        out.error = "Unable to parse 7z archive.";
        return false;
    }
    for (UInt32 folder = 0; folder < db.db.NumFolders; ++folder) {
        const UInt64 unpack = SzAr_GetFolderUnpackSize(&db.db, folder);
        if (unpack > out.maxSolidBlockBytes)
            out.maxSolidBlockBytes = unpack;
    }
    std::vector<UInt16> nameBuf;
    std::vector<RawArchiveFile> raw;
    for (UInt32 i = 0; i < db.NumFiles; ++i) {
        if (SzArEx_IsDir(&db, i))
            continue;
        const size_t nameLen = SzArEx_GetFileNameUtf16(&db, i, nullptr);
        nameBuf.resize(nameLen);
        SzArEx_GetFileNameUtf16(&db, i, nameBuf.data());
        raw.push_back({utf16ToUtf8(nameBuf.data(), nameLen),
                       SzArEx_GetFileSize(&db, i)});
    }
    const bool mapped = buildArchiveMapping(raw, out);
    SzArEx_Free(&db, &allocImp);
    ISzAlloc_Free(&allocImp, lookStream.buf);
    File_Close(&archiveStream.file);
    return mapped;
}

} // namespace

bool probePortArchive(const std::string& archivePath, PortArchiveProbe& out) {
    out = {};
    struct stat st {};
    if (lstat(archivePath.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
        out.error = "Archive is missing.";
        return false;
    }
    out.packedBytes = static_cast<uint64_t>(st.st_size);
    const std::string base = lowerAscii(basenameOf(archivePath));
    if (base == "switch.zip" || endsWithCi(archivePath, ".zip")) {
        out.ok = probeZip(archivePath, out);
        return out.ok;
    }
    if (base == "switch.7z" || endsWithCi(archivePath, ".7z")) {
        out.ok = probe7z(archivePath, out);
        return out.ok;
    }
    out.error = "Unsupported port archive type.";
    return false;
}

bool extractPortArchive(const std::string& archivePath,
                        const std::string& targetRoot,
                        const std::atomic<bool>& cancelled,
                        const std::function<void(uint64_t)>& progress,
                        const std::function<void(const std::string&)>& currentFile,
                        std::string& error) {
    PortArchiveProbe probe;
    if (!probePortArchive(archivePath, probe)) {
        error = probe.error;
        return false;
    }
    const std::string base = lowerAscii(basenameOf(archivePath));
    if (base == "switch.zip" || endsWithCi(archivePath, ".zip"))
        return extractZip(archivePath, targetRoot, probe, cancelled, progress,
                          currentFile, error);
    if (base == "switch.7z" || endsWithCi(archivePath, ".7z"))
        return extract7z(archivePath, targetRoot, probe, cancelled, progress,
                         currentFile, error);
    error = "Unsupported port archive type.";
    return false;
}

} // namespace pipensx
