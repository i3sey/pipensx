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
                   const std::vector<uint8_t>& expected) {
    const std::vector<uint8_t> actions = pipensx::selectUpdateFiles(preview);
    assert(actions.size() == expected.size());
    for (size_t i = 0; i < expected.size(); ++i)
        assert(actions[i] == expected[i]);
}

void testSelectsUpdateMarkedPackagesOnly() {
    TorrentPreview preview;
    preview.files = {package("Game [0100AAAA00B00000].nsp"),
                     package("Game Update [v131072].nsp"),
                     package("Game DLC 1.nsp"),
                     plain("readme.txt")};
    expectActions(preview, {
        static_cast<uint8_t>(FileAction::Skip),
        static_cast<uint8_t>(FileAction::Install),
        static_cast<uint8_t>(FileAction::Skip),
        static_cast<uint8_t>(FileAction::Skip),
    });
}

void testMatchesUpdateMarkersCaseInsensitively() {
    TorrentPreview preview;
    preview.files = {package("GAME.UPDATE.v1.2.0.nsz"),
                     package("game_upd_v1.1.0.nsz"),
                     package("Game.PATCH.v2.0.0.nsp")};
    expectActions(preview, {
        static_cast<uint8_t>(FileAction::Install),
        static_cast<uint8_t>(FileAction::Install),
        static_cast<uint8_t>(FileAction::Install),
    });
}

void testVersionTagWithNonZeroDigitIsAnUpdate() {
    TorrentPreview preview;
    preview.files = {package("Game [v0].nsp"),
                     package("Game [v196608].nsp")};
    expectActions(preview, {
        static_cast<uint8_t>(FileAction::Skip),
        static_cast<uint8_t>(FileAction::Install),
    });
}

void testFallsBackToAllPackagesWhenNothingMatches() {
    TorrentPreview preview;
    preview.files = {package("Game [v0].nsp"),
                     package("Game DLC 1.nsp"),
                     plain("readme.txt")};
    expectActions(preview, {
        static_cast<uint8_t>(FileAction::Install),
        static_cast<uint8_t>(FileAction::Install),
        static_cast<uint8_t>(FileAction::Skip),
    });
}

void testEmptyPreviewYieldsEmptyActions() {
    TorrentPreview preview;
    assert(pipensx::selectUpdateFiles(preview).empty());
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

} // namespace

int main() {
    testSelectsUpdateMarkedPackagesOnly();
    testMatchesUpdateMarkersCaseInsensitively();
    testVersionTagWithNonZeroDigitIsAnUpdate();
    testFallsBackToAllPackagesWhenNothingMatches();
    testEmptyPreviewYieldsEmptyActions();
    testMagnetPrefersCatalogEntry();
    testMagnetFallsBackToRuTrackerMagnetWhenNoCatalogEntry();
    std::puts("update file selection tests passed");
    return 0;
}
