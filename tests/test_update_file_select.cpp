#include "app/game_update_install.hpp"

#include <cassert>
#include <cstdio>
#include <string>
#include <vector>

namespace {

using pipensx::CatalogEntry;
using pipensx::FileAction;
using pipensx::TorrentPreview;

TorrentPreview::File package(const std::string& path) {
    TorrentPreview::File file;
    file.path = path;
    file.package = true;
    return file;
}

TorrentPreview::File plain(const std::string& path) {
    TorrentPreview::File file;
    file.path = path;
    file.package = false;
    return file;
}

void expectActions(const TorrentPreview& preview,
                   const std::string& latestVersion,
                   const std::vector<uint8_t>& expected,
                   const std::string& titleId = {}) {
    const std::vector<uint8_t> actions =
        pipensx::selectUpdateFiles(preview, latestVersion, titleId);
    assert(actions.size() == expected.size());
    for (size_t i = 0; i < expected.size(); ++i)
        assert(actions[i] == expected[i]);
}

void expectSmartActions(const TorrentPreview& preview,
                        const std::string& titleId,
                        const std::vector<uint8_t>& expected,
                        const std::vector<std::string>& installedTitleIds = {},
                        const std::vector<std::string>& installedDlcIds = {}) {
    const std::vector<uint8_t> actions = pipensx::selectSmartInstallFiles(
        preview, titleId, installedTitleIds, installedDlcIds);
    assert(actions.size() == expected.size());
    for (size_t i = 0; i < expected.size(); ++i)
        assert(actions[i] == expected[i]);
}

void testExactVersionTagIsTheUpdate() {
    TorrentPreview preview;
    preview.files = {package("Game [0100AAAA00B00000].nsp"),
                     package("Game Update [v131072].nsp"),
                     package("Game DLC 1.nsp"),
                     plain("readme.txt")};
    expectActions(preview, "131072", {
        static_cast<uint8_t>(FileAction::Skip),
        static_cast<uint8_t>(FileAction::Install),
        static_cast<uint8_t>(FileAction::Skip),
        static_cast<uint8_t>(FileAction::Skip),
    });
}

// The regression from the Switch log: a mod bundle named
// "TagNX exeFS Mod (1.26.30)/Minecraft [0100D71004694800][v9895936].nsp"
// next to the real "Minecraft [0100D71004694800][v10092544].nsp" must not be
// treated as the update.
void testModBundleWithOlderVersionTagIsExcluded() {
    TorrentPreview preview;
    preview.files = {package("Minecraft [0100D71004694800][v10092544].nsp"),
                     package("TagNX exeFS Mod (1.26.30)/"
                             "Minecraft [0100D71004694800][v9895936].nsp"),
                     plain("readme.txt")};
    expectActions(preview, "10092544", {
        static_cast<uint8_t>(FileAction::Install),
        static_cast<uint8_t>(FileAction::Skip),
        static_cast<uint8_t>(FileAction::Skip),
    });
}

void testExactVersionWinsOverOtherTags() {
    TorrentPreview preview;
    preview.files = {package("Game Update [v131072].nsp"),
                     package("Game [v999999].nsp")};
    expectActions(preview, "131072", {
        static_cast<uint8_t>(FileAction::Install),
        static_cast<uint8_t>(FileAction::Skip),
    });
}

void testMultipleFilesWithTheVersionAreAllReported() {
    TorrentPreview preview;
    preview.files = {package("Game Update [v131072].nsp"),
                     package("Mods/Game Update [v131072].nsp"),
                     package("Game [v0].nsp")};
    const std::vector<size_t> matches =
        pipensx::updateVersionMatches(preview, "131072");
    assert(matches.size() == 2);
    assert(matches[0] == 0 && matches[1] == 1);
    // The default mask installs both; the caller shows a chooser instead.
    expectActions(preview, "131072", {
        static_cast<uint8_t>(FileAction::Install),
        static_cast<uint8_t>(FileAction::Install),
        static_cast<uint8_t>(FileAction::Skip),
    });
}

void testNoVersionMatches() {
    TorrentPreview preview;
    preview.files = {package("Game Update [v131072].nsp"),
                     package("Game [v0].nsp")};
    assert(pipensx::updateVersionMatches(preview, "131073").empty());
    assert(pipensx::updateVersionMatches(preview, "").empty());
    assert(pipensx::updateVersionMatches(preview, "0").empty());
}

// strtoull("1.2.3") would yield 1 and match a [v1] package; only strict
// decimal versions may match.
void testNonDecimalVersionNeverMatches() {
    TorrentPreview preview;
    preview.files = {package("Game Update [v1].nsp"),
                     package("Game Update [v131072].nsp")};
    assert(pipensx::updateVersionMatches(preview, "1.2.3").empty());
    assert(pipensx::updateVersionMatches(preview, " 131072").empty());
    assert(pipensx::updateVersionMatches(preview, "v131072").empty());
    assert(pipensx::updateVersionMatches(preview, "99999999999999999999").empty());
}

void testUnknownVersionPrefersHighestTag() {
    TorrentPreview preview;
    preview.files = {package("Game Update [v131072].nsp"),
                     package("Game [v999999].nsp"),
                     package("Game DLC 1.nsp")};
    expectActions(preview, "131073", {
        static_cast<uint8_t>(FileAction::Skip),
        static_cast<uint8_t>(FileAction::Install),
        static_cast<uint8_t>(FileAction::Skip),
    });
}

void testMarkerFallbackWithoutTags() {
    TorrentPreview preview;
    preview.files = {package("GAME.UPDATE.v1.2.0.nsz"),
                     package("game_upd_v1.1.0.nsz"),
                     package("Game.PATCH.v2.0.0.nsp")};
    expectActions(preview, "", {
        static_cast<uint8_t>(FileAction::Install),
        static_cast<uint8_t>(FileAction::Install),
        static_cast<uint8_t>(FileAction::Install),
    });
}

void testFallsBackToAllSkipWhenNothingMatches() {
    TorrentPreview preview;
    preview.files = {package("Game [v0].nsp"),
                     package("Game DLC 1.nsp"),
                     plain("readme.txt")};
    expectActions(preview, "131072", {
        static_cast<uint8_t>(FileAction::Skip),
        static_cast<uint8_t>(FileAction::Skip),
        static_cast<uint8_t>(FileAction::Skip),
    });
}

void testSameVersionDifferentTitleIdUsesTitleId() {
    TorrentPreview preview;
    preview.files = {
        package("Game [0100AAAA00000000][v131072].nsp"),
        package("Mods/Other [0100BBBB00000000][v131072].nsp"),
        plain("readme.txt")};
    expectActions(preview, "131072", {
        static_cast<uint8_t>(FileAction::Install),
        static_cast<uint8_t>(FileAction::Skip),
        static_cast<uint8_t>(FileAction::Skip),
    }, "0100AAAA00000000");
}

void testSmartInstallIncludesApplicableDlc() {
    TorrentPreview preview;
    preview.files = {
        package("Game [0100AAAA00000000][v0].nsp"),
        package("Game Update [0100AAAA00000800][v131072].nsp"),
        package("Game DLC 1 [0100AAAA00001001].nsp"),
        package("Other Game DLC [0100BBBB00001001].nsp"),
        plain("readme.txt")};
    expectSmartActions(preview, "0100AAAA00000000", {
        static_cast<uint8_t>(FileAction::Install),
        static_cast<uint8_t>(FileAction::Install),
        static_cast<uint8_t>(FileAction::Install),
        static_cast<uint8_t>(FileAction::Skip),
        static_cast<uint8_t>(FileAction::Skip),
    });
}

void testSmartInstallSkipsBaseWhenInstalledButKeepsDlc() {
    TorrentPreview preview;
    preview.files = {
        package("Game [0100AAAA00000000][v0].nsp"),
        package("Game Update [0100AAAA00000800][v131072].nsp"),
        package("Game DLC 1 [0100AAAA00001001].nsp"),
        package("Game DLC 2 [0100AAAA00001002].nsp")};
    expectSmartActions(preview, "0100AAAA00000000", {
        static_cast<uint8_t>(FileAction::Skip),
        static_cast<uint8_t>(FileAction::Install),
        static_cast<uint8_t>(FileAction::Install),
        static_cast<uint8_t>(FileAction::Skip),
    }, {"0100AAAA00000000"}, {"0100AAAA00001002"});
}

void testSmartInstallLeavesUnknownPackagesSkipped() {
    TorrentPreview preview;
    preview.files = {package("Game DLC 1.nsp"),
                     package("Game [0100AAAA00000000][v0].nsp")};
    expectSmartActions(preview, "0100AAAA00000000", {
        static_cast<uint8_t>(FileAction::Skip),
        static_cast<uint8_t>(FileAction::Install),
    });
}

void testUpdateRecheckSettled() {
    using pipensx::DownloadStatus;
    assert(pipensx::updateRecheckSettled(false, DownloadStatus::Downloading));
    assert(!pipensx::updateRecheckSettled(true, DownloadStatus::Downloading));
    assert(!pipensx::updateRecheckSettled(true, DownloadStatus::Installing));
    assert(pipensx::updateRecheckSettled(true, DownloadStatus::Installed));
    assert(pipensx::updateRecheckSettled(true, DownloadStatus::Completed));
    assert(pipensx::updateRecheckSettled(true, DownloadStatus::Error));
    assert(pipensx::updateRecheckSettled(true, DownloadStatus::Removing));
}

void testEmptyPreviewYieldsEmptyActions() {
    TorrentPreview preview;
    assert(pipensx::selectUpdateFiles(preview, "131072").empty());
    assert(pipensx::updateVersionMatches(preview, "131072").empty());
}

void testSelectFilesInstallsExactlyThePicks() {
    TorrentPreview preview;
    preview.files = {package("A.nsp"),
                     package("B.nsp"),
                     plain("readme.txt")};
    const std::vector<uint8_t> actions = pipensx::selectFiles(preview, {1});
    assert(actions.size() == 3);
    assert(actions[0] == static_cast<uint8_t>(FileAction::Skip));
    assert(actions[1] == static_cast<uint8_t>(FileAction::Install));
    assert(actions[2] == static_cast<uint8_t>(FileAction::Skip));
}

void testMagnetPrefersCatalogEntry() {
    CatalogEntry entry;
    entry.infoHash = "E21269D03D34B557F63CE915DEA14F765C9C9798";
    entry.magnetUri = "magnet:?xt=urn:btih:E21269D03D34B557F63CE915DEA14F765C9C9798&tr=http://bt.t-ru.org/ann?magnet";
    assert(pipensx::updateMagnetFor("E21269D03D34B557F63CE915DEA14F765C9C9798",
                                    &entry) == entry.magnetUri);
    CatalogEntry noMagnet;
    noMagnet.infoHash = "E21269D03D34B557F63CE915DEA14F765C9C9798";
    assert(pipensx::updateMagnetFor("E21269D03D34B557F63CE915DEA14F765C9C9798",
                                    &noMagnet) ==
           "magnet:?xt=urn:btih:E21269D03D34B557F63CE915DEA14F765C9C9798"
           "&tr=http://bt.t-ru.org/ann?magnet");
}

void testMagnetFallsBackToRuTrackerMagnetWhenNoCatalogEntry() {
    assert(pipensx::updateMagnetFor("e21269d03d34b557f63ce915dea14f765c9c9798",
                                    nullptr) ==
           "magnet:?xt=urn:btih:e21269d03d34b557f63ce915dea14f765c9c9798"
           "&tr=http://bt.t-ru.org/ann?magnet");
}

void testUtf8TruncateBoundary() {
    const std::string cyr = "\xD0\xB0\xD0\xB1\xD0\xB2";  // "абв"
    assert(pipensx::utf8TruncateBoundary(cyr, 99) == cyr.size());
    assert(pipensx::utf8TruncateBoundary(cyr, 4) == 4);  // whole "аб"
    assert(pipensx::utf8TruncateBoundary(cyr, 3) == 2);  // mid "б" -> "а"
    assert(pipensx::utf8TruncateBoundary(cyr, 2) == 2);
    assert(pipensx::utf8TruncateBoundary(cyr, 1) == 0);
    assert(pipensx::utf8TruncateBoundary(cyr, 0) == 0);
    const std::string single = "\xC3\xA0";  // "à" as two bytes
    assert(pipensx::utf8TruncateBoundary(single, 2) == 2);
    assert(pipensx::utf8TruncateBoundary(single, 1) == 0);
    assert(pipensx::utf8TruncateBoundary("abc", 2) == 2);
    assert(pipensx::utf8TruncateBoundary("", 0) == 0);
}

} // namespace

int main() {
    testExactVersionTagIsTheUpdate();
    testModBundleWithOlderVersionTagIsExcluded();
    testExactVersionWinsOverOtherTags();
    testMultipleFilesWithTheVersionAreAllReported();
    testNoVersionMatches();
    testNonDecimalVersionNeverMatches();
    testUnknownVersionPrefersHighestTag();
    testMarkerFallbackWithoutTags();
    testFallsBackToAllSkipWhenNothingMatches();
    testSameVersionDifferentTitleIdUsesTitleId();
    testSmartInstallIncludesApplicableDlc();
    testSmartInstallSkipsBaseWhenInstalledButKeepsDlc();
    testSmartInstallLeavesUnknownPackagesSkipped();
    testUpdateRecheckSettled();
    testEmptyPreviewYieldsEmptyActions();
    testSelectFilesInstallsExactlyThePicks();
    testMagnetPrefersCatalogEntry();
    testMagnetFallsBackToRuTrackerMagnetWhenNoCatalogEntry();
    testUtf8TruncateBoundary();
    std::puts("update file selection tests passed");
    return 0;
}
