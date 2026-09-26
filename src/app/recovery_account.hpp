#pragma once

#include <cstdint>

namespace pipensx {

// What a resume is allowed to claim. The downloads percent is not an input:
// a hash scan can sit at 0% while these figures still name what is on disk.
enum class RecoverySource : uint8_t { Direct, Debrid };

enum class RecoveryMode : uint8_t { DownloadOnly, Stream, Port };

enum class RecoveryPhase : uint8_t { Preparing, Downloading, Installing };

enum class RecoveryEvent : uint8_t {
    Pause,
    Resume,
    Restart,
    Sleep,
    Wake,
    NetworkLoss,
};

enum class RecoveryWork : uint8_t {
    None,
    VerifySaved,     // re-read saved pieces; nothing is thrown away
    ResumePieces,    // trusted piece bitfield; not an arbitrary byte
    RereadPackage,   // open package starts over; earlier packages stay
    ResumePackage,   // install journal continues at a saved stream offset
    RefetchFile,     // current debrid file starts over; earlier files stay
    ResumeFile,      // current debrid file continues at a saved byte offset
    Recopy,          // port file copy starts over; finished downloads stay
};

enum class RecoveryNotice : uint8_t {
    None,
    Verifying,
    VerifyRework,
    RereadPackage,
    RefetchFile,
    Recopy,
    PieceResume,
    ByteResume,
};

struct RecoveryInput {
    RecoverySource source = RecoverySource::Direct;
    RecoveryMode mode = RecoveryMode::DownloadOnly;
    RecoveryPhase phase = RecoveryPhase::Preparing;
    RecoveryEvent event = RecoveryEvent::Pause;
    uint64_t downloadedBytes = 0;
    uint64_t downloadTotalBytes = 0;
    uint32_t packagesInstalled = 0;
    uint32_t packageCount = 0;
    uint64_t unitBytes = 0;
    uint64_t unitTotal = 0;
    bool trustedPieces = false;
    bool bytePoint = false;
};

struct RecoveryAccount {
    uint64_t downloadedBytes = 0;
    uint64_t downloadTotalBytes = 0;
    uint32_t packagesInstalled = 0;
    uint32_t packageCount = 0;
    bool showDownloaded = false;
    bool showInstalled = false;
    bool verifyingSaved = false;
    RecoveryWork work = RecoveryWork::None;
    uint64_t savedBytes = 0;
    uint64_t reworkBytes = 0;
    bool byteExact = false;
    // Always false. Completed packages are not installed again.
    bool repeatsCompletedPackages = false;
};

inline const char* recoveryWorkName(RecoveryWork work) {
    switch (work) {
        case RecoveryWork::None: return "none";
        case RecoveryWork::VerifySaved: return "verify";
        case RecoveryWork::ResumePieces: return "pieces";
        case RecoveryWork::RereadPackage: return "reread";
        case RecoveryWork::ResumePackage: return "package";
        case RecoveryWork::RefetchFile: return "refetch";
        case RecoveryWork::ResumeFile: return "file";
        case RecoveryWork::Recopy: return "recopy";
    }
    return "none";
}

// Saved download bytes, installed packages, and any re-read or re-fetch.
// `event` only decides whether a Direct resume is in the hash-scan window.
// It never changes the saved figures or reinstalls a finished package.
inline RecoveryAccount accountRecovery(const RecoveryInput& in) {
    RecoveryAccount account;
    account.downloadedBytes = in.downloadedBytes;
    account.downloadTotalBytes = in.downloadTotalBytes;
    account.packagesInstalled = in.packagesInstalled;
    account.packageCount = in.packageCount;
    account.repeatsCompletedPackages = false;

    const bool installMode = in.mode != RecoveryMode::DownloadOnly;
    account.showInstalled = installMode && in.packageCount > 0;
    account.showDownloaded = in.downloadedBytes > 0 || in.downloadTotalBytes > 0;

    if (in.phase == RecoveryPhase::Preparing) {
        account.showDownloaded = false;
        account.work = RecoveryWork::None;
        return account;
    }

    if (in.phase == RecoveryPhase::Installing) {
        if (in.bytePoint && in.unitBytes > 0 &&
            in.mode == RecoveryMode::Stream) {
            account.work = RecoveryWork::ResumePackage;
            account.byteExact = true;
            account.savedBytes = in.unitBytes;
        } else if (in.unitBytes > 0 && in.packageCount == 0 &&
                   in.mode == RecoveryMode::Port) {
            account.work = RecoveryWork::Recopy;
            account.reworkBytes = in.unitBytes;
            account.savedBytes = in.downloadedBytes;
        } else if (in.unitBytes > 0) {
            account.work = RecoveryWork::RereadPackage;
            account.reworkBytes = in.unitBytes;
        }
    } else if (in.source == RecoverySource::Direct) {
        if (in.trustedPieces) {
            account.work = RecoveryWork::ResumePieces;
            account.savedBytes = in.downloadedBytes;
        } else if (in.downloadedBytes > 0) {
            account.work = RecoveryWork::VerifySaved;
            account.savedBytes = in.downloadedBytes;
            account.reworkBytes = in.downloadedBytes;
        }
    } else if (in.bytePoint && in.unitBytes > 0) {
        account.work = RecoveryWork::ResumeFile;
        account.byteExact = true;
        account.savedBytes = in.downloadedBytes;
    } else if (in.unitBytes > 0) {
        account.work = RecoveryWork::RefetchFile;
        account.reworkBytes = in.unitTotal && in.unitBytes > in.unitTotal
            ? in.unitTotal : in.unitBytes;
        account.savedBytes = in.downloadedBytes > account.reworkBytes
            ? in.downloadedBytes - account.reworkBytes : 0;
    } else {
        account.savedBytes = in.downloadedBytes;
    }

    const bool scanning = in.event == RecoveryEvent::Resume ||
                          in.event == RecoveryEvent::Wake;
    account.verifyingSaved = scanning &&
        in.source == RecoverySource::Direct &&
        in.phase == RecoveryPhase::Downloading &&
        (account.work == RecoveryWork::VerifySaved ||
         account.work == RecoveryWork::ResumePieces);
    return account;
}

// `quiet` is a smooth transfer. Rework is still announced; a piece or byte
// resume is mentioned only when the transfer is paused, restarting, or scanning.
inline RecoveryNotice recoveryNotice(const RecoveryAccount& account,
                                     bool quiet) {
    if (account.work == RecoveryWork::VerifySaved && account.reworkBytes > 0)
        return RecoveryNotice::VerifyRework;
    if (account.work == RecoveryWork::RereadPackage && account.reworkBytes > 0)
        return RecoveryNotice::RereadPackage;
    if (account.work == RecoveryWork::RefetchFile && account.reworkBytes > 0)
        return RecoveryNotice::RefetchFile;
    if (account.work == RecoveryWork::Recopy && account.reworkBytes > 0)
        return RecoveryNotice::Recopy;
    if (!quiet && account.work == RecoveryWork::ResumePieces)
        return RecoveryNotice::PieceResume;
    if (!quiet && account.byteExact)
        return RecoveryNotice::ByteResume;
    if (account.verifyingSaved)
        return RecoveryNotice::Verifying;
    return RecoveryNotice::None;
}

}  // namespace pipensx
