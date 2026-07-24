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
//                          hints-budget|installed|settings|about|bug-report|
//                          bug-report-detail|bug-report-focus
//                 [--frames N] [--sandbox <dir>]
//
// downloads-back, torrent-selection-scroll, hints-budget and bug-report-focus
// are behaviour checks: they assert and exit non-zero instead of producing a
// baseline.
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
#include <string>
#include <vector>

#include <unistd.h>

#include "app/app_settings.hpp"
#include "app/catalog_service.hpp"
#include "app/download_manager.hpp"
#include "app/game_metadata_service.hpp"
#include "app/install_space.hpp"
#include "app/installed_title_service.hpp"
#include "ui/catalog/catalog_view.hpp"
#include "ui/common/ui_helpers.hpp"
#include "ui/detail/game_detail.hpp"
#include "ui/detail/torrent_selection.hpp"
#include "ui/downloads/downloads_view.hpp"
#include "ui/i18n.hpp"
#include "ui/installed/installed_view.hpp"
#include "ui/main_frame.hpp"
#include "ui/settings/about_view.hpp"
#include "ui/settings/bug_report_view.hpp"
#include "ui/settings/settings_view.hpp"
#include "ui/theme.hpp"

extern "C" {
#include "core/util.h"
}

namespace fs = std::filesystem;

using pipensx::AppSettings;
using pipensx::CatalogService;
using pipensx::DownloadManager;
using pipensx::GameMetadataService;
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
        if (withExitAction_)
            registerAction(tr("pipensx/app/exit"), brls::BUTTON_START,
                           [](brls::View*) { return true; }, /*hidden=*/true);
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

    CatalogService catalog("sdmc:/switch/pipensx",
                           (fixtures / "catalog.json").string());
    if (!catalog.load(error))
        std::fprintf(stderr, "golden_runner: catalog fixture: %s\n",
                     error.c_str());

    GameMetadataService metadata(
        "sdmc:/switch/pipensx",
        (fixtures / "game_metadata_index.json").string());
    if (!metadata.load(error))
        std::fprintf(stderr, "golden_runner: metadata fixture: %s\n",
                     error.c_str());
    metadata.setImageNetworkPaused(true); // placeholders only, no network

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
    int torrentSelectionRows = 0;
    bool torrentSelectionScroll = false;
    bool hintsBudget = false;
    CatalogView* hintsCatalog = nullptr;
    BugReportActivity* bugReportFocus = nullptr;
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
    } else if (screen == "detail") {
        const auto& entries = catalog.entries();
        if (entries.empty())
            return fail("detail screen needs a non-empty catalog fixture");
        activity = new GameDetailActivity(
            entries.front(), "", &manager, &metadata, &installed, &settings,
            &mods, [](const std::string&, const std::string&) {}, [] {},
            nullptr, &favorites);
    } else if (screen == "torrent-selection" ||
               screen == "torrent-selection-scroll") {
        // More files than fit on screen: the recycler only recycles once the
        // list is taller than its viewport, and a short list would hide the
        // whole class of cull/navigation bugs this screen guards.
        pipensx::TorrentPreview preview;
        preview.name = "Mixed release";
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
            {"extras/notes.txt", 4096ULL, false, false, false},
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
    } else if (screen == "downloads-back") {
        downloadsBackFrame = new MainFrame();
        auto* downloadsView = new MainView(&manager, &metadata, &settings);
        downloadsBackFrame->addNavTab(
            tr("pipensx/nav/downloads"), NavIconType::Downloads,
            [downloadsView] { return downloadsView; });
        activity = new GoldenActivity(downloadsBackFrame);
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
            return new InstalledView(&installed, &manager, &metadata);
        });
        tabs->addNavTab(tr("pipensx/nav/settings"), NavIconType::Settings,
                        [&] {
            return new SettingsView(&settings, &manager, &catalog, &metadata,
                                    &installed, nullptr, &mods);
        });
        tabs->addNavTab(tr("pipensx/nav/about"), NavIconType::About,
                        [] { return new AboutView(); });
        tabs->attachStorageFooter(&manager);
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
    } else if (screen == "installed") {
        activity = new GoldenActivity(
            new InstalledView(&installed, &manager, &metadata));
    } else if (screen == "settings") {
        activity = new GoldenActivity(new SettingsView(
            &settings, &manager, &catalog, &metadata, &installed, nullptr,
            &mods));
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
    if (downloadsBackFrame &&
        brls::Application::getCurrentFocus() != downloadsBackSidebarFocus)
        return fail("downloads refresh stole focus from the sidebar");
    if (downloadsBackFrame) {
        std::printf("golden_runner: downloads-back focus preserved\n");
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
