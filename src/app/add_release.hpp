#pragma once

#include "debrid_provider.hpp"
#include "download_manager.hpp"
#include "game_update_install.hpp"
#include "install_space.hpp"
#include "torrent_metainfo_fetch.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace pipensx {

// What the user is asking to add. Catalog is a known release (magnet plus
// optional info dict). Magnet and TorrentFile are arbitrary.
enum class AddInputKind { Catalog, Magnet, TorrentFile };

// What a source can accept. Direct talks to the swarm. Every current debrid
// provider accepts a catalog release, an arbitrary magnet, and a .torrent file
// without putting the console on the swarm. A future provider that cannot do
// one of those must say so here — the add path refuses before it queues.
struct AddCapabilities {
    bool catalog = false;
    bool arbitraryMagnet = false;
    bool torrentFile = false;
    bool usesSwarm = false;
};

AddCapabilities directAddCapabilities();
AddCapabilities debridAddCapabilities(DebridProviderKind kind);

const char* addSourceProviderName(DebridProviderKind kind);

// Saved source: torrenting on means Direct, even when a debrid account is
// linked. Torrenting off means the selected debrid provider, and only when
// its key or TorrServer URL is set. This never turns Direct on.
struct AddSourceChoice {
    bool accepted = false;
    bool direct = false;
    DebridProviderKind provider = DebridProviderKind::TorBox;
    std::string credential;
    std::string refusal;
};

AddSourceChoice decideAddSource(bool torrentingEnabled,
                                DebridProviderKind provider,
                                const std::string& credential,
                                AddInputKind input);

// Test seam: pass capabilities that differ from the live matrix.
AddSourceChoice decideAddSource(bool torrentingEnabled,
                                DebridProviderKind provider,
                                const std::string& credential,
                                AddInputKind input,
                                const AddCapabilities& directCaps,
                                const AddCapabilities& debridCaps);

// Installed-title facts the console one-tap uses to pick base, update, and
// DLC. Empty means the title is not installed and no catalog latest version
// is known — selectSmartInstallFiles then keeps the base and any bundled
// update it can see in the file list.
struct OneTapContext {
    bool titleInstalled = false;
    std::string installedVersion;
    std::string latestVersion;
    std::string titleId;
    std::vector<std::string> installedDlcIds;
};

enum class OneTapOutcome { QueueStream, QueuePort, NeedsChooser };

enum class OneTapBlock {
    None,
    Empty,
    NoPackages,
    CartridgeOnly,
    NothingSelected,
};

// The file list the console detail card queues for one release. Web catalog
// Install uses the same plan so the phone does not pick a different mode or
// mask. NeedsChooser is the case the console opens the file picker for; the
// phone explains it instead of queueing a different selection.
struct OneTapPlan {
    OneTapOutcome outcome = OneTapOutcome::NeedsChooser;
    OneTapBlock block = OneTapBlock::Empty;
    TransferMode mode = TransferMode::StreamInstall;
    TransferMode chooserMode = TransferMode::DownloadOnly;
    std::vector<uint8_t> fileSelection;
    uint32_t packageCount = 0;
    SkippedExtraNotice extras = SkippedExtraNotice::None;
};

OneTapPlan planCatalogOneTap(const TorrentPreview& preview,
                             const OneTapContext& context);

// English explanation for a phone add that would have opened the console
// file picker. Empty when the plan can be queued.
std::string oneTapBlockedMessage(const OneTapPlan& plan);

// Settings-driven install used by batch prepare and by arbitrary magnet /
// .torrent adds. Does not apply the caller's empty-selection fallback.
struct SettingsInstallPlan {
    TransferMode mode = TransferMode::DownloadOnly;
    std::vector<uint8_t> fileSelection;
    InstallSpaceEstimate space;
};

SettingsInstallPlan planSettingsInstall(const TorrentPreview& preview,
                                        TransferMode requested,
                                        StreamSelection selection);

// Web magnet/torrent install: a stream request with nothing to install
// becomes a plain download. Batch keeps its own PackagesOnly failure.
void fallbackInstallToDownload(SettingsInstallPlan& plan,
                               const TorrentPreview& preview);

TorrentPreview previewFromDebridFiles(const DebridInfo& info,
                                      const std::string& fallbackName = {},
                                      uint64_t fallbackBytes = 0);

using DebridProviderFactory = std::function<std::unique_ptr<DebridProvider>(
    DebridProviderKind kind, const std::string& credential)>;

struct AddRequest {
    AddInputKind input = AddInputKind::Magnet;
    std::string title;
    std::string magnetUri;
    std::string infoHashHex;
    std::vector<uint8_t> infoDict;
    std::string titleId;
    TransferMode requestedMode = TransferMode::StreamInstall;
    StreamSelection selection = StreamSelection::AllFiles;
    OneTapContext oneTap;
};

struct AddOutcome {
    bool ok = false;
    bool refused = false;
    std::string error;
    std::string taskId;
    std::string name;
    TaskSource source = TaskSource::Torrent;
    DebridProviderKind provider = DebridProviderKind::TorBox;
    TransferMode mode = TransferMode::DownloadOnly;
};

// Prepares and imports one add the way the console, batch installer, and web
// companion share: the saved source, then one plan for mode and files.
class ReleaseAdder {
public:
    explicit ReleaseAdder(DownloadManager& manager,
                          DebridProviderFactory factory = {});

    AddSourceChoice choose(AddInputKind input) const;

    // Direct only. torrentPath is a resolved .torrent the caller owns.
    AddOutcome importResolvedTorrent(
        const AddRequest& request, const std::string& torrentPath,
        const std::vector<uint8_t>& initialPeers);

    // Debrid magnet or catalog release. Does not resolve over the swarm.
    AddOutcome addViaDebrid(
        const AddRequest& request, std::atomic<bool>& cancelled,
        const std::function<void(DebridCreateStage)>& onStage = {});

    // A .torrent already on disk. Direct imports it; debrid uploads it.
    AddOutcome addTorrentFile(const AddRequest& request,
                              const std::string& torrentPath,
                              std::atomic<bool>& cancelled);

private:
    std::unique_ptr<DebridProvider> makeProvider(const AddSourceChoice& choice,
                                                 std::string& error) const;
    AddOutcome importDebridPlan(const AddRequest& request,
                                const AddSourceChoice& choice,
                                DebridProvider& provider,
                                const std::string& debridId,
                                const DebridInfo& info);

    DownloadManager& manager_;
    DebridProviderFactory factory_;
};

}  // namespace pipensx
