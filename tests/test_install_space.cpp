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
        assert((selection == std::vector<uint8_t>{1, 0}));

        InstallSpaceEstimate estimate = estimateInstallSpace(
            preview, selection, TransferMode::StreamInstall);
        assert(estimate.selectedFiles == 1);
        assert(estimate.packageFiles == 1);
        assert(estimate.requiredBytes == 1024);
        assert(estimate.certainty == SpaceEstimateCertainty::Conservative);
    }

    {
        TorrentPreview preview;
        preview.files = {
            {"compressed.nsz", 900, true, true, false},
        };
        InstallSpaceEstimate estimate = estimateInstallSpace(
            preview, {}, TransferMode::StreamInstall);
        assert(estimate.certainty ==
               SpaceEstimateCertainty::CompressedUnknown);

        StorageSpaceSnapshot storage;
        storage.available = true;
        storage.freeBytes = 800;
        InstallSpaceCheck check = assessInstallSpace(estimate, storage);
        assert(check.status == InstallSpaceCheckStatus::Insufficient);
        assert(check.shortfallBytes == 100);
    }

    std::puts("install space tests passed");
    return 0;
}
