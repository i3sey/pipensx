// golden_runner — F4 golden-screenshot harness (PC/SDL2 build only).
//
// Renders ONE pipensx screen with deterministic fixture data and writes a
// PNG. One screen per process keeps runs fully isolated (fresh focus state,
// fresh animations, fresh services). Driven by scripts/golden.sh.
//
// Usage:
//   golden_runner --fixtures <dir> --out <file.png> --theme light|dark
//                 [--locale en-US|ru]
//                 --screen catalog|shelf-scroll|shelf-header|detail|torrent-selection|
//                          torrent-selection-scroll|downloads|downloads-back|frame|
//                          hints-budget|installed|installed-populated|updates|
//                          installed-bundles|
//                          update-chooser|
//                          update-chooser-toggle|settings|settings-debrid|help|
//                          network-health|first-run|first-run-focus|first-run-disclaimer|debrid-link|
//                          about|bug-report|
//                          bug-report-detail|bug-report-focus|sidebar-touch
//                 [--frames N] [--sandbox <dir>]
//
// downloads-back, downloads-removing, torrent-selection-scroll, hints-budget,
// bug-report-focus, sidebar-touch, update-chooser-toggle, first-run-disclaimer and
// installed-bundles are behaviour checks: they assert and exit non-zero
// instead of producing a baseline.
//
// Determinism notes:
//   - run with LIBGL_ALWAYS_SOFTWARE=1 so Mesa llvmpipe rasterizes the same
//     locally and in CI (scripts/golden.sh sets this);
//   - the process chdirs into a scratch sandbox so all "sdmc:/..." paths of
//     the real app land in an empty, throwaway directory tree;
//   - GameMetadataService image networking is paused: remote artwork stays
//     at placeholders;
//   - the libnx shim (src/platform/pc/switch.h) reports an empty installed
//     library and a fixed firmware version;
//   - SD capacity is pinned via setStorageSpaceOverride, so every storage
//     meter renders the same numbers on any machine.

#include <glad/glad.h>

#include <borealis.hpp>
#include <borealis/views/hint.hpp> // not re-exported by borealis.hpp
#include <zlib.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <unistd.h>

#include "app/app_settings.hpp"
#include "app/catalog_service.hpp"
#include "app/download_manager.hpp"
#include "app/game_metadata_service.hpp"
#include "app/game_update_service.hpp"
#include "app/install_space.hpp"
#include "app/installed_title_service.hpp"
#include "ui/catalog/catalog_view.hpp"
#include "ui/common/ui_helpers.hpp"
#include "ui/detail/game_detail.hpp"
#include "ui/detail/screenshot_viewer.hpp"
#include "ui/detail/torrent_selection.hpp"
#include "ui/downloads/downloads_view.hpp"
#include "ui/downloads/task_files_activity.hpp"
#include "ui/first_run_view.hpp"
#include "ui/i18n.hpp"
#include "ui/installed/installed_view.hpp"
#include "ui/main_frame.hpp"
#include "ui/settings/about_view.hpp"
#include "ui/settings/network_health.hpp"
#include "ui/settings/bug_report_view.hpp"
#include "ui/settings/help_view.hpp"
#include "ui/settings/settings_view.hpp"
#include "ui/theme.hpp"

#include <ctime>

extern "C" {
#include "core/util.h"
}

namespace fs = std::filesystem;

using pipensx::AppSettings;
using pipensx::CatalogService;
using pipensx::DownloadManager;
using pipensx::GameMetadataService;
using pipensx::GameUpdateService;
using pipensx::InstalledTitle;
using pipensx::InstalledTitleService;
using pipensx::ModIndexService;
using namespace pipensx::ui;

namespace {

/* ---- minimal PNG writer (8-bit RGB, zlib) ---- */

void putBe32(std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(static_cast<uint8_t>(value >> 24));
    out.push_back(static_cast<uint8_t>(value >> 16));
    out.push_back(static_cast<uint8_t>(value >> 8));
    out.push_back(static_cast<uint8_t>(value));
}

bool writeChunk(FILE* file, const char type[4], const uint8_t* data,
                size_t size) {
    std::vector<uint8_t> head;
    putBe32(head, static_cast<uint32_t>(size));
    head.insert(head.end(), type, type + 4);
    uLong crc = crc32(0L, reinterpret_cast<const Bytef*>(type), 4);
    if (size)
        crc = crc32(crc, data, static_cast<uInt>(size));
    std::vector<uint8_t> tail;
    putBe32(tail, static_cast<uint32_t>(crc));
    return std::fwrite(head.data(), 1, head.size(), file) == head.size() &&
           (size == 0 || std::fwrite(data, 1, size, file) == size) &&
           std::fwrite(tail.data(), 1, tail.size(), file) == tail.size();
}

// rgba is bottom-up (glReadPixels order); PNG wants top-down RGB rows.
bool writePng(const std::string& path, int width, int height,
              const std::vector<uint8_t>& rgba) {
    std::vector<uint8_t> raw;
    raw.reserve(static_cast<size_t>(height) * (3u * width + 1u));
    for (int y = height - 1; y >= 0; --y) {
        raw.push_back(0); // filter: none
        const uint8_t* row = rgba.data() + static_cast<size_t>(y) * width * 4;
        for (int x = 0; x < width; ++x) {
            raw.push_back(row[x * 4 + 0]);
            raw.push_back(row[x * 4 + 1]);
            raw.push_back(row[x * 4 + 2]);
        }
    }
    uLongf compressedSize = compressBound(static_cast<uLong>(raw.size()));
    std::vector<uint8_t> compressed(compressedSize);
    if (compress2(compressed.data(), &compressedSize, raw.data(),
                  static_cast<uLong>(raw.size()), 6) != Z_OK)
        return false;
    compressed.resize(compressedSize);

    FILE* file = std::fopen(path.c_str(), "wb");
    if (!file)
        return false;
    static const uint8_t signature[8] = {0x89, 'P', 'N', 'G',
                                         '\r', '\n', 0x1a, '\n'};
    std::vector<uint8_t> ihdr;
    putBe32(ihdr, static_cast<uint32_t>(width));
    putBe32(ihdr, static_cast<uint32_t>(height));
    ihdr.push_back(8); // bit depth
    ihdr.push_back(2); // color type: truecolor RGB
    ihdr.push_back(0); // compression
    ihdr.push_back(0); // filter
    ihdr.push_back(0); // interlace
    bool ok =
        std::fwrite(signature, 1, sizeof(signature), file) ==
            sizeof(signature) &&
        writeChunk(file, "IHDR", ihdr.data(), ihdr.size()) &&
        writeChunk(file, "IDAT", compressed.data(), compressed.size()) &&
        writeChunk(file, "IEND", nullptr, 0);
    ok = std::fclose(file) == 0 && ok;
    return ok;
}

bool readFramebuffer(GLenum buffer, int width, int height,
                     std::vector<uint8_t>& rgba) {
    while (glGetError() != GL_NO_ERROR) {
    }
    rgba.assign(static_cast<size_t>(width) * height * 4, 0);
    glReadBuffer(buffer);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE,
                 rgba.data());
    glFinish();
    return glGetError() == GL_NO_ERROR;
}

bool hasVisiblePixel(const std::vector<uint8_t>& rgba) {
    for (size_t i = 0; i + 2 < rgba.size(); i += 4) {
        if (rgba[i] != 0 || rgba[i + 1] != 0 || rgba[i + 2] != 0)
            return true;
    }
    return false;
}

/* ---- screen wrapper ---- */

class GoldenActivity : public brls::Activity {
public:
    // withExitAction mirrors MainActivity::onContentAvailable — only the
    // hints-budget screen needs it, and only because an action on the frame
    // shows up in the bottom bar of every screen underneath it. Keep the flags
    // in sync with src/main_switch.cpp or the budget is measured short.
    explicit GoldenActivity(brls::View* content, bool withExitAction = false)
        : withExitAction_(withExitAction) {
        frame_ = new brls::AppletFrame(content);
        frame_->setTitle("pipensx");
    }

    brls::View* createContentView() override {
        return frame_;
    }

    void onContentAvailable() override {
        if (withExitAction_) {
            registerAction(tr("pipensx/app/exit"), brls::BUTTON_START,
                           [](brls::View*) { return true; }, /*hidden=*/true);
            // Visible on-device on every screen (web companion QR) — must be
            // part of the measured budget.
            registerAction(tr("pipensx/app/web_qr"), brls::BUTTON_BACK,
                           [](brls::View*) { return true; });
        }
    }

private:
    brls::AppletFrame* frame_;
    bool withExitAction_;
};

// Depth-first search for the bottom bar's hint row. BottomBar inflates it from
// XML, so there is no accessor to bind to.
brls::Hints* findHints(brls::View* view) {
    if (auto* hints = dynamic_cast<brls::Hints*>(view))
        return hints;
    auto* box = dynamic_cast<brls::Box*>(view);
    if (!box)
        return nullptr;
    for (brls::View* child : box->getChildren()) {
        if (brls::Hints* found = findHints(child))
            return found;
    }
    return nullptr;
}

// Seed the four fixture rows and the planted update-check states the
// installed golden screens pin. Shared by "installed-populated" (screenshot)
// and "installed-bundles" (behaviour): without this the PC shim reports an
// empty library and there is nothing to render or tap. The four rows cover
// every chip state: update available, up to date, no source, error.
void seedInstalledFixture(InstalledTitleService& installed) {
    std::vector<InstalledTitle> fixtureTitles;
    {
        InstalledTitle t;
        t.applicationId = 0x0100000000010000ULL;
        t.titleId = "0100000000010000";
        t.name = "Pipen Odyssey";
        t.publisher = "Pipensx Fixtures";
        t.version = "65536";
        fixtureTitles.push_back(std::move(t));
    }
    {
        InstalledTitle t;
        t.applicationId = 0x0100000000020000ULL;
        t.titleId = "0100000000020000";
        t.name = "Kart Nova Deluxe";
        t.publisher = "Pipensx Fixtures";
        t.version = "65536";
        fixtureTitles.push_back(std::move(t));
    }
    {
        InstalledTitle t;
        t.applicationId = 0x0100000000030000ULL;
        t.titleId = "0100000000030000";
        t.name = "Mystery Homebrew";
        t.publisher = "Solo Dev";
        t.version = "0";
        fixtureTitles.push_back(std::move(t));
    }
    {
        InstalledTitle t;
        t.applicationId = 0x0100000000040000ULL;
        t.titleId = "0100000000040000";
        t.name = "Broken Versions";
        t.publisher = "Pipensx Fixtures";
        t.version = "1.0";
        fixtureTitles.push_back(std::move(t));
    }
    installed.injectTitles(std::move(fixtureTitles));
    // Seed the update-check results: generations 0 vs the live (>0)
    // generation also pins the stale status text deterministically.
    std::ofstream state("sdmc:/switch/pipensx/game-updates.json",
                        std::ios::binary | std::ios::trunc);
    state
        << "{\"version\":1,\"installed_generation\":0,"
           "\"metadata_refresh_ms\":0,\"last_checked_at\":1,"
           "\"results\":["
           "{\"title_id\":\"0100000000010000\",\"state\":"
           "\"update_available\",\"current_version\":\"65536\","
           "\"found_version\":\"131072\",\"error\":\"\","
           "\"checked_at\":1},"
           "{\"title_id\":\"0100000000020000\",\"state\":"
           "\"up_to_date\",\"current_version\":\"65536\","
           "\"found_version\":\"65536\",\"error\":\"\","
           "\"checked_at\":1},"
           "{\"title_id\":\"0100000000030000\",\"state\":"
           "\"source_unknown\",\"current_version\":\"0\","
           "\"found_version\":\"\",\"error\":\"\",\"checked_at\":1},"
           "{\"title_id\":\"0100000000040000\",\"state\":"
           "\"check_error\",\"current_version\":\"1.0\","
           "\"found_version\":\"196608\",\"error\":\"not numeric\","
           "\"checked_at\":1}"
           "]}\n";
}

int fail(const char* message) {
    std::fprintf(stderr, "golden_runner: %s\n", message);
    return 1;
}

} // namespace

int main(int argc, char** argv) {
    std::string fixturesArg;
    std::string outArg;
    std::string sandboxArg;
    std::string theme = "light";
    std::string locale = "en-US";
    std::string screen;
    // 90 was not always enough for a RecyclerFrame to settle: the frame screen
    // captured one of two scroll offsets, roughly one run in three, and the
    // difference is ~57k px — far past the comparison budget. Measured stable
    // over six isolated runs at 200.
    int frames = 200;

    for (int i = 1; i + 1 < argc; i += 2) {
        std::string key = argv[i];
        std::string value = argv[i + 1];
        if (key == "--fixtures")
            fixturesArg = value;
        else if (key == "--out")
            outArg = value;
        else if (key == "--sandbox")
            sandboxArg = value;
        else if (key == "--theme")
            theme = value;
        else if (key == "--locale")
            locale = value;
        else if (key == "--screen")
            screen = value;
        else if (key == "--frames")
            frames = std::atoi(value.c_str());
        else
            return fail("unknown option (see header comment for usage)");
    }
    if (fixturesArg.empty() || outArg.empty() || screen.empty())
        return fail("--fixtures, --out and --screen are required");
    if (theme != "light" && theme != "dark")
        return fail("--theme must be light or dark");
    if (locale != "en-US" && locale != "ru")
        return fail("--locale must be en-US or ru");
    if (frames < 1 || frames > 100000)
        return fail("--frames out of range");

    std::error_code ec;
    const fs::path fixtures = fs::absolute(fixturesArg, ec);
    const fs::path outPng = fs::absolute(outArg, ec);
    fs::create_directories(outPng.parent_path(), ec);
    const fs::path sandbox = sandboxArg.empty()
        ? outPng.parent_path() / ("sandbox-" + screen + "-" + theme)
        : fs::absolute(sandboxArg, ec);

    // Fresh sandbox: the app writes logs/settings under "sdmc:/..." which
    // resolves relative to the CWD on PC ("sdmc:" is a plain directory).
    fs::remove_all(sandbox, ec);
    fs::create_directories(sandbox / "sdmc:" / "switch" / "pipensx", ec);
    if (chdir(sandbox.c_str()) != 0)
        return fail("cannot chdir into sandbox");

    // DesktopPlatform reads BOREALIS_THEME in its constructor.
    setenv("BOREALIS_THEME", theme == "dark" ? "DARK" : "LIGHT", 1);

    log_init(LogPath);
    // Never LOCALE_AUTO: a baseline must not depend on the host's LANG.
    brls::Platform::APP_LOCALE_DEFAULT =
        locale == "ru" ? brls::LOCALE_RU : brls::LOCALE_EN_US;
    if (!brls::Application::init())
        return fail("borealis Application::init failed");
    pipensx::ui::theme::registerColors();
    // Must run before the first Sidebar is inflated, exactly as in main_switch.
    pipensx::ui::installSidebarStyle();
    brls::Application::createWindow("pipensx-golden");
    brls::Application::setGlobalQuit(false);

    // Same wiring order as src/main_switch.cpp, minus network bring-up.
    std::string error;
    AppSettings settings(SettingsPath, TelemetryFlagPath);
    settings.load(error);
    // Keep metadata/mods dailyRefreshDue satisfied so CatalogView does not
    // open a live fetch against the offline golden fixtures. Stamp the wall
    // clock too: the freshness badge treats 0 as "never refreshed".
    {
        pipensx::AppSettingsData values = settings.get();
        const uint64_t now = now_ms();
        values.lastCatalogRefreshMs = now;
        values.lastCatalogRefreshWallSec =
            static_cast<uint64_t>(time(nullptr));
        values.lastMetadataRefreshMs = now;
        values.lastModsRefreshMs = now;
        std::string stampError;
        settings.update(values, stampError);
    }

    CatalogService catalog("sdmc:/switch/pipensx",
                           (fixtures / "catalog.json").string());
    if (!catalog.load(error))
        std::fprintf(stderr, "golden_runner: catalog fixture: %s\n",
                     error.c_str());
    // load() stamps snapshotEpochSec from the fixture file mtime (often old).
    // Re-adopt so auto-refresh sees "today" and stays offline.
    if (!catalog.entries().empty())
        catalog.adopt(
            std::vector<pipensx::CatalogEntry>(catalog.entries()));

    GameMetadataService metadata(
        "sdmc:/switch/pipensx",
        (fixtures / "game_metadata_index.json").string());
    if (!metadata.load(error))
        std::fprintf(stderr, "golden_runner: metadata fixture: %s\n",
                     error.c_str());
    metadata.setImageNetwork(
        GameMetadataService::ImageNetwork::Off); // placeholders, no network

    // Fixture mod index: no network, the table lists the fixture title so the
    // ModCD chip and fact row are covered by the golden screens.
    ModIndexService mods("sdmc:/switch/pipensx",
                         (fixtures / "modcd_table.md").string());
    if (!mods.load(error))
        std::fprintf(stderr, "golden_runner: mod index fixture: %s\n",
                     error.c_str());

    // Seeded with one favourite so the baselines cover the starred card badge,
    // the active ★ chip and the game page's "in wishlist" button, not just the
    // empty state.
    pipensx::FavoritesService favorites("sdmc:/switch/pipensx");
    std::string favoritesError;
    favorites.load(favoritesError);
    if (!catalog.entries().empty()) {
        favorites.toggle(catalog.entries().front().infoHash,
                         catalog.entries().front().title, favoritesError);
    }

    InstalledTitleService installed("sdmc:/switch/pipensx");
    installed.refresh(error); // shim: succeeds with an empty library
    GameUpdateService gameUpdates(&metadata,
                                  "sdmc:/switch/pipensx/game-updates.json");
    {
        std::string loadError;
        gameUpdates.load(loadError);
    }

    DownloadManager manager("sdmc:/switch/pipensx");

    // 512 GB card, 118.24 GB free — statvfs on the sandbox would otherwise
    // report whatever the build machine happens to have.
    pipensx::StorageSpaceSnapshot goldenStorage;
    goldenStorage.totalBytes = 512000000000ULL;
    goldenStorage.freeBytes = 126976000000ULL;
    goldenStorage.available = true;
    pipensx::setStorageSpaceOverride(&goldenStorage);

    brls::Activity* activity = nullptr;
    brls::View* focusAfterLayout = nullptr;
    MainFrame* downloadsBackFrame = nullptr;
    brls::View* downloadsBackSidebarFocus = nullptr;
    bool sidebarTouch = false;
    int torrentSelectionRows = 0;
    bool torrentSelectionScroll = false;
    bool portSelectionOk = true;
    bool settingsDebrid = false;
    bool hintsBudget = false;
    CatalogView* hintsCatalog = nullptr;
    bool catalogHeaderClearance = false;
    CatalogView* collapsedCatalog = nullptr;
    BugReportActivity* bugReportFocus = nullptr;
    FirstRunView* firstRunFocus = nullptr;
    InstalledView* installedBundles = nullptr;
    brls::Activity* detailRailNav = nullptr;
    UpdateFileChooserActivity* updateChooser = nullptr;
    std::vector<uint8_t> updateChooserMask;
    bool disclaimerOkFired = false;
    const std::string setupDiagnosticFixture =
        "cut-off secret body api_key=DO_NOT_SHOW\n"
        "[  13010] [diagnostic] schema=1 level=error stage=net "
        "tag=timeout api_key=DO_NOT_SHOW url=http://user:key@host\n"
        "[  14100] [diagnostic] schema=1 level=snapshot stage=system "
        "tag=setup version=1.0.0\n";
    if (screen == "catalog") {
        activity = new GoldenActivity(new CatalogView(
            &manager, &catalog, &metadata, &installed, &settings, [] {},
            &mods, &favorites));
    } else if (screen == "shelf-scroll") {
        auto* content = new brls::Box(brls::Axis::COLUMN);
        content->setPadding(32, 32, 32, 32);

        auto* heading = new brls::Label();
        heading->setText("Horizontal shelf scroll regression");
        heading->setFontSize(theme::kFontTitle);
        heading->setMarginBottom(16);
        content->addView(heading);

        auto* shelf = new HorizontalShelf(std::make_shared<std::string>());
        shelf->setWidth(900);
        std::vector<GridCardInfo> cards;
        for (int i = 0; i < grid::kShelfItems; ++i) {
            GridCardInfo card;
            card.entryIndex = i;
            card.infoHash = "fixture-" + std::to_string(i);
            card.title = "Shelf card " + std::to_string(i + 1);
            card.sub = "Fixture";
            cards.push_back(std::move(card));
        }
        shelf->setItems(cards, nullptr, [](int) {}, 1);
        content->addView(shelf);

        auto* strip = dynamic_cast<brls::Box*>(shelf->getChildren().front());
        if (!strip || strip->getChildren().size() <= 8)
            return fail("shelf-scroll fixture did not create enough cards");
        focusAfterLayout = strip->getChildren()[8];
        activity = new GoldenActivity(content);
    } else if (screen == "shelf-header") {
        auto* content = new brls::Box(brls::Axis::COLUMN);
        content->setPadding(32, 32, 32, 32);

        auto* focusHolder = new brls::Button();
        focusHolder->setStyle(&brls::BUTTONSTYLE_BORDERLESS);
        focusHolder->setWidth(220);
        focusHolder->setHeight(36);
        focusHolder->setMarginBottom(12);
        focusHolder->setText("Focus holder");
        content->addView(focusHolder);

        auto* cell = new ShelfCell(std::make_shared<std::string>());
        cell->setWidth(900);
        std::vector<GridCardInfo> cards;
        for (int i = 0; i < grid::kShelfItems; ++i) {
            GridCardInfo card;
            card.entryIndex = i;
            card.infoHash = "fixture-" + std::to_string(i);
            card.title = "Shelf card " + std::to_string(i + 1);
            card.sub = "Fixture";
            cards.push_back(std::move(card));
        }
        cell->setShelf("Popular", cards, nullptr, [](int) {}, 1, [] {});
        content->addView(cell);
        activity = new GoldenActivity(content);
    } else if (screen == "detail" || screen == "detail-rail-nav") {
        const auto& entries = catalog.entries();
        if (entries.empty())
            return fail("detail screen needs a non-empty catalog fixture");
        seedInstalledFixture(installed);
        activity = new GameDetailActivity(
            entries.front(), "", &manager, &metadata, &installed, &settings,
            &mods, [](const std::string&, const std::string&) {}, [] {},
            nullptr, &favorites);
        // Behaviour check, not a baseline: the screenshot rail is the only
        // focusable view in the right column, so nothing forces UP out of it.
        // A screenshot cannot see a dead end in the focus graph.
        if (screen == "detail-rail-nav")
            detailRailNav = activity;
    } else if (screen == "screenshot-viewer-missing") {
        // Decode fails (no such file): the viewer must show its labelled plate
        // rather than an empty frame that reads as a crash.
        activity = new ScreenshotViewerActivity(
            &metadata, {(fixtures / "no-such-screenshot.jpg").string()}, 0,
            "Fixture Game");
    } else if (screen == "screenshot-viewer" ||
               screen == "screenshot-viewer-preview") {
        // Absolute paths: GameMetadataService reads those straight off disk,
        // so the viewer decodes real pixels with image networking paused.
        // Two fixtures on purpose — a 1280x720 shot fills the frame at full
        // resolution, a 300x168 one (the shape of the catalogue's fastpic
        // links) is capped at 2x and labelled instead of being stretched.
        const std::string hires = (fixtures / "screenshot-hires.jpg").string();
        const std::string lowres =
            (fixtures / "screenshot-lowres.jpg").string();
        const size_t index = screen == "screenshot-viewer-preview" ? 1 : 0;
        activity = new ScreenshotViewerActivity(
            &metadata, {hires, lowres, hires}, index, "Fixture Game");
    } else if (screen == "update-chooser" ||
               screen == "update-chooser-toggle") {
        // The update-file chooser: a release with two packages carrying the
        // update's [vN] tag. Same-name candidates with a common deep prefix
        // are exactly the case the chooser exists for, so the fixture pins
        // one such pair — the rows must resolve by dimmed directory and
        // right-aligned byte size, not by label length. Both carry the
        // update's version, so the recommendation mask preselected both. The
        // readme between them is not a package: it must never become a row,
        // and toggling the second row must flip mask slot 2, not slot 1.
        pipensx::TorrentPreview preview;
        preview.name = "Pipen Odyssey [Multi]";
        preview.files = {
            {"Repack/Pipen Odyssey [v131072].nsp", 2361441976ULL, true,
             false, false},
            {"Repack/Readme.txt", 4096ULL, false, false, false},
            {"Repack/Mods/Pipen Odyssey [v131072].nsp", 934155878ULL, true,
             false, false},
        };
        preview.fileCount = static_cast<uint32_t>(preview.files.size());
        preview.totalBytes = 2361441976ULL + 4096ULL + 934155878ULL;
        preview.packageCount = 2;
        const uint8_t install = static_cast<uint8_t>(pipensx::FileAction::Install);
        const uint8_t skip = static_cast<uint8_t>(pipensx::FileAction::Skip);
        std::vector<uint8_t> updateActions = {install, skip, install};
        updateChooser = new UpdateFileChooserActivity(
            nullptr, std::move(preview), std::move(updateActions), {},
            [&](std::vector<uint8_t> mask, std::vector<uint8_t>) {
                updateChooserMask = std::move(mask);
            },
            [] {});
        activity = updateChooser;
    } else if (screen == "torrent-selection" ||
               screen == "torrent-selection-scroll") {
        // More files than fit on screen: the recycler only recycles once the
        // list is taller than its viewport, and a short list would hide the
        // whole class of cull/navigation bugs this screen guards.
        pipensx::TorrentPreview preview;
        preview.name = "Mixed release";
        preview.multi = true;
        preview.files = {
            {"game.nsp", 1073741824ULL, true, false, false},
            {"bonus/readme.txt", 1048576ULL, false, false, false},
            {"update.nsz", 805306368ULL, true, true, false},
            {"dlc/dlc-01.nsp", 268435456ULL, true, false, false},
            {"dlc/dlc-02.nsp", 201326592ULL, true, false, false},
            {"dlc/dlc-03.nsz", 134217728ULL, true, true, false},
            {"bonus/artbook.pdf", 52428800ULL, false, false, false},
            {"bonus/soundtrack.zip", 314572800ULL, false, false, false},
            {"bonus/wallpapers/1080p.zip", 20971520ULL, false, false, false},
            {"bonus/wallpapers/4k.zip", 83886080ULL, false, false, false},
            {"extras/cartridge.xci", 402653184ULL, false, false, true},
            {"switch/MyPort/MyPort.nro", 7340032ULL, false, false, false},
            {"patch/patch-01.nsp", 167772160ULL, true, false, false},
            {"patch/patch-02.nsp", 100663296ULL, true, false, false},
        };
        preview.fileCount = static_cast<uint32_t>(preview.files.size());
        for (const auto& file : preview.files) {
            preview.totalBytes += file.length;
            if (file.package)
                ++preview.packageCount;
            if (file.cartridge)
                ++preview.cartridgeCount;
        }
        torrentSelectionRows = static_cast<int>(preview.files.size());
        torrentSelectionScroll = screen == "torrent-selection-scroll";
        if (torrentSelectionScroll) {
            TorrentSelectionDataSource selection(nullptr);
            std::vector<TorrentSelectionEntry> entries;
            entries.reserve(preview.files.size());
            for (const auto& file : preview.files) {
                entries.push_back({file.path, file.length, file.package,
                                   file.compressed, file.cartridge,
                                   file.package ? pipensx::FileAction::Install
                                                : pipensx::FileAction::Download});
            }
            selection.setEntries(std::move(entries));
            selection.selectPortFiles(preview, preview.name + "/switch");
            const std::vector<uint8_t> actions = selection.fileActions();
            for (size_t i = 0; i < actions.size(); ++i) {
                const auto expected = i == 11 ? pipensx::FileAction::Download
                                              : pipensx::FileAction::Skip;
                if (actions[i] != static_cast<uint8_t>(expected))
                    portSelectionOk = false;
            }
        }
        // PackagesOnly rather than the settings default, so the baseline shows
        // all three row states: packages Install, everything else Skip, and
        // a Download row appears as soon as anything is toggled.
        activity = new TorrentSelectionActivity(
            &manager, "sdmc:/switch/pipensx/_golden_selection.torrent",
            std::move(preview), pipensx::TransferMode::StreamInstall,
            pipensx::StreamSelection::PackagesOnly);
    } else if (screen == "downloads") {
        activity = new GoldenActivity(
            new MainView(&manager, &metadata, &settings));
    } else if (screen == "download-files") {
        pipensx::TaskFileInventory inventory;
        inventory.taskId = "golden-port";
        inventory.rootPath = "SD:/switch/pipensx/downloads/Port-release";
        inventory.settled = true;
        inventory.presentBytes = 1288490188ULL;
        auto add = [&](const std::string& path, uint64_t size,
                       pipensx::TaskFileState state) {
            pipensx::TaskFileInfo file;
            file.logicalPath = path;
            file.localPath = path;
            file.size = size;
            file.state = state;
            inventory.files.push_back(std::move(file));
        };
        add("Release/switch/MyPort/MyPort.nro", 7340032,
            pipensx::TaskFileState::Present);
        add("Release/switch/MyPort/data/game.pak", 1280000000ULL,
            pipensx::TaskFileState::Present);
        add("Release/readme.txt", 4096,
            pipensx::TaskFileState::Skipped);
        add("Release/base.nsp", 4294967296ULL,
            pipensx::TaskFileState::Installed);
        activity = new TaskFilesActivity(std::move(inventory));
    } else if (screen == "deploy-preview") {
        pipensx::SwitchDeployInspection inspection;
        inspection.problem = pipensx::SwitchDeployProblem::Conflict;
        inspection.detail = "/switch/MyPort/config.ini";
        inspection.plan.taskId = "golden-port";
        inspection.plan.bytesToCopy = 1280000000ULL;
        inspection.plan.identicalFiles = 1;
        inspection.plan.ignoredFiles = 2;
        auto add = [&](const std::string& source,
                       const std::string& destination, uint64_t size,
                       pipensx::SwitchDeployEntryState state) {
            pipensx::SwitchDeployEntry entry;
            entry.sourceRelativePath = source;
            entry.destinationRelativePath = destination;
            entry.size = size;
            entry.state = state;
            inspection.plan.files.push_back(std::move(entry));
        };
        add("Release/switch/MyPort/data/game.pak", "MyPort/data/game.pak",
            1280000000ULL, pipensx::SwitchDeployEntryState::Missing);
        add("Release/switch/MyPort/MyPort.nro", "MyPort/MyPort.nro", 7340032,
            pipensx::SwitchDeployEntryState::ExistingIdentical);
        add("Release/switch/MyPort/config.ini", "MyPort/config.ini", 2048,
            pipensx::SwitchDeployEntryState::ExistingConflict);
        activity = new SwitchDeployPreviewActivity(std::move(inspection),
                                                   nullptr);
    } else if (screen == "downloads-back") {
        downloadsBackFrame = new MainFrame();
        auto* downloadsView = new MainView(&manager, &metadata, &settings);
        downloadsBackFrame->addNavTab(
            tr("pipensx/nav/downloads"), NavIconType::Downloads,
            [downloadsView] { return downloadsView; });
        activity = new GoldenActivity(downloadsBackFrame);
    } else if (screen == "downloads-removing") {
        pipensx::DownloadTask removing;
        removing.id = "removing-fixture";
        removing.name = "Large package cleanup";
        removing.status = pipensx::DownloadStatus::Removing;

        DownloadDataSource source(nullptr);
        source.setTasks({removing});
        const pipensx::DownloadTask* row =
            source.taskAt(brls::IndexPath(0, 0));
        if (source.numberOfSections(nullptr) != 1 ||
            source.numberOfRows(nullptr, 0) != 1 || !row ||
            row->status != pipensx::DownloadStatus::Removing)
            return fail("downloads-removing row is not visible");
        std::printf("golden_runner: downloads-removing row visible\n");
        manager.shutdown();
        std::fflush(nullptr);
        _exit(0);
    } else if (screen == "frame") {
        // Whole shell, same wiring as src/main_switch.cpp: covers the sidebar
        // and the storage footer docked at its bottom.
        auto* tabs = new MainFrame();
        tabs->addNavTab(tr("pipensx/nav/catalog"), NavIconType::Catalog, [&] {
            return new CatalogView(&manager, &catalog, &metadata, &installed,
                                   &settings, [] {}, &mods, &favorites);
        });
        tabs->addNavTab(tr("pipensx/nav/downloads"), NavIconType::Downloads,
                        [&] {
            return new MainView(&manager, &metadata, &settings);
        });
        tabs->addNavTab(tr("pipensx/nav/installed"), NavIconType::Installed,
                        [&] {
            // checkOnEntry=false: the installed-populated screens pin a
            // planted fixture state, an auto-check would overwrite it.
            return new InstalledView(&installed, &manager, &metadata,
                                     &settings, &catalog, &gameUpdates, false);
        });
        tabs->addNavTab(tr("pipensx/nav/updates"), NavIconType::Updates,
                        [&] {
            return new InstalledView(&installed, &manager, &metadata,
                                     &settings, &catalog, &gameUpdates, false,
                                     InstalledView::Mode::Updates);
        });
        tabs->addNavTab(tr("pipensx/nav/settings"), NavIconType::Settings,
                        [&] {
            return new SettingsView(&settings, &manager, &catalog, &metadata,
                                    &installed, nullptr, &mods);
        });
        tabs->addNavTab(tr("pipensx/nav/help"), NavIconType::Help, [&] {
            return new HelpView(&manager, &catalog, &metadata, &installed);
        });
        tabs->addNavTab(tr("pipensx/nav/about"), NavIconType::About,
                        [] { return new AboutView(); });
        tabs->attachStorageFooter(&manager);
        activity = new GoldenActivity(tabs);
    } else if (screen == "sidebar-touch") {
        // Behaviour check, not a baseline: the storage dock is pinned over the
        // whole sidebar column, and a plain Box there answers the hit test
        // itself — which silently made every tab unreachable by finger while
        // the gamepad kept working. Screenshots cannot see that.
        sidebarTouch = true;
        auto* tabs = new MainFrame();
        tabs->addNavTab(tr("pipensx/nav/catalog"), NavIconType::Catalog, [&] {
            return new CatalogView(&manager, &catalog, &metadata, &installed,
                                   &settings, [] {}, &mods, &favorites);
        });
        tabs->addNavTab(tr("pipensx/nav/downloads"), NavIconType::Downloads,
                        [&] {
            return new MainView(&manager, &metadata, &settings);
        });
        tabs->attachStorageFooter(&manager);
        activity = new GoldenActivity(tabs);
    } else if (screen == "catalog-header-clearance") {
        // Regression check: the catalog's first shelf must remain below the
        // filter row after focus folds the sidebar to its icon rail.
        catalogHeaderClearance = true;
        auto* tabs = new MainFrame();
        tabs->addNavTab(tr("pipensx/nav/catalog"), NavIconType::Catalog, [&] {
            collapsedCatalog = new CatalogView(
                &manager, &catalog, &metadata, &installed, &settings, [] {},
                &mods, &favorites);
            return collapsedCatalog;
        });
        activity = new GoldenActivity(tabs);
    } else if (screen == "hints-budget") {
        // Behaviour check, not a baseline: the catalog registers more gamepad
        // actions than the bottom bar can lay out, and the hints silently
        // squash into each other when they overrun. Build the production shell
        // with the catalog grid focused, then measure the row.
        hintsBudget = true;
        auto* tabs = new MainFrame();
        // Tab views are built lazily, on the first draw — hintsCatalog is only
        // readable from inside the frame loop below.
        tabs->addNavTab(tr("pipensx/nav/catalog"), NavIconType::Catalog, [&] {
            hintsCatalog = new CatalogView(&manager, &catalog, &metadata,
                                           &installed, &settings, [] {}, &mods,
                                           &favorites);
            return hintsCatalog;
        });
        tabs->addNavTab(tr("pipensx/nav/downloads"), NavIconType::Downloads,
                        [&] {
            return new MainView(&manager, &metadata, &settings);
        });
        tabs->attachStorageFooter(&manager);
        activity = new GoldenActivity(tabs, /*withExitAction=*/true);
    } else if (screen == "installed" || screen == "installed-populated" ||
               screen == "updates" ||
               screen == "installed-bundles") {
        if (screen == "installed-populated" || screen == "updates" ||
            screen == "installed-bundles") {
            seedInstalledFixture(installed);
            std::string loadError;
            gameUpdates.load(loadError);
        }
        auto* view = new InstalledView(&installed, &manager, &metadata,
                                       &settings, &catalog, &gameUpdates, false,
                                       screen == "updates"
                                           ? InstalledView::Mode::Updates
                                           : InstalledView::Mode::Library);
        activity = new GoldenActivity(view);
        if (screen == "installed-bundles")
            installedBundles = view;
    } else if (screen == "settings") {
        activity = new GoldenActivity(new SettingsView(
            &settings, &manager, &catalog, &metadata, &installed, nullptr,
            &mods));
    } else if (screen == "settings-debrid") {
        // The debrid section is well below the fold on the settings screen,
        // so it needs a shot of its own. A key is planted first: "linked" is
        // the state worth pinning, since it is the one the detail text
        // composes from the provider name.
        pipensx::AppSettingsData values = settings.get();
        values.torboxApiKey = "golden-fixture-key";
        values.debridProvider = pipensx::DebridProviderKind::TorBox;
        if (!settings.update(values, error))
            return fail("settings-debrid could not plant a linked key");
        activity = new GoldenActivity(new SettingsView(
            &settings, &manager, &catalog, &metadata, &installed, nullptr,
            &mods));
        settingsDebrid = true;
    } else if (screen == "network-health") {
        pipensx::AppSettingsData values = settings.get();
        values.torboxApiKey = "golden-fixture-key";
        values.torrserverUrl = "192.168.1.5:8090";
        values.proxyUrl = "";
        values.lastCatalogRefreshWallSec =
            static_cast<uint64_t>(time(nullptr)) - 3 * 3600;
        if (!settings.update(values, error))
            return fail("network-health could not plant provider settings");
        activity = new NetworkHealthActivity(&manager, &settings,
                                             "192.168.1.5");
    } else if (screen == "help") {
        activity = new GoldenActivity(
            new HelpView(&manager, &catalog, &metadata, &installed));
    } else if (screen == "first-run" || screen == "first-run-focus") {
        auto* view = new FirstRunView(&settings, &manager,
                                      [](pipensx::DebridProviderKind, bool) {});
        activity = new GoldenActivity(view);
        if (screen == "first-run-focus")
            firstRunFocus = view;
    } else if (screen == "first-run-disclaimer") {
        // Host for the disclaimer dialog; the dialog is opened right after
        // the activity is pushed below.
        activity = new GoldenActivity(new brls::Box());
    } else if (screen == "debrid-link") {
        pipensx::AppSettingsData values = settings.get();
        values.debridProvider = pipensx::DebridProviderKind::TorBox;
        values.torboxApiKey = "golden-fixture-key";
        if (!settings.update(values, error))
            return fail("debrid-link could not plant a linked key");
        activity = new GoldenActivity(new DebridLinkView(
            &settings, &manager, pipensx::DebridProviderKind::TorBox,
            DebridLinkFixture{{"192.168.50.42", setupDiagnosticFixture},
                              /*pairingAvailable=*/true,
                              /*validationSucceeded=*/true}));
    } else if (screen == "about") {
        activity = new GoldenActivity(new AboutView());
    } else if (screen == "bug-report" || screen == "bug-report-detail" ||
               screen == "bug-report-focus") {
        // Fixed log and device state: the live path snapshots
        // statvfs/firmware/clock, none of which are deterministic.
        // BugReportActivity is its own AppletFrame, so it is pushed directly
        // rather than wrapped. The fixture carries the per-image telemetry
        // chatter of a real session, so the encoder's "drop the noise before
        // cutting history" step is part of what the baseline pins down.
        std::string fixtureLog =
            "[  12345] [startup] boot\n"
            "[  12800] [meta] name='Fixture Title' hash=0011223344\n"
            "[  13010] [diagnostic] schema=1 level=error stage=net tag=timeout "
            "peer=10.0.0.5 msg=connection_reset\n";
        for (int i = 0; i < 40; ++i) {
            char line[160];
            std::snprintf(line, sizeof(line),
                          "[  %5d] [telemetry] schema=1 stage=image tag=- "
                          "event=load cache=source ok=1 duration_ms=%d "
                          "bytes=465124\n",
                          13200 + i * 7, 60 + i);
            fixtureLog += line;
        }
        fixtureLog +=
            "[  14100] [diagnostic] schema=1 stage=system tag=report "
            "version=1.0.0 hos=18.1.0 operation_mode=1 telemetry=0 catalog=42 "
            "installed=3 active=1 errors=1 sd_free_bytes=126976000000\n";
        pipensx::ui::BugReportFixture fixture{
            std::move(fixtureLog),
            pipensx::ui::SystemSnapshot{/*hos=*/0x12010000, /*mode=*/1,
                                        /*telemetry=*/false, /*catalog=*/42,
                                        /*metadata=*/38, /*installed=*/3,
                                        /*active=*/1, /*errors=*/1,
                                        /*freeBytes=*/126976000000ull}};
        auto* report = new BugReportActivity(&manager, &catalog, &metadata,
                                             &installed, std::move(fixture),
                                             screen == "bug-report-detail");
        activity = report;
        if (screen == "bug-report-focus")
            bugReportFocus = report;
    } else {
        return fail("unknown --screen");
    }

    brls::Application::pushActivity(activity);
    if (screen == "first-run-disclaimer")
        pipensx::ui::showCatalogDisclaimer(
            &settings, [&disclaimerOkFired] { disclaimerOkFired = true; });
    for (int i = 0; i < frames; ++i) {
        if (i == 10 && focusAfterLayout)
            brls::Application::giveFocus(focusAfterLayout);
        if (i == 10 && downloadsBackFrame)
            downloadsBackFrame->focusTab(0);
        // Focus has to sit in the grid, not the sidebar: hints are collected by
        // walking up from the focused view, and the sidebar sees none of the
        // catalog's actions.
        if (i == 10 && hintsCatalog)
            brls::Application::giveFocus(hintsCatalog);
        if (i == 10 && collapsedCatalog) {
            brls::View* focus = brls::Application::getCurrentFocus();
            brls::View* next = focus
                ? focus->getNextFocus(brls::FocusDirection::RIGHT, focus)
                : nullptr;
            if (!next)
                return fail("catalog-header-clearance: RIGHT never entered catalog");
            brls::Application::giveFocus(next);
        }
        // The settings scroller is NATURAL: it follows the d-pad, not focus,
        // and getNextFocus() cannot reach a row that is still off-screen. So
        // scroll it directly to the cell's own offset — that is independent
        // of how many rows sit above the section, unlike counting presses.
        // The headroom puts the section heading and its two other rows in
        // frame rather than the link cell alone.
        if (i == 30 && settingsDebrid) {
            brls::View* link =
                activity->getContentView()->getView("settings-debrid-link");
            if (!link)
                return fail("settings-debrid: link cell not found");
            brls::ScrollingFrame* scroller = nullptr;
            for (brls::View* node = reinterpret_cast<brls::View*>(
                     link->getParent());
                 node && !scroller;
                 node = reinterpret_cast<brls::View*>(node->getParent()))
                scroller = dynamic_cast<brls::ScrollingFrame*>(node);
            if (!scroller)
                return fail("settings-debrid: link cell is not in a scroller");
            constexpr float kHeadroom = 240.0f;
            float offset = link->getLocalY() - kHeadroom;
            scroller->setContentOffsetY(offset < 0 ? 0 : offset, false);
            brls::Application::giveFocus(link);
        }
        if (i == 20 && downloadsBackFrame) {
            downloadsBackSidebarFocus = brls::Application::getCurrentFocus();
            auto values = settings.get();
            if (!settings.update(values, error))
                return fail("downloads-back could not trigger refresh");
            usleep(800000);
        }
        if (!brls::Application::mainLoop())
            return fail("main loop ended before capture");
    }
    if (sidebarTouch) {
        // Focus starts on the first sidebar item, so it doubles as the tap
        // target: a finger on its centre has to reach the item that owns the
        // TapGestureRecognizer, not the readout docked on top of it.
        brls::View* item = brls::Application::getCurrentFocus();
        if (!item)
            return fail("sidebar-touch: nothing focused in the sidebar");
        brls::Rect box = item->getFrame();
        brls::View* hit = activity->getContentView()->hitTest(
            brls::Point(box.getMinX() + box.getWidth() / 2,
                        box.getMinY() + box.getHeight() / 2));
        bool reachesItem = false;
        for (brls::View* node = hit; node;
             node = reinterpret_cast<brls::View*>(node->getParent())) {
            if (node == item) {
                reachesItem = true;
                break;
            }
        }
        if (!reachesItem) {
            std::fprintf(stderr, "golden_runner: sidebar tap landed on %s\n",
                         hit ? hit->describe().c_str() : "nothing");
            return fail("a tap on a sidebar item never reaches it");
        }
        std::printf("golden_runner: sidebar item reachable by touch\n");
        manager.shutdown();
        std::fflush(nullptr);
        _exit(0);
    }

    if (firstRunFocus) {
        auto* option = dynamic_cast<FirstRunOption*>(
            brls::Application::getCurrentFocus());
        if (!option)
            return fail("first-run-focus did not start on an option");
        // B is locked until a method is picked: a real B press must be
        // consumed by the view's hidden action, never dismiss the chooser.
        if (!firstRunFocus->backLocked())
            return fail("first-run-focus does not lock B before a choice");
        // The frame's Back action must be replaced with a hidden no-op, or
        // the hint bar would advertise a Back button the lock makes useless.
        bool frameBackHidden = false;
        for (brls::View* node = option->getParent(); node;
             node = node->getParent()) {
            if (auto* frame = dynamic_cast<brls::AppletFrame*>(node)) {
                for (const auto& action : frame->getActions())
                    if (action->getType() == brls::ACTION_GAMEPAD &&
                        action->getButton() == brls::BUTTON_B)
                        frameBackHidden = action->isHidden();
                break;
            }
        }
        if (!frameBackHidden)
            return fail("first-run-focus still shows a Back hint");
        brls::Activity* top =
            brls::Application::getActivitiesStack().back();
        const std::string beforeB = firstRunFocus->summaryState();
        brls::Application::onControllerButtonPressed(brls::BUTTON_B, false);
        for (int frame = 0; frame < 5; ++frame)
            brls::Application::mainLoop();
        const auto& stack = brls::Application::getActivitiesStack();
        if (stack.empty() || stack.back() != top ||
            firstRunFocus->summaryState() != beforeB ||
            brls::Application::getCurrentFocus() != option)
            return fail("first-run-focus B dismissed the chooser");
        // Every mode's diagram must fit the panel. TorrServer is the widest
        // (three fixed-width chips, two arrows), Direct the narrowest — the
        // old end-of-loop check only ever looked at Direct, which is exactly
        // the mode that hides the server chip and always fits.
        auto checkFits = [&]() {
            if (firstRunFocus->summaryFits())
                return 0;
            std::fprintf(stderr, "golden_runner: %s\n",
                         firstRunFocus->summaryOverflow().c_str());
            return fail("first-run-focus summary clips on a mode");
        };
        std::vector<std::string> states{firstRunFocus->summaryState()};
        if (int rc = checkFits(); rc)
            return rc;
        for (int i = 0; i < 2; ++i) {
            brls::View* next = option->getNextFocus(brls::FocusDirection::DOWN,
                                                    option);
            option = dynamic_cast<FirstRunOption*>(next);
            if (!option)
                return fail("first-run-focus could not reach every option");
            brls::Application::giveFocus(option);
            for (int frame = 0; frame < 5; ++frame)
                brls::Application::mainLoop();
            states.push_back(firstRunFocus->summaryState());
            if (int rc = checkFits(); rc)
                return rc;
        }
        if (states[0] == states[1] || states[1] == states[2] ||
            states[0] == states[2] ||
            brls::Application::getCurrentFocus() != option)
            return fail("first-run-focus did not update the summary in place");
        std::printf("golden_runner: first-run summary followed all options\n");
        manager.shutdown();
        std::fflush(nullptr);
        _exit(0);
    }

    if (installedBundles) {
        // The update-available row (Pipen Odyssey, two fixture bundles) opens
        // the release-bundle chooser on A. The old "name  vN" button label
        // overflowed into its neighbour on long titles, so the candidate is
        // now labelled by version alone — this pins that the two buttons sit
        // side by side without overlap, that paging reaches the second
        // bundle, and that the last page ends in "later", not "more".
        auto pump = [](int frames) {
            for (int frame = 0; frame < frames; ++frame)
                brls::Application::mainLoop();
        };
        brls::View* cell = nullptr;
        std::function<void(brls::View*)> findCell = [&](brls::View* node) {
            if (cell)
                return;
            if (dynamic_cast<pipensx::ui::InstalledCell*>(node))
                cell = node;
            if (auto* box = dynamic_cast<brls::Box*>(node))
                for (brls::View* child : box->getChildren())
                    findCell(child);
        };
        findCell(installedBundles);
        if (!cell)
            return fail("installed-bundles found no installed row");
        brls::Application::giveFocus(cell);
        pump(5);
        brls::Action* installAction = nullptr;
        for (const auto& action : cell->getActions())
            if (action->getType() == brls::ACTION_GAMEPAD &&
                action->getButton() == brls::BUTTON_A)
                installAction = action.get();
        if (!installAction)
            return fail("installed-bundles row has no A action");
        installAction->getActionListener()(cell);
        brls::Activity* host = activity;
        brls::Activity* page1 = nullptr;
        for (int frame = 0; frame < 180 && !page1; ++frame) {
            brls::Application::mainLoop();
            if (brls::Application::getActivitiesStack().back() != host)
                page1 = brls::Application::getActivitiesStack().back();
        }
        if (!page1)
            return fail("installed-bundles A never opened the chooser");

        brls::Button* candidate = nullptr;
        brls::Button* more = nullptr;
        brls::Button* later = nullptr;
        const std::string moreLabel = tr("pipensx/installed/update_choose_more", 1);
        const std::string laterLabel = tr("pipensx/common/later");
        std::function<void(brls::View*)> findButtons = [&](brls::View* node) {
            if (auto* button = dynamic_cast<brls::Button*>(node)) {
                if (button->getText() == "v196608" ||
                    button->getText() == "v131072")
                    candidate = button;
                else if (button->getText() == moreLabel)
                    more = button;
                else if (button->getText() == laterLabel)
                    later = button;
            }
            if (auto* box = dynamic_cast<brls::Box*>(node))
                for (brls::View* child : box->getChildren())
                    findButtons(child);
        };
        findButtons(page1->getContentView());
        if (!candidate || candidate->getText() != "v196608")
            return fail("installed-bundles page 1 shows the wrong candidate");
        if (!more)
            return fail("installed-bundles page 1 is missing 'more'");
        if (later)
            return fail("installed-bundles page 1 must not show 'later'");
        // The old bug overflowed the label *text* past its own button into
        // the neighbour — the button boxes themselves never moved, so the
        // overlap must be measured on the button's label child, not on the
        // button frames.
        auto labelFits = [](brls::Button* button) {
            brls::Label* label = nullptr;
            std::function<void(brls::View*)> findLabel = [&](brls::View* node) {
                if (label)
                    return;
                if (dynamic_cast<brls::Label*>(node))
                    label = dynamic_cast<brls::Label*>(node);
                if (auto* box = dynamic_cast<brls::Box*>(node))
                    for (brls::View* child : box->getChildren())
                        findLabel(child);
            };
            findLabel(button);
            if (!label)
                return false;
            const brls::Rect text = label->getFrame();
            const brls::Rect box = button->getFrame();
            return text.getMinX() >= box.getMinX() - 0.5f &&
                   text.getMaxX() <= box.getMaxX() + 0.5f &&
                   text.getMinY() >= box.getMinY() - 0.5f &&
                   text.getMaxY() <= box.getMaxY() + 0.5f;
        };
        if (!labelFits(candidate) || !labelFits(more))
            return fail("installed-bundles a button label overflows its box");
        const brls::Rect candidateFrame = candidate->getFrame();
        const brls::Rect moreFrame = more->getFrame();
        if (candidateFrame.getMaxX() > moreFrame.getMinX() + 0.5f)
            return fail("installed-bundles candidate overlaps the 'more' "
                        "button");

        // "More" pages to the second bundle: "v131072" plus "later".
        brls::Action* moreAction = nullptr;
        for (const auto& action : more->getActions())
            if (action->getType() == brls::ACTION_GAMEPAD &&
                action->getButton() == brls::BUTTON_A)
                moreAction = action.get();
        if (!moreAction)
            return fail("installed-bundles 'more' has no A action");
        moreAction->getActionListener()(more);
        brls::Activity* page2 = nullptr;
        for (int frame = 0; frame < 180 && !page2; ++frame) {
            brls::Application::mainLoop();
            if (brls::Application::getActivitiesStack().back() != page1)
                page2 = brls::Application::getActivitiesStack().back();
        }
        if (!page2)
            return fail("installed-bundles 'more' never opened page 2");
        candidate = nullptr;
        more = nullptr;
        later = nullptr;
        findButtons(page2->getContentView());
        if (!candidate || candidate->getText() != "v131072")
            return fail("installed-bundles page 2 shows the wrong candidate");
        if (!later)
            return fail("installed-bundles last page must show 'later'");
        if (more)
            return fail("installed-bundles last page must not show 'more'");

        brls::Action* laterAction = nullptr;
        for (const auto& action : later->getActions())
            if (action->getType() == brls::ACTION_GAMEPAD &&
                action->getButton() == brls::BUTTON_A)
                laterAction = action.get();
        if (!laterAction)
            return fail("installed-bundles 'later' has no A action");
        laterAction->getActionListener()(later);
        for (int frame = 0; frame < 180; ++frame) {
            brls::Application::mainLoop();
            if (brls::Application::getActivitiesStack().back() == activity)
                break;
        }
        if (brls::Application::getActivitiesStack().back() != activity)
            return fail("installed-bundles 'later' never closed the dialog");
        std::printf("golden_runner: bundle chooser pages and fits the dialog\n");
        manager.shutdown();
        std::fflush(nullptr);
        _exit(0);
    }

    if (updateChooser && screen == "update-chooser-toggle") {
        // The multi-select update chooser: rows open at the recommendation
        // mask (packages only — the readme between them is not a row), A
        // toggles a row, Continue returns the final mask — and with
        // everything toggled off Continue must refuse to proceed. The mask
        // stays parallel to preview.files, so toggling the second row flips
        // slot 2, never the readme's slot 1.
        if (!updateChooserMask.empty())
            return fail("update-chooser-toggle onPick fired before Continue");
        const auto& selection = updateChooser->selection();
        const uint8_t install =
            static_cast<uint8_t>(pipensx::FileAction::Install);
        const uint8_t skip = static_cast<uint8_t>(pipensx::FileAction::Skip);
        const auto wantMask = [&](uint8_t a, uint8_t b) {
            return selection.size() == 3 && selection[0] == a &&
                   selection[1] == skip && selection[2] == b;
        };
        if (!wantMask(install, install))
            return fail("update-chooser-toggle ignored the recommendation mask");

        brls::View* cell =
            brls::Application::getCurrentFocus();
        if (!dynamic_cast<TorrentSelectionCell*>(cell))
            return fail("update-chooser-toggle did not focus a row");

        // Toggle by package-row index — not by pressing A on a cell pointer.
        // UpdateFileChooserActivity::toggle calls reloadData(), which recycles
        // cells; a TorrentSelectionCell* taken before a toggle can land on the
        // wrong index path afterwards (observed as flipping mask slot 0 twice).
        updateChooser->toggleRowForTest(0);
        for (int frame = 0; frame < 5; ++frame)
            brls::Application::mainLoop();
        if (!wantMask(skip, install))
            return fail("update-chooser-toggle did not flip the first row");
        updateChooser->toggleRowForTest(1);
        for (int frame = 0; frame < 5; ++frame)
            brls::Application::mainLoop();
        // The readme occupies mask slot 1; the second row is slot 2. If the
        // row-to-index mapping was off by one, this assertion fails.
        if (!wantMask(skip, skip))
            return fail("update-chooser-toggle flipped the wrong mask slot");

        // Everything off: Continue must refuse to confirm and return a mask.
        brls::Button* confirm = nullptr;
        std::function<void(brls::View*)> findConfirm =
            [&](brls::View* node) {
                if (confirm)
                    return;
                if (auto* button = dynamic_cast<brls::Button*>(node))
                    if (button->getText() == tr("pipensx/common/continue"))
                        confirm = button;
                if (auto* box = dynamic_cast<brls::Box*>(node))
                    for (brls::View* child : box->getChildren())
                        findConfirm(child);
            };
        findConfirm(updateChooser->getContentView());
        if (!confirm)
            return fail("update-chooser-toggle has no Continue button");
        brls::Action* continueAction = nullptr;
        for (const auto& action : confirm->getActions())
            if (action->getType() == brls::ACTION_GAMEPAD &&
                action->getButton() == brls::BUTTON_A)
                continueAction = action.get();
        if (!continueAction)
            return fail("update-chooser-toggle Continue has no A action");
        continueAction->getActionListener()(confirm);
        for (int frame = 0; frame < 5; ++frame)
            brls::Application::mainLoop();
        if (!updateChooserMask.empty())
            return fail("update-chooser-toggle confirmed with nothing selected");

        // One row back on: Continue confirms and hands the full mask back.
        updateChooser->toggleRowForTest(0);
        for (int frame = 0; frame < 5; ++frame)
            brls::Application::mainLoop();
        if (!wantMask(install, skip))
            return fail("update-chooser-toggle re-selected the wrong row");
        // Footer CTA is the install label once a row is selected — re-find
        // the live primary button (anything that is not Cancel).
        confirm = nullptr;
        std::function<void(brls::View*)> findPrimary =
            [&](brls::View* node) {
                if (confirm)
                    return;
                if (auto* button = dynamic_cast<brls::Button*>(node)) {
                    const std::string& label = button->getText();
                    if (label != tr("pipensx/common/cancel") && !label.empty())
                        confirm = button;
                }
                if (auto* box = dynamic_cast<brls::Box*>(node))
                    for (brls::View* child : box->getChildren())
                        findPrimary(child);
            };
        findPrimary(updateChooser->getContentView());
        if (!confirm)
            return fail("update-chooser-toggle lost Continue after re-select");
        continueAction = nullptr;
        for (const auto& action : confirm->getActions())
            if (action->getType() == brls::ACTION_GAMEPAD &&
                action->getButton() == brls::BUTTON_A)
                continueAction = action.get();
        if (!continueAction)
            return fail("update-chooser-toggle Continue has no A action");
        continueAction->getActionListener()(confirm);
        for (int frame = 0; frame < 5; ++frame)
            brls::Application::mainLoop();
        if (updateChooserMask.size() != 3 ||
            updateChooserMask[0] != install ||
            updateChooserMask[1] != skip ||
            updateChooserMask[2] != skip)
            return fail("update-chooser-toggle returned the wrong mask");
        std::printf("golden_runner: update chooser toggled and confirmed "
                    "(mask=%u,%u,%u)\n", updateChooserMask[0],
                    updateChooserMask[1], updateChooserMask[2]);
        manager.shutdown();
        std::fflush(nullptr);
        _exit(0);
    }

    if (screen == "first-run-disclaimer") {
        // The catalog disclaimer is non-cancelable: B must not dismiss it
        // (and must not acknowledge it or continue the chain), and only OK
        // persists the flag and fires the continuation that opens the
        // provider link screen on a fresh first run.
        if (settings.get().catalogDisclaimerAcknowledged)
            return fail("first-run-disclaimer started with the flag set");
        auto stack = brls::Application::getActivitiesStack();
        if (stack.size() < 2)
            return fail("first-run-disclaimer never opened over the host");
        brls::Activity* dialogActivity = stack.back();
        brls::Application::onControllerButtonPressed(brls::BUTTON_B, false);
        for (int frame = 0; frame < 5; ++frame)
            brls::Application::mainLoop();
        if (brls::Application::getActivitiesStack().back() != dialogActivity)
            return fail("first-run-disclaimer B dismissed the dialog");
        if (settings.get().catalogDisclaimerAcknowledged)
            return fail("first-run-disclaimer B acknowledged the dialog");
        if (disclaimerOkFired)
            return fail("first-run-disclaimer B continued the chain");

        // Press OK: the flag persists and the continuation fires.
        brls::Button* ok = nullptr;
        std::function<void(brls::View*)> findOk = [&](brls::View* node) {
            if (ok)
                return;
            if (auto* button = dynamic_cast<brls::Button*>(node))
                if (button->getText() == tr("pipensx/common/ok"))
                    ok = button;
            if (auto* box = dynamic_cast<brls::Box*>(node))
                for (brls::View* child : box->getChildren())
                    findOk(child);
        };
        findOk(dialogActivity->getContentView());
        if (!ok)
            return fail("first-run-disclaimer has no OK button");
        brls::Action* okAction = nullptr;
        for (const auto& action : ok->getActions())
            if (action->getType() == brls::ACTION_GAMEPAD &&
                action->getButton() == brls::BUTTON_A)
                okAction = action.get();
        if (!okAction)
            return fail("first-run-disclaimer OK has no A action");
        okAction->getActionListener()(ok);
        // buttonClick runs the callback only after the dismiss animation
        // completes, so pump until the dialog is gone or we give up.
        for (int frame = 0; frame < 180; ++frame) {
            brls::Application::mainLoop();
            if (brls::Application::getActivitiesStack().back() == activity)
                break;
        }
        if (brls::Application::getActivitiesStack().back() != activity)
            return fail("first-run-disclaimer OK never dismissed the dialog");
        if (!settings.get().catalogDisclaimerAcknowledged)
            return fail("first-run-disclaimer OK did not persist the flag");
        if (!disclaimerOkFired)
            return fail("first-run-disclaimer OK did not continue the chain");
        std::printf("golden_runner: disclaimer blocks B and continues on OK\n");
        manager.shutdown();
        std::fflush(nullptr);
        _exit(0);
    }

    if (downloadsBackFrame &&
        brls::Application::getCurrentFocus() != downloadsBackSidebarFocus)
        return fail("downloads refresh stole focus from the sidebar");
    if (downloadsBackFrame) {
        std::printf("golden_runner: downloads-back focus preserved\n");
        manager.shutdown();
        std::fflush(nullptr);
        _exit(0);
    }

    if (detailRailNav) {
        // The right column holds the fact table, the rail and the description,
        // but only the rail is focusable — the other two are plain Labels. So
        // the upward walk finds nothing in the column, then meets content's ROW
        // axis (which ignores UP) and dies at the frame, and the rail becomes a
        // one-way trip: the shots are reachable but nothing leads back out
        // upwards. Assert the rail routes UP to the primary action.
        brls::View* root = detailRailNav->getContentView();
        // The cover in the left column is an AsyncRgbaImage too, but only the
        // rail's thumbnails are focusable.
        std::function<brls::View*(brls::View*)> firstShot =
            [&](brls::View* node) -> brls::View* {
            if (auto* image = dynamic_cast<AsyncRgbaImage*>(node))
                if (image->isFocusable())
                    return image;
            if (auto* box = dynamic_cast<brls::Box*>(node))
                for (brls::View* child : box->getChildren())
                    if (brls::View* found = firstShot(child))
                        return found;
            return nullptr;
        };
        brls::View* shot = firstShot(root);
        if (!shot)
            return fail("detail-rail-nav: no focusable screenshot in the rail");
        brls::Application::giveFocus(shot);
        for (int i = 0; i < 5; ++i)
            brls::Application::mainLoop();
        if (brls::Application::getCurrentFocus() != shot)
            return fail("detail-rail-nav: could not park focus on the rail");

        // Application::navigate() is private; this mirrors its precedence —
        // the custom route wins over the tree walk, and only checking the walk
        // would miss the fix entirely.
        brls::View* next = nullptr;
        if (shot->hasCustomNavigationRouteByPtr(brls::FocusDirection::UP))
            next = shot->getCustomNavigationRoutePtr(brls::FocusDirection::UP);
        else if (shot->hasParent())
            next = shot->getNextFocus(brls::FocusDirection::UP, shot);
        if (!next)
            return fail("detail-rail-nav: UP out of the screenshot rail leads "
                        "nowhere");
        if (!dynamic_cast<InstallButton*>(next)) {
            std::fprintf(stderr, "golden_runner: UP from the rail landed on "
                                 "%s\n", next->describe().c_str());
            return fail("detail-rail-nav: UP left the rail for something other "
                        "than the primary action");
        }
        std::printf("golden_runner: screenshot rail routes UP to the install "
                    "button\n");
        manager.shutdown();
        std::fflush(nullptr);
        _exit(0);
    }

    if (bugReportFocus) {
        // The report screen owns no buttons, so nothing forces it to be
        // focusable - and when it was not, focus silently stayed on the
        // settings row that pushed it: that row's highlight and hints painted
        // over the report, and Y went to the wrong activity. A screenshot
        // cannot show any of that, because the stray highlight belongs to the
        // activity underneath.
        brls::View* root = bugReportFocus->getContentView();
        auto insideReport = [root](brls::View* view) {
            for (brls::View* node = view; node;
                 node = reinterpret_cast<brls::View*>(node->getParent()))
                if (node == root)
                    return true;
            return false;
        };
        if (!insideReport(brls::Application::getCurrentFocus()))
            return fail("bug-report never took focus from the pushing screen");

        const std::string before = bugReportFocus->renderedState();
        brls::Action* toggle = nullptr;
        for (const auto& action : root->getActions()) {
            if (action->getType() == brls::ACTION_GAMEPAD &&
                action->getButton() == brls::BUTTON_Y)
                toggle = action.get();
        }
        if (!toggle)
            return fail("bug-report registered no Y action");
        toggle->getActionListener()(root);
        for (int i = 0; i < 10; ++i) {
            if (!brls::Application::mainLoop())
                return fail("main loop ended while toggling detail mode");
        }
        const std::string after = bugReportFocus->renderedState();
        if (after == before)
            return fail("Y did not re-encode the report");
        if (!insideReport(brls::Application::getCurrentFocus()))
            return fail("rebuilding the grid dropped focus out of the screen");

        std::printf("golden_runner: bug-report kept focus and toggled "
                    "(%s -> %s)\n", before.c_str(), after.c_str());
        manager.shutdown();
        std::fflush(nullptr);
        _exit(0);
    }

    if (catalogHeaderClearance) {
        if (!collapsedCatalog)
            return fail("catalog-header-clearance: catalog tab was never built");
        const auto& children = collapsedCatalog->getChildren();
        if (children.empty())
            return fail("catalog-header-clearance: no catalog header");
        brls::View* header = children.front();
        auto* host = dynamic_cast<brls::Box*>(children.back());
        auto* recycler = host && !host->getChildren().empty()
            ? dynamic_cast<brls::RecyclerFrame*>(host->getChildren().front())
            : nullptr;
        const std::vector<ShelfCell*> shelves = visibleCells<ShelfCell>(recycler);
        if (shelves.empty())
            return fail("catalog-header-clearance: no rendered shelf");
        const float clearance = shelves.front()->getFrame().getMinY() -
                                header->getFrame().getMaxY();
        if (clearance < 28.0f)
            return fail("catalog-header-clearance: first shelf overlaps filters");
        std::printf("golden_runner: first shelf clears collapsed header by %.0fpx\n",
                    clearance);
        manager.shutdown();
        std::fflush(nullptr);
        _exit(0);
    }

    if (hintsBudget) {
        // The two status widgets exist only on hardware
        // (SwitchPlatform::canShowBatteryLevel returns true;
        // DesktopPlatform's returns false on Linux), so this runner renders a
        // bar that is this much wider than the console's. Charge it up front,
        // or the baseline machine happily accepts a row that overruns on a
        // Switch. 44px view + 21px margin each, see brls BottomBar's XML.
        constexpr float kSwitchStatusWidgets = 2 * (44.0f + 21.0f);
        if (!hintsCatalog)
            return fail("hints-budget: catalog tab was never built");
        brls::Hints* hints = findHints(activity->getContentView());
        if (!hints)
            return fail("hints-budget: no Hints view in the bottom bar");
        brls::Box* row = hints->getParent();
        if (!row)
            return fail("hints-budget: hint row has no parent");

        // Sum the children rather than hints->getWidth(): both are squashed
        // once the row overflows (Yoga runs with web defaults, so everything
        // here shrinks), but the children at least report per-hint numbers for
        // the failure message.
        float used = 0.0f;
        std::string widths;
        for (brls::View* hint : hints->getChildren()) {
            used += hint->getWidth();
            widths += (widths.empty() ? "" : " ") +
                      std::to_string(static_cast<int>(hint->getWidth()));
        }
        if (hints->getChildren().empty())
            return fail("hints-budget: bottom bar rendered no hints at all");

        // Whatever else shares the row is the clock cluster.
        float clock = 0.0f;
        for (brls::View* sibling : row->getChildren()) {
            if (sibling != hints)
                clock += sibling->getWidth();
        }
        const float budget = row->getWidth() - clock - kSwitchStatusWidgets;
        std::printf("golden_runner: hints-budget %s: %d hints, %.0fpx used of "
                    "%.0fpx available on a Switch (widths: %s)\n",
                    locale.c_str(),
                    static_cast<int>(hints->getChildren().size()), used, budget,
                    widths.c_str());
        if (used > budget) {
            std::fprintf(stderr,
                         "golden_runner: hints-budget: bottom bar overruns by "
                         "%.0fpx in %s — hide an action "
                         "(registerAction(..., hidden=true)) or shorten a "
                         "label\n",
                         used - budget, locale.c_str());
            manager.shutdown();
            std::fflush(nullptr);
            _exit(1);
        }
        manager.shutdown();
        std::fflush(nullptr);
        _exit(0);
    }

    // RecyclerFrame culls off-screen cells and getNextCellFocus() can only
    // focus a cell that is currently rendered, so a mis-aligned cull window
    // silently drops rows out of gamepad navigation. Walk the whole list down
    // and back up, one row per step, pumping frames in between so the
    // recycling loop (which runs in draw) gets to react to each move.
    if (torrentSelectionScroll) {
        if (!portSelectionOk)
            return fail("port selection retained files outside switch/");
        // Centered scrolling is animated, and the recycling loop only runs in
        // draw(), so each move needs enough frames for the scroll to settle
        // before the next one — otherwise the test measures the animation
        // rather than the navigation.
        auto pump = [&](int count) {
            for (int i = 0; i < count; ++i)
                brls::Application::mainLoop();
        };
        auto focusedRow = [](int& row) {
            auto* cell = dynamic_cast<brls::RecyclerCell*>(
                brls::Application::getCurrentFocus());
            if (!cell)
                return false;
            row = cell->getIndexPath().row;
            return true;
        };
        // Application::navigate() is private; this is its core, and the part
        // that matters here — RecyclerContentBox::getNextFocus routes into
        // RecyclerFrame::getNextCellFocus, which is what can only see rendered
        // cells. Application::inputType defaults to GAMEPAD, so giving focus
        // also drives ScrollingFrame's centered scrolling exactly as a real
        // d-pad press would.
        auto navigate = [](brls::FocusDirection direction) {
            brls::View* current = brls::Application::getCurrentFocus();
            if (!current || !current->hasParent())
                return;
            if (brls::View* next = current->getNextFocus(direction, current))
                brls::Application::giveFocus(next);
        };

        int row = -1;
        if (!focusedRow(row) || row != 0)
            return fail("torrent-selection did not start focused on row 0");

        for (int expected = 1; expected < torrentSelectionRows; ++expected) {
            navigate(brls::FocusDirection::DOWN);
            pump(30);
            if (!focusedRow(row))
                return fail("focus left the file list while scrolling down");
            if (row != expected) {
                std::fprintf(stderr,
                             "golden_runner: DOWN skipped row %d (landed on "
                             "%d)\n",
                             expected, row);
                return fail("file list skipped a row scrolling down");
            }
        }

        for (int expected = torrentSelectionRows - 2; expected >= 0;
             --expected) {
            navigate(brls::FocusDirection::UP);
            pump(30);
            if (!focusedRow(row))
                return fail("focus left the file list while scrolling up");
            if (row != expected) {
                std::fprintf(stderr,
                             "golden_runner: UP skipped row %d (landed on "
                             "%d)\n",
                             expected, row);
                return fail("file list skipped a row scrolling up");
            }
        }

        // Toggling repaints the focused cell in place rather than reloading the
        // recycler. Two things can silently break: the repaint finds no live
        // cell and does nothing, or it reloads and throws the cursor back to
        // row 0. Press A on a row in the middle of the list and check both the
        // rendered text and the cursor.
        for (int i = 0; i < 5; ++i) {
            navigate(brls::FocusDirection::DOWN);
            pump(30);
        }
        if (!focusedRow(row) || row != 5)
            return fail("could not park the cursor on row 5 to toggle it");

        auto* cell = static_cast<TorrentSelectionCell*>(
            brls::Application::getCurrentFocus());
        const std::string before = cell->renderedState();
        bool pressed = false;
        for (const auto& action : cell->getActions()) {
            if (action->getType() != brls::ACTION_GAMEPAD ||
                action->getButton() != brls::BUTTON_A)
                continue;
            action->getActionListener()(cell);
            pressed = true;
            break;
        }
        if (!pressed)
            return fail("file row has no A action to press");
        pump(5);

        int afterRow = -1;
        if (!focusedRow(afterRow) || afterRow != 5)
            return fail("toggling a row moved the cursor");
        if (brls::Application::getCurrentFocus() != cell)
            return fail("toggling a row recycled the focused cell");
        if (cell->renderedState() == before) {
            std::fprintf(stderr, "golden_runner: row 5 still reads \"%s\"\n",
                         before.c_str());
            return fail("toggling a row did not repaint it");
        }

        std::printf("golden_runner: torrent-selection walked %d rows down and "
                    "back up, and toggled row 5 in place (%s -> %s)\n",
                    torrentSelectionRows, before.c_str(),
                    cell->renderedState().c_str());
        manager.shutdown();
        std::fflush(nullptr);
        _exit(0);
    }

    GLint viewport[4] = {0, 0, 0, 0};
    glGetIntegerv(GL_VIEWPORT, viewport);
    const int width = viewport[2];
    const int height = viewport[3];
    if (width <= 0 || height <= 0)
        return fail("empty GL viewport");

    // SDL/Xvfb exposes an unreliable compositor-owned front buffer after
    // SDL_GL_SwapWindow: depending on timing it can be black, stale, or only
    // partially preserved. The back buffer consistently contains the previous
    // completed frame. The UI has settled after 90 draws, so capturing that
    // frame is deterministic and visually equivalent to the just-swapped one.
    std::vector<uint8_t> rgba;
    if (!readFramebuffer(GL_BACK, width, height, rgba) ||
        !hasVisiblePixel(rgba))
        return fail("GL back buffer is empty");

    if (!writePng(outPng.string(), width, height, rgba))
        return fail("failed to write PNG");

    std::printf("golden_runner: %s (%dx%d, back buffer) -> %s\n",
                screen.c_str(), width, height,
                outPng.string().c_str());

    manager.shutdown();
    std::fflush(nullptr);
    _exit(0); // skip GL/window teardown; the frame is already on disk
}
