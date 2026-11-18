#include "app/add_release.hpp"
#include "app/download_manager.hpp"

extern "C" {
#include "core/sha1.h"
}

#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <string>
#include <unistd.h>
#include <vector>

using namespace pipensx;

namespace {

struct Counts {
    std::atomic<int> magnets{0};
    std::atomic<int> files{0};
    std::atomic<int> removes{0};
    std::string fileName = "game.nsp";
    bool failCreate = false;
    DebridInfo::Phase phase = DebridInfo::Phase::Ready;
};

class CountingProvider : public DebridProvider {
public:
    explicit CountingProvider(std::shared_ptr<Counts> counts)
        : counts_(std::move(counts)) {}

    bool validate(std::string&) override { return true; }
    bool createFromMagnet(const std::string&, std::string& id,
                          std::string& error) override {
        counts_->magnets.fetch_add(1);
        if (counts_->failCreate) {
            error = "stub rejected the magnet";
            return false;
        }
        id = "remote-1";
        return true;
    }
    bool createFromFile(const std::string&, std::string& id,
                        std::string& error) override {
        counts_->files.fetch_add(1);
        if (counts_->failCreate) {
            error = "stub rejected the torrent";
            return false;
        }
        id = "remote-file";
        return true;
    }
    bool fetchInfo(const std::string&, DebridInfo& info,
                   std::string&) override {
        info = DebridInfo{};
        info.phase = counts_->phase;
        info.rawState = info.phase == DebridInfo::Phase::Creating ? "queued" : "downloaded";
        info.name = "Stub Game";
        info.bytes = 100;
        info.files.push_back({"1", counts_->fileName, 100});
        return true;
    }
    bool selectFiles(const std::string&, const std::vector<std::string>&,
                     std::string&) override {
        return true;
    }
    bool resolveDownloadUrl(const std::string&, const DebridInfo&, size_t,
                            const DebridFile&, std::string&,
                            std::string&) override {
        return false;
    }
    bool remove(const std::string&, std::string&) override {
        counts_->removes.fetch_add(1);
        return true;
    }
    const char* name() const override { return "stub"; }

private:
    std::shared_ptr<Counts> counts_;
};

std::string makeTorrent(const std::string& name, const std::string& payload) {
    uint8_t digest[20];
    sha1(reinterpret_cast<const uint8_t*>(payload.data()), payload.size(),
         digest);
    std::string torrent = "d8:announce14:http://tracker4:infod6:lengthi";
    torrent += std::to_string(payload.size());
    torrent += "e4:name" + std::to_string(name.size()) + ":" + name;
    torrent += "12:piece lengthi";
    torrent += std::to_string(payload.size());
    torrent += "e6:pieces20:";
    torrent.append(reinterpret_cast<const char*>(digest), 20);
    torrent += "ee";
    return torrent;
}

void setCredential(DownloadManager& manager, DebridProviderKind kind) {
    switch (kind) {
        case DebridProviderKind::TorBox:
            manager.setTorboxApiKey("key");
            break;
        case DebridProviderKind::TorrServer:
            manager.setTorrserverUrl("http://127.0.0.1:8090");
            break;
        case DebridProviderKind::RealDebrid:
            manager.setRealdebridApiKey("key");
            break;
        case DebridProviderKind::AllDebrid:
            manager.setAlldebridApiKey("key");
            break;
    }
}

DebridProviderFactory factoryFor(const std::shared_ptr<Counts>& counts,
                                 DebridProviderKind expected) {
    return [counts, expected](DebridProviderKind got, const std::string& key) {
        assert(got == expected);
        assert(!key.empty());
        return std::unique_ptr<DebridProvider>(new CountingProvider(counts));
    };
}

void testCapabilitiesAndRefusal() {
    const AddCapabilities direct = directAddCapabilities();
    assert(direct.catalog && direct.arbitraryMagnet && direct.torrentFile);
    assert(direct.usesSwarm);

    const DebridProviderKind kinds[] = {
        DebridProviderKind::TorBox, DebridProviderKind::TorrServer,
        DebridProviderKind::RealDebrid, DebridProviderKind::AllDebrid};
    for (DebridProviderKind kind : kinds) {
        const AddCapabilities caps = debridAddCapabilities(kind);
        assert(caps.catalog && caps.arbitraryMagnet && caps.torrentFile);
        assert(!caps.usesSwarm);

        const AddSourceChoice directChoice = decideAddSource(
            true, kind, "key", AddInputKind::Catalog);
        assert(directChoice.accepted && directChoice.direct);

        const AddSourceChoice debridChoice = decideAddSource(
            false, kind, "key", AddInputKind::Magnet);
        assert(debridChoice.accepted && !debridChoice.direct);
        assert(debridChoice.provider == kind);

        const AddSourceChoice fileChoice = decideAddSource(
            false, kind, "key", AddInputKind::TorrentFile);
        assert(fileChoice.accepted && !fileChoice.direct);

        const AddSourceChoice missing = decideAddSource(
            false, kind, "   ", AddInputKind::Catalog);
        assert(!missing.accepted);
        assert(missing.refusal.find(addSourceProviderName(kind)) !=
               std::string::npos);
        assert(missing.refusal.find("not configured") != std::string::npos);
        assert(missing.refusal.find("Direct BitTorrent stays off") !=
               std::string::npos);
    }

    AddCapabilities noFile = debridAddCapabilities(DebridProviderKind::TorBox);
    noFile.torrentFile = false;
    const AddSourceChoice unsupported = decideAddSource(
        false, DebridProviderKind::TorBox, "key", AddInputKind::TorrentFile,
        directAddCapabilities(), noFile);
    assert(!unsupported.accepted);
    assert(unsupported.refusal.find("cannot add a torrent file") !=
           std::string::npos);
    assert(unsupported.refusal.find("Direct BitTorrent stays off") !=
           std::string::npos);
}

void testDirectAndProviders(const std::string& root) {
    const std::string torrentPath = root + "/game.torrent";
    {
        std::ofstream out(torrentPath, std::ios::binary);
        const std::string bytes = makeTorrent("game.nsp", "nsp-bytes");
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }
    TorrentPreview preview;
    std::string error;
    assert(DownloadManager::previewTorrent(torrentPath, preview, error));
    assert(preview.files.size() == 1);
    const std::string hash = preview.infoHash;
    assert(hash.size() == 40);

    OneTapContext context;
    const OneTapPlan expected = planCatalogOneTap(preview, context);
    assert(expected.outcome == OneTapOutcome::QueueStream);
    assert(expected.mode == TransferMode::StreamInstall);
    assert(expected.fileSelection.size() == 1);
    assert(expected.fileSelection[0] ==
           static_cast<uint8_t>(FileAction::Install));

    {
        TorrentPreview bundle;
        bundle.files = {
            {"Game [0100AAAA00000000][v0].nsp", 100, true, false, false},
            {"Game [0100AAAA00000800][v131072].nsp", 50, true, false, false},
            {"DLC [0100AAAA00001001][v0].nsp", 10, true, false, false},
        };
        bundle.packageCount = 3;
        const auto plan = planCatalogOneTap(bundle, context);
        assert(plan.outcome == OneTapOutcome::QueueStream);
        assert(plan.packageCount == 3);
        assert(plan.fileSelection == std::vector<uint8_t>({2, 2, 2}));
        bundle.files[2].path = "Game DLC 1.nsp";
        const auto unknown = planCatalogOneTap(bundle, context);
        assert(unknown.outcome == OneTapOutcome::NeedsChooser);
        assert(unknown.block == OneTapBlock::UnidentifiedPackages);
        assert(unknown.chooserMode == TransferMode::StreamInstall);
        assert(unknown.fileSelection == std::vector<uint8_t>({2, 2, 0}));
    }

    {
        TorrentPreview port;
        port.files = {
            {"switch.7z", 55750, false, false, false},
            {"Super Mario World (port) [0108dc35a9030000].nsp",
             386, true, false, false},
            {"Homebrew (1.0.1)/switch/smwr_nx/smwr.nro",
             5940, false, false, false},
        };
        port.packageCount = 1;
        const OneTapPlan plan = planCatalogOneTap(port, context);
        assert(plan.outcome == OneTapOutcome::QueuePort);
        assert(plan.mode == TransferMode::PortInstall);
        assert(plan.packageCount == 1);
        assert(plan.fileSelection == std::vector<uint8_t>({2, 2, 2}));
        port.files.push_back({"switch_russmod.7z", 1658000000, false, false, false});
        port.files.push_back({"switch_extra.7z", 1000, false, false, false});
        const auto original = planCatalogOneTap(port, context);
        assert(original.mode == TransferMode::PortInstall);
        assert(original.fileSelection == std::vector<uint8_t>({2, 2, 2, 0, 2}));
        port.files.erase(port.files.begin()); // Standalone Russian version is valid.
        const auto standalone = planCatalogOneTap(port, context);
        assert(standalone.fileSelection == std::vector<uint8_t>({2, 2, 2, 2}));
        port.files.erase(port.files.begin() + 2, port.files.end());
        port.files.insert(port.files.begin(), {"switch.7z", 55750, false, false, false});
        port.files[1].path = "Native Game.nsp";
        assert(planCatalogOneTap(port, context).outcome == OneTapOutcome::QueuePort);
        port.files[1].length = 32 * 1024 * 1024;
        assert(planCatalogOneTap(port, context).outcome !=
               OneTapOutcome::QueuePort);
    }

    {
        TorrentPreview nfs;
        nfs.files.push_back({"NFS Underground 2 Xbox recomp [03DB1E5631154000][v0].nsp",
                             400000, true, false, false});
        nfs.files.push_back({"Homebrew/switch/nfsu2/nfsu2.nro", 6000000, false, false, false});
        for (int i = 0; i < 9; ++i)
            nfs.files.push_back({"Homebrew/switch/nfsu2/data/asset" + std::to_string(i),
                                 1000, false, false, false});
        const auto plan = planCatalogOneTap(nfs, context);
        assert(plan.outcome == OneTapOutcome::QueuePort);
        assert(plan.fileSelection == std::vector<uint8_t>(11, 2));
        // Recompiled NSP with external data can have no separate NRO.
        nfs.files[0].length = 40 * 1024 * 1024;
        nfs.files[1].path = "Homebrew/switch/nfsu2/game.dat";
        assert(planCatalogOneTap(nfs, context).outcome == OneTapOutcome::QueuePort);
        assert(planCatalogOneTap(nfs, context).fileSelection == std::vector<uint8_t>(11, 2));
    }

    {
        DownloadManager manager(root + "/direct", false);
        manager.setTorrentingEnabled(true);
        manager.setActiveDebridProvider(DebridProviderKind::RealDebrid);
        manager.setRealdebridApiKey("key");
        ReleaseAdder adder(manager);
        AddRequest request;
        request.input = AddInputKind::Catalog;
        request.title = "Direct Game";
        request.infoHashHex = hash;
        request.requestedMode = TransferMode::StreamInstall;
        request.oneTap = context;
        const AddOutcome outcome =
            adder.importResolvedTorrent(request, torrentPath, {});
        assert(outcome.ok);
        assert(outcome.source == TaskSource::Torrent);
        const DownloadTask task = manager.snapshot().at(0);
        assert(task.id == hash);
        assert(task.source == TaskSource::Torrent);
        assert(task.mode == TransferMode::StreamInstall);
        assert(task.fileSelection == expected.fileSelection);
        assert(manager.torrentingEnabled());
    }

    {
        DownloadManager manager(root + "/explicit", false);
        manager.setTorrentingEnabled(true);
        ReleaseAdder adder(manager);
        AddRequest request;
        request.infoHashHex = hash;
        request.fileChoices = {{"../other.nsp", 2}};
        assert(!adder.importResolvedTorrent(request, torrentPath, {}).ok);
        request.fileChoices = {{"game.nsp", 0}};
        assert(!adder.importResolvedTorrent(request, torrentPath, {}).ok);
        request.fileChoices = {{"game.nsp", 2}, {"game.nsp", 1}};
        assert(!adder.importResolvedTorrent(request, torrentPath, {}).ok);
        assert(manager.snapshot().empty());
        request.fileChoices = {{"game.nsp", 1}};
        const auto outcome = adder.importResolvedTorrent(request, torrentPath, {});
        assert(outcome.ok && outcome.mode == TransferMode::DownloadOnly);
        assert(manager.snapshot().at(0).fileSelection == std::vector<uint8_t>({1}));
    }
    const DebridProviderKind kinds[] = {
        DebridProviderKind::TorBox, DebridProviderKind::TorrServer,
        DebridProviderKind::RealDebrid, DebridProviderKind::AllDebrid};
    int index = 0;
    for (DebridProviderKind kind : kinds) {
        auto counts = std::make_shared<Counts>();
        DownloadManager manager(
            root + "/debrid-" + std::to_string(index++), false);
        manager.setTorrentingEnabled(false);
        manager.setActiveDebridProvider(kind);
        setCredential(manager, kind);
        ReleaseAdder adder(manager, factoryFor(counts, kind));

        AddRequest request;
        request.input = AddInputKind::Catalog;
        request.title = "Stub Game";
        request.magnetUri = "magnet:?xt=urn:btih:" + hash;
        request.infoHashHex = hash;
        request.requestedMode = TransferMode::StreamInstall;
        request.oneTap = context;
        std::atomic<bool> cancelled{false};
        const AddOutcome outcome = adder.addViaDebrid(request, cancelled, {});
        assert(outcome.ok);
        assert(outcome.source == TaskSource::Debrid);
        assert(outcome.provider == kind);
        assert(!manager.torrentingEnabled());
        assert(counts->magnets.load() == 1);
        assert(counts->files.load() == 0);
        assert(counts->removes.load() == 0);
        const DownloadTask task = manager.snapshot().at(0);
        assert(task.id == hash);
        assert(task.source == TaskSource::Debrid);
        assert(task.debridProvider == kind);
        assert(task.debridId == "remote-1");
        assert(task.mode == expected.mode);
        assert(task.fileSelection == expected.fileSelection);
    }

    {
        // RD can expose the metadata while the cloud download is still queued.
        // Keep that torrent and let the transfer worker wait for its links.
        auto counts = std::make_shared<Counts>();
        counts->phase = DebridInfo::Phase::Creating;
        DownloadManager manager(root + "/queued-metadata", false);
        manager.setTorrentingEnabled(false);
        manager.setActiveDebridProvider(DebridProviderKind::RealDebrid);
        manager.setRealdebridApiKey("key");
        ReleaseAdder adder(manager, factoryFor(counts, DebridProviderKind::RealDebrid));
        AddRequest request;
        request.input = AddInputKind::Magnet;
        request.magnetUri = "magnet:?xt=urn:btih:" + hash;
        request.infoHashHex = hash;
        request.requestedMode = TransferMode::DownloadOnly;
        std::atomic<bool> cancelled{false};
        const AddOutcome outcome = adder.addViaDebrid(request, cancelled, {});
        assert(outcome.ok);
        assert(counts->magnets.load() == 1);
        assert(counts->files.load() == 0);
        assert(counts->removes.load() == 0);
        const DownloadTask task = manager.snapshot().at(0);
        assert(task.source == TaskSource::Debrid);
        assert(task.debridId == "remote-1");
        assert(!manager.torrentingEnabled());
    }

    {
        auto counts = std::make_shared<Counts>();
        DownloadManager manager(root + "/short-hash", false);
        manager.setTorrentingEnabled(false);
        manager.setActiveDebridProvider(DebridProviderKind::AllDebrid);
        manager.setAlldebridApiKey("key");
        ReleaseAdder adder(manager, factoryFor(counts, DebridProviderKind::AllDebrid));
        AddRequest request;
        request.input = AddInputKind::Magnet;
        request.magnetUri = "magnet:?xt=urn:btih:abc";
        request.infoHashHex = "abc";
        request.requestedMode = TransferMode::DownloadOnly;
        std::atomic<bool> cancelled{false};
        const AddOutcome outcome = adder.addViaDebrid(request, cancelled, {});
        assert(!outcome.ok);
        assert(outcome.refused);
        assert(outcome.error.find("Direct BitTorrent stays off") !=
               std::string::npos);
        assert(counts->magnets.load() == 0);
        assert(manager.snapshot().empty());
        assert(!manager.torrentingEnabled());
    }

    {
        auto counts = std::make_shared<Counts>();
        counts->fileName = "readme.txt";
        DownloadManager manager(root + "/notes", false);
        manager.setTorrentingEnabled(false);
        manager.setActiveDebridProvider(DebridProviderKind::TorBox);
        manager.setTorboxApiKey("key");
        ReleaseAdder adder(manager, factoryFor(counts, DebridProviderKind::TorBox));
        AddRequest request;
        request.input = AddInputKind::Catalog;
        request.magnetUri = "magnet:?xt=urn:btih:" + hash;
        request.infoHashHex = hash;
        request.requestedMode = TransferMode::StreamInstall;
        std::atomic<bool> cancelled{false};
        const AddOutcome outcome = adder.addViaDebrid(request, cancelled, {});
        assert(!outcome.ok);
        assert(!outcome.refused);
        assert(outcome.error.find("no installable packages") !=
               std::string::npos);
        assert(counts->magnets.load() == 1);
        assert(counts->removes.load() == 1);
        assert(manager.snapshot().empty());
        assert(!manager.torrentingEnabled());
    }

    {
        auto counts = std::make_shared<Counts>();
        DownloadManager manager(root + "/torrent-file", false);
        manager.setTorrentingEnabled(false);
        manager.setActiveDebridProvider(DebridProviderKind::RealDebrid);
        manager.setRealdebridApiKey("key");
        ReleaseAdder adder(
            manager, factoryFor(counts, DebridProviderKind::RealDebrid));
        AddRequest request;
        request.input = AddInputKind::TorrentFile;
        request.requestedMode = TransferMode::DownloadOnly;
        std::atomic<bool> cancelled{false};
        const AddOutcome outcome =
            adder.addTorrentFile(request, torrentPath, cancelled);
        assert(outcome.ok);
        assert(outcome.source == TaskSource::Debrid);
        assert(counts->files.load() == 1);
        assert(counts->magnets.load() == 0);
        const DownloadTask task = manager.snapshot().at(0);
        assert(task.source == TaskSource::Debrid);
        assert(task.debridProvider == DebridProviderKind::RealDebrid);
        assert(task.mode == TransferMode::DownloadOnly);
        assert(!manager.torrentingEnabled());
    }
}

void testPendingDebridRetainAndReuse() {
    struct State {
        int creates = 0;
        int removes = 0;
        bool readyNow = false;
        std::atomic<bool>* cancelOnFetch = nullptr;
    };
    class RetainReuseProvider final : public DebridProvider {
    public:
        explicit RetainReuseProvider(std::shared_ptr<State> state)
            : state_(std::move(state)) {}

        bool validate(std::string&) override { return true; }
        bool createFromMagnet(const std::string&, std::string& id,
                              std::string&) override {
            ++state_->creates;
            id = "tx-501";
            return true;
        }
        bool createFromFile(const std::string&, std::string&,
                            std::string&) override {
            return false;
        }
        bool fetchInfo(const std::string& id, DebridInfo& info,
                       std::string&) override {
            assert(id == "tx-501");
            info = DebridInfo{};
            info.name = "Cached Game";
            if (!state_->readyNow) {
                info.phase = DebridInfo::Phase::Downloading;
                info.progress = 0.40;
                if (state_->cancelOnFetch)
                    state_->cancelOnFetch->store(true);
                return true;
            }
            info.phase = DebridInfo::Phase::Ready;
            info.progress = 1.0;
            info.bytes = 4096;
            info.files.push_back({"1", "game.nsp", 4096});
            return true;
        }
        bool selectFiles(const std::string&, const std::vector<std::string>&,
                         std::string&) override {
            return true;
        }
        bool resolveDownloadUrl(const std::string&, const DebridInfo&, size_t,
                                const DebridFile&, std::string&,
                                std::string&) override {
            return false;
        }
        bool remove(const std::string&, std::string&) override {
            ++state_->removes;
            return true;
        }
        const char* name() const override { return "torbox"; }

    private:
        std::shared_ptr<State> state_;
    };

    char tmp[] = "/tmp/pipensx-add-pending-XXXXXX";
    assert(mkdtemp(tmp));
    const std::string root(tmp);

    {
        DownloadManager manager(root, false);
        manager.setTorrentingEnabled(false);
        manager.setActiveDebridProvider(DebridProviderKind::TorBox);
        manager.setTorboxApiKey("secret");
        manager.setKeepUncachedDebrid(true);

        auto state = std::make_shared<State>();
        ReleaseAdder adder(
            manager,
            [state](DebridProviderKind, const std::string&) {
                return std::unique_ptr<DebridProvider>(
                    new RetainReuseProvider(state));
            });

        const std::string hash = "1234567890abcdef1234567890abcdef12345678";
        AddRequest req;
        req.input = AddInputKind::Catalog;
        req.title = "Cached Game";
        req.infoHashHex = hash;
        req.magnetUri = "magnet:?xt=urn:btih:" + hash;
        req.requestedMode = TransferMode::DownloadOnly;
        req.selection = StreamSelection::AllFiles;

        std::atomic<bool> cancelled{false};
        state->cancelOnFetch = &cancelled;
        const AddOutcome first = adder.addViaDebrid(req, cancelled);
        assert(!first.ok);
        assert(!first.refused);
        assert(first.error.find("kept") != std::string::npos);
        assert(state->creates == 1);
        assert(state->removes == 0);

        auto pending =
            manager.pendingDebrid().find(DebridProviderKind::TorBox, hash);
        assert(pending.has_value());
        assert(pending->debridId == "tx-501");
        assert(!pending->ready);
        assert(pending->progress > 0.39 && pending->progress < 0.41);

        state->cancelOnFetch = nullptr;
        state->readyNow = true;
        std::atomic<bool> cancelledSecond{false};
        const AddOutcome second = adder.addViaDebrid(req, cancelledSecond);
        assert(second.ok);
        assert(state->creates == 1);
        assert(!manager.pendingDebrid()
                    .find(DebridProviderKind::TorBox, hash)
                    .has_value());
    }

    assert(std::system(("rm -rf " + root).c_str()) == 0);
}

}  // namespace

int main() {
    char dir[] = "/tmp/pipensx-add-XXXXXX";
    assert(mkdtemp(dir));
    testCapabilitiesAndRefusal();
    testDirectAndProviders(dir);
    testPendingDebridRetainAndReuse();
    std::puts("add release tests passed");
    return 0;
}
