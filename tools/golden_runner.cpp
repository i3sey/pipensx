// golden_runner — F4 golden-screenshot harness (PC/SDL2 build only).
//
// Renders ONE pipensx screen with deterministic fixture data and writes a
// PNG. One screen per process keeps runs fully isolated (fresh focus state,
// fresh animations, fresh services). Driven by scripts/golden.sh.
//
// Usage:
//   golden_runner --fixtures <dir> --out <file.png> --theme light|dark
//                 --screen catalog|shelf-scroll|detail|downloads|installed|settings|about
//                 [--frames N] [--sandbox <dir>]
//
// Determinism notes:
//   - run with LIBGL_ALWAYS_SOFTWARE=1 so Mesa llvmpipe rasterizes the same
//     locally and in CI (scripts/golden.sh sets this);
//   - the process chdirs into a scratch sandbox so all "sdmc:/..." paths of
//     the real app land in an empty, throwaway directory tree;
//   - GameMetadataService image networking is paused: remote artwork stays
//     at placeholders;
//   - the libnx shim (src/platform/pc/switch.h) reports an empty installed
//     library and a fixed firmware version.

#include <glad/glad.h>

#include <borealis.hpp>
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
#include "app/installed_title_service.hpp"
#include "ui/catalog/catalog_view.hpp"
#include "ui/common/ui_helpers.hpp"
#include "ui/detail/game_detail.hpp"
#include "ui/downloads/downloads_view.hpp"
#include "ui/installed/installed_view.hpp"
#include "ui/settings/about_view.hpp"
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
    explicit GoldenActivity(brls::View* content) {
        frame_ = new brls::AppletFrame(content);
        frame_->setTitle("pipensx");
    }

    brls::View* createContentView() override {
        return frame_;
    }

private:
    brls::AppletFrame* frame_;
};

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
    std::string screen;
    int frames = 90;

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
    brls::Platform::APP_LOCALE_DEFAULT = brls::LOCALE_EN_US;
    if (!brls::Application::init())
        return fail("borealis Application::init failed");
    pipensx::ui::theme::registerColors();
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

    InstalledTitleService installed("sdmc:/switch/pipensx");
    installed.refresh(error); // shim: succeeds with an empty library

    DownloadManager manager("sdmc:/switch/pipensx");

    brls::Activity* activity = nullptr;
    brls::View* focusAfterLayout = nullptr;
    if (screen == "catalog") {
        activity = new GoldenActivity(new CatalogView(
            &manager, &catalog, &metadata, &installed, &settings, [] {}));
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
    } else if (screen == "detail") {
        const auto& entries = catalog.entries();
        if (entries.empty())
            return fail("detail screen needs a non-empty catalog fixture");
        activity = new GameDetailActivity(
            entries.front(), "", &manager, &metadata, &installed, &settings,
            [](const std::string&, const std::string&) {}, [] {});
    } else if (screen == "downloads") {
        activity = new GoldenActivity(
            new MainView(&manager, &metadata, &settings));
    } else if (screen == "installed") {
        activity = new GoldenActivity(
            new InstalledView(&installed, &manager, &metadata));
    } else if (screen == "settings") {
        activity = new GoldenActivity(new SettingsView(
            &settings, &manager, &catalog, &metadata, &installed));
    } else if (screen == "about") {
        activity = new GoldenActivity(new AboutView());
    } else {
        return fail("unknown --screen");
    }

    brls::Application::pushActivity(activity);
    for (int i = 0; i < frames; ++i) {
        if (i == 10 && focusAfterLayout)
            brls::Application::giveFocus(focusAfterLayout);
        if (!brls::Application::mainLoop())
            return fail("main loop ended before capture");
    }

    GLint viewport[4] = {0, 0, 0, 0};
    glGetIntegerv(GL_VIEWPORT, viewport);
    const int width = viewport[2];
    const int height = viewport[3];
    if (width <= 0 || height <= 0)
        return fail("empty GL viewport");

    // SDL/Xvfb may expose a compositor-owned, all-black front buffer after
    // SDL_GL_SwapWindow. Prefer it where it is readable, then fall back to the
    // previous completed frame in the back buffer. The UI has settled after
    // 90 draws, so the one-frame difference is immaterial to the snapshot.
    std::vector<uint8_t> rgba;
    const char* captureBuffer = "front";
    if (!readFramebuffer(GL_FRONT, width, height, rgba) ||
        !hasVisiblePixel(rgba)) {
        captureBuffer = "back";
        if (!readFramebuffer(GL_BACK, width, height, rgba) ||
            !hasVisiblePixel(rgba))
            return fail("both GL front and back buffers are empty");
    }

    if (!writePng(outPng.string(), width, height, rgba))
        return fail("failed to write PNG");

    std::printf("golden_runner: %s (%dx%d, %s buffer) -> %s\n",
                screen.c_str(), width, height, captureBuffer,
                outPng.string().c_str());

    manager.shutdown();
    std::fflush(nullptr);
    _exit(0); // skip GL/window teardown; the frame is already on disk
}
