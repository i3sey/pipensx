#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "package_stream.hpp"

namespace pipensx::install {

// Persistent snapshot of an interrupted streaming install
// (IMPROVEMENT_PLAN F-B). Serialized as one bencoded dict: a version tag,
// the package identity (so a stale journal is never applied to a different
// package), the PackageStream safe point and an opaque backend blob with
// backend-specific placeholder bookkeeping.
struct InstallJournal {
    static constexpr int64_t kVersion = 1;

    std::string packageId;     // stable identity (infohash/path/URL)
    uint64_t packageSize = 0;  // total package bytes, 0 when unknown
    bool compressed = false;   // NSZ (true) vs plain NSP
    std::string backendState;  // opaque backend snapshot, may be empty
    PackageStreamState state;

    std::string serialize() const;
    // Strict: any missing key, type mismatch, bad version, malformed
    // length or trailing bytes fails and leaves *this untouched.
    bool load(const char* data, size_t size);
};

// Atomic file helpers (write <path>.tmp, then rename over <path>).
// On failure, errno detail is written to *error when non-null.
bool saveInstallJournal(const std::string& path, const InstallJournal& journal,
                        std::string* error = nullptr);
bool loadInstallJournal(const std::string& path, InstallJournal& journal);
// True when the journal no longer exists (including "never existed").
bool removeInstallJournal(const std::string& path);

// One journal file per task, next to the queue state.
inline std::string installJournalPath(const std::string& root,
                                      const std::string& taskId) {
    return root + "/install-journal-" + taskId + ".bencode";
}

// F5: liveness marker for a live package stream. Written when a package
// stream starts (before the first journal safe point exists) and refreshed
// at every journal save; removed on any clean teardown (commit, pause,
// error rollback). A marker still present at the next startup means the
// previous session died while this install was live — a console reboot,
// panic or hard power-off — and the recorded memory / target headroom is
// the post-mortem the QR log can no longer contain, because the session
// died before it could write one.
struct InstallMarker {
    static constexpr int64_t kVersion = 1;

    std::string taskId;
    std::string packageId;
    uint64_t packageSize = 0;      // total package bytes, 0 when unknown
    uint64_t consumed = 0;         // last safe-point stream position
    uint64_t heapAvailableBytes = 0;    // 0 = not detected
    uint64_t kernelHeadroomBytes = 0;   // 0 = not detected
    uint64_t storageFreeBytes = 0;      // 0 = unknown

    std::string serialize() const;
    // Strict: any missing key, type mismatch, bad version, malformed
    // length or trailing bytes fails and leaves *this untouched.
    bool load(const char* data, size_t size);
};

bool saveInstallMarker(const std::string& path, const InstallMarker& marker,
                       std::string* error = nullptr);
bool loadInstallMarker(const std::string& path, InstallMarker& marker);
// True when the marker no longer exists (including "never existed").
bool removeInstallMarker(const std::string& path);

inline std::string installMarkerPath(const std::string& root,
                                     const std::string& taskId) {
    return root + "/install-active-" + taskId + ".bencode";
}

} // namespace pipensx::install
