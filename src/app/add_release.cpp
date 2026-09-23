#include "add_release.hpp"

#include "alldebrid_provider.hpp"
#include "nx_file_types.hpp"
#include "port_selection.hpp"
#include "realdebrid_provider.hpp"
#include "torbox_provider.hpp"
#include "torrserver_provider.hpp"

#include <atomic>
#include <cctype>
#include <utility>

namespace pipensx {
namespace {

std::atomic<uint64_t> gAddSerial{1};

std::string lowerAscii(std::string value) {
    for (char& c : value)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return value;
}

bool credentialSet(const std::string& value) {
    for (unsigned char c : value)
        if (!std::isspace(c))
            return true;
    return false;
}

bool supports(const AddCapabilities& caps, AddInputKind input) {
    switch (input) {
        case AddInputKind::Catalog:
            return caps.catalog;
        case AddInputKind::Magnet:
            return caps.arbitraryMagnet;
        case AddInputKind::TorrentFile:
            return caps.torrentFile;
    }
    return false;
}

const char* inputLabel(AddInputKind input) {
    switch (input) {
        case AddInputKind::Catalog:
            return "catalog release";
        case AddInputKind::Magnet:
            return "magnet link";
        case AddInputKind::TorrentFile:
            return "torrent file";
    }
    return "download";
}

uint32_t countSelectedPackages(const TorrentPreview& preview,
                               const std::vector<uint8_t>& actions,
                               bool installOnly) {
    uint32_t count = 0;
    for (size_t i = 0; i < preview.files.size() && i < actions.size(); ++i) {
        if (!preview.files[i].package)
            continue;
        const uint8_t action = actions[i];
        if (installOnly) {
            if (action == static_cast<uint8_t>(FileAction::Install))
                ++count;
        } else if (action != static_cast<uint8_t>(FileAction::Skip)) {
            ++count;
        }
    }
    return count;
}

std::unique_ptr<DebridProvider> defaultDebridFactory(
    DebridProviderKind kind, const std::string& credential) {
    if (kind == DebridProviderKind::TorrServer)
        return std::make_unique<TorrserverProvider>(credential);
    if (kind == DebridProviderKind::RealDebrid)
        return std::make_unique<RealdebridProvider>(credential);
    if (kind == DebridProviderKind::AllDebrid)
        return std::make_unique<AlldebridProvider>(credential);
    return std::make_unique<TorboxProvider>(credential);
}

}  // namespace

AddCapabilities directAddCapabilities() {
    AddCapabilities caps;
    caps.catalog = true;
    caps.arbitraryMagnet = true;
    caps.torrentFile = true;
    caps.usesSwarm = true;
    return caps;
}

AddCapabilities debridAddCapabilities(DebridProviderKind kind) {
    // TorBox, TorrServer, Real-Debrid, and AllDebrid all implement
    // createFromMagnet and createFromFile. None of them open the swarm.
    switch (kind) {
        case DebridProviderKind::TorBox:
        case DebridProviderKind::TorrServer:
        case DebridProviderKind::RealDebrid:
        case DebridProviderKind::AllDebrid:
            break;
    }
    AddCapabilities caps;
    caps.catalog = true;
    caps.arbitraryMagnet = true;
    caps.torrentFile = true;
    caps.usesSwarm = false;
    return caps;
}

const char* addSourceProviderName(DebridProviderKind kind) {
    switch (kind) {
        case DebridProviderKind::TorrServer:
            return "TorrServer";
        case DebridProviderKind::RealDebrid:
            return "Real-Debrid";
        case DebridProviderKind::AllDebrid:
            return "AllDebrid";
        case DebridProviderKind::TorBox:
            return "TorBox";
    }
    return "TorBox";
}

AddSourceChoice decideAddSource(bool torrentingEnabled,
                                DebridProviderKind provider,
                                const std::string& credential,
                                AddInputKind input) {
    return decideAddSource(torrentingEnabled, provider, credential, input,
                           directAddCapabilities(),
                           debridAddCapabilities(provider));
}

AddSourceChoice decideAddSource(bool torrentingEnabled,
                                DebridProviderKind provider,
                                const std::string& credential,
                                AddInputKind input,
                                const AddCapabilities& directCaps,
                                const AddCapabilities& debridCaps) {
    AddSourceChoice choice;
    choice.provider = provider;
    choice.credential = credential;
    if (torrentingEnabled) {
        if (!supports(directCaps, input)) {
            choice.refusal = std::string("Direct BitTorrent cannot add a ") +
                             inputLabel(input) + ".";
            return choice;
        }
        choice.accepted = true;
        choice.direct = true;
        return choice;
    }
    const char* name = addSourceProviderName(provider);
    if (!credentialSet(credential)) {
        choice.refusal = std::string("Torrenting is off and ") + name +
                         " is not configured. Link it in Settings. "
                         "Direct BitTorrent stays off.";
        return choice;
    }
    if (!supports(debridCaps, input)) {
        choice.refusal = std::string(name) + " cannot add a " +
                         inputLabel(input) +
                         ". Direct BitTorrent stays off.";
        return choice;
    }
    choice.accepted = true;
    choice.direct = false;
    return choice;
}

OneTapPlan planCatalogOneTap(const TorrentPreview& preview,
                             const OneTapContext& context) {
    OneTapPlan plan;
    if (cardOneTapUsesPortInstall(preview)) {
        plan.outcome = OneTapOutcome::QueuePort;
        plan.block = OneTapBlock::None;
        plan.mode = TransferMode::PortInstall;
        plan.fileSelection = selectPortInstallActions(preview);
        plan.packageCount =
            countSelectedPackages(preview, plan.fileSelection, false);
        return plan;
    }
    if (preview.files.empty() || preview.packageCount == 0) {
        plan.outcome = OneTapOutcome::NeedsChooser;
        plan.chooserMode = TransferMode::DownloadOnly;
        plan.block = preview.files.empty()
                         ? OneTapBlock::Empty
                         : preview.cartridgeCount > 0
                               ? OneTapBlock::CartridgeOnly
                               : OneTapBlock::NoPackages;
        return plan;
    }
    plan.fileSelection = selectSmartInstallFiles(
        preview, context.titleInstalled, context.installedVersion,
        context.latestVersion, context.titleId, context.installedDlcIds);
    plan.packageCount = countSelectedPackages(preview, plan.fileSelection, true);
    if (plan.packageCount == 0) {
        plan.outcome = OneTapOutcome::NeedsChooser;
        plan.block = OneTapBlock::NothingSelected;
        plan.chooserMode = TransferMode::StreamInstall;
        plan.mode = TransferMode::StreamInstall;
        return plan;
    }
    plan.outcome = OneTapOutcome::QueueStream;
    plan.block = OneTapBlock::None;
    plan.mode = TransferMode::StreamInstall;
    plan.extras = skippedExtraNotice(preview, plan.fileSelection);
    return plan;
}

std::string oneTapBlockedMessage(const OneTapPlan& plan) {
    switch (plan.block) {
        case OneTapBlock::CartridgeOnly:
            return "This release is a cartridge dump. Choose Download only, "
                   "or add it on the console and pick files.";
        case OneTapBlock::NothingSelected:
            return "Nothing in this release can be installed automatically. "
                   "Add it on the console and choose files.";
        case OneTapBlock::Empty:
        case OneTapBlock::NoPackages:
            return "This release has no installable packages. Choose Download "
                   "only, or add it on the console and pick files.";
        case OneTapBlock::None:
            return {};
    }
    return "This release cannot be installed automatically.";
}

SettingsInstallPlan planSettingsInstall(const TorrentPreview& preview,
                                        TransferMode requested,
                                        StreamSelection selection) {
    SettingsInstallPlan plan;
    if (requested != TransferMode::StreamInstall) {
        plan.mode = requested;
        plan.space = estimateInstallSpace(preview, {}, requested);
        return plan;
    }
    plan.mode = defaultTransferMode(preview, requested);
    plan.fileSelection = defaultInstallSelection(preview, plan.mode, selection);
    plan.space = estimateInstallSpace(preview, plan.fileSelection, plan.mode);
    return plan;
}

void fallbackInstallToDownload(SettingsInstallPlan& plan,
                               const TorrentPreview& preview) {
    if (plan.space.packageFiles == 0 &&
        plan.mode != TransferMode::PortInstall) {
        plan.mode = TransferMode::DownloadOnly;
        plan.space = estimateInstallSpace(preview, plan.fileSelection, plan.mode);
    }
}

TorrentPreview previewFromDebridFiles(const DebridInfo& info,
                                      const std::string& fallbackName,
                                      uint64_t fallbackBytes) {
    TorrentPreview preview;
    preview.name = info.name.empty() ? fallbackName : info.name;
    preview.totalBytes = info.bytes ? info.bytes : fallbackBytes;
    preview.fileCount = static_cast<uint32_t>(info.files.size());
    for (const DebridFile& file : info.files) {
        TorrentPreview::File row;
        row.path = file.path;
        row.length = file.bytes;
        row.package = isPackageName(file.path);
        row.compressed = isCompressedName(file.path);
        row.cartridge = isCartridgeName(file.path);
        preview.packageCount += row.package ? 1 : 0;
        preview.cartridgeCount += row.cartridge ? 1 : 0;
        preview.files.push_back(std::move(row));
    }
    return preview;
}

ReleaseAdder::ReleaseAdder(DownloadManager& manager,
                           DebridProviderFactory factory)
    : manager_(manager), factory_(std::move(factory)) {
    if (!factory_)
        factory_ = defaultDebridFactory;
}

AddSourceChoice ReleaseAdder::choose(AddInputKind input) const {
    return decideAddSource(manager_.torrentingEnabled(),
                           manager_.activeDebridProvider(),
                           manager_.activeDebridCredential(), input);
}

std::unique_ptr<DebridProvider> ReleaseAdder::makeProvider(
    const AddSourceChoice& choice, std::string& error) const {
    auto provider = factory_(choice.provider, choice.credential);
    if (!provider)
        error = std::string(addSourceProviderName(choice.provider)) +
                " is not available.";
    return provider;
}

AddOutcome ReleaseAdder::importDebridPlan(const AddRequest& request,
                                          const AddSourceChoice& choice,
                                          DebridProvider& provider,
                                          const std::string& debridId,
                                          const DebridInfo& info) {
    AddOutcome outcome;
    outcome.provider = choice.provider;
    outcome.source = TaskSource::Debrid;
    const TorrentPreview preview = previewFromDebridFiles(
        info, request.title, 0);
    outcome.name = preview.name.empty() ? request.title : preview.name;

    TransferMode mode = TransferMode::DownloadOnly;
    std::vector<uint8_t> selection;
    uint32_t packageCount = 0;
    if (request.input == AddInputKind::Catalog &&
        request.requestedMode == TransferMode::StreamInstall) {
        const OneTapPlan plan = planCatalogOneTap(preview, request.oneTap);
        if (plan.outcome == OneTapOutcome::NeedsChooser) {
            std::string ignored;
            provider.remove(debridId, ignored);
            outcome.error = oneTapBlockedMessage(plan);
            return outcome;
        }
        mode = plan.mode;
        selection = plan.fileSelection;
        packageCount = plan.packageCount;
    } else {
        SettingsInstallPlan plan = planSettingsInstall(
            preview, request.requestedMode, request.selection);
        if (request.requestedMode == TransferMode::StreamInstall)
            fallbackInstallToDownload(plan, preview);
        mode = plan.mode;
        selection = std::move(plan.fileSelection);
        packageCount = plan.space.packageFiles;
    }

    DebridImport import;
    import.infoHash = lowerAscii(request.infoHashHex);
    import.name = outcome.name;
    import.totalBytes = preview.totalBytes;
    import.provider = choice.provider;
    import.debridId = debridId;
    import.mode = mode;
    import.fileSelection = std::move(selection);
    import.packageCount = packageCount;
    std::string taskId;
    std::string error;
    if (!manager_.importDebrid(import, taskId, error)) {
        std::string ignored;
        provider.remove(debridId, ignored);
        outcome.error = error.empty() ? "Import failed." : error;
        return outcome;
    }
    outcome.ok = true;
    outcome.taskId = std::move(taskId);
    outcome.mode = mode;
    return outcome;
}

AddOutcome ReleaseAdder::importResolvedTorrent(
    const AddRequest& request, const std::string& torrentPath,
    const std::vector<uint8_t>& initialPeers) {
    AddOutcome outcome;
    const AddSourceChoice choice = choose(request.input);
    if (!choice.accepted || !choice.direct) {
        outcome.refused = true;
        outcome.error = choice.refusal.empty()
                            ? "Direct BitTorrent is not the selected source."
                            : choice.refusal;
        return outcome;
    }
    TorrentPreview preview;
    std::string error;
    if (!DownloadManager::previewTorrent(torrentPath, preview, error)) {
        outcome.error = error.empty() ? "invalid torrent" : error;
        return outcome;
    }
    const std::string wanted = lowerAscii(request.infoHashHex);
    if (!wanted.empty() && wanted != lowerAscii(preview.infoHash)) {
        outcome.error = "Resolved torrent does not match the requested hash.";
        return outcome;
    }
    outcome.name = preview.name.empty() ? request.title : preview.name;
    outcome.source = TaskSource::Torrent;

    std::string taskId;
    bool ok = false;
    if (request.input == AddInputKind::Catalog &&
        request.requestedMode == TransferMode::StreamInstall) {
        const OneTapPlan plan = planCatalogOneTap(preview, request.oneTap);
        if (plan.outcome == OneTapOutcome::NeedsChooser) {
            outcome.error = oneTapBlockedMessage(plan);
            return outcome;
        }
        ok = manager_.importTorrentActions(torrentPath, plan.fileSelection,
                                           taskId, error, initialPeers);
        outcome.mode = plan.mode;
    } else {
        SettingsInstallPlan plan = planSettingsInstall(
            preview, request.requestedMode, request.selection);
        if (request.requestedMode == TransferMode::StreamInstall)
            fallbackInstallToDownload(plan, preview);
        ok = manager_.importTorrent(torrentPath, plan.mode, plan.fileSelection,
                                    taskId, error, initialPeers);
        outcome.mode = plan.mode;
    }
    if (!ok) {
        outcome.error = error.empty() ? "Import failed." : error;
        return outcome;
    }
    outcome.ok = true;
    outcome.taskId = std::move(taskId);
    return outcome;
}

AddOutcome ReleaseAdder::addViaDebrid(
    const AddRequest& request, std::atomic<bool>& cancelled,
    const std::function<void(DebridCreateStage)>& onStage) {
    AddOutcome outcome;
    const AddSourceChoice choice = choose(request.input);
    if (!choice.accepted || choice.direct) {
        outcome.refused = true;
        outcome.error = choice.refusal.empty()
                            ? "Debrid is not the selected source."
                            : choice.refusal;
        return outcome;
    }
    const std::string hash = lowerAscii(request.infoHashHex);
    if (hash.size() != 40) {
        outcome.refused = true;
        outcome.error = std::string(addSourceProviderName(choice.provider)) +
                        " needs a 40-character info hash to add this, and "
                        "this magnet does not have one. Direct BitTorrent "
                        "stays off.";
        return outcome;
    }
    std::string error;
    auto provider = makeProvider(choice, error);
    if (!provider) {
        outcome.error = error;
        return outcome;
    }
    const std::string tmp =
        manager_.rootPath() + "/_add_" + hash + "_" +
        std::to_string(gAddSerial.fetch_add(1)) + ".torrent";
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(60);
    std::string debridId;
    DebridInfo info;
    AddRequest hashed = request;
    hashed.infoHashHex = hash;
    if (!createDebridWithMetainfoFallback(
            *provider, hashed.magnetUri, hash, hashed.infoDict, tmp, cancelled,
            deadline, debridId, info, error, onStage)) {
        outcome.error = cancelled.load()
                            ? "Cancelled."
                            : (error.empty() ? "Unable to resolve torrent metadata."
                                             : error);
        return outcome;
    }
    if (cancelled.load()) {
        if (!debridId.empty()) {
            std::string ignored;
            provider->remove(debridId, ignored);
        }
        outcome.error = "Cancelled.";
        return outcome;
    }
    return importDebridPlan(hashed, choice, *provider, debridId, info);
}

AddOutcome ReleaseAdder::addTorrentFile(const AddRequest& request,
                                        const std::string& torrentPath,
                                        std::atomic<bool>& cancelled) {
    AddOutcome outcome;
    const AddSourceChoice choice = choose(AddInputKind::TorrentFile);
    if (!choice.accepted) {
        outcome.refused = true;
        outcome.error = choice.refusal;
        return outcome;
    }
    if (choice.direct) {
        AddRequest direct = request;
        direct.input = AddInputKind::TorrentFile;
        return importResolvedTorrent(direct, torrentPath, {});
    }
    TorrentPreview local;
    std::string error;
    if (!DownloadManager::previewTorrent(torrentPath, local, error)) {
        outcome.error = error.empty() ? "invalid torrent" : error;
        return outcome;
    }
    auto provider = makeProvider(choice, error);
    if (!provider) {
        outcome.error = error;
        return outcome;
    }
    std::string debridId;
    if (!provider->createFromFile(torrentPath, debridId, error)) {
        outcome.error = error.empty() ? "Import failed." : error;
        return outcome;
    }
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(60);
    DebridInfo info;
    if (!pollDebridUntilFiles(*provider, debridId, cancelled, deadline, info,
                              error) ||
        cancelled.load()) {
        std::string ignored;
        provider->remove(debridId, ignored);
        outcome.error = cancelled.load()
                            ? "Cancelled."
                            : (error.empty() ? "Unable to resolve torrent metadata."
                                             : error);
        return outcome;
    }
    AddRequest hashed = request;
    hashed.input = AddInputKind::TorrentFile;
    hashed.infoHashHex = lowerAscii(local.infoHash);
    hashed.title = request.title.empty() ? local.name : request.title;
    return importDebridPlan(hashed, choice, *provider, debridId, info);
}

}  // namespace pipensx
