#include "installed_title_service.hpp"
#include "nacp_language.hpp"

extern "C" {
#include "../core/util.h"
}

#include <switch.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <memory>
#include <sys/stat.h>
#include <unistd.h>
#include <unordered_map>

namespace pipensx {
namespace {

std::string resultText(const char* operation, Result result) {
    char text[160];
    std::snprintf(text, sizeof(text), "%s (0x%08x).", operation, result);
    return text;
}

std::string upperAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) {
                       return static_cast<char>(std::toupper(c));
                   });
    return value;
}

bool writeIconFile(const std::string& path, const uint8_t* bytes,
                   size_t size, bool overwrite = false) {
    struct stat st {};
    if (!overwrite && stat(path.c_str(), &st) == 0 && st.st_size > 0)
        return true;
    if (!bytes || size < 8)
        return false;
    std::string temporary = path + ".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output)
            return false;
        output.write(reinterpret_cast<const char*>(bytes),
                     static_cast<std::streamsize>(size));
        output.flush();
        if (!output.good()) {
            unlink(temporary.c_str());
            return false;
        }
    }
    if (rename(temporary.c_str(), path.c_str()) == 0)
        return true;
    if ((unlink(path.c_str()) == 0 || errno == ENOENT) &&
        rename(temporary.c_str(), path.c_str()) == 0)
        return true;
    unlink(temporary.c_str());
    return false;
}

bool readFileBytes(const std::string& path, std::vector<uint8_t>& out) {
    out.clear();
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f)
        return false;
    if (std::fseek(f, 0, SEEK_END) != 0) {
        std::fclose(f);
        return false;
    }
    const long end = std::ftell(f);
    if (end <= 0 || end > 16 * 1024 * 1024 || std::fseek(f, 0, SEEK_SET) != 0) {
        std::fclose(f);
        return false;
    }
    out.resize(static_cast<size_t>(end));
    const size_t read = std::fread(out.data(), 1, out.size(), f);
    std::fclose(f);
    if (read != out.size()) {
        out.clear();
        return false;
    }
    return true;
}

bool removeStaleArchiveFiles(const std::string& rootPath,
                             const std::string& iconRoot,
                             const std::string& upperId) {
    const std::string archiveBase = rootPath + "/archives/" + upperId;
    const std::string archIcon = iconRoot + "/" + upperId + "-arch.jpg";
    struct stat st {};
    const bool hadArchive =
        stat((archiveBase + ".json").c_str(), &st) == 0 ||
        stat(archIcon.c_str(), &st) == 0;
    if (!hadArchive)
        return false;
    unlink((archiveBase + ".json").c_str());
    unlink((archiveBase + ".jpg").c_str());
    unlink((archiveBase + ".torrent").c_str());
    unlink(archIcon.c_str());
    return true;
}

} // namespace

bool directoryHasEntries(const std::string& path) {
    if (path.empty())
        return false;
    DIR* dir = opendir(path.c_str());
    if (!dir)
        return false;
    bool found = false;
    while (dirent* entry = readdir(dir)) {
        const char* name = entry->d_name;
        if (!std::strcmp(name, ".") || !std::strcmp(name, ".."))
            continue;
        found = true;
        break;
    }
    closedir(dir);
    return found;
}

bool titleHasLayeredFsMods(const std::string& sdRoot,
                            const std::string& titleId) {
    const std::string dir = layeredFsModDirForTitle(sdRoot, titleId);
    if (dir.empty())
        return false;
    return directoryHasEntries(dir);
}

InstalledTitleService::InstalledTitleService(std::string rootPath)
    : rootPath_(std::move(rootPath)),
      iconRoot_(rootPath_ + "/installed-icons") {
    mkdir(iconRoot_.c_str(), 0755);
}

bool InstalledTitleService::uninstall(const std::string& titleId,
                                      std::string& error) {
    std::string refreshError;
    if (!uninstall(titleId, error, refreshError))
        return false;
    if (!refreshError.empty()) {
        error = std::move(refreshError);
        return false;
    }
    return true;
}

bool InstalledTitleService::uninstall(const std::string& titleId,
                                      std::string& error,
                                      std::string& refreshError) {
    error.clear();
    refreshError.clear();
    uint64_t applicationId = 0;
    if (!parseTitleId(titleId, applicationId)) {
        error = "Invalid title id.";
        return false;
    }
    {
        std::lock_guard<std::mutex> refreshLock(refreshMutex_);
        const Result rc = nsDeleteApplicationCompletely(applicationId);
        if (R_FAILED(rc)) {
            error = resultText("Unable to uninstall application", rc);
            diagnostic_error("installed", titleId.c_str(),
                             "event=uninstall result=0x%08x", rc);
            return false;
        }
    }
    removeStaleArchiveFiles(rootPath_, iconRoot_, upperAscii(titleId));
    log_msg("[installed] uninstalled %s\n", titleId.c_str());
    telemetry_log("installed", titleId.c_str(), "event=uninstall");
    if (!refresh(refreshError))
        diagnostic_error("installed", titleId.c_str(),
                         "event=uninstall_refresh error=%s",
                         refreshError.c_str());
    return true;
}

bool InstalledTitleService::contains(const std::string& titleId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return !titleId.empty() && titleIds_.count(upperAscii(titleId)) != 0;
}

bool InstalledTitleService::isArchived(const std::string& titleId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return !titleId.empty() && archivedTitleIds_.count(upperAscii(titleId)) != 0;
}

bool InstalledTitleService::containsInstalledOrArchived(
    const std::string& titleId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (titleId.empty())
        return false;
    const std::string upper = upperAscii(titleId);
    return titleIds_.count(upper) != 0 || archivedTitleIds_.count(upper) != 0;
}

bool InstalledTitleService::containsDlc(const std::string& titleId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return !titleId.empty() && dlcTitleIds_.count(upperAscii(titleId)) != 0;
}

void InstalledTitleService::invalidateCachedIcon(const std::string& titleId) {
    if (titleId.empty())
        return;
    const std::string upper = upperAscii(titleId);
    unlink((iconRoot_ + "/" + upper + ".jpg").c_str());
    unlink((iconRoot_ + "/" + upper + "-arch.jpg").c_str());
}

bool InstalledTitleService::readControlIcon(const std::string& titleId,
                                            std::vector<uint8_t>& jpegOut,
                                            std::string& error) const {
    jpegOut.clear();
    error.clear();
    uint64_t applicationId = 0;
    if (!parseTitleId(titleId, applicationId)) {
        error = "Invalid title id.";
        return false;
    }
    const std::string upper = formatTitleId(applicationId);
#ifdef __SWITCH__
    auto control = std::make_unique<NsApplicationControlData>();
    std::memset(control.get(), 0, sizeof(*control));
    u64 actualSize = 0;
    const Result rc = nsGetApplicationControlData(
        NsApplicationControlSource_Storage, applicationId,
        control.get(), sizeof(*control), &actualSize);
    if (R_SUCCEEDED(rc) && actualSize > sizeof(NacpStruct)) {
        const size_t iconSize = std::min<size_t>(
            static_cast<size_t>(actualSize - sizeof(NacpStruct)),
            sizeof(control->icon));
        if (iconSize >= 8) {
            jpegOut.assign(control->icon, control->icon + iconSize);
            return true;
        }
    }
#endif
    if (readFileBytes(rootPath_ + "/archives/" + upper + ".jpg", jpegOut) ||
        readFileBytes(iconRoot_ + "/" + upper + ".jpg", jpegOut))
        return true;
    error = "Application icon is unavailable.";
    return false;
}

std::vector<InstalledTitle> InstalledTitleService::titles() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return titles_;
}

std::unordered_set<std::string> InstalledTitleService::titleIds() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return titleIds_;
}

std::vector<std::string> InstalledTitleService::dlcTitleIds() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return {dlcTitleIds_.begin(), dlcTitleIds_.end()};
}

size_t InstalledTitleService::dlcCountForBase(const std::string& titleId) const {
    uint64_t base = 0;
    if (!parseTitleId(titleId, base))
        return 0;
    base = nxBaseApplicationId(base);
    std::lock_guard<std::mutex> lock(mutex_);
    size_t count = 0;
    for (const std::string& id : dlcTitleIds_) {
        uint64_t parsed = 0;
        if (parseTitleId(id, parsed) && nxBaseApplicationId(parsed) == base)
            ++count;
    }
    return count;
}

uint64_t InstalledTitleService::generation() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return generation_;
}

void InstalledTitleService::injectTitles(std::vector<InstalledTitle> titles) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::unordered_set<std::string> ids;
    std::unordered_set<std::string> archivedIds;
    ids.reserve(titles.size());
    for (const InstalledTitle& title : titles) {
        if (title.isArchived)
            archivedIds.insert(upperAscii(title.titleId));
        else
            ids.insert(upperAscii(title.titleId));
    }
    titles_ = std::move(titles);
    titleIds_ = std::move(ids);
    archivedTitleIds_ = std::move(archivedIds);
    dlcTitleIds_.clear();
    ++generation_;
}

void InstalledTitleService::injectDlcTitleIds(
    std::vector<std::string> dlcTitleIds) {
    std::lock_guard<std::mutex> lock(mutex_);
    dlcTitleIds_.clear();
    dlcTitleIds_.reserve(dlcTitleIds.size());
    for (const std::string& id : dlcTitleIds)
        dlcTitleIds_.insert(upperAscii(id));
    ++generation_;
}

bool InstalledTitleService::refresh(std::string& error) {
    std::lock_guard<std::mutex> refreshLock(refreshMutex_);
    const uint64_t startedMs = now_ms();
    error.clear();
    std::vector<NsApplicationRecord> records;
    constexpr s32 PageSize = 64;
    s32 offset = 0;
    while (records.size() < 4096) {
        std::array<NsApplicationRecord, PageSize> page {};
        s32 returned = 0;
        Result rc = nsListApplicationRecord(page.data(), PageSize, offset,
                                            &returned);
        if (R_FAILED(rc)) {
            error = resultText("Unable to list installed applications", rc);
            diagnostic_error("installed", "list", "result=0x%08x", rc);
            return false;
        }
        if (returned <= 0)
            break;
        returned = std::min(returned, PageSize);
        records.insert(records.end(), page.begin(), page.begin() + returned);
        offset += returned;
        if (returned < PageSize)
            break;
    }

    // Installed title version = the Patch content meta's title version (the
    // base application meta is always v0). Read once per storage; a title
    // with no patch installed has version 0. The list call is single-shot
    // (no offset), so probe the total first, then fetch the full set.
    // Any open/list failure makes the map incomplete — leave version empty
    // (CheckError) rather than inventing "0" and offering a false update.
    std::unordered_map<uint64_t, uint32_t> patchVersions;
    std::unordered_set<std::string> dlcIds;
    bool patchMetaComplete = true;
    {
        constexpr s32 MaxPatches = 8192;
        constexpr s32 MaxDlc = 32768;
        const NcmStorageId storages[] = {NcmStorageId_BuiltInUser,
                                         NcmStorageId_SdCard};
        for (NcmStorageId storage : storages) {
            NcmContentMetaDatabase database;
            const Result openRc = ncmOpenContentMetaDatabase(&database,
                                                             storage);
            if (R_FAILED(openRc)) {
#ifdef __SWITCH__
                diagnostic_error("installed", "ncm_open",
                                 "result=0x%08x", openRc);
#endif
                patchMetaComplete = false;
                continue;
            }
            s32 total = 0;
            s32 written = 0;
            Result rc = ncmContentMetaDatabaseList(
                &database, &total, &written, nullptr, 0,
                NcmContentMetaType_Patch, 0, 0, 0xFFFFFFFFFFFFFFFFULL,
                NcmContentInstallType_Unknown);
            if (R_FAILED(rc)) {
                ncmContentMetaDatabaseClose(&database);
                diagnostic_error("installed", "ncm_list",
                                 "result=0x%08x", rc);
                patchMetaComplete = false;
                continue;
            }
            if (total > MaxPatches) {
                diagnostic_error("installed", "ncm_truncate",
                                 "count=%d", total);
                total = MaxPatches;
            }
            std::vector<NcmContentMetaKey> keys(
                static_cast<size_t>(total));
            if (!keys.empty()) {
                rc = ncmContentMetaDatabaseList(
                    &database, &total, &written, keys.data(),
                    static_cast<s32>(keys.size()),
                    NcmContentMetaType_Patch, 0, 0,
                    0xFFFFFFFFFFFFFFFFULL, NcmContentInstallType_Unknown);
                if (R_SUCCEEDED(rc)) {
                    for (s32 index = 0; index < written; ++index)
                        patchVersions[keys[static_cast<size_t>(index)].id] =
                            keys[static_cast<size_t>(index)].version;
                } else {
                    diagnostic_error("installed", "ncm_list",
                                     "result=0x%08x", rc);
                    patchMetaComplete = false;
                }
            }

            total = 0;
            written = 0;
            rc = ncmContentMetaDatabaseList(
                &database, &total, &written, nullptr, 0,
                NcmContentMetaType_AddOnContent, 0, 0,
                0xFFFFFFFFFFFFFFFFULL, NcmContentInstallType_Unknown);
            if (R_FAILED(rc)) {
                diagnostic_error("installed", "ncm_dlc_list",
                                 "result=0x%08x", rc);
            } else {
                if (total > MaxDlc) {
                    diagnostic_error("installed", "ncm_dlc_truncate",
                                     "count=%d", total);
                    total = MaxDlc;
                }
                std::vector<NcmContentMetaKey> dlcKeys(
                    static_cast<size_t>(total));
                if (!dlcKeys.empty()) {
                    rc = ncmContentMetaDatabaseList(
                        &database, &total, &written, dlcKeys.data(),
                        static_cast<s32>(dlcKeys.size()),
                        NcmContentMetaType_AddOnContent, 0, 0,
                        0xFFFFFFFFFFFFFFFFULL, NcmContentInstallType_Unknown);
                    if (R_SUCCEEDED(rc)) {
                        for (s32 index = 0; index < written; ++index)
                            dlcIds.insert(formatTitleId(
                                dlcKeys[static_cast<size_t>(index)].id));
                    } else {
                        diagnostic_error("installed", "ncm_dlc_list",
                                         "result=0x%08x", rc);
                    }
                }
            }
            ncmContentMetaDatabaseClose(&database);
        }
    }

    std::vector<InstalledTitle> next;
    next.reserve(records.size());
    auto control = std::make_unique<NsApplicationControlData>();
    int preferred = 0;
#ifdef __SWITCH__
    // Same SetLanguage → NACP slot map as libnx nacpGetLanguageEntry.
    static const u32 kNacpLanguageTable[18] = {
        2, 0, 3, 4, 7, 6, 14, 12, 8, 10, 11, 13, 1, 9, 5, 14, 13, 15};
    u64 languageCode = 0;
    SetLanguage language = SetLanguage_ENUS;
    if (R_SUCCEEDED(setInitialize())) {
        if (R_SUCCEEDED(setGetSystemLanguage(&languageCode)))
            setMakeLanguage(languageCode, &language);
        setExit();
    }
    if (language >= 0 &&
        static_cast<u32>(language) <
            sizeof(kNacpLanguageTable) / sizeof(kNacpLanguageTable[0]))
        preferred = static_cast<int>(
            kNacpLanguageTable[static_cast<u32>(language)]);
#endif
    for (const NsApplicationRecord& record : records) {
        InstalledTitle title;
        title.applicationId = record.application_id;
        title.titleId = formatTitleId(record.application_id);
        title.name = title.titleId;
        title.iconPath = iconRoot_ + "/" + title.titleId + ".jpg";

        title.version = installedPatchVersionString(
            record.application_id, patchVersions, patchMetaComplete);

        std::memset(control.get(), 0, sizeof(*control));
        u64 actualSize = 0;
        Result rc = nsGetApplicationControlData(
            NsApplicationControlSource_Storage, record.application_id,
            control.get(), sizeof(*control), &actualSize);
        if (R_SUCCEEDED(rc)) {
#ifdef __SWITCH__
            title.isArchived = std::memcmp(
                &control->nacp.application_error_code_category,
                "pnxarch", 7) == 0;
#endif
            title.iconPath = iconRoot_ + "/" + title.titleId +
                (title.isArchived ? "-arch.jpg" : ".jpg");
            std::string name;
            std::string author;
            if (nacpReadLanguage(&control->nacp, sizeof(control->nacp),
                                 preferred, name, author) &&
                !name.empty())
                title.name = std::move(name);
            title.publisher = std::move(author);
            // B6: remember what is installed before an update replaces it.
            title.displayVersion = nacpDisplayVersionString(
                control->nacp.display_version,
                sizeof(control->nacp.display_version));
            size_t iconSize = actualSize > sizeof(NacpStruct)
                ? static_cast<size_t>(actualSize - sizeof(NacpStruct)) : 0;
            iconSize = std::min(iconSize, sizeof(control->icon));
            bool overwriteIcon = false;
            if (!title.isArchived) {
                overwriteIcon = removeStaleArchiveFiles(
                    rootPath_, iconRoot_, title.titleId);
            }
            if (!writeIconFile(title.iconPath, control->icon, iconSize,
                               overwriteIcon)) {
                title.iconPath.clear();
            }
        } else {
            title.iconPath.clear();
            diagnostic_error("installed", title.titleId.c_str(),
                             "event=control_data result=0x%08x", rc);
        }
        // LayeredFS mods live on the SD card outside ncm, so a plain
        // opendir is enough on both builds (PC path just misses -> false).
        title.hasLayeredFsMods =
            titleHasLayeredFsMods("sdmc:/", title.titleId);
        next.push_back(std::move(title));
    }

    std::stable_sort(next.begin(), next.end(),
                     [](const InstalledTitle& left,
                        const InstalledTitle& right) {
                         return left.name < right.name;
                     });
    std::unordered_set<std::string> ids;
    std::unordered_set<std::string> archivedIds;
    ids.reserve(next.size());
    for (const InstalledTitle& title : next) {
        if (title.isArchived)
            archivedIds.insert(title.titleId);
        else
            ids.insert(title.titleId);
    }
    const size_t count = next.size();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        titles_ = std::move(next);
        titleIds_ = std::move(ids);
        archivedTitleIds_ = std::move(archivedIds);
        dlcTitleIds_ = std::move(dlcIds);
        ++generation_;
    }
    log_msg("[installed] loaded %zu applications\n", count);
    telemetry_log("installed", "system",
                  "event=refresh count=%zu duration_ms=%llu", count,
                  static_cast<unsigned long long>(now_ms() - startedMs));
    return true;
}

} // namespace pipensx
