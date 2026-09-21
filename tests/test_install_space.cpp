#include "app/install_space.hpp"

#include <cassert>
#include <cstdio>

using namespace pipensx;

int main() {
    {
        TorrentPreview preview;
        preview.files = {
            {"game.nsp", 1024, true, false, false},
            {"readme.txt", 256, false, false, false},
        };

        InstallSpaceEstimate estimate = estimateInstallSpace(
            preview, {}, TransferMode::DownloadOnly);

        assert(estimate.selectedFiles == 2);
        assert(estimate.selectedBytes == 1280);
        assert(estimate.downloadBytes == 1280);
        assert(estimate.packageBytes == 0);
        assert(estimate.requiredBytes == 1280);
        assert(estimate.certainty == SpaceEstimateCertainty::Exact);
        assert(!estimate.overflow);
    }

    {
        TorrentPreview preview;
        preview.files = {
            {"game.nsp", 1024, true, false, false},
            {"readme.txt", 256, false, false, false},
        };
        std::vector<uint8_t> selection = defaultInstallSelection(
            preview, TransferMode::StreamInstall,
            StreamSelection::PackagesOnly);
        assert((selection == std::vector<uint8_t>{
            static_cast<uint8_t>(FileAction::Install),
            static_cast<uint8_t>(FileAction::Skip),
        }));

        InstallSpaceEstimate estimate = estimateInstallSpace(
            preview, selection, TransferMode::StreamInstall);
        assert(estimate.selectedFiles == 1);
        assert(estimate.packageFiles == 1);
        assert(estimate.requiredBytes == 1024);
        assert(estimate.certainty == SpaceEstimateCertainty::Conservative);
    }

    {
        TorrentPreview preview;
        preview.multi = true;
        preview.name = "port-bundle";
        preview.files = {
            {"game.nsp", 1024, true, false, false},
            {"switch/game/game.nro", 256, false, false, false},
            {"switch/game/config.json", 128, false, false, false},
            {"readme.txt", 64, false, false, false},
        };
        const TransferMode mode = defaultTransferMode(
            preview, TransferMode::StreamInstall);
        assert(mode == TransferMode::PortInstall);
        std::vector<uint8_t> selection = defaultInstallSelection(
            preview, mode, StreamSelection::PackagesOnly);
        assert((selection == std::vector<uint8_t>{
            static_cast<uint8_t>(FileAction::Download),
            static_cast<uint8_t>(FileAction::Download),
            static_cast<uint8_t>(FileAction::Download),
            static_cast<uint8_t>(FileAction::Skip),
        }));

        InstallSpaceEstimate estimate = estimateInstallSpace(
            preview, selection, TransferMode::PortInstall);
        assert(estimate.selectedFiles == 3);
        assert(estimate.packageFiles == 1);
        assert(estimate.downloadBytes == 1408);
        assert(estimate.packageBytes == 1024);
        assert(estimate.requiredBytes == 2432);
    }

    {
        TorrentPreview preview;
        preview.files = {
            {"game.nsp", 1024, true, false, false},
            {"Game/Game.nro", 256, false, false, false},
        };
        const std::vector<uint8_t> actions = {
            static_cast<uint8_t>(FileAction::Download),
            static_cast<uint8_t>(FileAction::Download),
        };
        InstallSpaceEstimate estimate = estimateInstallSpace(
            preview, actions, TransferMode::PortInstall);
        assert(estimate.packageFiles == 1);
        assert(estimate.downloadBytes == 1280);
        assert(estimate.packageBytes == 1024);
        assert(estimate.requiredBytes == 2304);
    }

    // A retail dump plus a payload archive is still stream-install: the zip
    // is an extra, not a reason to treat the torrent as a port.
    {
        TorrentPreview preview;
        preview.files = {
            {"game.nsp", 1024, true, false, false},
            {"switch.7z", 512, false, false, false},
        };
        const TransferMode mode = defaultTransferMode(
            preview, TransferMode::StreamInstall);
        assert(mode == TransferMode::StreamInstall);
        std::vector<uint8_t> selection = defaultInstallSelection(
            preview, mode, StreamSelection::PackagesOnly);
        assert((selection == std::vector<uint8_t>{
            static_cast<uint8_t>(FileAction::Install),
            static_cast<uint8_t>(FileAction::Skip),
        }));
    }

    {
        TorrentPreview preview;
        preview.files = {
            {"game.nsp", 1024, true, false, false},
            {"readme.txt", 256, false, false, false},
        };
        std::vector<uint8_t> actions = {
            static_cast<uint8_t>(FileAction::Download),
            static_cast<uint8_t>(FileAction::Skip),
        };

        InstallSpaceEstimate estimate = estimateInstallSpace(
            preview, actions, TransferMode::DownloadOnly);
        assert(estimate.selectedFiles == 1);
        assert(estimate.packageFiles == 0);
        assert(estimate.downloadBytes == 1024);
        assert(estimate.packageBytes == 0);
        assert(estimate.requiredBytes == 1024);
        assert(estimate.certainty == SpaceEstimateCertainty::Exact);
    }

    {
        TorrentPreview preview;
        preview.files = {
            {"compressed.nsz", 900, true, true, false},
        };
        InstallSpaceEstimate estimate = estimateInstallSpace(
            preview, {}, TransferMode::StreamInstall);
        assert(estimate.selectedBytes == 900);
        assert(estimate.packageBytes == 2700);
        assert(estimate.requiredBytes == 2700);
        assert(estimate.certainty ==
               SpaceEstimateCertainty::CompressedUnknown);

        StorageSpaceSnapshot storage;
        storage.available = true;
        storage.freeBytes = 800;
        InstallSpaceCheck check = assessInstallSpace(estimate, storage);
        assert(check.status == InstallSpaceCheckStatus::Insufficient);
        assert(check.shortfallBytes == 1900);

        storage.freeBytes = 3000;
        check = assessInstallSpace(estimate, storage);
        assert(check.status == InstallSpaceCheckStatus::Enough);
    }

    // Scene-style NSZ names advertise the expanded install footprint. The
    // selection meter must use it instead of the compressed torrent length.
    {
        constexpr uint64_t gib = 1ULL << 30;
        const uint64_t expanded = 13 * gib + (48 * gib + 50) / 100;
        TorrentPreview preview;
        preview.files = {{
            "The Legend of Zelda Breath of the Wild "
            "[01007EF00011E000][v0] (13.48 GB).nsz",
            8 * gib, true, true, false,
        }};

        InstallSpaceEstimate estimate = estimateInstallSpace(
            preview, {}, TransferMode::StreamInstall);
        assert(estimate.selectedBytes == 8 * gib);
        assert(estimate.downloadBytes == 0);
        assert(estimate.packageBytes == expanded);
        assert(estimate.requiredBytes == expanded);
        assert(estimate.certainty == SpaceEstimateCertainty::Conservative);

        estimate = estimateInstallSpace(
            preview, {}, TransferMode::DownloadOnly);
        assert(estimate.downloadBytes == 8 * gib);
        assert(estimate.packageBytes == 0);
        assert(estimate.requiredBytes == 8 * gib);
        assert(estimate.certainty == SpaceEstimateCertainty::Exact);
    }

    // Known and unknown NSZ sizes are handled per file. In particular, the
    // fallback must not multiply the NSP or the named NSZ a second time.
    {
        TorrentPreview preview;
        preview.files = {
            {"plain.nsp", 1000, true, false, false},
            {"named (2 KB).nsz", 900, true, true, false},
            {"unknown.nsz", 100, true, true, false},
        };
        InstallSpaceEstimate estimate = estimateInstallSpace(
            preview, {}, TransferMode::StreamInstall);
        assert(estimate.packageBytes == 1000 + 2048 + 300);
        assert(estimate.requiredBytes == estimate.packageBytes);
        assert(estimate.certainty ==
               SpaceEstimateCertainty::CompressedUnknown);
    }

    // Never trust a rounded name to claim less space than the NSZ itself.
    {
        TorrentPreview preview;
        preview.files = {
            {"rounded (1 MB).nsz", 2ULL << 20, true, true, false},
        };
        InstallSpaceEstimate estimate = estimateInstallSpace(
            preview, {}, TransferMode::StreamInstall);
        assert(estimate.packageBytes == 2ULL << 20);
        assert(estimate.certainty == SpaceEstimateCertainty::Conservative);
    }

    // Malformed names retain the conservative fallback, including overflow.
    {
        TorrentPreview preview;
        preview.files = {
            {"bad (many GB).nsz", 10, true, true, false},
        };
        InstallSpaceEstimate estimate = estimateInstallSpace(
            preview, {}, TransferMode::StreamInstall);
        assert(estimate.packageBytes == 30);
        assert(estimate.certainty ==
               SpaceEstimateCertainty::CompressedUnknown);

        preview.files[0].length = UINT64_MAX / 2;
        estimate = estimateInstallSpace(
            preview, {}, TransferMode::StreamInstall);
        assert(estimate.overflow);
        assert(estimate.packageBytes == UINT64_MAX);
    }

    {
        InstallSpaceEstimate estimate;
        estimate.downloadBytes = 500;
        estimate.packageBytes = 1000;
        estimate.requiredBytes = 1500;
        StorageSpaceSnapshot sd;
        sd.available = true;
        sd.freeBytes = 2000;
        StorageSpaceSnapshot nand;
        nand.available = true;
        nand.freeBytes = 800;
        InstallSpaceCheck check = assessTransferSpace(estimate, sd, nand);
        assert(check.status == InstallSpaceCheckStatus::Insufficient);
        assert(check.shortfallBytes == 200);

        nand.freeBytes = 1000;
        check = assessTransferSpace(estimate, sd, nand);
        assert(check.status == InstallSpaceCheckStatus::Enough);
    }

    // catalogEntryFitsFreeSpace: the catalog "fits on SD" filter must never
    // hide an entry it cannot judge.
    {
        StorageSpaceSnapshot storage;
        storage.available = true;
        storage.freeBytes = 1000;

        assert(catalogEntryFitsFreeSpace(999, storage));
        assert(catalogEntryFitsFreeSpace(1000, storage)); // exactly full fits
        assert(!catalogEntryFitsFreeSpace(1001, storage));
        assert(catalogEntryFitsFreeSpace(0, storage));    // size unknown

        StorageSpaceSnapshot unavailable;
        unavailable.available = false;
        unavailable.freeBytes = 0;
        assert(catalogEntryFitsFreeSpace(1ULL << 40, unavailable));
    }

    std::puts("install space tests passed");
    return 0;
}
