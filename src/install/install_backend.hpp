#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace pipensx::install {

// Where committed content lands. SD is the historical default; NAND
// (BuiltInUser / eMMC) is the write-ceiling experiment from PERF_PLAN 7.4 —
// eMMC is typically faster than the SD/FS path that caps install at ~16 MB/s.
enum class InstallStorageTarget {
    SdCard,
    Nand,
};

// Safe margin reserved in system memory (BuiltInUser / NAND) to prevent
// Horizon OS panics or NCM allocation failures when partition space is low.
inline constexpr uint64_t kNandSafetyMarginBytes = 512 * 1024 * 1024; // 512 MB

class InstallBackend {
public:
    // Diagnostic passes write fresh owned placeholders but never register
    // content, title metadata or tickets. Unsupported backends fail closed.
    virtual bool setBenchmarkMode(bool enabled) { return !enabled; }
    virtual ~InstallBackend() = default;

    virtual bool beginPackage(const std::string& taskId,
                              const std::string& packageName) = 0;
    // Entries the backend wants dropped before any processing (PERF_PLAN 3.4:
    // delta fragments identified by an early CNMT parse). Default: keep all.
    virtual bool shouldSkipFile(const std::string& name) const {
        (void)name;
        return false;
    }
    virtual bool beginFile(const std::string& name, uint64_t size) = 0;
    virtual bool setFileSize(uint64_t size) = 0;
    virtual bool writeFile(const uint8_t* data, size_t size) = 0;
    virtual bool endFile() = 0;
    virtual bool commitPackage(bool& alreadyInstalled) = 0;
    virtual void rollbackPackage() = 0;

    // IMPROVEMENT_PLAN F-B: persistent resume of interrupted installs.
    //
    // checkpointPackage() snapshots backend bookkeeping (per-content
    // progress, ncm placeholder ids, the running SHA-256 state) as an opaque
    // blob for the install journal. Must be taken at a package-stream safe
    // point (between writeFile() calls); may flush buffered output so the
    // on-disk state matches the snapshot. Empty = nothing to checkpoint.
    virtual std::string checkpointPackage() { return {}; }

    // Detach from the in-flight package keeping partially written data on
    // disk (files / ncm placeholders) so a later resumePackage() can
    // continue. Backends without resume support fall back to full rollback.
    virtual void suspendPackage() { rollbackPackage(); }

    // Re-attach to a suspended package from a checkpointPackage() blob.
    // Verifies the recorded artifacts still exist and match; on failure any
    // leftover partial data is discarded, the backend stays idle and the
    // caller should fall back to beginPackage(). Streaming then continues
    // with writeFile() from the journaled stream position.
    virtual bool resumePackage(const std::string& taskId,
                               const std::string& packageName,
                               const std::string& state) {
        (void)taskId;
        (void)packageName;
        (void)state;
        return false;
    }

    virtual uint64_t installedBytes() const = 0;
    virtual uint64_t expectedBytes() const = 0;
    virtual const std::string& error() const = 0;

    // Base application id parsed from the package CNMT after the package
    // stream has finished, but before commit. Zero means it is not available.
    // LayeredFS transactions use this to reject an NSP for a different title
    // before any title metadata is committed. Declared last so adding it does
    // not shift the vtable slots of any pre-existing virtual.
    virtual uint64_t packageApplicationId() const { return 0; }

    // F5: free bytes on the install target right now (0 = unknown). Sampled
    // into the install liveness marker so a reboot mid-install can report
    // whether the target was running out of space. Declared last for the
    // same vtable reason as packageApplicationId().
    virtual uint64_t freeSpaceBytes() const { return 0; }
};

struct TitleStorageFootprint {
    uint64_t totalBytes = 0;
    uint64_t reclaimableBytes = 0;
    uint64_t controlBytes = 0;
    uint32_t metaRecordCount = 0;
    uint32_t contentCount = 0;
    bool hasSdContent = false;
    bool hasNandContent = false;
    bool found = false;
};

std::unique_ptr<InstallBackend> createInstallBackend(
    const std::string& workingRoot,
    InstallStorageTarget target = InstallStorageTarget::SdCard);

// Query NCM storage footprint (base + patch + DLC) for an installed title.
bool inspectInstalledTitleFootprint(const std::string& workingRoot,
                                    uint64_t applicationId,
                                    TitleStorageFootprint& out,
                                    std::string& error);

// Remove registered NCM content and metadata for applicationId across SD and
// NAND without modifying SaveData or installed tickets.
bool removeTitleContentPreservingSaves(const std::string& workingRoot,
                                       uint64_t applicationId,
                                       uint64_t* freedBytes,
                                       std::string& error);

} // namespace pipensx::install
