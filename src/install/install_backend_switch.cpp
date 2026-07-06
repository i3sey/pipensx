#include "install_backend.hpp"

#ifdef __SWITCH__

extern "C" {
#include "../core/util.h"
}

#include <switch.h>
#include <switch-ipcext.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
#include <type_traits>
#include <unistd.h>
#include <utility>
#include <vector>

namespace pipensx::install {
namespace {

constexpr const char* TempRoot = "sdmc:/switch/pipensx/install-temp";

// PERF_PLAN 7.4: install target is selectable so NAND (eMMC) can be measured
// against the ~16 MB/s SD write ceiling. NcmContentStorage APIs act on the
// opened handle, so free-space and placeholder writes follow the target with
// no path changes; only the storage id, the record storage id, and the
// telemetry `target=` string differ.
NcmStorageId ncmStorageId(InstallStorageTarget target) {
    return target == InstallStorageTarget::Nand ? NcmStorageId_BuiltInUser
                                                : NcmStorageId_SdCard;
}

const char* targetName(InstallStorageTarget target) {
    return target == InstallStorageTarget::Nand ? "nand" : "sd";
}

bool makeDirectories(const std::string& path) {
    char buffer[FS_MAX_PATH];
    if (path.empty() || path.size() >= sizeof(buffer))
        return false;
    std::snprintf(buffer, sizeof(buffer), "%s", path.c_str());
    for (char* p = buffer + 1; *p; ++p) {
        if (*p != '/')
            continue;
        *p = '\0';
        if (mkdir(buffer, 0755) != 0 && errno != EEXIST)
            return false;
        *p = '/';
    }
    return mkdir(buffer, 0755) == 0 || errno == EEXIST;
}

bool removeTree(const std::string& path) {
    struct stat st {};
    if (lstat(path.c_str(), &st) != 0)
        return errno == ENOENT;
    if (!S_ISDIR(st.st_mode))
        return unlink(path.c_str()) == 0;
    DIR* dir = opendir(path.c_str());
    if (!dir)
        return false;
    bool ok = true;
    while (dirent* entry = readdir(dir)) {
        if (!std::strcmp(entry->d_name, ".") ||
            !std::strcmp(entry->d_name, ".."))
            continue;
        if (!removeTree(path + "/" + entry->d_name))
            ok = false;
    }
    closedir(dir);
    return ok && rmdir(path.c_str()) == 0;
}

bool parseContentId(const std::string& name, NcmContentId& id) {
    std::string base = name;
    const std::string suffix = ".cnmt.nca";
    if (base.size() > suffix.size() &&
        base.substr(base.size() - suffix.size()) == suffix)
        base.resize(base.size() - suffix.size());
    else if (base.size() > 4 && base.substr(base.size() - 4) == ".nca")
        base.resize(base.size() - 4);
    else
        return false;
    if (base.size() != 32)
        return false;
    for (size_t i = 0; i < 16; ++i) {
        char pair[3] { base[i * 2], base[i * 2 + 1], 0 };
        char* end = nullptr;
        unsigned long value = std::strtoul(pair, &end, 16);
        if (!end || *end)
            return false;
        id.c[i] = static_cast<uint8_t>(value);
    }
    return true;
}

bool containsContentId(const std::vector<NcmContentId>& ids,
                       const NcmContentId& id) {
    for (const auto& candidate : ids)
        if (std::memcmp(candidate.c, id.c, sizeof(id.c)) == 0)
            return true;
    return false;
}

std::string hexBytes(const uint8_t* data, size_t size) {
    static constexpr char Digits[] = "0123456789abcdef";
    std::string result(size * 2, '0');
    for (size_t i = 0; i < size; ++i) {
        result[i * 2] = Digits[data[i] >> 4];
        result[i * 2 + 1] = Digits[data[i] & 0x0f];
    }
    return result;
}

// F-B journal blob helpers: little-endian, length-prefixed.
void putU64(std::string& out, uint64_t value) {
    for (int i = 0; i < 8; ++i)
        out.push_back(static_cast<char>((value >> (i * 8)) & 0xff));
}

void putRaw(std::string& out, const void* data, size_t size) {
    out.append(static_cast<const char*>(data), size);
}

void putStr(std::string& out, const std::string& value) {
    putU64(out, value.size());
    out.append(value);
}

void putBytes(std::string& out, const std::vector<uint8_t>& value) {
    putU64(out, value.size());
    out.append(reinterpret_cast<const char*>(value.data()), value.size());
}

struct BlobReader {
    const char* data = nullptr;
    size_t size = 0;
    size_t pos = 0;

    bool raw(void* out, size_t count) {
        if (size - pos < count)
            return false;
        std::memcpy(out, data + pos, count);
        pos += count;
        return true;
    }
    bool u8(uint8_t& value) { return raw(&value, 1); }
    bool u64(uint64_t& value) {
        uint8_t bytes8[8];
        if (!raw(bytes8, sizeof(bytes8)))
            return false;
        value = 0;
        for (int i = 0; i < 8; ++i)
            value |= static_cast<uint64_t>(bytes8[i]) << (i * 8);
        return true;
    }
    bool str(std::string& value) {
        uint64_t count = 0;
        if (!u64(count) || size - pos < count)
            return false;
        value.assign(data + pos, static_cast<size_t>(count));
        pos += static_cast<size_t>(count);
        return true;
    }
    bool bytes(std::vector<uint8_t>& value) {
        uint64_t count = 0;
        if (!u64(count) || size - pos < count)
            return false;
        const auto* begin = reinterpret_cast<const uint8_t*>(data + pos);
        value.assign(begin, begin + static_cast<size_t>(count));
        pos += static_cast<size_t>(count);
        return true;
    }
    bool done() const { return pos == size; }
};

uint64_t baseApplicationId(uint64_t id, NcmContentMetaType type) {
    if (type == NcmContentMetaType_Patch)
        return id ^ 0x800;
    if (type == NcmContentMetaType_AddOnContent)
        return (id ^ 0x1000) & ~0xFFFULL;
    return id;
}

struct Content {
    std::string name;
    NcmContentId id {};
    NcmPlaceHolderId placeholder {};
    uint64_t size = 0;
    uint64_t written = 0;
    bool existing = false;
    bool registered = false;
    bool meta = false;
};

struct ParsedMeta {
    NcmExtPackagedContentMetaHeader header {};
    std::vector<uint8_t> extendedHeader;
    std::vector<NcmPackagedContentInfo> contents;   // delta fragments removed
    std::vector<NcmContentMetaInfo> metaInfos;
    std::vector<NcmContentId> deltaIds;             // content ids we skip
    uint32_t extendedDataSize = 0;                  // patch delta history size
};

bool readPackagedMeta(const std::string& ncaPath, ParsedMeta& out,
                      std::string& error) {
    FsFileSystem filesystem {};
    Result rc = fsOpenFileSystemWithId(&filesystem, 0,
        FsFileSystemType_ContentMeta, ncaPath.c_str(),
        FsContentAttributes_All);
    if (R_FAILED(rc)) {
        char text[96];
        std::snprintf(text, sizeof(text),
                      "Unable to open CNMT NCA (0x%08x).", rc);
        error = text;
        return false;
    }

    FsDir dir {};
    rc = fsFsOpenDirectory(&filesystem, "/",
                           FsDirOpenMode_ReadFiles, &dir);
    if (R_FAILED(rc)) {
        fsFsClose(&filesystem);
        error = "Unable to list CNMT filesystem.";
        return false;
    }
    FsDirectoryEntry entry {};
    s64 count = 0;
    std::string cnmtName;
    uint64_t cnmtSize = 0;
    while (R_SUCCEEDED(fsDirRead(&dir, &count, 1, &entry)) && count == 1) {
        std::string name = entry.name;
        if (name.size() >= 5 && name.substr(name.size() - 5) == ".cnmt") {
            cnmtName = "/" + name;
            cnmtSize = static_cast<uint64_t>(entry.file_size);
            break;
        }
    }
    fsDirClose(&dir);
    if (cnmtName.empty() || cnmtSize < sizeof(out.header) ||
        cnmtSize > 16 * 1024 * 1024) {
        fsFsClose(&filesystem);
        error = "CNMT file is missing or invalid.";
        return false;
    }

    FsFile file {};
    rc = fsFsOpenFile(&filesystem, cnmtName.c_str(), FsOpenMode_Read, &file);
    if (R_FAILED(rc)) {
        fsFsClose(&filesystem);
        error = "Unable to open CNMT file.";
        return false;
    }
    std::vector<uint8_t> data(cnmtSize);
    uint64_t read = 0;
    rc = fsFileRead(&file, 0, data.data(), data.size(),
                    FsReadOption_None, &read);
    fsFileClose(&file);
    fsFsClose(&filesystem);
    if (R_FAILED(rc) || read != data.size()) {
        error = "Unable to read CNMT file.";
        return false;
    }

    std::memcpy(&out.header, data.data(), sizeof(out.header));
    NcmContentMetaType type =
        static_cast<NcmContentMetaType>(out.header.type);
    if (type != NcmContentMetaType_Application &&
        type != NcmContentMetaType_Patch &&
        type != NcmContentMetaType_AddOnContent) {
        error = "Only games, updates and DLC are supported.";
        return false;
    }
    uint64_t required = sizeof(out.header) +
        out.header.extended_header_size +
        static_cast<uint64_t>(out.header.content_count) *
            sizeof(NcmPackagedContentInfo) +
        static_cast<uint64_t>(out.header.content_meta_count) *
            sizeof(NcmContentMetaInfo);
    if (required > data.size()) {
        error = "CNMT content table is truncated.";
        return false;
    }
    const uint8_t* cursor = data.data() + sizeof(out.header);
    out.extendedHeader.assign(cursor,
        cursor + out.header.extended_header_size);
    cursor += out.header.extended_header_size;
    std::vector<NcmPackagedContentInfo> packaged(out.header.content_count);
    std::memcpy(packaged.data(), cursor,
                packaged.size() * sizeof(NcmPackagedContentInfo));
    cursor += packaged.size() * sizeof(NcmPackagedContentInfo);
    out.metaInfos.resize(out.header.content_meta_count);
    std::memcpy(out.metaInfos.data(), cursor,
                out.metaInfos.size() * sizeof(NcmContentMetaInfo));

    // Update packages bundle delta fragments alongside the full NCAs. They are
    // not installed for a fresh, full install (matching Awoo/Tinfoil); keep
    // them out of the registered content and the stored meta, but remember
    // their ids so the streamed placeholders can be discarded.
    for (const auto& content : packaged) {
        if (content.info.content_type == NcmContentType_DeltaFragment)
            out.deltaIds.push_back(content.info.content_id);
        else
            out.contents.push_back(content);
    }

    // A patch meta carries delta history as extended data after the records.
    // The installed meta reserves the same space (zero-filled) so ns accepts
    // it, even though we drop the deltas themselves.
    if (type == NcmContentMetaType_Patch &&
        out.extendedHeader.size() >= sizeof(NcmPatchMetaExtendedHeader)) {
        NcmPatchMetaExtendedHeader patchHeader {};
        std::memcpy(&patchHeader, out.extendedHeader.data(),
                    sizeof(patchHeader));
        out.extendedDataSize = patchHeader.extended_data_size;
    }
    return true;
}

class SwitchInstallBackend final : public InstallBackend {
public:
    SwitchInstallBackend(std::string root, InstallStorageTarget target)
        : root_(std::move(root)),
          storageId_(ncmStorageId(target)),
          targetName_(targetName(target)) {}

    ~SwitchInstallBackend() override { rollbackPackage(); }

    bool beginPackage(const std::string& taskId,
                      const std::string& packageName) override {
        rollbackPackage();
        error_.clear();
        taskId_ = taskId;
        packageStartedMs_ = now_ms();
        tempDirectory_ = std::string(TempRoot) + "/" + taskId;
        removeTree(tempDirectory_);
        if (!makeDirectories(tempDirectory_)) {
            error_ = "Unable to create installation workspace.";
            return false;
        }
        Result rc = ncmOpenContentStorage(&storage_, storageId_);
        if (R_SUCCEEDED(rc))
            rc = ncmOpenContentMetaDatabase(&database_, storageId_);
        if (R_FAILED(rc)) {
            errorResult("Unable to open content storage", rc);
            closeServices();
            return false;
        }
        active_ = true;
        packageName_ = packageName;
        log_msg("[install] package begin '%s'\n", packageName_.c_str());
        telemetry_log("ncm", taskId_.c_str(),
                      "event=package_begin target=%s", targetName_);
        return true;
    }

    bool beginFile(const std::string& name, uint64_t size) override {
        if (!active_ || current_) {
            error_ = "Invalid installer file state.";
            return false;
        }
        currentName_ = name;
        bool ticket = name.size() >= 4 &&
                      name.substr(name.size() - 4) == ".tik";
        bool certificate = name.size() >= 5 &&
                           name.substr(name.size() - 5) == ".cert";
        if (ticket || certificate) {
            auxiliary_.clear();
            auxiliaryExpected_ = size;
            auxiliaryKind_ = ticket ? ".tik" : ".cert";
            expected_ += size;
            return true;
        }
        NcmContentId id {};
        if (!parseContentId(name, id)) {
            ignoredRemaining_ = size;
            return true;
        }
        contents_.push_back({});
        current_ = &contents_.back();
        current_->name = name;
        current_->id = id;
        current_->meta = name.size() >= 9 &&
                         name.substr(name.size() - 9) == ".cnmt.nca";
        resetFileTelemetry(now_ms());
        if (size == 0) {
            log_msg("[install] NCA begin '%s' size=pending id=%s meta=%d\n",
                    name.c_str(), hexBytes(id.c, sizeof(id.c)).c_str(),
                    current_->meta ? 1 : 0);
        } else {
            log_msg("[install] NCA begin '%s' size=%llu id=%s meta=%d\n",
                    name.c_str(), static_cast<unsigned long long>(size),
                    hexBytes(id.c, sizeof(id.c)).c_str(),
                    current_->meta ? 1 : 0);
        }
        return size == 0 || setFileSize(size);
    }

    bool setFileSize(uint64_t size) override {
        if (!current_ || current_->size != 0 || size == 0) {
            error_ = "Invalid NCA size.";
            return false;
        }
        uint64_t setupStartedUs = telemetry_enabled() ? now_us() : 0;
        current_->size = size;
        expected_ += size;
        log_msg("[install] NCA size resolved '%s' bytes=%llu\n",
                current_->name.c_str(),
                static_cast<unsigned long long>(size));
        bool exists = false;
        Result rc = ncmContentStorageHas(&storage_, &exists, &current_->id);
        if (R_FAILED(rc)) {
            errorResult("Unable to query installed content", rc);
            return false;
        }
        current_->existing = exists;
        if (!exists) {
            s64 freeSpace = 0;
            rc = ncmContentStorageGetFreeSpaceSize(&storage_, &freeSpace);
            if (R_FAILED(rc)) {
                errorResult("Unable to query free content space", rc);
                return false;
            }
            if (freeSpace < 0 || static_cast<uint64_t>(freeSpace) < size) {
                char required[32];
                char available[32];
                fmt_bytes(required, sizeof(required), size);
                fmt_bytes(available, sizeof(available),
                          freeSpace > 0 ? static_cast<uint64_t>(freeSpace) : 0);
                const char* where = storageId_ == NcmStorageId_BuiltInUser
                                        ? "system memory" : "SD card";
                error_ = std::string("Not enough ") + where + " space for " +
                         current_->name + ": need " + required + ", free " +
                         available + ".";
                log_msg("[install] insufficient space name='%s' need=%llu free=%lld\n",
                        current_->name.c_str(),
                        static_cast<unsigned long long>(size),
                        static_cast<long long>(freeSpace));
                return false;
            }
            rc = ncmContentStorageGeneratePlaceHolderId(
                &storage_, &current_->placeholder);
            if (R_SUCCEEDED(rc))
                rc = ncmContentStorageCreatePlaceHolder(
                    &storage_, &current_->id, &current_->placeholder, size);
            if (R_FAILED(rc)) {
                errorResult("Unable to create content placeholder", rc);
                return false;
            }
        }
        sha256ContextCreate(&sha_);
        hashActive_ = true;
        if (setupStartedUs) {
            uint64_t setupUs = now_us() - setupStartedUs;
            telemetry_log("ncm", taskId_.c_str(),
                "event=file_setup target=%s bytes=%llu existing=%d setup_us=%llu",
                targetName_, (unsigned long long)size,
                current_->existing ? 1 : 0,
                (unsigned long long)setupUs);
        }
        return true;
    }

    bool writeFile(const uint8_t* data, size_t size) override {
        if (!active_)
            return false;
        if (!auxiliaryKind_.empty()) {
            if (auxiliary_.size() + size > auxiliaryExpected_) {
                error_ = "Auxiliary package file exceeds declared size.";
                return false;
            }
            auxiliary_.insert(auxiliary_.end(), data, data + size);
            installed_ += size;
            return true;
        }
        if (ignoredRemaining_) {
            if (size > ignoredRemaining_)
                return false;
            ignoredRemaining_ -= size;
            return true;
        }
        if (!current_ || current_->written + size > current_->size) {
            error_ = "NCA data exceeds declared size.";
            return false;
        }
        prepareFileTelemetry();
        bool track = telemetry_enabled();
        uint64_t ncmUs = 0;
        if (!current_->existing) {
            uint64_t ncmStartedUs = track ? now_us() : 0;
            Result rc = ncmContentStorageWritePlaceHolder(
                &storage_, &current_->placeholder, current_->written,
                data, size);
            if (ncmStartedUs)
                ncmUs = now_us() - ncmStartedUs;
            if (R_FAILED(rc)) {
                errorResult("Unable to write content placeholder", rc);
                return false;
            }
        }
        uint64_t shaStartedUs = track ? now_us() : 0;
        sha256ContextUpdate(&sha_, data, size);
        uint64_t shaUs = shaStartedUs ? now_us() - shaStartedUs : 0;
        current_->written += size;
        installed_ += size;
        if (track) {
            telemetryWriteBytes_ += size;
            telemetryNcmUs_ += ncmUs;
            telemetryShaUs_ += shaUs;
            telemetryWriteCalls_++;
            telemetryNcmMaxUs_ = std::max(telemetryNcmMaxUs_, ncmUs);
            telemetryTotalWriteBytes_ += size;
            telemetryTotalNcmUs_ += ncmUs;
            telemetryTotalShaUs_ += shaUs;
            telemetryTotalWriteCalls_++;
            telemetryTotalNcmMaxUs_ = std::max(
                telemetryTotalNcmMaxUs_, ncmUs);
            if (current_->existing)
                telemetryTotalExistingBytes_ += size;
            uint64_t now = now_ms();
            if (ncmUs >= 100000 && now - telemetryLastStallLogMs_ >= 1000) {
                telemetry_log("ncm_stall", taskId_.c_str(),
                    "target=%s write_us=%llu bytes=%zu offset=%llu existing=%d",
                    targetName_, (unsigned long long)ncmUs, size,
                    (unsigned long long)(current_->written - size),
                    current_->existing ? 1 : 0);
                telemetryLastStallLogMs_ = now;
            }
            emitFileTelemetry(now, false, false);
        }
        return true;
    }

    bool endFile() override {
        if (!active_)
            return false;
        if (!auxiliaryKind_.empty()) {
            if (auxiliary_.size() != auxiliaryExpected_) {
                error_ = "Auxiliary package file is truncated.";
                return false;
            }
            if (auxiliaryKind_ == ".tik")
                ticket_ = auxiliary_;
            else
                certificate_ = auxiliary_;
            auxiliary_.clear();
            auxiliaryKind_.clear();
            auxiliaryExpected_ = 0;
            currentName_.clear();
            return true;
        }
        if (ignoredRemaining_) {
            error_ = "Ignored package file is truncated.";
            return false;
        }
        if (!current_) {
            currentName_.clear();
            return true;
        }
        if (current_->written != current_->size) {
            error_ = "NCA file is truncated.";
            return false;
        }
        std::array<uint8_t, 32> digest {};
        uint64_t finishStartedUs = telemetry_enabled() ? now_us() : 0;
        if (!hashActive_) {
            error_ = "Unable to finalize NCA hash.";
            return false;
        }
        sha256ContextGetHash(&sha_, digest.data());
        hashActive_ = false;
        if (finishStartedUs) {
            uint64_t finishUs = now_us() - finishStartedUs;
            telemetryShaUs_ += finishUs;
            telemetryTotalShaUs_ += finishUs;
        }
        if (std::memcmp(digest.data(), current_->id.c, 16) != 0) {
            // Patched/translated NCAs commonly retain the original content ID.
            // Torrent piece hashes still protect the streamed bytes in transit.
            log_msg("[install] warning: NCA hash mismatch '%s' expected=%s actual=%s\n",
                    current_->name.c_str(),
                    hexBytes(current_->id.c, sizeof(current_->id.c)).c_str(),
                    hexBytes(digest.data(), sizeof(current_->id.c)).c_str());
        }
        log_msg("[install] NCA complete '%s' bytes=%llu\n",
                current_->name.c_str(),
                static_cast<unsigned long long>(current_->written));
        emitFileTelemetry(now_ms(), true, true);
        if (current_->meta)
            prepareEarlyMeta(*current_);
        current_ = nullptr;
        currentName_.clear();
        return true;
    }

    // Delta fragments identified by the early CNMT parse are dropped by the
    // package stream before decode/AES/SHA/ncm (PERF_PLAN 3.4). Only works
    // for deltas that follow the CNMT NCA in the PFS0; earlier ones stream
    // as before and their placeholders are discarded at commit.
    bool shouldSkipFile(const std::string& name) const override {
        if (!earlyMetaValid_)
            return false;
        NcmContentId id {};
        if (!parseContentId(name, id))
            return false;
        if (!containsContentId(earlyMeta_.deltaIds, id))
            return false;
        skippedDeltaIds_.push_back(id);
        log_msg("[install] delta fragment '%s' excluded from install\n",
                name.c_str());
        return true;
    }

    bool commitPackage(bool& alreadyInstalled) override {
        uint64_t commitStartedUs = telemetry_enabled() ? now_us() : 0;
        alreadyInstalled = false;
        Content* metaContent = nullptr;
        for (auto& content : contents_)
            if (content.meta)
                metaContent = &content;
        if (!active_ || current_ || !metaContent) {
            error_ = "Package does not contain a complete CNMT NCA.";
            return false;
        }

        // FS resolves NCAs by their content-storage path, not by the libnx
        // "sdmc:" devoptab path of the on-disk copy. Register the CNMT NCA so we
        // can mount it through NCM. On an already-installed title the CNMT NCA's
        // content id already exists (existing == true), so we never register here.
        if (!metaContent->existing && !metaContent->registered) {
            Result rc = ncmContentStorageRegister(
                &storage_, &metaContent->id, &metaContent->placeholder);
            if (R_FAILED(rc)) {
                errorResult("Unable to register CNMT NCA", rc);
                return false;
            }
            metaContent->registered = true;
        }
        ParsedMeta meta;
        if (earlyMetaValid_) {
            meta = earlyMeta_;
        } else {
            char metaNcaPath[FS_MAX_PATH];
            Result pathRc = ncmContentStorageGetPath(&storage_, metaNcaPath,
                                                     sizeof(metaNcaPath),
                                                     &metaContent->id);
            if (R_FAILED(pathRc)) {
                errorResult("Unable to resolve CNMT NCA path", pathRc);
                return false;
            }
            if (!readPackagedMeta(metaNcaPath, meta, error_))
                return false;
        }
        log_msg("[install] CNMT parsed title=%016llx version=%u contents=%u\n",
                static_cast<unsigned long long>(meta.header.id),
                meta.header.version, meta.header.content_count);

        NcmContentMetaKey key {
            meta.header.id,
            meta.header.version,
            meta.header.type,
            NcmContentInstallType_Full,
            {}
        };
        bool exists = false;
        Result rc = ncmContentMetaDatabaseHas(&database_, &exists, &key);
        if (R_FAILED(rc)) {
            errorResult("Unable to query installed title metadata", rc);
            return false;
        }
        if (exists) {
            alreadyInstalled = true;
            discardPlaceholders();
            finishSuccess();
            telemetry_log("ncm", taskId_.c_str(),
                "event=package_commit target=%s already_installed=1 commit_us=%llu",
                targetName_, (unsigned long long)(commitStartedUs
                    ? now_us() - commitStartedUs : 0));
            return true;
        }

        for (const auto& packaged : meta.contents) {
            uint64_t packagedSize = 0;
            ncmContentInfoSizeToU64(&packaged.info, &packagedSize);
            auto installed = std::find_if(
                contents_.begin(), contents_.end(),
                [&packaged](const Content& content) {
                    return !content.meta &&
                           std::memcmp(content.id.c,
                                       packaged.info.content_id.c,
                                       sizeof(content.id.c)) == 0;
                });
            if (installed == contents_.end() ||
                installed->size != packagedSize) {
                error_ = "CNMT references missing or mismatched NCA content.";
                return false;
            }
        }

        for (auto& content : contents_) {
            if (content.existing || content.registered)
                continue;
            if (containsContentId(meta.deltaIds, content.id)) {
                // Delta fragment: streamed but not installed; drop placeholder.
                // Reaching here (rather than shouldSkipFile) means the entry
                // preceded the CNMT NCA in the PFS0 — skip was structurally
                // impossible for it (PERF_PLAN 7.5).
                telemetry_log("package", taskId_.c_str(),
                    "event=skip_missed name=%s bytes=%llu reason=before_cnmt",
                    content.name.c_str(),
                    (unsigned long long)content.size);
                ncmContentStorageDeletePlaceHolder(&storage_,
                                                   &content.placeholder);
                continue;
            }
            rc = ncmContentStorageRegister(&storage_, &content.id,
                                           &content.placeholder);
            if (R_FAILED(rc)) {
                errorResult("Unable to register installed content", rc);
                return false;
            }
            content.registered = true;
        }

        // Delta ids from the CNMT that never showed up as a streamed content
        // at all — not shipped in this package, so skip had nothing to act on
        // (PERF_PLAN 7.5: distinguish from the before_cnmt case above).
        for (const auto& deltaId : meta.deltaIds) {
            if (containsContentId(skippedDeltaIds_, deltaId))
                continue;
            bool streamed = std::any_of(contents_.begin(), contents_.end(),
                [&deltaId](const Content& content) {
                    return std::memcmp(content.id.c, deltaId.c,
                                       sizeof(deltaId.c)) == 0;
                });
            if (!streamed)
                telemetry_log("package", taskId_.c_str(),
                    "event=skip_missed id=%s reason=not_in_package",
                    hexBytes(deltaId.c, sizeof(deltaId.c)).c_str());
        }

        NcmContentInfo self {};
        self.content_id = metaContent->id;
        self.content_type = NcmContentType_Meta;
        ncmU64ToContentInfoSize(metaContent->size, &self);

        size_t metaSize = sizeof(NcmContentMetaHeader) +
                          meta.extendedHeader.size() +
                          (meta.contents.size() + 1) *
                              sizeof(NcmContentInfo) +
                          meta.metaInfos.size() *
                              sizeof(NcmContentMetaInfo) +
                          meta.extendedDataSize;
        std::vector<uint8_t> installMeta(metaSize);
        auto* header = reinterpret_cast<NcmContentMetaHeader*>(
            installMeta.data());
        header->extended_header_size =
            static_cast<uint16_t>(meta.extendedHeader.size());
        header->content_count =
            static_cast<uint16_t>(meta.contents.size() + 1);
        header->content_meta_count = meta.header.content_meta_count;
        header->attributes = meta.header.attributes;
        header->storage_id = meta.header.storage_id;
        uint8_t* cursor = installMeta.data() + sizeof(*header);
        std::memcpy(cursor, meta.extendedHeader.data(),
                    meta.extendedHeader.size());
        cursor += meta.extendedHeader.size();
        std::memcpy(cursor, &self, sizeof(self));
        cursor += sizeof(self);
        for (const auto& content : meta.contents) {
            std::memcpy(cursor, &content.info, sizeof(content.info));
            cursor += sizeof(content.info);
        }
        std::memcpy(cursor, meta.metaInfos.data(),
                    meta.metaInfos.size() * sizeof(NcmContentMetaInfo));

        rc = ncmContentMetaDatabaseSet(&database_, &key,
                                       installMeta.data(),
                                       installMeta.size());
        if (R_SUCCEEDED(rc))
            rc = ncmContentMetaDatabaseCommit(&database_);
        if (R_FAILED(rc)) {
            errorResult("Unable to commit title metadata", rc);
            return false;
        }
        metaCommitted_ = true;
        committedKey_ = key;

        uint64_t appId = baseApplicationId(
            meta.header.id,
            static_cast<NcmContentMetaType>(meta.header.type));
        std::vector<NsExtContentStorageMetaKey> records;
        s32 count = 0;
        if (R_SUCCEEDED(nsCountApplicationContentMeta(appId, &count)) &&
            count > 0) {
            records.resize(static_cast<size_t>(count));
            uint32_t written = 0;
            if (R_FAILED(nsextListApplicationRecordContentMeta(
                    0, appId, records.data(), records.size(), &written)))
                records.clear();
            else
                records.resize(written);
        }
        previousRecords_ = records;
        applicationId_ = appId;
        records.push_back({ key, storageId_ });
        nsextDeleteApplicationRecord(appId);
        rc = nsextPushApplicationRecord(
            appId, NsExtApplicationEvent_Present,
            records.data(), records.size());
        if (R_FAILED(rc)) {
            errorResult("Unable to update application record", rc);
            return false;
        }
        applicationRecordTouched_ = true;

        if (!ticket_.empty()) {
            if (certificate_.empty()) {
                error_ = "Ticket certificate is missing.";
                return false;
            }
            rc = esImportTicket(ticket_.data(), ticket_.size(),
                                certificate_.data(), certificate_.size());
            if (R_FAILED(rc)) {
                errorResult("Unable to import title ticket", rc);
                return false;
            }
        }

        finishSuccess();
        log_msg("[install] package committed '%s'\n", packageName_.c_str());
        telemetry_log("ncm", taskId_.c_str(),
            "event=package_commit target=%s already_installed=0 commit_us=%llu package_ms=%llu",
            targetName_, (unsigned long long)(commitStartedUs
                ? now_us() - commitStartedUs : 0),
            (unsigned long long)(now_ms() - packageStartedMs_));
        return true;
    }

    void rollbackPackage() override {
        if (active_) {
            uint64_t rollbackNow = now_ms();
            emitFileTelemetry(rollbackNow, true, true);
            telemetry_log("ncm", taskId_.c_str(),
                          "event=rollback target=%s package_ms=%llu",
                          targetName_,
                          (unsigned long long)(rollbackNow - packageStartedMs_));
        }
        hashActive_ = false;
        if (active_) {
            if (applicationRecordTouched_) {
                nsextDeleteApplicationRecord(applicationId_);
                if (!previousRecords_.empty()) {
                    nsextPushApplicationRecord(
                        applicationId_, NsExtApplicationEvent_Present,
                        previousRecords_.data(), previousRecords_.size());
                }
            }
            if (metaCommitted_) {
                ncmContentMetaDatabaseRemove(&database_, &committedKey_);
                ncmContentMetaDatabaseCommit(&database_);
            }
            for (auto& content : contents_) {
                if (content.registered)
                    ncmContentStorageDelete(&storage_, &content.id);
                else if (!content.existing && content.size)
                    ncmContentStorageDeletePlaceHolder(
                        &storage_, &content.placeholder);
            }
        }
        closeServices();
        if (!tempDirectory_.empty())
            removeTree(tempDirectory_);
        resetState();
    }

    // F-B: opaque snapshot of the ncm bookkeeping at a stream safe point.
    // Placeholder writes are synchronous IPC, so no flush is needed: every
    // acknowledged writeFile() byte is already inside the placeholder.
    std::string checkpointPackage() override {
        static_assert(std::is_trivially_copyable<Sha256Context>::value,
                      "Sha256Context must be raw-serializable");
        if (!active_)
            return {};
        std::string blob("NXB1");
        putU64(blob, static_cast<uint64_t>(storageId_));
        putStr(blob, taskId_);
        putStr(blob, packageName_);
        putU64(blob, expected_);
        putU64(blob, installed_);
        putU64(blob, ignoredRemaining_);
        putStr(blob, auxiliaryKind_);
        putU64(blob, auxiliaryExpected_);
        putBytes(blob, auxiliary_);
        putBytes(blob, ticket_);
        putBytes(blob, certificate_);
        putStr(blob, currentName_);
        putU64(blob, contents_.size());
        uint64_t currentIndexPlusOne = 0;
        for (size_t i = 0; i < contents_.size(); ++i) {
            const Content& content = contents_[i];
            if (&content == current_)
                currentIndexPlusOne = i + 1;
            putStr(blob, content.name);
            putRaw(blob, content.id.c, sizeof(content.id.c));
            putRaw(blob, &content.placeholder, sizeof(content.placeholder));
            putU64(blob, content.size);
            putU64(blob, content.written);
            uint8_t flags = (content.existing ? 1 : 0) |
                            (content.registered ? 2 : 0) |
                            (content.meta ? 4 : 0);
            blob.push_back(static_cast<char>(flags));
        }
        putU64(blob, currentIndexPlusOne);
        blob.push_back(hashActive_ ? 1 : 0);
        if (hashActive_) {
            putU64(blob, sizeof(sha_));
            putRaw(blob, &sha_, sizeof(sha_));
        }
        putU64(blob, skippedDeltaIds_.size());
        for (const auto& id : skippedDeltaIds_)
            putRaw(blob, id.c, sizeof(id.c));
        return blob;
    }

    // F-B: detach without deleting placeholders or a registered CNMT NCA;
    // resumePackage() re-attaches to them from the journal blob.
    void suspendPackage() override {
        if (!active_) {
            rollbackPackage();
            return;
        }
        emitFileTelemetry(now_ms(), true, true);
        telemetry_log("ncm", taskId_.c_str(),
                      "event=package_suspend target=%s installed=%llu "
                      "contents=%zu",
                      targetName_, (unsigned long long)installed_,
                      contents_.size());
        log_msg("[install] package suspended '%s' installed=%llu\n",
                packageName_.c_str(),
                static_cast<unsigned long long>(installed_));
        hashActive_ = false;
        closeServices();
        // The workspace is recreated on resume; placeholders live in ncm,
        // not here.
        if (!tempDirectory_.empty())
            removeTree(tempDirectory_);
        resetState();
    }

    bool resumePackage(const std::string& taskId,
                       const std::string& packageName,
                       const std::string& state) override {
        rollbackPackage();
        error_.clear();
        BlobReader in { state.data(), state.size(), 0 };
        char magic[4] = {};
        uint64_t storageId = 0;
        std::string blobTaskId;
        std::string blobPackage;
        uint64_t expected = 0;
        uint64_t installed = 0;
        uint64_t ignoredRemaining = 0;
        std::string auxiliaryKind;
        uint64_t auxiliaryExpected = 0;
        std::vector<uint8_t> auxiliary;
        std::vector<uint8_t> ticket;
        std::vector<uint8_t> certificate;
        std::string currentName;
        uint64_t contentCount = 0;
        bool parsed =
            in.raw(magic, sizeof(magic)) &&
            std::memcmp(magic, "NXB1", sizeof(magic)) == 0 &&
            in.u64(storageId) && in.str(blobTaskId) &&
            in.str(blobPackage) && in.u64(expected) && in.u64(installed) &&
            in.u64(ignoredRemaining) && in.str(auxiliaryKind) &&
            in.u64(auxiliaryExpected) && in.bytes(auxiliary) &&
            in.bytes(ticket) && in.bytes(certificate) &&
            in.str(currentName) && in.u64(contentCount) &&
            contentCount <= 4096;
        std::vector<Content> contents;
        for (uint64_t i = 0; parsed && i < contentCount; ++i) {
            Content content;
            uint8_t flags = 0;
            parsed = in.str(content.name) &&
                     in.raw(content.id.c, sizeof(content.id.c)) &&
                     in.raw(&content.placeholder,
                            sizeof(content.placeholder)) &&
                     in.u64(content.size) && in.u64(content.written) &&
                     in.u8(flags) && content.written <= content.size;
            if (parsed) {
                content.existing = flags & 1;
                content.registered = flags & 2;
                content.meta = flags & 4;
                contents.push_back(std::move(content));
            }
        }
        uint64_t currentIndexPlusOne = 0;
        uint8_t hashActive = 0;
        Sha256Context sha {};
        if (parsed)
            parsed = in.u64(currentIndexPlusOne) &&
                     currentIndexPlusOne <= contents.size() &&
                     in.u8(hashActive) && hashActive <= 1;
        if (parsed && hashActive) {
            uint64_t shaSize = 0;
            parsed = in.u64(shaSize) && shaSize == sizeof(sha) &&
                     in.raw(&sha, sizeof(sha)) && currentIndexPlusOne != 0;
        }
        uint64_t skippedCount = 0;
        std::vector<NcmContentId> skipped;
        if (parsed)
            parsed = in.u64(skippedCount) && skippedCount <= 4096;
        for (uint64_t i = 0; parsed && i < skippedCount; ++i) {
            NcmContentId id {};
            parsed = in.raw(id.c, sizeof(id.c));
            if (parsed)
                skipped.push_back(id);
        }
        if (!parsed || !in.done()) {
            error_ = "Install journal backend state is malformed.";
            return false;
        }
        if (storageId != static_cast<uint64_t>(storageId_)) {
            error_ = "Install journal targets a different storage.";
            return false;
        }
        if (blobPackage != packageName) {
            error_ = "Install journal does not match this package.";
            return false;
        }

        Result rc = ncmOpenContentStorage(&storage_, storageId_);
        if (R_SUCCEEDED(rc))
            rc = ncmOpenContentMetaDatabase(&database_, storageId_);
        if (R_FAILED(rc)) {
            errorResult("Unable to open content storage", rc);
            closeServices();
            return false;
        }
        // Every checkpointed content must still be present on this console;
        // otherwise discard the leftovers and let the caller start fresh.
        bool valid = true;
        for (const auto& content : contents) {
            bool has = false;
            if (content.existing || content.registered)
                valid = R_SUCCEEDED(ncmContentStorageHas(
                            &storage_, &has, &content.id)) && has;
            else if (content.size)
                valid = R_SUCCEEDED(ncmContentStorageHasPlaceHolder(
                            &storage_, &has, &content.placeholder)) && has;
            else
                valid = content.written == 0;
            if (!valid) {
                log_msg("[install] resume rejected: '%s' is missing\n",
                        content.name.c_str());
                break;
            }
        }
        if (!valid) {
            for (const auto& content : contents) {
                if (content.registered && !content.existing)
                    ncmContentStorageDelete(&storage_, &content.id);
                else if (!content.existing && content.size)
                    ncmContentStorageDeletePlaceHolder(&storage_,
                                                       &content.placeholder);
            }
            closeServices();
            error_ = "Checkpointed install no longer matches this console.";
            return false;
        }

        taskId_ = taskId;
        packageName_ = packageName;
        tempDirectory_ = std::string(TempRoot) + "/" + taskId;
        if (!makeDirectories(tempDirectory_)) {
            closeServices();
            resetState();
            error_ = "Unable to create installation workspace.";
            return false;
        }
        contents_ = std::move(contents);
        ticket_ = std::move(ticket);
        certificate_ = std::move(certificate);
        auxiliary_ = std::move(auxiliary);
        auxiliaryKind_ = std::move(auxiliaryKind);
        auxiliaryExpected_ = auxiliaryExpected;
        currentName_ = std::move(currentName);
        ignoredRemaining_ = ignoredRemaining;
        expected_ = expected;
        installed_ = installed;
        skippedDeltaIds_ = std::move(skipped);
        current_ = currentIndexPlusOne
                       ? &contents_[currentIndexPlusOne - 1] : nullptr;
        if (hashActive) {
            std::memcpy(&sha_, &sha, sizeof(sha_));
            hashActive_ = true;
        }
        active_ = true;
        packageStartedMs_ = now_ms();
        resetFileTelemetry(now_ms());
        // Re-derive the early CNMT parse (PERF_PLAN 3.4) so delta skipping
        // keeps working after the restart.
        for (auto& content : contents_)
            if (content.meta && content.size &&
                content.written == content.size)
                prepareEarlyMeta(content);
        log_msg("[install] package resumed '%s' contents=%zu installed=%llu\n",
                packageName_.c_str(), contents_.size(),
                static_cast<unsigned long long>(installed_));
        telemetry_log("ncm", taskId_.c_str(),
                      "event=package_resume target=%s contents=%zu "
                      "installed=%llu",
                      targetName_, contents_.size(),
                      (unsigned long long)installed_);
        return true;
    }

    uint64_t installedBytes() const override { return installed_; }
    uint64_t expectedBytes() const override { return expected_; }
    const std::string& error() const override { return error_; }

private:
    // PERF_PLAN 3.4: parse the CNMT as soon as its NCA completes, so delta
    // fragments that follow it in the PFS0 can be skipped by shouldSkipFile
    // instead of being streamed through zstd/AES/SHA/ncm and dropped at
    // commit. Best effort: any failure here is retried authoritatively at
    // commitPackage, which then fails with a proper error.
    void prepareEarlyMeta(Content& metaContent) {
        if (earlyMetaValid_)
            return;
        if (!metaContent.existing && !metaContent.registered) {
            Result rc = ncmContentStorageRegister(
                &storage_, &metaContent.id, &metaContent.placeholder);
            if (R_FAILED(rc)) {
                log_msg("[install] early CNMT register failed (0x%08x)\n", rc);
                return;
            }
            metaContent.registered = true;
        }
        char path[FS_MAX_PATH];
        Result rc = ncmContentStorageGetPath(&storage_, path, sizeof(path),
                                             &metaContent.id);
        if (R_FAILED(rc)) {
            log_msg("[install] early CNMT path failed (0x%08x)\n", rc);
            return;
        }
        ParsedMeta meta;
        std::string parseError;
        if (!readPackagedMeta(path, meta, parseError)) {
            log_msg("[install] early CNMT parse failed: %s\n",
                    parseError.c_str());
            return;
        }
        earlyMeta_ = std::move(meta);
        earlyMetaValid_ = true;
        log_msg("[install] CNMT parsed early: contents=%zu deltas=%zu\n",
                earlyMeta_.contents.size(), earlyMeta_.deltaIds.size());
    }

    void resetFileTelemetry(uint64_t now) {
        telemetryGeneration_ = telemetry_generation();
        telemetryFileStartedMs_ = now;
        telemetryLastMs_ = now;
        telemetryWriteBytes_ = 0;
        telemetryNcmUs_ = 0;
        telemetryShaUs_ = 0;
        telemetryNcmMaxUs_ = 0;
        telemetryWriteCalls_ = 0;
        telemetryTotalWriteBytes_ = 0;
        telemetryTotalExistingBytes_ = 0;
        telemetryTotalNcmUs_ = 0;
        telemetryTotalShaUs_ = 0;
        telemetryTotalNcmMaxUs_ = 0;
        telemetryTotalWriteCalls_ = 0;
    }

    void resetFileInterval(uint64_t now) {
        telemetryLastMs_ = now;
        telemetryWriteBytes_ = 0;
        telemetryNcmUs_ = 0;
        telemetryShaUs_ = 0;
        telemetryNcmMaxUs_ = 0;
        telemetryWriteCalls_ = 0;
    }

    void prepareFileTelemetry() {
        if (!current_)
            return;
        uint32_t generation = telemetry_generation();
        if (telemetryGeneration_ != generation)
            resetFileTelemetry(now_ms());
    }

    void emitFileTelemetry(uint64_t now, bool force, bool summary) {
        if (!telemetry_enabled() || !telemetryFileStartedMs_)
            return;
        uint64_t elapsedMs = summary
            ? now - telemetryFileStartedMs_ : now - telemetryLastMs_;
        if (!force && elapsedMs < 5000)
            return;
        if (!elapsedMs)
            elapsedMs = 1;
        uint64_t bytes = summary
            ? telemetryTotalWriteBytes_ : telemetryWriteBytes_;
        uint64_t ncmUs = summary ? telemetryTotalNcmUs_ : telemetryNcmUs_;
        uint64_t shaUs = summary ? telemetryTotalShaUs_ : telemetryShaUs_;
        uint64_t maxUs = summary
            ? telemetryTotalNcmMaxUs_ : telemetryNcmMaxUs_;
        uint32_t calls = summary
            ? telemetryTotalWriteCalls_ : telemetryWriteCalls_;
        telemetry_log("ncm", taskId_.c_str(),
            "event=%s target=%s interval_ms=%llu bytes=%llu bps=%llu calls=%u "
            "ncm_us=%llu sha_us=%llu ncm_max_us=%llu existing_bytes=%llu",
            summary ? "summary" : "interval",
            targetName_,
            (unsigned long long)elapsedMs, (unsigned long long)bytes,
            (unsigned long long)(bytes * 1000 / elapsedMs), calls,
            (unsigned long long)ncmUs, (unsigned long long)shaUs,
            (unsigned long long)maxUs,
            (unsigned long long)(summary ? telemetryTotalExistingBytes_ : 0));
        if (!summary)
            resetFileInterval(now);
    }

    void errorResult(const char* message, Result rc) {
        char text[160];
        std::snprintf(text, sizeof(text), "%s (0x%08x).", message, rc);
        error_ = text;
        log_msg("[install] error: %s file='%s' package='%s'\n",
                error_.c_str(), currentName_.c_str(), packageName_.c_str());
    }

    void discardPlaceholders() {
        for (auto& content : contents_)
            if (!content.existing && content.size)
                ncmContentStorageDeletePlaceHolder(
                    &storage_, &content.placeholder);
    }

    void closeServices() {
        ncmContentMetaDatabaseClose(&database_);
        ncmContentStorageClose(&storage_);
        std::memset(&database_, 0, sizeof(database_));
        std::memset(&storage_, 0, sizeof(storage_));
    }

    void finishSuccess() {
        closeServices();
        removeTree(tempDirectory_);
        active_ = false;
        tempDirectory_.clear();
        contents_.clear();
        ticket_.clear();
        certificate_.clear();
        current_ = nullptr;
        earlyMeta_ = ParsedMeta {};
        earlyMetaValid_ = false;
        skippedDeltaIds_.clear();
    }

    void resetState() {
        active_ = false;
        current_ = nullptr;
        contents_.clear();
        ticket_.clear();
        certificate_.clear();
        auxiliary_.clear();
        auxiliaryKind_.clear();
        tempDirectory_.clear();
        expected_ = 0;
        installed_ = 0;
        ignoredRemaining_ = 0;
        earlyMeta_ = ParsedMeta {};
        earlyMetaValid_ = false;
        skippedDeltaIds_.clear();
        metaCommitted_ = false;
        applicationRecordTouched_ = false;
        applicationId_ = 0;
        previousRecords_.clear();
    }

    std::string root_;
    NcmStorageId storageId_;
    const char* targetName_;
    std::string taskId_;
    std::string packageName_;
    std::string tempDirectory_;
    std::string currentName_;
    std::string auxiliaryKind_;
    std::string error_;
    NcmContentStorage storage_ {};
    NcmContentMetaDatabase database_ {};
    NcmContentMetaKey committedKey_ {};
    std::vector<Content> contents_;
    std::vector<uint8_t> ticket_;
    std::vector<uint8_t> certificate_;
    std::vector<uint8_t> auxiliary_;
    std::vector<NsExtContentStorageMetaKey> previousRecords_;
    Content* current_ = nullptr;
    Sha256Context sha_ {};
    ParsedMeta earlyMeta_;
    bool earlyMetaValid_ = false;
    // Delta ids shouldSkipFile actually dropped (PERF_PLAN 7.5) — recorded so
    // commitPackage can tell "skipped" apart from "streamed late" / "absent".
    mutable std::vector<NcmContentId> skippedDeltaIds_;
    uint64_t auxiliaryExpected_ = 0;
    uint64_t ignoredRemaining_ = 0;
    uint64_t expected_ = 0;
    uint64_t installed_ = 0;
    uint64_t applicationId_ = 0;
    uint32_t telemetryGeneration_ = 0;
    uint64_t packageStartedMs_ = 0;
    uint64_t telemetryFileStartedMs_ = 0;
    uint64_t telemetryLastMs_ = 0;
    uint64_t telemetryLastStallLogMs_ = 0;
    uint64_t telemetryWriteBytes_ = 0;
    uint64_t telemetryNcmUs_ = 0;
    uint64_t telemetryShaUs_ = 0;
    uint64_t telemetryNcmMaxUs_ = 0;
    uint32_t telemetryWriteCalls_ = 0;
    uint64_t telemetryTotalWriteBytes_ = 0;
    uint64_t telemetryTotalExistingBytes_ = 0;
    uint64_t telemetryTotalNcmUs_ = 0;
    uint64_t telemetryTotalShaUs_ = 0;
    uint64_t telemetryTotalNcmMaxUs_ = 0;
    uint32_t telemetryTotalWriteCalls_ = 0;
    bool active_ = false;
    bool hashActive_ = false;
    bool metaCommitted_ = false;
    bool applicationRecordTouched_ = false;
};

} // namespace

std::unique_ptr<InstallBackend> createInstallBackend(
    const std::string& workingRoot, InstallStorageTarget target) {
    return std::make_unique<SwitchInstallBackend>(workingRoot, target);
}

} // namespace pipensx::install

#endif
