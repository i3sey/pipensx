#include "install_backend.hpp"

#ifdef __SWITCH__

extern "C" {
#include "../core/util.h"
}

#include <switch.h>
#include <switch-ipcext.h>

#include <mbedtls/sha256.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace pipensx::install {
namespace {

constexpr const char* TempRoot = "sdmc:/switch/pipensx/install-temp";

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
    explicit SwitchInstallBackend(std::string root)
        : root_(std::move(root)) {}

    ~SwitchInstallBackend() override { rollbackPackage(); }

    bool beginPackage(const std::string& taskId,
                      const std::string& packageName) override {
        rollbackPackage();
        error_.clear();
        tempDirectory_ = std::string(TempRoot) + "/" + taskId;
        removeTree(tempDirectory_);
        if (!makeDirectories(tempDirectory_)) {
            error_ = "Unable to create installation workspace.";
            return false;
        }
        Result rc = ncmOpenContentStorage(&storage_, NcmStorageId_SdCard);
        if (R_SUCCEEDED(rc))
            rc = ncmOpenContentMetaDatabase(&database_, NcmStorageId_SdCard);
        if (R_FAILED(rc)) {
            errorResult("Unable to open SD content storage", rc);
            closeServices();
            return false;
        }
        active_ = true;
        packageName_ = packageName;
        log_msg("[install] package begin '%s'\n", packageName_.c_str());
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
        mbedtls_sha256_init(&sha_);
        if (mbedtls_sha256_starts_ret(&sha_, 0) != 0) {
            error_ = "Unable to initialize NCA hash.";
            return false;
        }
        hashActive_ = true;
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
        if (!current_->existing) {
            Result rc = ncmContentStorageWritePlaceHolder(
                &storage_, &current_->placeholder, current_->written,
                data, size);
            if (R_FAILED(rc)) {
                errorResult("Unable to write content placeholder", rc);
                return false;
            }
        }
        if (mbedtls_sha256_update_ret(&sha_, data, size) != 0) {
            error_ = "Unable to update NCA hash.";
            return false;
        }
        current_->written += size;
        installed_ += size;
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
        if (!hashActive_ ||
            mbedtls_sha256_finish_ret(&sha_, digest.data()) != 0) {
            error_ = "Unable to finalize NCA hash.";
            return false;
        }
        mbedtls_sha256_free(&sha_);
        hashActive_ = false;
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
        current_ = nullptr;
        currentName_.clear();
        return true;
    }

    bool commitPackage(bool& alreadyInstalled) override {
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
        char metaNcaPath[FS_MAX_PATH];
        Result rc = ncmContentStorageGetPath(&storage_, metaNcaPath,
                                             sizeof(metaNcaPath),
                                             &metaContent->id);
        if (R_FAILED(rc)) {
            errorResult("Unable to resolve CNMT NCA path", rc);
            return false;
        }
        ParsedMeta meta;
        if (!readPackagedMeta(metaNcaPath, meta, error_))
            return false;
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
        rc = ncmContentMetaDatabaseHas(&database_, &exists, &key);
        if (R_FAILED(rc)) {
            errorResult("Unable to query installed title metadata", rc);
            return false;
        }
        if (exists) {
            alreadyInstalled = true;
            discardPlaceholders();
            finishSuccess();
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
        records.push_back({ key, NcmStorageId_SdCard });
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
        return true;
    }

    void rollbackPackage() override {
        if (hashActive_) {
            mbedtls_sha256_free(&sha_);
            hashActive_ = false;
        }
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

    uint64_t installedBytes() const override { return installed_; }
    uint64_t expectedBytes() const override { return expected_; }
    const std::string& error() const override { return error_; }

private:
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
        metaCommitted_ = false;
        applicationRecordTouched_ = false;
        applicationId_ = 0;
        previousRecords_.clear();
    }

    std::string root_;
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
    mbedtls_sha256_context sha_ {};
    uint64_t auxiliaryExpected_ = 0;
    uint64_t ignoredRemaining_ = 0;
    uint64_t expected_ = 0;
    uint64_t installed_ = 0;
    uint64_t applicationId_ = 0;
    bool active_ = false;
    bool hashActive_ = false;
    bool metaCommitted_ = false;
    bool applicationRecordTouched_ = false;
};

} // namespace

std::unique_ptr<InstallBackend> createInstallBackend(
    const std::string& workingRoot) {
    return std::make_unique<SwitchInstallBackend>(workingRoot);
}

} // namespace pipensx::install

#endif
