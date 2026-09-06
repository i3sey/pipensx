#include "app/game_update_install.hpp"

#include <cassert>
#include <cstdio>
#include <cstring>
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

void expectSmartActions(const TorrentPreview& preview, bool titleInstalled,
                        const std::string& installedVersion,
                        const std::string& latestVersion,
                        const std::string& titleId,
                        const std::vector<uint8_t>& expected,
                        const std::vector<std::string>& installedDlcIds = {}) {
    const std::vector<uint8_t> actions = pipensx::selectSmartInstallFiles(
        preview, titleInstalled, installedVersion, latestVersion, titleId,
        installedDlcIds);
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

void testComboDumpDoesNotSelectOtherTitlePatch() {
    TorrentPreview preview;
    preview.files = {
        package("Super Mario Galaxy [0100AAAA00000000][v0].nsp"),
        package("Super Mario Galaxy [0100AAAA00000800][v131072].nsp"),
        package("Super Mario Galaxy 2 [0100BBBB00000000][v0].nsp"),
        package("Super Mario Galaxy 2 [0100BBBB00000800][v327680].nsp")};
    expectSmartActions(preview, true, "0", "131072", "0100AAAA00000000", {
        static_cast<uint8_t>(FileAction::Skip),
        static_cast<uint8_t>(FileAction::Install),
        static_cast<uint8_t>(FileAction::Skip),
        static_cast<uint8_t>(FileAction::Skip),
    });
    expectSmartActions(preview, true, "0", "327680", "0100AAAA00000000", {
        static_cast<uint8_t>(FileAction::Skip),
        static_cast<uint8_t>(FileAction::Skip),
        static_cast<uint8_t>(FileAction::Skip),
        static_cast<uint8_t>(FileAction::Skip),
    });
    expectActions(preview, "327680", {
        static_cast<uint8_t>(FileAction::Skip),
        static_cast<uint8_t>(FileAction::Skip),
        static_cast<uint8_t>(FileAction::Skip),
        static_cast<uint8_t>(FileAction::Install),
    }, "0100BBBB00000000");
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
        package("Game Update [0100AAAA00000000][v131072].nsp"),
        package("Game DLC 1 [0100AAAA00001001].nsp"),
        package("Other Game DLC [0100BBBB00001001].nsp"),
        plain("readme.txt")};
    expectSmartActions(preview, false, "", "131072", "0100AAAA00000000", {
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
        package("Game Update [0100AAAA00000000][v131072].nsp"),
        package("Game DLC 1 [0100AAAA00001001].nsp"),
        package("Game DLC 2 [0100AAAA00001002].nsp")};
    expectSmartActions(preview, true, "131072", "131072", "0100AAAA00000000", {
        static_cast<uint8_t>(FileAction::Skip),
        static_cast<uint8_t>(FileAction::Skip),
        static_cast<uint8_t>(FileAction::Install),
        static_cast<uint8_t>(FileAction::Skip),
    }, {"0100AAAA00001002"});
}

void testSmartInstallLeavesUnknownPackagesSkipped() {
    TorrentPreview preview;
    preview.files = {package("Game DLC 1.nsp"),
                     package("Game [0100AAAA00000000][v0].nsp")};
    expectSmartActions(preview, false, "", "", "0100AAAA00000000", {
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

void testSmartInstallMissingTitleInstallsPackagesOnly() {
    TorrentPreview preview;
    preview.files = {package("Game [0100AAAA00000000][v0].nsp"),
                     package("Game Update [0100AAAA00000000][v131072].nsp"),
                     plain("readme.txt")};
    const std::vector<uint8_t> actions = pipensx::selectSmartInstallFiles(
        preview, false, "", "131072", "0100AAAA00000000");
    assert(actions.size() == 3);
    assert(actions[0] == static_cast<uint8_t>(FileAction::Install));
    assert(actions[1] == static_cast<uint8_t>(FileAction::Install));
    assert(actions[2] == static_cast<uint8_t>(FileAction::Skip));
}

void testSmartInstallMissingTitleSkipsDlcPackages() {
    TorrentPreview preview;
    preview.files = {package("Game [0100AAAA00000000][v0].nsp"),
                     package("Game Update [0100AAAA00000000][v131072].nsp"),
                     package("DLC [0100AAAA00000001][v131072].nsp")};
    const std::vector<uint8_t> actions = pipensx::selectSmartInstallFiles(
        preview, false, "", "131072", "0100AAAA00000000");
    assert(actions.size() == 3);
    assert(actions[0] == static_cast<uint8_t>(FileAction::Install));
    assert(actions[1] == static_cast<uint8_t>(FileAction::Install));
    assert(actions[2] == static_cast<uint8_t>(FileAction::Skip));
}

void testSmartInstallSkipsSameTitleMods() {
    TorrentPreview preview;
    preview.files = {package("Game [0100AAAA00000000][v0].nsp"),
                     package("Game Update [0100AAAA00000000][v131072].nsp"),
                     package("Mods/Cheat [0100AAAA00000000][v131072].nsp")};
    const std::vector<uint8_t> actions = pipensx::selectSmartInstallFiles(
        preview, false, "", "131072", "0100AAAA00000000");
    assert(actions.size() == 3);
    assert(actions[0] == static_cast<uint8_t>(FileAction::Install));
    assert(actions[1] == static_cast<uint8_t>(FileAction::Install));
    assert(actions[2] == static_cast<uint8_t>(FileAction::Skip));
}

void testSmartInstallInstalledTitleInstallsOnlyNewerUpdate() {
    TorrentPreview preview;
    preview.files = {package("Game [0100AAAA00000000][v0].nsp"),
                     package("Game Update [0100AAAA00000000][v131072].nsp"),
                     package("DLC [0100AAAA00000001][v131072].nsp")};
    const std::vector<uint8_t> actions = pipensx::selectSmartInstallFiles(
        preview, true, "65536", "131072", "0100AAAA00000000");
    assert(actions.size() == 3);
    assert(actions[0] == static_cast<uint8_t>(FileAction::Skip));
    assert(actions[1] == static_cast<uint8_t>(FileAction::Install));
    assert(actions[2] == static_cast<uint8_t>(FileAction::Skip));
}

void testSmartInstallInstalledTitleSkipsSameOrUnknownVersion() {
    TorrentPreview preview;
    preview.files = {package("Game [0100AAAA00000000][v0].nsp"),
                     package("Game Update [0100AAAA00000000][v131072].nsp")};
    const std::vector<uint8_t> same = pipensx::selectSmartInstallFiles(
        preview, true, "131072", "131072", "0100AAAA00000000");
    const std::vector<uint8_t> unknown = pipensx::selectSmartInstallFiles(
        preview, true, "", "131072", "0100AAAA00000000");
    assert(same.size() == 2 && unknown.size() == 2);
    assert(same[0] == static_cast<uint8_t>(FileAction::Skip));
    assert(same[1] == static_cast<uint8_t>(FileAction::Skip));
    assert(unknown[0] == static_cast<uint8_t>(FileAction::Skip));
    assert(unknown[1] == static_cast<uint8_t>(FileAction::Skip));
}

void testSmartInstallInstalledTitleDoesNotUseHeuristicFallback() {
    TorrentPreview preview;
    preview.files = {package("Game [0100AAAA00000000][v0].nsp"),
                     package("Game Update [0100AAAA00000000][v131073].nsp")};
    const std::vector<uint8_t> actions = pipensx::selectSmartInstallFiles(
        preview, true, "65536", "131072", "0100AAAA00000000");
    assert(actions.size() == 2);
    assert(actions[0] == static_cast<uint8_t>(FileAction::Skip));
    assert(actions[1] == static_cast<uint8_t>(FileAction::Skip));
}

// Scene BOTW-style names: Patch title id …800, not the base …000. DLC must
// stay DLC even when its [vN] is also non-zero.
void testSmartInstallMatchesPatchTitleId() {
    TorrentPreview preview;
    preview.files = {
        package("Zelda [01007EF00011E000][v0] (13.48 GB).nsz"),
        package("Zelda [01007EF00011E800][v1114112] (0.94 GB).nsz"),
        package("Zelda DLC [01007EF00011F001][v196608] (1.30 GB).nsz"),
        package("Zelda DLC [01007EF00011F002][v196608].nsp")};
    expectSmartActions(preview, true, "0", "1114112", "01007EF00011E000", {
        static_cast<uint8_t>(FileAction::Skip),
        static_cast<uint8_t>(FileAction::Install),
        static_cast<uint8_t>(FileAction::Install),
        static_cast<uint8_t>(FileAction::Install),
    });
    expectActions(preview, "1114112", {
        static_cast<uint8_t>(FileAction::Skip),
        static_cast<uint8_t>(FileAction::Install),
        static_cast<uint8_t>(FileAction::Skip),
        static_cast<uint8_t>(FileAction::Skip),
    }, "01007EF00011E000");
}

void testSmartInstallUsesBundledPatchVersionWhenLatestEmpty() {
    TorrentPreview preview;
    preview.files = {
        package("Zelda [01007EF00011E000][v0].nsz"),
        package("Zelda [01007EF00011E800][v1114112].nsz"),
        package("Zelda DLC [01007EF00011F001][v196608].nsz")};
    expectSmartActions(preview, true, "0", "", "01007EF00011E000", {
        static_cast<uint8_t>(FileAction::Skip),
        static_cast<uint8_t>(FileAction::Install),
        static_cast<uint8_t>(FileAction::Install),
    });
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

// --- B6 update guard: BOTW/Broforce mocks ---
// BOTW scene shape: base …000 installed at patch 0, update …800 at
// 1114112 (17.0.0), LayeredFS mods present. The preflight must state the
// installed→target transition and raise the mods warning.
void testPreflightBotwWithMods() {
    pipensx::InstalledTitle installed;
    installed.titleId = "01007EF00011E000";
    installed.version = "0";
    installed.displayVersion = "1.0.0";
    installed.hasLayeredFsMods = true;
    const pipensx::UpdatePreflight pre =
        pipensx::describeUpdatePreflight(installed, "1114112", true);
    assert(pre.titleInstalled);
    assert(pre.installedXyz == "0.0.0");
    assert(pre.targetXyz == "17.0.0");
    assert(pre.displayVersion == "1.0.0");
    assert(pre.hasMods);
    assert(pre.versionKnown());
    assert(pre.warnMods());
}

// Broforce shape: patched title (2.0.0) updated to 3.0.0, no mods. The
// preflight states the transition without a mods warning; the HOS
// requirement (when the CNMT stamps one) classifies as NeedsNewHos.
void testPreflightBroforceWithoutMods() {
    pipensx::InstalledTitle installed;
    installed.titleId = "01000000000B00CE";
    installed.version = "131072";
    installed.displayVersion = "1.0.1";
    installed.hasLayeredFsMods = false;
    const pipensx::UpdatePreflight pre =
        pipensx::describeUpdatePreflight(installed, "196608", true);
    assert(pre.installedXyz == "2.0.0");
    assert(pre.targetXyz == "3.0.0");
    assert(pre.versionKnown());
    assert(!pre.warnMods());
}

void testPreflightUnknownVersionStillWarnsMods() {
    pipensx::InstalledTitle installed;
    installed.titleId = "01007EF00011E000";
    installed.version = "";
    installed.hasLayeredFsMods = true;
    const pipensx::UpdatePreflight pre =
        pipensx::describeUpdatePreflight(installed, "1114112", true);
    assert(pre.installedXyz.empty());
    assert(!pre.versionKnown());
    assert(pre.warnMods());
}

void testPreflightNotInstalledNeverWarns() {
    pipensx::InstalledTitle installed;
    installed.titleId = "01007EF00011E000";
    installed.version = "0";
    installed.hasLayeredFsMods = true;
    const pipensx::UpdatePreflight pre =
        pipensx::describeUpdatePreflight(installed, "1114112", false);
    assert(!pre.warnMods());
}

void testRequiredHosVersionFormat() {
    assert(pipensx::formatRequiredHosVersion(0).empty());
    assert(pipensx::formatRequiredHosVersion(
               pipensx::makeRequiredSystemVersion(19, 0, 0)) == "19.0.0");
    assert(pipensx::formatRequiredHosVersion(
               pipensx::makeRequiredSystemVersion(18, 1, 0)) == "18.1.0");
}

void testUpdateRequiresNewerHos() {
    const uint32_t need19 =
        pipensx::makeRequiredSystemVersion(19, 0, 0);
    assert(!pipensx::updateRequiresNewerHos(0, 18, 1, 0));
    assert(pipensx::updateRequiresNewerHos(need19, 18, 1, 0));
    assert(!pipensx::updateRequiresNewerHos(need19, 19, 0, 0));
    assert(!pipensx::updateRequiresNewerHos(need19, 20, 0, 0));
    const uint32_t need181 =
        pipensx::makeRequiredSystemVersion(18, 1, 0);
    assert(pipensx::updateRequiresNewerHos(need181, 18, 0, 0));
    assert(!pipensx::updateRequiresNewerHos(need181, 18, 1, 0));
}

void testClassifyModConflict() {
    using pipensx::UpdateFailureHint;
    assert(pipensx::classifyUpdateFailure("Some install error", true, 0,
                                           20, 1, 0) ==
           UpdateFailureHint::ModConflict);
    assert(pipensx::classifyUpdateFailure("Some install error", false, 0,
                                           20, 1, 0) ==
           UpdateFailureHint::None);
    assert(pipensx::classifyUpdateFailure("", true, 0, 20, 1, 0) ==
           UpdateFailureHint::ModConflict);
}

void testClassifyNeedsNewHos() {
    using pipensx::UpdateFailureHint;
    const uint32_t need19 =
        pipensx::makeRequiredSystemVersion(19, 0, 0);
    assert(pipensx::classifyUpdateFailure("", false, need19, 18, 1, 0) ==
           UpdateFailureHint::NeedsNewHos);
    assert(pipensx::classifyUpdateFailure("", false, need19, 19, 0, 0) ==
           UpdateFailureHint::None);
}

void testClassifySigPatchesWins() {
    using pipensx::UpdateFailureHint;
    const uint32_t need19 =
        pipensx::makeRequiredSystemVersion(19, 0, 0);
    const std::string ticket =
        "Title ticket import failed (0x00000291). ES signature patches "
        "(sys-patch) may be missing or outdated for this firmware.";
    // Even with mods and a firmware requirement, the ticket error names
    // the fix, so it wins.
    assert(pipensx::classifyUpdateFailure(ticket, true, need19, 18, 1, 0) ==
           UpdateFailureHint::SigPatches);
    assert(pipensx::classifyUpdateFailure("failed with 0x291", false, 0,
                                           20, 1, 0) ==
           UpdateFailureHint::SigPatches);
    assert(pipensx::classifyUpdateFailure("missing sys-patch", false, 0,
                                           20, 1, 0) ==
           UpdateFailureHint::SigPatches);
}

void testFormatFailureHint() {
    using pipensx::UpdateFailureHint;
    assert(pipensx::formatUpdateFailureHint(UpdateFailureHint::None, "17.0.0",
                                             0)
               .empty());
    const std::string mods = pipensx::formatUpdateFailureHint(
        UpdateFailureHint::ModConflict, "17.0.0", 0);
    assert(mods.find("17.0.0") != std::string::npos);
    assert(mods.find("atmosphere/contents") != std::string::npos);
    const uint32_t need19 =
        pipensx::makeRequiredSystemVersion(19, 0, 0);
    const std::string hos = pipensx::formatUpdateFailureHint(
        UpdateFailureHint::NeedsNewHos, "3.0.0", need19);
    assert(hos.find("3.0.0") != std::string::npos);
    assert(hos.find("19.0.0") != std::string::npos);
    const std::string sig = pipensx::formatUpdateFailureHint(
        UpdateFailureHint::SigPatches, "", 0);
    assert(sig.find("0x291") != std::string::npos);
}

void testInstalledVersionLabel() {
    using pipensx::formatInstalledVersionLabel;
    assert(formatInstalledVersionLabel("65536", "1.0.1") ==
           "1.0.0 (1.0.1)");
    assert(formatInstalledVersionLabel("65536", "") == "1.0.0");
    assert(formatInstalledVersionLabel("", "1.0.1") == "1.0.1");
    assert(formatInstalledVersionLabel("", "").empty());
    assert(formatInstalledVersionLabel("junk", "").empty());
    assert(formatInstalledVersionLabel("junk", "1.0") == "1.0");
}

void testLayeredFsModDir() {
    using pipensx::layeredFsModDirForTitle;
    assert(layeredFsModDirForTitle("sdmc:/", "01007EF00011E000") ==
           "sdmc:/atmosphere/contents/01007EF00011E000");
    // Patch id (…800) normalises onto the base id mods live under.
    assert(layeredFsModDirForTitle("sdmc:/", "01007EF00011E800") ==
           "sdmc:/atmosphere/contents/01007EF00011E000");
    // Lowercase input still maps onto the canonical uppercase dir.
    assert(layeredFsModDirForTitle("sdmc:/", "01007ef00011e000") ==
           "sdmc:/atmosphere/contents/01007EF00011E000");
    assert(layeredFsModDirForTitle("sdmc:/", "not-a-title").empty());
    assert(layeredFsModDirForTitle("", "01007EF00011E000").empty());
}

void testNacpDisplayVersionString() {
    using pipensx::nacpDisplayVersionString;
    char field[16] = {};
    std::memcpy(field, "1.26.30", 7);
    assert(nacpDisplayVersionString(field, sizeof(field)) == "1.26.30");
    char empty[16] = {};
    assert(nacpDisplayVersionString(empty, sizeof(empty)).empty());
    assert(nacpDisplayVersionString(nullptr, 16).empty());
    assert(nacpDisplayVersionString(field, 0).empty());
    // Full-width field without a NUL still reads whole.
    char full[16];
    std::memset(full, 'A', sizeof(full));
    assert(nacpDisplayVersionString(full, sizeof(full)) ==
           std::string(16, 'A'));
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
    testComboDumpDoesNotSelectOtherTitlePatch();
    testSmartInstallIncludesApplicableDlc();
    testSmartInstallSkipsBaseWhenInstalledButKeepsDlc();
    testSmartInstallLeavesUnknownPackagesSkipped();
    testUpdateRecheckSettled();
    testEmptyPreviewYieldsEmptyActions();
    testSelectFilesInstallsExactlyThePicks();
    testSmartInstallMissingTitleInstallsPackagesOnly();
    testSmartInstallMissingTitleSkipsDlcPackages();
    testSmartInstallSkipsSameTitleMods();
    testSmartInstallInstalledTitleInstallsOnlyNewerUpdate();
    testSmartInstallInstalledTitleSkipsSameOrUnknownVersion();
    testSmartInstallInstalledTitleDoesNotUseHeuristicFallback();
    testSmartInstallMatchesPatchTitleId();
    testSmartInstallUsesBundledPatchVersionWhenLatestEmpty();
    testMagnetPrefersCatalogEntry();
    testMagnetFallsBackToRuTrackerMagnetWhenNoCatalogEntry();
    testUtf8TruncateBoundary();
    testPreflightBotwWithMods();
    testPreflightBroforceWithoutMods();
    testPreflightUnknownVersionStillWarnsMods();
    testPreflightNotInstalledNeverWarns();
    testRequiredHosVersionFormat();
    testUpdateRequiresNewerHos();
    testClassifyModConflict();
    testClassifyNeedsNewHos();
    testClassifySigPatchesWins();
    testFormatFailureHint();
    testInstalledVersionLabel();
    testLayeredFsModDir();
    testNacpDisplayVersionString();
    std::puts("update file selection tests passed");
    return 0;
}
