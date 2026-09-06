#pragma once

#include "catalog_service.hpp"
#include "download_manager.hpp"
#include "installed_title_service.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace pipensx {

// Largest byte index <= maxBytes that does not split a UTF-8 code point.
// RuTracker file names are Cyrillic, so truncating a user-visible label to a
// byte count must never leave a partial character.
inline size_t utf8TruncateBoundary(const std::string& text, size_t maxBytes) {
    if (maxBytes >= text.size())
        return text.size();
    if (maxBytes == 0)
        return 0;
    size_t lead = maxBytes - 1;
    while (lead > 0 &&
           (static_cast<unsigned char>(text[lead]) & 0xC0) == 0x80)
        --lead;
    const unsigned char b = static_cast<unsigned char>(text[lead]);
    if (b < 0xC0)
        return maxBytes;
    const size_t need = b >= 0xF0 ? 3 : (b >= 0xE0 ? 2 : 1);
    if (lead + 1 + need <= maxBytes)
        return maxBytes;
    return lead;
}

// Indices of the packages whose [vN] tag numerically equals latestVersion —
// the update the metadata index points at (the generator derives
// latestVersion from exactly those tags). When titleId is non-empty, the
// path must carry that title's base id or its Patch id (…800); DLC ids that
// share the base are excluded. Empty when no package matches; selectUpdateFiles
// then falls back to its own heuristics. The caller feeds the recommendation
// into the update-file chooser, which always opens.
std::vector<size_t> updateVersionMatches(const TorrentPreview& preview,
                                         const std::string& latestVersion,
                                         const std::string& titleId = {});

// Per-file action mask installing exactly the packages in `picks` (everything
// else is skipped). `picks` may be empty, yielding an all-skip mask.
std::vector<uint8_t> selectFiles(const TorrentPreview& preview,
                                 const std::vector<size_t>& picks);

// Builds the per-file action mask that installs a release torrent as an
// update of an already-installed title. Release bundles mix base game, update
// and DLC packages; the metadata index derives latestVersion from the [vN]
// tags in release file names.
//
// The package whose [vN] tag equals latestVersion (and whose path carries
// the base or Patch title id when provided) is installed and everything else
// is skipped, so only the update bytes hit the wire. Lookalikes such as a mod bundle named
// "... [v9895936].nsp" beside the real "... [v10092544].nsp" are excluded by
// the exact tag; same-tag mods under a different title id are excluded by
// titleId. Without an exact match, the highest-tagged update-marked package
// is used; when nothing can be identified, the recommendation is all-Skip
// so the chooser opens with Continue disabled until the user picks.
//
// This mask only preselects: the update-file chooser always opens and hands
// the final, user-tuned mask back to the importer.
std::vector<uint8_t> selectUpdateFiles(const TorrentPreview& preview,
                                        const std::string& latestVersion,
                                        const std::string& titleId = {});

// Builds the default one-tap install mask for a catalog release. When the base
// title is not installed, base packages for that title and an exact bundled
// update are selected while DLC/mod/extra packages are skipped. When the title
// is already installed and a newer update is known (catalog latestVersion, or
// the highest base/Patch [vN] in this torrent when that field is empty), only
// an exact update package is selected. AddOnContent packages whose title id maps
// to the selected base title are also installed when not already present
// (installedDlcIds), so an installed game pulls its DLC without re-pulling the
// base. If the intended base/update packages cannot be identified safely, the
// caller should fall back to the manual chooser.
std::vector<uint8_t> selectSmartInstallFiles(
    const TorrentPreview& preview,
    bool titleInstalled,
    const std::string& installedVersion,
    const std::string& latestVersion,
    const std::string& titleId = {},
    const std::vector<std::string>& installedDlcIds = {});

// Settled when the tracked task is gone or in a terminal download status —
// used by InstalledView's post-install re-check tick (and tested directly so
// the snapshot-by-value path cannot grow another dangling pointer).
inline bool updateRecheckTerminal(DownloadStatus status) {
    switch (status) {
    case DownloadStatus::Installed:
    case DownloadStatus::Completed:
    case DownloadStatus::Error:
    case DownloadStatus::Removing:
        return true;
    default:
        return false;
    }
}

inline bool updateRecheckSettled(bool found, DownloadStatus status) {
    return !found || updateRecheckTerminal(status);
}

// Magnet used to resolve the update torrent for `infoHash`. Prefers the
// catalog entry's trusted magnet (trackers plus the pre-resolved info dict);
// falls back to a RuTracker magnet for the hash when the catalog does not
// know it — the resolver only accepts RuTracker trackers, and the index is
// RuTracker-derived.
std::string updateMagnetFor(const std::string& infoHash,
                            const CatalogEntry* entry);

// ---------------------------------------------------------------------------
// B6 update guard: state what is installed before replacing it, and explain
// a post-update launch failure instead of leaving a silent break.
// ---------------------------------------------------------------------------
// Snapshot of the pre-update facts the Installed/Updates UI shows before an
// update replaces the installed title: the eShop x.y.z form of the installed
// and target patch versions ("" when the decimal is unknown), the NACP
// display_version ("1.26.30", "" when unknown) and whether LayeredFS
// mods are installed for the title.
struct UpdatePreflight {
    bool titleInstalled = false;
    std::string installedXyz;
    std::string targetXyz;
    std::string displayVersion;
    bool hasMods = false;
    // Both version ends are known, so the dialog can state the transition.
    bool versionKnown() const {
        return !installedXyz.empty() && !targetXyz.empty();
    }
    // Mods are present on an installed title: the update may conflict with
    // them (B6: BOTW mods, Broforce-style post-update "Program closed").
    bool warnMods() const { return titleInstalled && hasMods; }
};

// Builds the pre-update snapshot from an installed title and the catalog
// latestVersion. Pure and C++17 so the BOTW/Broforce mocks can pin it.
UpdatePreflight describeUpdatePreflight(const InstalledTitle& installed,
                                         const std::string& latestVersion,
                                         bool titleInstalled);

// CNMT required_system_version (u32 at extended-header offset 8) as eShop
// HOS "major.minor.micro". Encoding is (major<<26)|(minor<<20)|(micro<<16)
// (see install_backend_switch.cpp); 0 means "no requirement" and formats
// as an empty string so the caller can hide the row.
std::string formatRequiredHosVersion(uint32_t requiredSystemVersion);

// Test seam for the encoding above.
uint32_t makeRequiredSystemVersion(unsigned major, unsigned minor,
                                   unsigned micro);

// True when the update's required system version is newer than the running
// HOS triple (hosversionGet() split into major/minor/micro). A false here
// never means "safe to launch" — only that firmware is not the known
// blocker.
bool updateRequiresNewerHos(uint32_t requiredSystemVersion,
                             unsigned hosMajor, unsigned hosMinor,
                             unsigned hosMicro);

// Actionable post-failure hint kind for an update that installed but leaves
// the title unable to launch (or an install that failed hex-first).
// None keeps the previous behaviour (raw backend error only).
enum class UpdateFailureHint {
    None,
    // Mods are installed: point at atmosphere/contents/<titleId>/ first.
    ModConflict,
    // The update's required_system_version is newer than the running HOS.
    NeedsNewHos,
    // Ticket import failed with 0x291 / ES signature-patch wording.
    SigPatches,
};

// Picks the hint: signature-patch markers in the backend error win (they
// name the fix), then a firmware requirement newer than HOS, then installed
// mods. Pure so unit tests can pin the BOTW (mods) and Broforce (HOS)
// cases without a console.
UpdateFailureHint classifyUpdateFailure(const std::string& installError,
                                        bool hasMods,
                                        uint32_t requiredSystemVersion,
                                        unsigned hosMajor, unsigned hosMinor,
                                        unsigned hosMicro);

// English fallback text for the hint (unit-test and log surface). The UI
// prefers the pipensx/detail + pipensx/installed locale keys and only uses
// this when a translation is missing. Empty for UpdateFailureHint::None.
std::string formatUpdateFailureHint(UpdateFailureHint hint,
                                    const std::string& targetXyz,
                                    uint32_t requiredSystemVersion);

} // namespace pipensx
