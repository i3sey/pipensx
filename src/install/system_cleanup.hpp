#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace pipensx::install {

// System-level installation garbage tracked by Horizon's content manager.
// This is deliberately separate from StorageManager's app-owned files: these
// bytes live in NCM content storage and are invisible under
// sdmc:/switch/pipensx.
struct SystemCleanupSnapshot {
    uint64_t placeholderBytes = 0;
    uint64_t orphanContentBytes = 0;
    uint32_t placeholderCount = 0;
    uint32_t protectedPlaceholderCount = 0;
    uint32_t orphanContentCount = 0;
    uint32_t protectedContentCount = 0;
    uint32_t oldUpdateCount = 0;
    uint32_t unusedTicketCount = 0;
    bool available = false;
    bool ticketsAvailable = false;

    uint64_t knownRecoverableBytes() const {
        return placeholderBytes > UINT64_MAX - orphanContentBytes
            ? UINT64_MAX : placeholderBytes + orphanContentBytes;
    }

    uint32_t installLeftoverItems() const {
        uint64_t total = static_cast<uint64_t>(placeholderCount) +
                         orphanContentCount + oldUpdateCount;
        return total > UINT32_MAX ? UINT32_MAX
                                  : static_cast<uint32_t>(total);
    }
};

struct SystemCleanupOptions {
    bool removePlaceholders = true;
    bool removeOrphanContent = true;
    bool removeOldUpdates = true;
    // A ticket can remain useful for an archived eShop purchase even when no
    // installed content currently references it. Keep this opt-in.
    bool removeUnusedTickets = false;

    static SystemCleanupOptions unusedTicketsOnly() {
        SystemCleanupOptions options;
        options.removePlaceholders = false;
        options.removeOrphanContent = false;
        options.removeOldUpdates = false;
        options.removeUnusedTickets = true;
        return options;
    }
};

// Inspect/clean SD and user NAND content storage. Install journals belonging
// to live tasks protect their resumable placeholders and staged content.
bool scanSystemInstallGarbage(const std::string& rootPath,
                              const std::vector<std::string>& liveTaskIds,
                              SystemCleanupSnapshot& out,
                              std::string& error);

bool clearSystemInstallGarbage(const std::string& rootPath,
                               const std::vector<std::string>& liveTaskIds,
                               const SystemCleanupOptions& options,
                               SystemCleanupSnapshot& removed,
                               std::string& error);

// A removed task must not lose the only map to its NCM placeholders. Reattach
// to its checkpoint, roll it back, and delete the journal atomically from the
// caller's point of view. Missing journals are a successful no-op; damaged
// journals are discarded because they cannot protect or resume any content.
bool discardSavedInstall(const std::string& rootPath,
                         const std::string& taskId,
                         std::string& error);

} // namespace pipensx::install
