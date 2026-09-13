#include "system_cleanup.hpp"

#include "install_backend.hpp"
#include "install_journal.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#ifdef __SWITCH__
#include <switch.h>
#include <switch-ipcext.h>

extern "C" {
#include "../core/util.h"
}
#endif

namespace pipensx::install {
namespace {

#ifdef __SWITCH__
bool addBytes(uint64_t& target, uint64_t value) {
    if (value > UINT64_MAX - target) {
        target = UINT64_MAX;
        return false;
    }
    target += value;
    return true;
}

bool checkpointTarget(const std::string& state,
                      InstallStorageTarget& target) {
    if (state.size() < 12 || state.compare(0, 4, "NXB1") != 0)
        return false;
    uint64_t storage = 0;
    for (unsigned i = 0; i < 8; ++i)
        storage |= static_cast<uint64_t>(
            static_cast<unsigned char>(state[4 + i])) << (i * 8);
    if (storage == static_cast<uint64_t>(NcmStorageId_SdCard)) {
        target = InstallStorageTarget::SdCard;
        return true;
    }
    if (storage == static_cast<uint64_t>(NcmStorageId_BuiltInUser)) {
        target = InstallStorageTarget::Nand;
        return true;
    }
    return false;
}
#endif

} // namespace

bool discardSavedInstall(const std::string& rootPath,
                         const std::string& taskId,
                         std::string& error) {
    error.clear();
    const std::string path = installJournalPath(rootPath, taskId);
    InstallJournal journal;
    if (!loadInstallJournal(path, journal)) {
        // Distinguish a missing journal from an unreadable path. A corrupt
        // journal cannot be resumed or used to identify its placeholders;
        // once the user explicitly removes the task, forget it so the global
        // orphan/placeholder cleanup is no longer blocked by that task.
        errno = 0;
        std::FILE* file = std::fopen(path.c_str(), "rb");
        if (!file) {
            if (errno == ENOENT)
                return true;
            error = "Unable to read the saved install journal.";
            return false;
        }
        std::fclose(file);
        if (!removeInstallJournal(path)) {
            error = "Unable to remove the damaged install journal.";
            return false;
        }
        return true;
    }

    InstallStorageTarget target = InstallStorageTarget::SdCard;
#ifdef __SWITCH__
    if (!checkpointTarget(journal.backendState, target)) {
        error = "The saved install backend state is invalid.";
        return false;
    }
#endif
    std::unique_ptr<InstallBackend> backend =
        createInstallBackend(rootPath, target);
    if (!backend->resumePackage(taskId, journal.packageId,
                                journal.backendState)) {
        error = backend->error().empty()
            ? "Unable to reopen the saved install state."
            : backend->error();
        return false;
    }
    backend->rollbackPackage();
    if (!removeInstallJournal(path)) {
        error = "Unable to remove the saved install journal.";
        return false;
    }
    return true;
}

#ifdef __SWITCH__
namespace {

constexpr s32 kMaxPlaceholders = 32768;
constexpr s32 kMaxContents = 131072;
constexpr s32 kMaxPatchMetas = 8192;
constexpr u32 kMaxTickets = 65536;

std::string rawId(const void* bytes, size_t size) {
    return std::string(static_cast<const char*>(bytes), size);
}

std::string resultError(const char* operation, Result rc) {
    char text[192];
    std::snprintf(text, sizeof(text), "%s (0x%08x).", operation, rc);
    return text;
}

class CheckpointReader {
public:
    explicit CheckpointReader(const std::string& value)
        : data_(value.data()), size_(value.size()) {}

    bool raw(void* output, size_t count) {
        if (size_ - pos_ < count)
            return false;
        if (output)
            std::memcpy(output, data_ + pos_, count);
        pos_ += count;
        return true;
    }
    bool u8(uint8_t& value) { return raw(&value, sizeof(value)); }
    bool u64(uint64_t& value) {
        uint8_t bytes[8];
        if (!raw(bytes, sizeof(bytes)))
            return false;
        value = 0;
        for (unsigned i = 0; i < 8; ++i)
            value |= static_cast<uint64_t>(bytes[i]) << (i * 8);
        return true;
    }
    bool skipBytes() {
        uint64_t count = 0;
        return u64(count) && count <= size_ - pos_ && raw(nullptr, count);
    }
    bool str(std::string& value) {
        uint64_t count = 0;
        if (!u64(count) || count > size_ - pos_)
            return false;
        value.assign(data_ + pos_, static_cast<size_t>(count));
        pos_ += static_cast<size_t>(count);
        return true;
    }
    bool done() const { return pos_ == size_; }

private:
    const char* data_;
    size_t size_;
    size_t pos_ = 0;
};

struct ProtectedStorage {
    std::unordered_set<std::string> placeholders;
    std::unordered_set<std::string> contents;
};

struct ProtectedInstallState {
    ProtectedStorage sd;
    ProtectedStorage nand;
};

ProtectedStorage* mutableProtectedFor(ProtectedInstallState& state,
                                      uint64_t storageId) {
    if (storageId == static_cast<uint64_t>(NcmStorageId_SdCard))
        return &state.sd;
    if (storageId == static_cast<uint64_t>(NcmStorageId_BuiltInUser))
        return &state.nand;
    return nullptr;
}

const ProtectedStorage& protectedForStorage(
    const ProtectedInstallState& state, NcmStorageId storageId) {
    return storageId == NcmStorageId_SdCard ? state.sd : state.nand;
}

bool parseProtectedCheckpoint(const std::string& blob,
                              const std::string& expectedTaskId,
                              ProtectedInstallState& protectedState) {
    CheckpointReader in(blob);
    char magic[4] = {};
    uint64_t storageId = 0;
    std::string taskId;
    std::string ignored;
    uint64_t value = 0;
    if (!in.raw(magic, sizeof(magic)) ||
        std::memcmp(magic, "NXB1", sizeof(magic)) != 0 ||
        !in.u64(storageId) || !in.str(taskId) || taskId != expectedTaskId ||
        !in.str(ignored) || !in.u64(value) || !in.u64(value) ||
        !in.u64(value) || !in.str(ignored) || !in.u64(value) ||
        !in.skipBytes() || !in.skipBytes() || !in.skipBytes() ||
        !in.str(ignored))
        return false;

    ProtectedStorage* target = mutableProtectedFor(protectedState, storageId);
    if (!target)
        return false;
    uint64_t contentCount = 0;
    if (!in.u64(contentCount) || contentCount > 4096)
        return false;

    std::vector<std::pair<std::string, bool>> placeholders;
    std::vector<std::pair<std::string, bool>> contents;
    placeholders.reserve(static_cast<size_t>(contentCount));
    contents.reserve(static_cast<size_t>(contentCount));
    for (uint64_t i = 0; i < contentCount; ++i) {
        NcmContentId contentId {};
        NcmPlaceHolderId placeholderId {};
        uint64_t size = 0;
        uint64_t written = 0;
        uint8_t flags = 0;
        if (!in.str(ignored) ||
            !in.raw(contentId.c, sizeof(contentId.c)) ||
            !in.raw(&placeholderId, sizeof(placeholderId)) ||
            !in.u64(size) || !in.u64(written) || !in.u8(flags) ||
            written > size || (flags & ~7u))
            return false;
        const bool existing = (flags & 1u) != 0;
        const bool registered = (flags & 2u) != 0;
        if (!existing && registered)
            contents.emplace_back(
                rawId(contentId.c, sizeof(contentId.c)), true);
        else if (!existing && !registered && size)
            placeholders.emplace_back(
                rawId(&placeholderId, sizeof(placeholderId)), true);
    }
    uint64_t currentIndex = 0;
    uint8_t hashActive = 0;
    if (!in.u64(currentIndex) || currentIndex > contentCount ||
        !in.u8(hashActive) || hashActive > 1)
        return false;
    if (hashActive && !in.skipBytes())
        return false;
    uint64_t skippedCount = 0;
    if (!in.u64(skippedCount) || skippedCount > 4096)
        return false;
    for (uint64_t i = 0; i < skippedCount; ++i) {
        NcmContentId skipped {};
        if (!in.raw(skipped.c, sizeof(skipped.c)))
            return false;
    }
    if (!in.done())
        return false;
    for (const auto& item : placeholders)
        target->placeholders.insert(item.first);
    for (const auto& item : contents)
        target->contents.insert(item.first);
    return true;
}

bool collectProtected(const std::string& rootPath,
                      const std::vector<std::string>& liveTaskIds,
                      ProtectedInstallState& out, std::string& error) {
    for (const std::string& taskId : liveTaskIds) {
        InstallJournal journal;
        const std::string path = installJournalPath(rootPath, taskId);
        if (!loadInstallJournal(path, journal)) {
            std::FILE* file = std::fopen(path.c_str(), "rb");
            if (!file)
                continue;
            std::fclose(file);
            error = "A live install journal is damaged; cleanup was cancelled.";
            return false;
        }
        if (!parseProtectedCheckpoint(journal.backendState, taskId, out)) {
            error = "A live install checkpoint is invalid; cleanup was cancelled.";
            return false;
        }
    }
    return true;
}

struct StorageInventory {
    NcmStorageId id = NcmStorageId_None;
    NcmContentStorage storage {};
    NcmContentMetaDatabase database {};
    std::vector<NcmPlaceHolderId> placeholders;
    std::vector<NcmContentId> contents;
    std::vector<NcmContentMetaKey> patchKeys;
    bool open = false;
};

void closeInventory(StorageInventory& inventory) {
    if (!inventory.open)
        return;
    ncmContentMetaDatabaseClose(&inventory.database);
    ncmContentStorageClose(&inventory.storage);
    inventory.open = false;
}

bool loadInventory(StorageInventory& inventory, NcmStorageId id,
                   std::string& error) {
    inventory.id = id;
    Result rc = ncmOpenContentStorage(&inventory.storage, id);
    if (R_SUCCEEDED(rc))
        rc = ncmOpenContentMetaDatabase(&inventory.database, id);
    if (R_FAILED(rc)) {
        ncmContentStorageClose(&inventory.storage);
        error = resultError("Unable to open system content storage", rc);
        return false;
    }
    inventory.open = true;

    inventory.placeholders.resize(kMaxPlaceholders);
    s32 written = 0;
    rc = ncmContentStorageListPlaceHolder(
        &inventory.storage, inventory.placeholders.data(),
        static_cast<s32>(inventory.placeholders.size()), &written);
    if (R_FAILED(rc) || written < 0 || written >= kMaxPlaceholders) {
        error = R_FAILED(rc)
            ? resultError("Unable to list installation placeholders", rc)
            : "Too many installation placeholders to clean safely.";
        return false;
    }
    inventory.placeholders.resize(static_cast<size_t>(written));

    s32 contentCount = 0;
    rc = ncmContentStorageGetContentCount(&inventory.storage, &contentCount);
    if (R_FAILED(rc) || contentCount < 0 || contentCount > kMaxContents) {
        error = R_FAILED(rc)
            ? resultError("Unable to count installed content", rc)
            : "Too many installed content records to clean safely.";
        return false;
    }
    inventory.contents.resize(static_cast<size_t>(contentCount));
    s32 offset = 0;
    while (offset < contentCount) {
        s32 pageWritten = 0;
        const s32 page = std::min<s32>(512, contentCount - offset);
        rc = ncmContentStorageListContentId(
            &inventory.storage, inventory.contents.data() + offset,
            page, &pageWritten, offset);
        if (R_FAILED(rc) || pageWritten <= 0 || pageWritten > page) {
            error = R_FAILED(rc)
                ? resultError("Unable to list installed content", rc)
                : "Installed content listing stopped unexpectedly.";
            return false;
        }
        offset += pageWritten;
    }

    s32 total = 0;
    written = 0;
    rc = ncmContentMetaDatabaseList(
        &inventory.database, &total, &written, nullptr, 0,
        NcmContentMetaType_Patch, 0, 0, UINT64_MAX,
        NcmContentInstallType_Unknown);
    if (R_FAILED(rc) || total < 0 || total > kMaxPatchMetas) {
        error = R_FAILED(rc)
            ? resultError("Unable to list installed updates", rc)
            : "Too many installed updates to clean safely.";
        return false;
    }
    inventory.patchKeys.resize(static_cast<size_t>(total));
    if (total) {
        rc = ncmContentMetaDatabaseList(
            &inventory.database, &total, &written,
            inventory.patchKeys.data(),
            static_cast<s32>(inventory.patchKeys.size()),
            NcmContentMetaType_Patch, 0, 0, UINT64_MAX,
            NcmContentInstallType_Unknown);
        if (R_FAILED(rc) || written < 0 || written > total) {
            error = R_FAILED(rc)
                ? resultError("Unable to read installed updates", rc)
                : "Installed update listing is inconsistent.";
            return false;
        }
        inventory.patchKeys.resize(static_cast<size_t>(written));
    }
    return true;
}

struct OldUpdate {
    NcmStorageId storage;
    NcmContentMetaKey key;
};

std::vector<OldUpdate> findOldUpdates(
    const std::array<StorageInventory, 2>& inventories) {
    std::unordered_map<uint64_t, uint32_t> newest;
    for (const auto& inventory : inventories)
        for (const auto& key : inventory.patchKeys) {
            auto found = newest.find(key.id);
            if (found == newest.end() || found->second < key.version)
                newest[key.id] = key.version;
        }
    std::vector<OldUpdate> old;
    for (const auto& inventory : inventories)
        for (const auto& key : inventory.patchKeys)
            if (key.version < newest[key.id])
                old.push_back({inventory.id, key});
    return old;
}

bool keyEqual(const NcmContentMetaKey& left,
              const NcmContentMetaKey& right) {
    return left.id == right.id && left.version == right.version &&
           left.type == right.type &&
           left.install_type == right.install_type;
}

void removeOldApplicationRecords(const std::vector<OldUpdate>& old) {
    std::unordered_map<uint64_t, std::vector<OldUpdate>> byApplication;
    for (const OldUpdate& item : old)
        byApplication[item.key.id ^ 0x800ULL].push_back(item);
    for (const auto& group : byApplication) {
        s32 count = 0;
        if (R_FAILED(nsCountApplicationContentMeta(group.first, &count)) ||
            count <= 0)
            continue;
        std::vector<NsExtContentStorageMetaKey> records(
            static_cast<size_t>(count));
        uint32_t written = 0;
        if (R_FAILED(nsextListApplicationRecordContentMeta(
                0, group.first, records.data(), records.size(), &written)))
            continue;
        records.resize(std::min<size_t>(written, records.size()));
        records.erase(std::remove_if(records.begin(), records.end(),
            [&group](const NsExtContentStorageMetaKey& record) {
                for (const OldUpdate& item : group.second)
                    if (record.storage_id ==
                            static_cast<uint64_t>(item.storage) &&
                        keyEqual(record.meta_key, item.key))
                        return true;
                return false;
            }), records.end());
        if (R_FAILED(nsextDeleteApplicationRecord(group.first)))
            continue;
        if (!records.empty() && R_FAILED(nsextPushApplicationRecord(
                group.first, NsExtApplicationEvent_Present,
                records.data(), records.size())))
            diagnostic_error("storage", "records",
                             "app=%016llx event=rebuild_failed",
                             (unsigned long long)group.first);
    }
}

bool lookupOrphans(StorageInventory& inventory,
                   std::vector<uint8_t>& orphaned,
                   std::string& error) {
    orphaned.assign(inventory.contents.size(), 0);
    size_t offset = 0;
    while (offset < inventory.contents.size()) {
        const s32 count = static_cast<s32>(std::min<size_t>(
            256, inventory.contents.size() - offset));
        std::array<bool, 256> page {};
        Result rc = ncmContentMetaDatabaseLookupOrphanContent(
            &inventory.database, page.data(),
            inventory.contents.data() + offset, count);
        if (R_FAILED(rc)) {
            error = resultError("Unable to identify orphaned content", rc);
            return false;
        }
        for (s32 i = 0; i < count; ++i)
            orphaned[offset + static_cast<size_t>(i)] = page[i] ? 1 : 0;
        offset += static_cast<size_t>(count);
    }
    return true;
}

bool contentSize(NcmContentStorage& storage, const NcmContentId& id,
                 uint64_t& size, std::string& error) {
    s64 value = 0;
    Result rc = ncmContentStorageGetSizeFromContentId(&storage, &value, &id);
    if (R_FAILED(rc) || value < 0) {
        error = R_FAILED(rc) ? resultError("Unable to size installed content", rc)
                             : "Installed content has an invalid size.";
        return false;
    }
    size = static_cast<uint64_t>(value);
    return true;
}

bool placeholderSize(NcmContentStorage& storage,
                     const NcmPlaceHolderId& id, uint64_t& size,
                     std::string& error) {
    s64 value = 0;
    Result rc = ncmContentStorageGetSizeFromPlaceHolderId(
        &storage, &value, &id);
    if (R_FAILED(rc) || value < 0) {
        error = R_FAILED(rc)
            ? resultError("Unable to size installation placeholder", rc)
            : "Installation placeholder has an invalid size.";
        return false;
    }
    size = static_cast<uint64_t>(value);
    return true;
}

bool collectUsedRights(
    std::array<StorageInventory, 2>& inventories,
    const std::array<std::vector<uint8_t>, 2>& orphaned,
    std::unordered_set<std::string>& used, std::string& error) {
    for (size_t s = 0; s < inventories.size(); ++s) {
        auto& inventory = inventories[s];
        for (size_t i = 0; i < inventory.contents.size(); ++i) {
            if (orphaned[s][i])
                continue;
            NcmRightsId rights {};
            Result rc = ncmContentStorageGetRightsIdFromContentId(
                &inventory.storage, &rights,
                &inventory.contents[i], FsContentAttributes_All);
            if (R_FAILED(rc)) {
                error = resultError("Unable to inspect content rights", rc);
                return false;
            }
            static const std::array<uint8_t, 16> zero {};
            if (std::memcmp(rights.rights_id.c, zero.data(), zero.size()) != 0)
                used.insert(rawId(rights.rights_id.c,
                                  sizeof(rights.rights_id.c)));
        }
    }
    return true;
}

bool listTickets(std::vector<EsRightsId>& tickets, std::string& error) {
    const u32 common = esCountCommonTicket();
    const u32 personal = esCountPersonalizedTicket();
    if (common > kMaxTickets || personal > kMaxTickets ||
        static_cast<uint64_t>(common) + personal > kMaxTickets) {
        error = "Too many tickets to clean safely.";
        return false;
    }
    std::vector<EsRightsId> page(std::max(common, personal));
    u32 written = 0;
    if (common) {
        Result rc = esListCommonTicket(&written, page.data(),
                                       common * sizeof(EsRightsId));
        if (R_FAILED(rc) || written > common) {
            error = R_FAILED(rc) ? resultError("Unable to list tickets", rc)
                                 : "Ticket listing is inconsistent.";
            return false;
        }
        tickets.insert(tickets.end(), page.begin(), page.begin() + written);
    }
    written = 0;
    if (personal) {
        Result rc = esListPersonalizedTicket(&written, page.data(),
                                             personal * sizeof(EsRightsId));
        if (R_FAILED(rc) || written > personal) {
            error = R_FAILED(rc) ? resultError("Unable to list tickets", rc)
                                 : "Ticket listing is inconsistent.";
            return false;
        }
        tickets.insert(tickets.end(), page.begin(), page.begin() + written);
    }
    return true;
}

bool runSystemCleanup(const std::string& rootPath,
                      const std::vector<std::string>& liveTaskIds,
                      bool remove, const SystemCleanupOptions& options,
                      SystemCleanupSnapshot& out,
                      std::string& error) {
    out = {};
    error.clear();
    ProtectedInstallState protectedState;
    if (!collectProtected(rootPath, liveTaskIds, protectedState, error))
        return false;

    std::array<StorageInventory, 2> inventories;
    const NcmStorageId storageIds[2] = {NcmStorageId_BuiltInUser,
                                        NcmStorageId_SdCard};
    for (size_t i = 0; i < inventories.size(); ++i) {
        if (!loadInventory(inventories[i], storageIds[i], error)) {
            for (auto& inventory : inventories)
                closeInventory(inventory);
            return false;
        }
    }

    const std::vector<OldUpdate> oldUpdates = findOldUpdates(inventories);
    if (!remove || options.removeOldUpdates)
        out.oldUpdateCount = static_cast<uint32_t>(oldUpdates.size());
    if (remove && options.removeOldUpdates && !oldUpdates.empty()) {
        for (const OldUpdate& item : oldUpdates) {
            StorageInventory& inventory =
                item.storage == inventories[0].id ? inventories[0]
                                                   : inventories[1];
            Result rc = ncmContentMetaDatabaseRemove(&inventory.database,
                                                     &item.key);
            if (R_FAILED(rc)) {
                error = resultError("Unable to remove an old update", rc);
                for (auto& opened : inventories)
                    closeInventory(opened);
                return false;
            }
        }
        for (auto& inventory : inventories) {
            Result rc = ncmContentMetaDatabaseCommit(&inventory.database);
            if (R_FAILED(rc)) {
                error = resultError("Unable to commit update cleanup", rc);
                for (auto& opened : inventories)
                    closeInventory(opened);
                return false;
            }
        }
        removeOldApplicationRecords(oldUpdates);
    }

    std::array<std::vector<uint8_t>, 2> orphaned;
    for (size_t s = 0; s < inventories.size(); ++s) {
        StorageInventory& inventory = inventories[s];
        const ProtectedStorage& keep = protectedForStorage(protectedState,
                                                            inventory.id);
        for (const NcmPlaceHolderId& id : inventory.placeholders) {
            if (keep.placeholders.count(rawId(&id, sizeof(id)))) {
                ++out.protectedPlaceholderCount;
                continue;
            }
            if (remove && !options.removePlaceholders)
                continue;
            uint64_t bytes = 0;
            if (!placeholderSize(inventory.storage, id, bytes, error)) {
                for (auto& opened : inventories)
                    closeInventory(opened);
                return false;
            }
            if (remove && options.removePlaceholders) {
                Result rc = ncmContentStorageDeletePlaceHolder(
                    &inventory.storage, &id);
                if (R_FAILED(rc)) {
                    error = resultError("Unable to delete a placeholder", rc);
                    for (auto& opened : inventories)
                        closeInventory(opened);
                    return false;
                }
            }
            if (!remove || options.removePlaceholders) {
                ++out.placeholderCount;
                addBytes(out.placeholderBytes, bytes);
            }
        }

        if (!lookupOrphans(inventory, orphaned[s], error)) {
            for (auto& opened : inventories)
                closeInventory(opened);
            return false;
        }
        for (size_t i = 0; i < inventory.contents.size(); ++i) {
            if (!orphaned[s][i])
                continue;
            const NcmContentId& id = inventory.contents[i];
            if (keep.contents.count(rawId(id.c, sizeof(id.c)))) {
                orphaned[s][i] = 0;
                ++out.protectedContentCount;
                continue;
            }
            if (remove && !options.removeOrphanContent)
                continue;
            uint64_t bytes = 0;
            if (!contentSize(inventory.storage, id, bytes, error)) {
                for (auto& opened : inventories)
                    closeInventory(opened);
                return false;
            }
            if (remove && options.removeOrphanContent) {
                Result rc = ncmContentStorageDelete(&inventory.storage, &id);
                if (R_FAILED(rc)) {
                    error = resultError("Unable to delete orphaned content", rc);
                    for (auto& opened : inventories)
                        closeInventory(opened);
                    return false;
                }
            }
            if (!remove || options.removeOrphanContent) {
                ++out.orphanContentCount;
                addBytes(out.orphanContentBytes, bytes);
            }
        }
    }

    if (!remove || options.removeUnusedTickets) {
        std::unordered_set<std::string> usedRights;
        std::vector<EsRightsId> tickets;
        const bool ticketsOk = collectUsedRights(
            inventories, orphaned, usedRights, error) &&
            listTickets(tickets, error);
        if (!ticketsOk) {
            if (remove) {
                for (auto& opened : inventories)
                    closeInventory(opened);
                return false;
            }
            diagnostic_error("storage", "ticket_scan", "error=%s",
                             error.c_str());
            error.clear();
        } else {
            std::unordered_set<std::string> seenTickets;
            for (const EsRightsId& ticket : tickets) {
                const std::string id = rawId(ticket.fs_id.c,
                                             sizeof(ticket.fs_id.c));
                if (!seenTickets.insert(id).second || usedRights.count(id))
                    continue;
                if (remove) {
                    Result rc = esDeleteTicket(&ticket);
                    if (R_FAILED(rc)) {
                        error = resultError(
                            "Unable to delete an unused ticket", rc);
                        for (auto& opened : inventories)
                            closeInventory(opened);
                        return false;
                    }
                }
                ++out.unusedTicketCount;
            }
            out.ticketsAvailable = true;
        }
    }

    for (auto& inventory : inventories)
        closeInventory(inventory);
    out.available = true;
    telemetry_log("storage", "system_cleanup",
        "event=%s placeholders=%u placeholder_bytes=%llu orphan_content=%u "
        "orphan_bytes=%llu old_updates=%u unused_tickets=%u protected_placeholders=%u protected_content=%u",
        remove ? "clean" : "scan", out.placeholderCount,
        (unsigned long long)out.placeholderBytes, out.orphanContentCount,
        (unsigned long long)out.orphanContentBytes, out.oldUpdateCount,
        out.unusedTicketCount, out.protectedPlaceholderCount,
        out.protectedContentCount);
    return true;
}

} // namespace

bool scanSystemInstallGarbage(const std::string& rootPath,
                              const std::vector<std::string>& liveTaskIds,
                              SystemCleanupSnapshot& out,
                              std::string& error) {
    return runSystemCleanup(rootPath, liveTaskIds, false,
                            SystemCleanupOptions {}, out, error);
}

bool clearSystemInstallGarbage(const std::string& rootPath,
                               const std::vector<std::string>& liveTaskIds,
                               const SystemCleanupOptions& options,
                               SystemCleanupSnapshot& removed,
                               std::string& error) {
    return runSystemCleanup(rootPath, liveTaskIds, true, options, removed,
                            error);
}

#else

bool scanSystemInstallGarbage(const std::string&,
                              const std::vector<std::string>&,
                              SystemCleanupSnapshot& out,
                              std::string& error) {
    out = {};
    error.clear();
    return true;
}

bool clearSystemInstallGarbage(const std::string&,
                               const std::vector<std::string>&,
                               const SystemCleanupOptions&,
                               SystemCleanupSnapshot& removed,
                               std::string& error) {
    removed = {};
    error.clear();
    return true;
}

#endif

} // namespace pipensx::install
