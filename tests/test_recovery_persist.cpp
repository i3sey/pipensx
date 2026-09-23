// Queue state keeps the saved download and install figures. Reloading them
// is not the same check as reading the on-screen percent.

#include "../src/app/download_manager.hpp"

#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

static std::string bstr(const std::string& value) {
    return std::to_string(value.size()) + ":" + value;
}

static std::string bint(uint64_t value) {
    return "i" + std::to_string(value) + "e";
}

static int scanPercent(uint32_t done, uint32_t total) {
    if (!total)
        return 0;
    return static_cast<int>((static_cast<uint64_t>(done) * 100ull) / total);
}

int main() {
    const std::string root = "/tmp/pipensx-recovery-persist";
    assert(system(("rm -rf " + root).c_str()) == 0);
    assert(mkdir(root.c_str(), 0755) == 0);
    assert(mkdir((root + "/downloads").c_str(), 0755) == 0);
    const std::string data = root + "/downloads/package";

    std::string task = "d";
    task += "9:completed" + bint(80);
    task += "4:data" + bstr(data);
    task += "9:debrid-id" + bstr("ad-1");
    task += "5:error0:";
    task += "2:id" + bstr("abc");
    task += "13:install-bytes" + bint(40);
    task += "13:install-total" + bint(100);
    task += "8:metainfo0:";
    task += "4:mode" + bstr("install");
    task += "4:name" + bstr("Game");
    task += "13:package-count" + bint(3);
    task += "13:packages-done" + bint(1);
    task += "11:pieces-done" + bint(0);
    task += "12:pieces-total" + bint(10);
    task += "8:provider" + bstr("alldebrid");
    task += "13:recovery-byte" + bint(0);
    task += "13:recovery-unit" + bint(40);
    task += "19:recovery-unit-total" + bint(100);
    task += "9:selection0:";
    task += "6:source" + bstr("debrid");
    task += "6:status" + bstr("paused");
    task += "5:total" + bint(200);
    task += "16:wanted-completed" + bint(80);
    task += "12:wanted-total" + bint(200);
    task += "e";
    const std::string state = "d5:tasksl" + task + "e7:versioni8ee";
    {
        std::ofstream out(root + "/queue.bencode",
                          std::ios::binary | std::ios::trunc);
        out << state;
    }

    {
        pipensx::DownloadManager manager(root, false);
        const auto tasks = manager.snapshot();
        assert(tasks.size() == 1);
        const pipensx::DownloadTask& loaded = tasks[0];
        assert(loaded.status == pipensx::DownloadStatus::Paused);
        assert(loaded.installedBytes == 40);
        assert(loaded.installTotalBytes == 100);
        assert(loaded.packagesInstalled == 1);
        assert(loaded.recoveryUnitBytes == 40);
        assert(!loaded.recoveryBytePoint);
        assert(loaded.wantedCompletedBytes == 80);
        const int shown = scanPercent(loaded.piecesDone, loaded.piecesTotal);
        const pipensx::RecoveryAccount account =
            pipensx::recoveryAccountOf(loaded);
        assert(shown == 0);
        assert(account.downloadedBytes == 80);
        assert(account.packagesInstalled == 1);
        assert(account.work == pipensx::RecoveryWork::RereadPackage);
        assert(account.reworkBytes == 40);
        assert(!account.byteExact);
        assert(!account.repeatsCompletedPackages);
        assert(!account.verifyingSaved);
        std::string error;
        assert(manager.save(error));
    }
    {
        pipensx::DownloadManager again(root, false);
        const auto loaded = again.snapshot()[0];
        const pipensx::RecoveryAccount account =
            pipensx::recoveryAccountOf(loaded);
        assert(loaded.installedBytes == 40);
        assert(loaded.recoveryUnitBytes == 40);
        assert(loaded.packagesInstalled == 1);
        assert(account.downloadedBytes == 80);
        assert(account.work == pipensx::RecoveryWork::RereadPackage);
        assert(scanPercent(loaded.piecesDone, loaded.piecesTotal) == 0);
    }

    // A v7 queue has no recovery keys. Saved download bytes still load, and a
    // debrid task does not turn a leftover bitfield into a full re-read.
    {
        std::string bitfield(1, static_cast<char>(0x80));
        std::string old = "d";
        old += "9:completed" + bint(70);
        old += "4:data" + bstr(data);
        old += "9:debrid-id0:";
        old += "5:error0:";
        old += "2:id" + bstr("def");
        old += "8:metainfo0:";
        old += "4:mode" + bstr("download");
        old += "4:name" + bstr("Old");
        old += "13:package-count" + bint(0);
        old += "13:packages-done" + bint(0);
        old += "11:pieces-done" + bint(0);
        old += "12:pieces-total" + bint(8);
        old += "8:provider" + bstr("torbox");
        old += "9:resume-bf" + bstr(bitfield);
        old += "9:selection0:";
        old += "6:source" + bstr("debrid");
        old += "6:status" + bstr("paused");
        old += "5:total" + bint(100);
        old += "16:wanted-completed" + bint(70);
        old += "12:wanted-total" + bint(100);
        old += "e";
        std::ofstream out(root + "/queue.bencode",
                          std::ios::binary | std::ios::trunc);
        out << "d5:tasksl" + old + "e7:versioni7ee";
    }
    {
        pipensx::DownloadManager manager(root, false);
        const auto loaded = manager.snapshot()[0];
        assert(loaded.wantedCompletedBytes == 70);
        assert(loaded.recoveryTrusted);
        assert(loaded.recoveryUnitBytes == 0);
        const pipensx::RecoveryAccount account =
            pipensx::recoveryAccountOf(loaded);
        assert(account.downloadedBytes == 70);
        assert(account.work == pipensx::RecoveryWork::None);
        assert(account.reworkBytes == 0);
        assert(!account.byteExact);
        assert(scanPercent(loaded.piecesDone, loaded.piecesTotal) == 0);
    }

    assert(system(("rm -rf " + root).c_str()) == 0);
    return 0;
}
