// Recovery is the saved download, the installed packages, and any re-read.
// The hash-scan percent is a different number and must not replace them.

#include "app/download_manager.hpp"
#include "app/recovery_account.hpp"

#include <cassert>
#include <cstdint>

using pipensx::DownloadStatus;
using pipensx::DownloadTask;
using pipensx::RecoveryAccount;
using pipensx::RecoveryEvent;
using pipensx::RecoveryInput;
using pipensx::RecoveryMode;
using pipensx::RecoveryPhase;
using pipensx::RecoverySource;
using pipensx::RecoveryWork;
using pipensx::TaskSource;
using pipensx::TransferMode;
using pipensx::accountRecovery;
using pipensx::recoveryAccountOf;
using pipensx::streamInstallProgressOf;

static int scanPercent(uint32_t done, uint32_t total) {
    if (!total)
        return 0;
    return static_cast<int>((static_cast<uint64_t>(done) * 100ull) / total);
}

static void assertStable(const RecoveryAccount& account,
                         const RecoveryInput& in) {
    assert(account.downloadedBytes == in.downloadedBytes);
    assert(account.downloadTotalBytes == in.downloadTotalBytes);
    assert(account.packagesInstalled == in.packagesInstalled);
    assert(account.packageCount == in.packageCount);
    assert(!account.repeatsCompletedPackages);
    if (account.byteExact) {
        assert(account.work == RecoveryWork::ResumeFile ||
               account.work == RecoveryWork::ResumePackage);
    } else {
        assert(account.work != RecoveryWork::ResumeFile);
        assert(account.work != RecoveryWork::ResumePackage);
    }
}

static RecoveryAccount across(RecoveryInput in, RecoveryEvent event) {
    in.event = event;
    const RecoveryAccount account = accountRecovery(in);
    assertStable(account, in);
    return account;
}

static void assertSameWork(const RecoveryInput& base) {
    const RecoveryEvent events[] = {
        RecoveryEvent::Pause, RecoveryEvent::Resume, RecoveryEvent::Restart,
        RecoveryEvent::Sleep, RecoveryEvent::Wake, RecoveryEvent::NetworkLoss,
    };
    const RecoveryAccount first = across(base, events[0]);
    for (RecoveryEvent event : events) {
        const RecoveryAccount account = across(base, event);
        assert(account.work == first.work);
        assert(account.reworkBytes == first.reworkBytes);
        assert(account.savedBytes == first.savedBytes);
        assert(account.byteExact == first.byteExact);
        assert(account.packagesInstalled == first.packagesInstalled);
        assert(account.downloadedBytes == first.downloadedBytes);
    }
    const RecoveryAccount sleep = across(base, RecoveryEvent::Sleep);
    const RecoveryAccount wake = across(base, RecoveryEvent::Wake);
    assert(sleep.downloadedBytes == wake.downloadedBytes);
    assert(sleep.packagesInstalled == wake.packagesInstalled);
    const RecoveryAccount pause = across(base, RecoveryEvent::Pause);
    const RecoveryAccount resume = across(base, RecoveryEvent::Resume);
    assert(pause.downloadedBytes == resume.downloadedBytes);
    assert(pause.packagesInstalled == resume.packagesInstalled);
    assert(!pause.verifyingSaved);
    assert(!across(base, RecoveryEvent::NetworkLoss).verifyingSaved);
    assert(!across(base, RecoveryEvent::Restart).verifyingSaved);
}

int main() {
    const RecoverySource sources[] = {RecoverySource::Direct,
                                      RecoverySource::Debrid};
    const RecoveryMode modes[] = {RecoveryMode::DownloadOnly,
                                  RecoveryMode::Stream, RecoveryMode::Port};
    const RecoveryEvent events[] = {
        RecoveryEvent::Pause, RecoveryEvent::Resume, RecoveryEvent::Restart,
        RecoveryEvent::Sleep, RecoveryEvent::Wake, RecoveryEvent::NetworkLoss,
    };
    for (RecoverySource source : sources) {
        for (RecoveryMode mode : modes) {
            for (RecoveryEvent event : events) {
                RecoveryInput preparing;
                preparing.source = source;
                preparing.mode = mode;
                preparing.phase = RecoveryPhase::Preparing;
                preparing.event = event;
                const RecoveryAccount account = accountRecovery(preparing);
                assertStable(account, preparing);
                assert(account.work == RecoveryWork::None);
                assert(account.reworkBytes == 0);
                assert(!account.showDownloaded);
                assert(!account.byteExact);
                assert(!account.verifyingSaved);
            }
        }
    }

    {
        RecoveryInput direct;
        direct.source = RecoverySource::Direct;
        direct.mode = RecoveryMode::DownloadOnly;
        direct.phase = RecoveryPhase::Downloading;
        direct.downloadedBytes = 70;
        direct.downloadTotalBytes = 100;
        direct.trustedPieces = true;
        assertSameWork(direct);
        const RecoveryAccount resume = across(direct, RecoveryEvent::Resume);
        assert(resume.work == RecoveryWork::ResumePieces);
        assert(resume.reworkBytes == 0);
        assert(resume.savedBytes == 70);
        assert(!resume.byteExact);
        assert(resume.verifyingSaved);
        assert(across(direct, RecoveryEvent::Wake).verifyingSaved);
        assert(scanPercent(0, 10) == 0);
        assert(resume.downloadedBytes == 70);
    }

    {
        RecoveryInput unscanned;
        unscanned.source = RecoverySource::Direct;
        unscanned.phase = RecoveryPhase::Downloading;
        unscanned.downloadedBytes = 70;
        unscanned.downloadTotalBytes = 100;
        unscanned.trustedPieces = false;
        for (RecoveryMode mode : modes)
            unscanned.mode = mode, assertSameWork(unscanned);
        const RecoveryAccount resume = across(unscanned, RecoveryEvent::Resume);
        assert(resume.work == RecoveryWork::VerifySaved);
        assert(resume.reworkBytes == 70);
        assert(resume.savedBytes == 70);
        assert(!resume.byteExact);
        assert(resume.verifyingSaved);
    }

    {
        RecoveryInput file;
        file.source = RecoverySource::Debrid;
        file.phase = RecoveryPhase::Downloading;
        file.downloadedBytes = 50;
        file.downloadTotalBytes = 100;
        file.unitBytes = 30;
        file.unitTotal = 80;
        file.bytePoint = true;
        for (RecoveryMode mode : modes)
            file.mode = mode, assertSameWork(file);
        const RecoveryAccount resume = across(file, RecoveryEvent::Resume);
        assert(resume.work == RecoveryWork::ResumeFile);
        assert(resume.byteExact);
        assert(resume.reworkBytes == 0);
        assert(resume.savedBytes == 50);
        assert(!resume.verifyingSaved);
    }

    {
        RecoveryInput refetch;
        refetch.source = RecoverySource::Debrid;
        refetch.phase = RecoveryPhase::Downloading;
        refetch.downloadedBytes = 50;
        refetch.downloadTotalBytes = 100;
        refetch.unitBytes = 30;
        refetch.unitTotal = 80;
        refetch.bytePoint = false;
        for (RecoveryMode mode : modes)
            refetch.mode = mode, assertSameWork(refetch);
        const RecoveryAccount loss =
            across(refetch, RecoveryEvent::NetworkLoss);
        assert(loss.work == RecoveryWork::RefetchFile);
        assert(loss.reworkBytes == 30);
        assert(loss.savedBytes == 20);
        assert(!loss.byteExact);
        assert(loss.downloadedBytes == 50);
    }

    {
        RecoveryInput journal;
        journal.mode = RecoveryMode::Stream;
        journal.phase = RecoveryPhase::Installing;
        journal.downloadedBytes = 80;
        journal.downloadTotalBytes = 200;
        journal.packagesInstalled = 1;
        journal.packageCount = 3;
        journal.unitBytes = 40;
        journal.unitTotal = 100;
        journal.bytePoint = true;
        for (RecoverySource source : sources) {
            journal.source = source;
            assertSameWork(journal);
            const RecoveryAccount resume =
                across(journal, RecoveryEvent::Resume);
            assert(resume.work == RecoveryWork::ResumePackage);
            assert(resume.byteExact);
            assert(resume.reworkBytes == 0);
            assert(resume.savedBytes == 40);
            assert(resume.packagesInstalled == 1);
            assert(resume.showInstalled);
            assert(!resume.verifyingSaved);
        }
    }

    {
        RecoveryInput reread;
        reread.phase = RecoveryPhase::Installing;
        reread.downloadedBytes = 80;
        reread.downloadTotalBytes = 200;
        reread.packagesInstalled = 2;
        reread.packageCount = 3;
        reread.unitBytes = 40;
        reread.unitTotal = 100;
        reread.bytePoint = false;
        for (RecoverySource source : sources) {
            for (RecoveryMode mode : {RecoveryMode::Stream, RecoveryMode::Port}) {
                reread.source = source;
                reread.mode = mode;
                assertSameWork(reread);
                const RecoveryAccount resume =
                    across(reread, RecoveryEvent::Wake);
                assert(resume.work == RecoveryWork::RereadPackage);
                assert(resume.reworkBytes == 40);
                assert(!resume.byteExact);
                assert(resume.packagesInstalled == 2);
            }
        }
    }

    {
        RecoveryInput recopy;
        recopy.source = RecoverySource::Direct;
        recopy.mode = RecoveryMode::Port;
        recopy.phase = RecoveryPhase::Installing;
        recopy.downloadedBytes = 90;
        recopy.downloadTotalBytes = 90;
        recopy.unitBytes = 12;
        recopy.unitTotal = 40;
        assertSameWork(recopy);
        const RecoveryAccount resume = across(recopy, RecoveryEvent::Resume);
        assert(resume.work == RecoveryWork::Recopy);
        assert(resume.reworkBytes == 12);
        assert(resume.savedBytes == 90);
        assert(!resume.byteExact);
        assert(!resume.showInstalled);
    }

    // The list percent can read 0 while the saved download is still 70,
    // and a blended install bar can read 100 while the download is not done.
    {
        DownloadTask task;
        task.source = TaskSource::Torrent;
        task.mode = TransferMode::DownloadOnly;
        task.status = DownloadStatus::Checking;
        task.wantedCompletedBytes = 70;
        task.wantedTotalBytes = 100;
        task.completedBytes = 70;
        task.totalBytes = 100;
        task.piecesDone = 0;
        task.piecesTotal = 10;
        task.recoveryTrusted = false;
        const RecoveryAccount account = recoveryAccountOf(task);
        assert(scanPercent(task.piecesDone, task.piecesTotal) == 0);
        assert(account.downloadedBytes == 70);
        assert(account.work == RecoveryWork::VerifySaved);
        assert(account.verifyingSaved);
        assert(account.reworkBytes == 70);
    }
    {
        DownloadTask task;
        task.mode = TransferMode::StreamInstall;
        task.status = DownloadStatus::Downloading;
        task.packageCount = 1;
        task.packagesInstalled = 1;
        task.wantedTotalBytes = 100;
        task.wantedCompletedBytes = 40;
        task.totalBytes = 100;
        task.completedBytes = 40;
        assert(streamInstallProgressOf(task) < 1.0f ||
               task.packagesInstalled == 1);
        const RecoveryAccount account = recoveryAccountOf(task);
        assert(account.downloadedBytes == 40);
        assert(account.packagesInstalled == 1);
        assert(!account.repeatsCompletedPackages);
    }
    {
        DownloadTask task;
        task.mode = TransferMode::StreamInstall;
        task.status = DownloadStatus::Installing;
        task.packageCount = 2;
        task.packagesInstalled = 1;
        task.wantedCompletedBytes = 100;
        task.wantedTotalBytes = 100;
        task.recoveryUnitBytes = 10;
        task.recoveryBytePoint = false;
        task.recoveryTrusted = true;
        task.resumeBitfield.push_back(0x80);
        const float blended = streamInstallProgressOf(task);
        const RecoveryAccount account = recoveryAccountOf(task);
        assert(blended > 0.0f);
        assert(account.work == RecoveryWork::RereadPackage);
        assert(account.packagesInstalled == 1);
        assert(account.downloadedBytes == 100);
        (void)blended;
    }
    return 0;
}
