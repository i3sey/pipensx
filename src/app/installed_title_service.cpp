#include "installed_title_service.hpp"

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
#include <fstream>
#include <memory>
#include <sys/stat.h>
#include <unistd.h>

namespace pipensx {
namespace {

std::string resultText(const char* operation, Result result) {
    char text[160];
    std::snprintf(text, sizeof(text), "%s (0x%08x).", operation, result);
    return text;
}

std::string boundedText(const char* value, size_t size) {
    if (!value || size == 0)
        return {};
    size_t length = strnlen(value, size);
    return std::string(value, length);
}

std::string upperAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) {
                       return static_cast<char>(std::toupper(c));
                   });
    return value;
}

bool writeIconIfMissing(const std::string& path, const uint8_t* bytes,
                        size_t size) {
    struct stat st {};
    if (stat(path.c_str(), &st) == 0 && st.st_size > 0)
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

} // namespace

InstalledTitleService::InstalledTitleService(std::string rootPath)
    : rootPath_(std::move(rootPath)),
      iconRoot_(rootPath_ + "/installed-icons") {
    mkdir(iconRoot_.c_str(), 0755);
}

std::string InstalledTitleService::formatTitleId(uint64_t applicationId) {
    char text[17];
    std::snprintf(text, sizeof(text), "%016llX",
                  static_cast<unsigned long long>(applicationId));
    return text;
}

bool InstalledTitleService::contains(const std::string& titleId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return !titleId.empty() && titleIds_.count(upperAscii(titleId)) != 0;
}

std::vector<InstalledTitle> InstalledTitleService::titles() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return titles_;
}

uint64_t InstalledTitleService::generation() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return generation_;
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

    std::vector<InstalledTitle> next;
    next.reserve(records.size());
    auto control = std::make_unique<NsApplicationControlData>();
    for (const NsApplicationRecord& record : records) {
        InstalledTitle title;
        title.applicationId = record.application_id;
        title.titleId = formatTitleId(record.application_id);
        title.name = title.titleId;
        title.iconPath = iconRoot_ + "/" + title.titleId + ".jpg";

        std::memset(control.get(), 0, sizeof(*control));
        u64 actualSize = 0;
        Result rc = nsGetApplicationControlData(
            NsApplicationControlSource_Storage, record.application_id,
            control.get(), sizeof(*control), &actualSize);
        if (R_SUCCEEDED(rc)) {
            NacpLanguageEntry* language = nullptr;
            if (R_SUCCEEDED(nacpGetLanguageEntry(&control->nacp, &language)) &&
                language) {
                std::string name = boundedText(language->name,
                                               sizeof(language->name));
                if (!name.empty())
                    title.name = std::move(name);
                title.publisher = boundedText(language->author,
                                              sizeof(language->author));
            }
            size_t iconSize = actualSize > sizeof(NacpStruct)
                ? static_cast<size_t>(actualSize - sizeof(NacpStruct)) : 0;
            iconSize = std::min(iconSize, sizeof(control->icon));
            if (!writeIconIfMissing(title.iconPath, control->icon, iconSize))
                title.iconPath.clear();
        } else {
            title.iconPath.clear();
            diagnostic_error("installed", title.titleId.c_str(),
                             "event=control_data result=0x%08x", rc);
        }
        next.push_back(std::move(title));
    }

    std::stable_sort(next.begin(), next.end(),
                     [](const InstalledTitle& left,
                        const InstalledTitle& right) {
                         return left.name < right.name;
                     });
    std::unordered_set<std::string> ids;
    ids.reserve(next.size());
    for (const InstalledTitle& title : next)
        ids.insert(title.titleId);
    const size_t count = next.size();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        titles_ = std::move(next);
        titleIds_ = std::move(ids);
        ++generation_;
    }
    log_msg("[installed] loaded %zu applications\n", count);
    telemetry_log("installed", "system",
                  "event=refresh count=%zu duration_ms=%llu", count,
                  static_cast<unsigned long long>(now_ms() - startedMs));
    return true;
}

} // namespace pipensx
