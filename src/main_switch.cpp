#include "app/app_settings.hpp"
#include "app/catalog_batch_installer.hpp"
#include "app/download_manager.hpp"
#include "app/catalog_service.hpp"
#include "app/catalog_presentation.hpp"
#include "app/magnet_resolver.hpp"
#include "app/game_metadata_service.hpp"
#include "app/installed_title_service.hpp"
#include "app/install_space.hpp"
#include "core/antizapret.h"
#include "platform/switch_crashlog.h"
#include "platform/switch_performance.hpp"

extern "C" {
#include "core/util.h"
}

#include <borealis.hpp>
#include <curl/curl.h>
#include <dirent.h>
#include <switch.h>
#include <switch-ipcext.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <exception>
#include <fstream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include <vector>

using pipensx::DownloadManager;
using pipensx::DownloadStatus;
using pipensx::DownloadTask;
using pipensx::CatalogEntry;
using pipensx::CatalogBatchInstaller;
using pipensx::CatalogService;
using pipensx::AppSettings;
using pipensx::AppSettingsData;
using pipensx::CatalogFilter;
using pipensx::InstalledTitle;
using pipensx::InstalledTitleService;
using pipensx::StreamSelection;
using pipensx::GameMetadata;
using pipensx::GameMetadataService;
using pipensx::MagnetResolver;
using pipensx::BatchPreparation;
using pipensx::PreparedCatalogInstall;
using pipensx::StorageSpaceSnapshot;
using pipensx::InstallSpaceCheckStatus;
using pipensx::SpaceEstimateCertainty;
using pipensx::SwitchPerformanceController;
using pipensx::TransferMode;

namespace {

FILE* gBorealisLog = nullptr;
std::atomic<uint32_t> gCatalogTempSerial{0};
constexpr const char* TelemetryFlagPath =
    "sdmc:/switch/pipensx/throughput_telemetry.enabled";
constexpr const char* SettingsPath =
    "sdmc:/switch/pipensx/settings.json";
constexpr const char* LogPath =
    "sdmc:/switch/pipensx/pipensx.log";

void openBorealisLog() {
    if (gBorealisLog)
        return;
    gBorealisLog = std::fopen(LogPath, "a");
    if (gBorealisLog) {
        brls::Logger::setLogOutput(gBorealisLog);
        brls::Logger::setLogLevel(brls::LogLevel::LOG_DEBUG);
    }
}

bool clearApplicationLog() {
    if (gBorealisLog) {
        std::fflush(gBorealisLog);
        brls::Logger::setLogOutput(nullptr);
        std::fclose(gBorealisLog);
        gBorealisLog = nullptr;
    }
    bool ok = log_clear() != 0;
    openBorealisLog();
    return ok;
}

void startupStage(const char* stage) {
    switch_crashlog_stage(stage);
    log_msg("[startup] %s\n", stage);
}

bool isApplicationMode() {
    AppletType type = appletGetAppletType();
    return type == AppletType_Application ||
           type == AppletType_SystemApplication;
}

void showApplicationModeRequired() {
    consoleInit(nullptr);
    std::printf("\npipensx requires application mode.\n\n");
    std::printf("Close hbmenu, then hold R while launching a game.\n");
    std::printf("Keep holding R until hbmenu opens, then start pipensx.\n\n");
    std::printf("Album applet mode does not provide enough memory and\n");
    std::printf("network sessions for the GUI torrent client.\n\n");
    std::printf("Press + to exit.\n");
    consoleUpdate(nullptr);

    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);
    while (appletMainLoop()) {
        padUpdate(&pad);
        if (padGetButtonsDown(&pad) & HidNpadButton_Plus)
            break;
    }
    consoleExit(nullptr);
}

std::string formatBytes(uint64_t bytes) {
    char buffer[32];
    fmt_bytes(buffer, sizeof(buffer), bytes);
    return buffer;
}

std::string formatSpeed(uint64_t bytes) {
    char buffer[32];
    fmt_speed(buffer, sizeof(buffer), bytes);
    return buffer;
}

uint64_t emaUpdate(uint64_t previous, uint64_t sample) {
    if (sample >= previous)
        return previous + (sample - previous) * 3 / 10;
    return previous - (previous - sample) * 3 / 10;
}

float progressOf(const DownloadTask& task) {
    if (!task.totalBytes)
        return 0.0f;
    return std::min(1.0f, static_cast<float>(task.completedBytes) /
                              static_cast<float>(task.totalBytes));
}

float installProgressOf(const DownloadTask& task) {
    if (!task.installTotalBytes)
        return 0.0f;
    return std::min(1.0f, static_cast<float>(task.installedBytes) /
                              static_cast<float>(task.installTotalBytes));
}

int percentOf(float progress) {
    return static_cast<int>(std::clamp(progress, 0.0f, 1.0f) * 100.0f);
}

std::string taskStatusText(const DownloadTask& task) {
    auto withPercent = [](const std::string& label, int percent) {
        return label + " " + std::to_string(percent) + "%";
    };

    switch (task.status) {
        case DownloadStatus::Checking:
        case DownloadStatus::Downloading:
        case DownloadStatus::Verifying:
            return withPercent(pipensx::statusName(task.status),
                               percentOf(progressOf(task)));
        case DownloadStatus::Installing:
        case DownloadStatus::Committing:
            return withPercent(pipensx::statusName(task.status),
                               percentOf(installProgressOf(task)));
        case DownloadStatus::Paused: {
            int pct = percentOf(progressOf(task));
            return pct > 0 ? withPercent("Paused", pct) : "Paused";
        }
        case DownloadStatus::Error: {
            int pct = percentOf(progressOf(task));
            return pct > 0 ? withPercent("Error", pct) : "Error";
        }
        case DownloadStatus::Completed:
            return "Downloaded";
        case DownloadStatus::Installed:
            return "Installed";
        case DownloadStatus::Queued:
        case DownloadStatus::Removing:
            return pipensx::statusName(task.status);
    }
    return pipensx::statusName(task.status);
}

struct ImageRequestState {
    std::atomic<uint64_t> generation {0};
    std::atomic<bool> pending {false};
};

class AsyncRgbaImage;

struct AsyncImageLifetime {
    std::mutex mutex;
    AsyncRgbaImage* image = nullptr;
};

class AsyncRgbaImage : public brls::Image {
public:
    AsyncRgbaImage() : lifetime_(std::make_shared<AsyncImageLifetime>()) {
        lifetime_->image = this;
    }

    ~AsyncRgbaImage() override {
        std::lock_guard<std::mutex> lock(lifetime_->mutex);
        lifetime_->image = nullptr;
    }

    void setRgbaAsync(std::function<void(std::function<void(
        std::shared_ptr<const std::vector<uint8_t>>, int, int)>)> provider) {
        std::weak_ptr<AsyncImageLifetime> weakLifetime = lifetime_;
        provider([weakLifetime](
            std::shared_ptr<const std::vector<uint8_t>> pixels,
            int width, int height) {
            brls::sync([weakLifetime, pixels = std::move(pixels),
                        width, height] {
                auto lifetime = weakLifetime.lock();
                if (!lifetime)
                    return;
                std::lock_guard<std::mutex> lock(lifetime->mutex);
                if (!lifetime->image || !pixels || pixels->empty() ||
                    width <= 0 || height <= 0)
                    return;
                NVGcontext* vg = brls::Application::getNVGContext();
                lifetime->image->innerSetImage(nvgCreateImageRGBA(
                    vg, width, height, 0, pixels->data()));
            });
        });
    }

private:
    std::shared_ptr<AsyncImageLifetime> lifetime_;
};

std::string placeholderLetter(const std::string& title) {
    size_t offset = 0;
    while (offset < title.size() &&
           std::isspace(static_cast<unsigned char>(title[offset])))
        ++offset;
    if (offset == title.size())
        return "?";
    unsigned char lead = static_cast<unsigned char>(title[offset]);
    size_t length = lead < 0x80 ? 1 : lead < 0xe0 ? 2 : lead < 0xf0 ? 3 : 4;
    length = std::min(length, title.size() - offset);
    std::string letter = title.substr(offset, length);
    if (length == 1)
        letter[0] = static_cast<char>(std::toupper(lead));
    return letter;
}

void loadImageInto(AsyncRgbaImage* image, GameMetadataService* service,
                   const std::string& url,
                   const std::shared_ptr<ImageRequestState>& state,
                   uint64_t generation) {
    if (!image)
        return;
    image->clear();
    if (!service || url.empty()) {
        state->pending = false;
        return;
    }
    state->pending = true;
    image->setRgbaAsync([service, url, state, generation](
        std::function<void(std::shared_ptr<const std::vector<uint8_t>>,
                           int, int)> done) {
        service->requestImage(url, [done, state, generation](
            GameMetadataService::ImageData bytes) {
            if (state->generation.load() != generation) {
                done(nullptr, 0, 0);
                return;
            }
            state->pending = false;
            if (!bytes || bytes->pixels.empty()) {
                done(nullptr, 0, 0);
                return;
            }
            std::shared_ptr<const std::vector<uint8_t>> pixels(
                bytes, &bytes->pixels);
            done(std::move(pixels), bytes->width, bytes->height);
        });
    });
}

void loadImageInto(AsyncRgbaImage* image, GameMetadataService* service,
                   const std::string& url) {
    auto state = std::make_shared<ImageRequestState>();
    uint64_t generation = ++state->generation;
    loadImageInto(image, service, url, state, generation);
}

void setArtworkUrl(AsyncRgbaImage* image, GameMetadataService* service,
                   const std::string& url, std::string& currentUrl,
                   const std::shared_ptr<ImageRequestState>& state) {
    if (currentUrl == url &&
        (image->getTexture() != 0 || state->pending.load()))
        return;
    currentUrl = url;
    uint64_t generation = ++state->generation;
    loadImageInto(image, service, url, state, generation);
}

class ProgressBar : public brls::View {
public:
    ProgressBar() {
        setHeight(7);
        setCornerRadius(3.5f);
    }

    void setProgress(float progress) {
        progress_ = std::max(0.0f, std::min(1.0f, progress));
    }

    void draw(NVGcontext* vg, float x, float y, float width, float height,
              brls::Style, brls::FrameContext*) override {
        nvgBeginPath(vg);
        nvgRoundedRect(vg, x, y, width, height, height / 2);
        nvgFillColor(vg, nvgRGBA(128, 128, 128, 70));
        nvgFill(vg);
        if (progress_ > 0) {
            nvgBeginPath(vg);
            nvgRoundedRect(vg, x, y, width * progress_, height, height / 2);
            nvgFillColor(vg, nvgRGB(0, 195, 227));
            nvgFill(vg);
        }
    }

private:
    float progress_ = 0;
};

class SpeedGraphView : public brls::View {
public:
    SpeedGraphView() {
        setHeight(82);
        setMarginBottom(13);
    }

    void setSamples(std::vector<uint64_t> download,
                    std::vector<uint64_t> install) {
        download_ = std::move(download);
        install_ = std::move(install);
    }

    void draw(NVGcontext* vg, float x, float y, float width, float height,
              brls::Style, brls::FrameContext*) override {
        if (width <= 1 || height <= 1)
            return;

        nvgBeginPath(vg);
        nvgRoundedRect(vg, x, y, width, height, 6);
        nvgFillColor(vg, nvgRGBA(30, 31, 36, 120));
        nvgFill(vg);

        const float pad = 8.0f;
        const float left = x + pad;
        const float right = x + width - pad;
        const float top = y + pad;
        const float bottom = y + height - pad;
        const float plotWidth = std::max(1.0f, right - left);
        const float plotHeight = std::max(1.0f, bottom - top);

        nvgStrokeWidth(vg, 1.0f);
        nvgStrokeColor(vg, nvgRGBA(180, 180, 190, 35));
        for (int i = 1; i <= 3; i++) {
            float gy = top + plotHeight * static_cast<float>(i) / 4.0f;
            nvgBeginPath(vg);
            nvgMoveTo(vg, left, gy);
            nvgLineTo(vg, right, gy);
            nvgStroke(vg);
        }

        uint64_t maxSpeed = 512ULL * 1024ULL;
        for (uint64_t speed : download_)
            maxSpeed = std::max(maxSpeed, speed);
        for (uint64_t speed : install_)
            maxSpeed = std::max(maxSpeed, speed);

        drawSeries(vg, download_, maxSpeed, left, top, plotWidth, plotHeight,
                   nvgRGB(0, 195, 227), 2.5f);
        drawSeries(vg, install_, maxSpeed, left, top, plotWidth, plotHeight,
                   nvgRGB(96, 220, 130), 2.0f);
    }

private:
    static float yFor(uint64_t speed, uint64_t maxSpeed, float top,
                      float plotHeight) {
        double ratio = maxSpeed ? static_cast<double>(speed) /
                                      static_cast<double>(maxSpeed)
                                : 0.0;
        ratio = std::clamp(ratio, 0.0, 1.0);
        return top + plotHeight * static_cast<float>(1.0 - ratio);
    }

    static void drawSeries(NVGcontext* vg, const std::vector<uint64_t>& samples,
                           uint64_t maxSpeed, float left, float top,
                           float plotWidth, float plotHeight, NVGcolor color,
                           float strokeWidth) {
        if (samples.empty())
            return;
        if (samples.size() == 1) {
            nvgBeginPath(vg);
            nvgCircle(vg, left, yFor(samples.front(), maxSpeed, top, plotHeight),
                      2.5f);
            nvgFillColor(vg, color);
            nvgFill(vg);
            return;
        }

        nvgBeginPath(vg);
        for (size_t i = 0; i < samples.size(); i++) {
            float px = left + plotWidth * static_cast<float>(i) /
                                static_cast<float>(samples.size() - 1);
            float py = yFor(samples[i], maxSpeed, top, plotHeight);
            if (i == 0)
                nvgMoveTo(vg, px, py);
            else
                nvgLineTo(vg, px, py);
        }
        nvgStrokeWidth(vg, strokeWidth);
        nvgStrokeColor(vg, color);
        nvgStroke(vg);
    }

    std::vector<uint64_t> download_;
    std::vector<uint64_t> install_;
};

class DownloadCell : public brls::RecyclerCell {
public:
    DownloadCell() {
        setFocusable(true);
        setAxis(brls::Axis::ROW);
        setAlignItems(brls::AlignItems::CENTER);
        setPadding(12, 20, 12, 20);
        setHeight(108);

        thumb_ = new brls::Box();
        thumb_->setWidth(72);
        thumb_->setHeight(72);
        thumb_->setCornerRadius(6);
        thumb_->setBackgroundColor(nvgRGB(58, 58, 66));
        thumb_->setMarginRight(16);
        thumb_->setAlignItems(brls::AlignItems::CENTER);
        thumb_->setJustifyContent(brls::JustifyContent::CENTER);
        placeholder_ = new brls::Label();
        placeholder_->setFontSize(30);
        placeholder_->setTextColor(nvgRGB(185, 185, 195));
        thumb_->addView(placeholder_);
        image_ = new AsyncRgbaImage();
        image_->setWidth(72);
        image_->setHeight(72);
        image_->setPositionType(brls::PositionType::ABSOLUTE);
        image_->setPositionTop(0);
        image_->setPositionLeft(0);
        image_->setCornerRadius(6);
        image_->setScalingType(brls::ImageScalingType::FILL);
        thumb_->addView(image_);
        addView(thumb_);

        auto* right = new brls::Box(brls::Axis::COLUMN);
        right->setGrow(1);
        right->setJustifyContent(brls::JustifyContent::CENTER);

        auto* top = new brls::Box(brls::Axis::ROW);
        top->setAlignItems(brls::AlignItems::CENTER);
        title_ = new brls::Label();
        title_->setSingleLine(true);
        title_->setFontSize(22);
        title_->setGrow(1);
        status_ = new brls::Label();
        status_->setSingleLine(true);
        status_->setFontSize(17);
        status_->setTextColor(nvgRGB(0, 195, 227));
        top->addView(title_);
        top->addView(status_);

        meta_ = new brls::Label();
        meta_->setSingleLine(true);
        meta_->setFontSize(16);
        meta_->setMarginTop(6);
        meta_->setMarginBottom(9);

        progress_ = new ProgressBar();
        right->addView(top);
        right->addView(meta_);
        right->addView(progress_);
        addView(right);
    }

    void setTask(const DownloadTask& task, GameMetadataService* service) {
        title_->setText(task.name);
        placeholder_->setText(placeholderLetter(task.name));
        status_->setText(taskStatusText(task));
        float progress = (task.status == DownloadStatus::Installing ||
                          task.status == DownloadStatus::Committing)
            ? installProgressOf(task)
            : progressOf(task);
        progress_->setProgress(progress);

        std::string meta = formatBytes(task.completedBytes) + " / " +
                           formatBytes(task.totalBytes);
        if (task.status == DownloadStatus::Installing ||
            task.status == DownloadStatus::Committing) {
            meta = "Package " + std::to_string(task.packagesInstalled + 1) +
                   " / " + std::to_string(task.packageCount);
            if (!task.currentPackage.empty())
                meta += "   " + task.currentPackage;
        } else if (task.status == DownloadStatus::Installed) {
            meta = std::to_string(task.packagesInstalled) +
                   " package(s) installed to SD";
        } else if (task.status == DownloadStatus::Downloading)
            meta += "   " + formatSpeed(task.speedBytesPerSecond) +
                    "   " + std::to_string(task.peers) + " peers";
        else if (task.status == DownloadStatus::Queued)
            meta += "   Waiting for the active download";
        else if (task.status == DownloadStatus::Error && !task.error.empty())
            meta += "   " + task.error;
        meta_->setText(meta);

        std::string iconUrl;
        if (service) {
            const GameMetadata* found = service->findByInfoHash(task.id);
            if (found)
                iconUrl = found->iconUrl;
        }
        setArtworkUrl(image_, service, iconUrl, currentIconUrl_, imageState_);
    }

private:
    brls::Box* thumb_;
    brls::Label* placeholder_;
    AsyncRgbaImage* image_;
    brls::Label* title_;
    brls::Label* status_;
    brls::Label* meta_;
    ProgressBar* progress_;
    std::string currentIconUrl_;
    std::shared_ptr<ImageRequestState> imageState_ =
        std::make_shared<ImageRequestState>();
};

class MessageCell : public brls::RecyclerCell {
public:
    MessageCell() {
        setFocusable(true);
        setHeight(100);
        setPadding(24);
        label_ = new brls::Label();
        label_->setFontSize(21);
        label_->setText("No downloads yet. Press X to add a .torrent file.");
        addView(label_);
    }

private:
    brls::Label* label_;
};

class MainView;

class DownloadDataSource : public brls::RecyclerDataSource {
public:
    explicit DownloadDataSource(MainView* owner) : owner_(owner) {}

    void setTasks(std::vector<DownloadTask> tasks);
    std::string taskIdAt(brls::IndexPath index) const;
    brls::IndexPath indexForTask(const std::string& taskId) const;
    int numberOfSections(brls::RecyclerFrame*) override;
    int numberOfRows(brls::RecyclerFrame*, int section) override;
    std::string titleForHeader(brls::RecyclerFrame*, int section) override;
    brls::RecyclerCell* cellForRow(brls::RecyclerFrame* recycler,
                                    brls::IndexPath index) override;
    void didSelectRowAt(brls::RecyclerFrame*, brls::IndexPath index) override;

private:
    struct Section {
        std::string title;
        std::vector<DownloadTask> tasks;
    };
    MainView* owner_;
    std::vector<Section> sections_;
};

class DetailsActivity : public brls::Activity {
public:
    DetailsActivity(std::string taskId, DownloadManager* manager)
        : taskId_(std::move(taskId)), manager_(manager) {
        auto* content = new brls::Box(brls::Axis::COLUMN);
        content->setFocusable(true);
        content->setGrow(1);
        content->setPadding(30, 55, 30, 55);
        content->setJustifyContent(brls::JustifyContent::CENTER);
        content->setAlignItems(brls::AlignItems::STRETCH);
        content->setBackgroundColor(nvgRGBA(45, 45, 50, 180));
        content->setCornerRadius(12);

        status_ = addLine(content, 27);
        progress_ = addLine(content, 22);
        install_ = addLine(content, 20);
        transfer_ = addLine(content, 22);
        speedGraph_ = new SpeedGraphView();
        content->addView(speedGraph_);
        peers_ = addLine(content, 22);
        pieces_ = addLine(content, 22);
        path_ = addLine(content, 18);
        error_ = addLine(content, 18);
        error_->setTextColor(nvgRGB(255, 80, 80));

        auto* frame = new brls::AppletFrame(content);
        frame_ = frame;
    }

    brls::View* createContentView() override {
        return frame_;
    }

    void onContentAvailable() override {
        refresh();
        registerAction("Pause / Resume / Verify", brls::BUTTON_Y,
            [this](brls::View*) {
                auto task = currentTask();
                if (!task)
                    return false;
                if (task->status == DownloadStatus::Paused ||
                    task->status == DownloadStatus::Error)
                    manager_->resume(taskId_);
                else if (task->status == DownloadStatus::Completed)
                    manager_->verify(taskId_);
                else if (task->status == DownloadStatus::Queued ||
                         task->status == DownloadStatus::Checking ||
                         task->status == DownloadStatus::Downloading ||
                         task->status == DownloadStatus::Installing ||
                         task->status == DownloadStatus::Committing ||
                         task->status == DownloadStatus::Verifying)
                    manager_->pause(taskId_);
                refresh();
                return true;
            });
        registerAction("Remove", brls::BUTTON_X,
            [this](brls::View*) {
                openRemoveDialog();
                return true;
            });

        timer_.setCallback([this] { refresh(); });
        timer_.start(500);
    }

    ~DetailsActivity() override {
        timer_.stop();
    }

private:
    static brls::Label* addLine(brls::Box* box, float size) {
        auto* label = new brls::Label();
        label->setWidth(brls::View::AUTO);
        label->setFontSize(size);
        label->setMarginBottom(13);
        box->addView(label);
        return label;
    }

    const DownloadTask* currentTask() {
        cache_ = manager_->snapshot();
        for (const auto& task : cache_)
            if (task.id == taskId_)
                return &task;
        return nullptr;
    }

    void refresh() {
        const DownloadTask* task = currentTask();
        if (!task) {
            brls::Application::popActivity();
            return;
        }
        frame_->setTitle(task->name);
        status_->setText(std::string("Status: ") +
                         pipensx::statusName(task->status));
        int percent = task->totalBytes
            ? static_cast<int>(task->completedBytes * 100 / task->totalBytes)
            : 0;
        progress_->setText("Progress: " + std::to_string(percent) + "%  (" +
                           formatBytes(task->completedBytes) + " / " +
                           formatBytes(task->totalBytes) + ")");
        if (task->mode == TransferMode::StreamInstall) {
            int installPercent = task->installTotalBytes
                ? static_cast<int>(task->installedBytes * 100 /
                                   task->installTotalBytes)
                : 0;
            install_->setText(
                "Install: " + std::to_string(task->packagesInstalled) +
                " / " + std::to_string(task->packageCount) + " packages" +
                (task->currentPackage.empty()
                    ? ""
                    : "   " + task->currentPackage + "   " +
                      std::to_string(installPercent) + "%"));
        } else {
            install_->setText("");
        }
        recordSpeedSample(*task);
        if (task->mode == TransferMode::StreamInstall) {
            transfer_->setText(
                "Download: " + formatSpeed(task->speedBytesPerSecond) +
                "   Install: " + formatSpeed(installSpeedSmoothed_));
        } else {
            transfer_->setText(
                "Download: " + formatSpeed(task->speedBytesPerSecond));
        }
        peers_->setText("Peers: " + std::to_string(task->peers) +
                        "   DHT: " + std::to_string(task->dhtGood) + " good / " +
                        std::to_string(task->dhtDubious) + " dubious");
        pieces_->setText("Pieces: " + std::to_string(task->piecesDone) + " / " +
                         std::to_string(task->piecesTotal) +
                         "   Checked: " +
                         std::to_string(task->piecesVerified));
        path_->setText("Output: " + task->dataPath);
        error_->setText(task->error.empty() ? "" : "Error: " + task->error);
    }

    static void appendSpeedSample(std::vector<uint64_t>& samples,
                                  uint64_t value) {
        constexpr size_t kMaxSpeedSamples = 60;
        if (samples.size() == kMaxSpeedSamples)
            samples.erase(samples.begin());
        samples.push_back(value);
    }

    void recordSpeedSample(const DownloadTask& task) {
        uint64_t now = now_ms();
        appendSpeedSample(downloadSpeedSamples_, task.speedBytesPerSecond);

        if (task.mode == TransferMode::StreamInstall) {
            uint64_t installSpeed = 0;
            if (hasInstallSample_ && now > lastInstallSampleMs_ &&
                task.installedBytes >= lastInstallBytes_) {
                uint64_t elapsed = now - lastInstallSampleMs_;
                installSpeed =
                    (task.installedBytes - lastInstallBytes_) * 1000 / elapsed;
            }
            lastInstallBytes_ = task.installedBytes;
            lastInstallSampleMs_ = now;
            hasInstallSample_ = true;
            installSpeedSmoothed_ = emaUpdate(installSpeedSmoothed_,
                                              installSpeed);
            appendSpeedSample(installSpeedSamples_, installSpeedSmoothed_);
        } else {
            hasInstallSample_ = false;
            lastInstallBytes_ = 0;
            lastInstallSampleMs_ = now;
            installSpeedSmoothed_ = 0;
            installSpeedSamples_.clear();
        }

        speedGraph_->setSamples(downloadSpeedSamples_, installSpeedSamples_);
    }

    void openRemoveDialog() {
        auto* dialog = new brls::Dialog(
            "Remove this download from pipensx?");
        dialog->addButton("Keep downloaded data", [this] {
            std::string error;
            if (!manager_->remove(taskId_, false, error))
                brls::Application::notify(error);
            else
                brls::Application::popActivity();
        });
        dialog->addButton("Delete downloaded data", [this] {
            std::string error;
            if (!manager_->remove(taskId_, true, error))
                brls::Application::notify(error);
            else
                brls::Application::popActivity();
        });
        dialog->addButton("Cancel", [] {});
        dialog->open();
    }

    std::string taskId_;
    DownloadManager* manager_;
    brls::AppletFrame* frame_;
    brls::Label* status_;
    brls::Label* progress_;
    brls::Label* install_;
    brls::Label* transfer_;
    SpeedGraphView* speedGraph_;
    brls::Label* peers_;
    brls::Label* pieces_;
    brls::Label* path_;
    brls::Label* error_;
    brls::RepeatingTimer timer_;
    std::vector<DownloadTask> cache_;
    std::vector<uint64_t> downloadSpeedSamples_;
    std::vector<uint64_t> installSpeedSamples_;
    uint64_t installSpeedSmoothed_ = 0;
    uint64_t lastInstallBytes_ = 0;
    uint64_t lastInstallSampleMs_ = 0;
    bool hasInstallSample_ = false;
};

struct FileEntry {
    std::string name;
    std::string path;
    bool directory = false;
};

class FilePickerActivity;

class FileDataSource : public brls::RecyclerDataSource {
public:
    explicit FileDataSource(FilePickerActivity* owner) : owner_(owner) {}
    int numberOfRows(brls::RecyclerFrame*, int) override;
    brls::RecyclerCell* cellForRow(brls::RecyclerFrame*,
                                    brls::IndexPath) override;
    void didSelectRowAt(brls::RecyclerFrame*, brls::IndexPath) override;

private:
    FilePickerActivity* owner_;
};

class FileCell : public brls::RecyclerCell {
public:
    FileCell() {
        setFocusable(true);
        setHeight(64);
        setPadding(12, 24, 12, 24);
        label_ = new brls::Label();
        label_->setSingleLine(true);
        label_->setFontSize(21);
        addView(label_);
    }
    void setEntry(const FileEntry& entry) {
        label_->setText(entry.directory ? "[Folder]  " + entry.name
                                        : entry.name);
    }
private:
    brls::Label* label_;
};

class FilePickerActivity : public brls::Activity {
public:
    explicit FilePickerActivity(DownloadManager* manager)
        : manager_(manager), currentPath_("sdmc:/") {
        recycler_ = new brls::RecyclerFrame();
        recycler_->setPadding(8, 32, 8, 32);
        recycler_->estimatedRowHeight = 64;
        recycler_->registerCell("File", [] { return new FileCell(); });
        recycler_->setDataSource(new FileDataSource(this));
        frame_ = new brls::AppletFrame(recycler_);
        loadDirectory(currentPath_);
    }

    brls::View* createContentView() override {
        return frame_;
    }

    const std::vector<FileEntry>& entries() const { return entries_; }

    void select(size_t index) {
        if (index >= entries_.size())
            return;
        const FileEntry entry = entries_[index];
        if (entry.directory) {
            brls::sync([this, path = entry.path] {
                loadDirectory(path);
            });
            return;
        }

        pipensx::TorrentPreview preview;
        std::string error;
        if (!DownloadManager::previewTorrent(entry.path, preview, error)) {
            brls::Application::notify(error);
            return;
        }
        std::string text = preview.name + "\n" +
                           formatBytes(preview.totalBytes) + "   " +
                           std::to_string(preview.fileCount) + " files   " +
                           std::to_string(preview.trackerCount) + " trackers";
        if (preview.packageCount)
            text += "\n" + std::to_string(preview.packageCount) +
                    " installable NSP/NSZ package(s)";
        auto* dialog = new brls::Dialog(text);
        auto add = [this, path = entry.path](TransferMode mode) {
            std::string id;
            std::string error;
            if (manager_->importTorrent(path, mode, id, error)) {
                brls::Application::notify("Torrent added to the queue.");
                brls::Application::popActivity();
            } else {
                brls::Application::notify(error);
            }
        };
        if (preview.packageCount) {
            dialog->addButton("Install to SD while downloading",
                [add] { add(TransferMode::StreamInstall); });
            dialog->addButton("Download only",
                [add] { add(TransferMode::DownloadOnly); });
        } else {
            dialog->addButton("Add to queue",
                [add] { add(TransferMode::DownloadOnly); });
        }
        dialog->addButton("Cancel", [] {});
        dialog->open();
    }

private:
    static bool hasTorrentExtension(const std::string& name) {
        if (name.size() < 8)
            return false;
        std::string extension = name.substr(name.size() - 8);
        std::transform(extension.begin(), extension.end(), extension.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        return extension == ".torrent";
    }

    static std::string parentPath(const std::string& path) {
        if (path == "sdmc:/" || path == "/")
            return path;
        std::string trimmed = path;
        while (trimmed.size() > 1 && trimmed.back() == '/')
            trimmed.pop_back();
        size_t slash = trimmed.find_last_of('/');
        if (slash == std::string::npos)
            return "sdmc:/";
        return trimmed.substr(0, slash + 1);
    }

    void loadDirectory(const std::string& path) {
        DIR* dir = opendir(path.c_str());
        if (!dir) {
            brls::Application::notify("Unable to open this directory.");
            return;
        }
        std::vector<FileEntry> directories;
        std::vector<FileEntry> files;
        while (dirent* item = readdir(dir)) {
            if (std::strcmp(item->d_name, ".") == 0 ||
                std::strcmp(item->d_name, "..") == 0)
                continue;
            std::string child = path;
            if (!child.empty() && child.back() != '/')
                child += '/';
            child += item->d_name;
            struct stat st {};
            if (stat(child.c_str(), &st) != 0)
                continue;
            if (S_ISDIR(st.st_mode))
                directories.push_back({item->d_name, child, true});
            else if (hasTorrentExtension(item->d_name))
                files.push_back({item->d_name, child, false});
        }
        closedir(dir);
        auto byName = [](const FileEntry& a, const FileEntry& b) {
            return a.name < b.name;
        };
        std::sort(directories.begin(), directories.end(), byName);
        std::sort(files.begin(), files.end(), byName);
        entries_.clear();
        if (path != "sdmc:/" && path != "/")
            entries_.push_back({"..", parentPath(path), true});
        entries_.insert(entries_.end(), directories.begin(), directories.end());
        entries_.insert(entries_.end(), files.begin(), files.end());
        currentPath_ = path;
        frame_->setTitle("Select .torrent   " + currentPath_);
        reloadRecycler();
    }

    void reloadRecycler() {
        brls::View* focused = brls::Application::getCurrentFocus();
        bool ownsFocus = focused && recycler_->getParentActivity() &&
                         focused->getParentActivity() ==
                             recycler_->getParentActivity();
        if (ownsFocus) {
            recycler_->setFocusable(true);
            brls::Application::giveFocus(recycler_);
        }
        recycler_->setDefaultCellFocus(brls::IndexPath(0, 0));
        recycler_->reloadData();
        if (ownsFocus) {
            recycler_->setFocusable(false);
            brls::Application::giveFocus(recycler_);
        }
    }

    DownloadManager* manager_;
    std::string currentPath_;
    std::vector<FileEntry> entries_;
    brls::RecyclerFrame* recycler_;
    brls::AppletFrame* frame_;

    friend class FileDataSource;
};

int FileDataSource::numberOfRows(brls::RecyclerFrame*, int) {
    return static_cast<int>(owner_->entries().size());
}

brls::RecyclerCell* FileDataSource::cellForRow(
    brls::RecyclerFrame* recycler, brls::IndexPath index) {
    auto* cell = static_cast<FileCell*>(
        recycler->dequeueReusableCell("File"));
    cell->setEntry(owner_->entries()[index.row]);
    return cell;
}

void FileDataSource::didSelectRowAt(brls::RecyclerFrame*,
                                    brls::IndexPath index) {
    owner_->select(static_cast<size_t>(index.row));
}

class MainView : public brls::Box {
public:
    MainView(DownloadManager* manager, GameMetadataService* metadata,
             AppSettings* settings)
        : brls::Box(brls::Axis::COLUMN), manager_(manager), metadata_(metadata),
          settings_(settings) {
        recycler_ = new brls::RecyclerFrame();
        recycler_->setGrow(1);
        recycler_->setPadding(6, 32, 6, 32);
        recycler_->estimatedRowHeight = 108;
        recycler_->registerCell("Download", [] { return new DownloadCell(); });
        recycler_->registerCell("Message", [] { return new MessageCell(); });
        dataSource_ = new DownloadDataSource(this);
        recycler_->setDataSource(dataSource_);
        addView(recycler_);
        refresh();
        timer_.setCallback([this] {
            refresh();
            if (fastRefresh_) {
                fastRefresh_ = false;
                timer_.setPeriod(750);
            }
        });
        registerAction("Import torrent", brls::BUTTON_X, [this](brls::View*) {
            openFilePicker();
            return true;
        });
        startRefreshing();
    }

    ~MainView() override {
        timer_.stop();
    }

    void openDetails(const std::string& taskId) {
        brls::Application::pushActivity(
            new DetailsActivity(taskId, manager_));
    }

    void openFilePicker() {
        brls::Application::pushActivity(new FilePickerActivity(manager_));
    }

    void startRefreshing(bool fast = false) {
        timer_.stop();
        fastRefresh_ = fast;
        timer_.start(fast ? 100 : 750);
    }

    void stopRefreshing() {
        timer_.stop();
    }

    GameMetadataService* metadataService() const { return metadata_; }

private:
    void refresh() {
        auto next = manager_->snapshot();
        if (settings_ && !settings_->get().showCompletedDownloads) {
            next.erase(std::remove_if(next.begin(), next.end(),
                [](const DownloadTask& task) {
                    return task.status == DownloadStatus::Completed ||
                           task.status == DownloadStatus::Installed;
                }), next.end());
        }
        uint64_t settingsGeneration = settings_ ? settings_->generation() : 0;
        bool settingsChanged = settingsGeneration != settingsGeneration_;
        bool structureChanged = !initialized_ || settingsChanged ||
                                next.size() != tasks_.size();
        bool changed = structureChanged;
        if (!structureChanged) {
            for (size_t i = 0; i < next.size(); ++i) {
                if (next[i].id != tasks_[i].id ||
                    next[i].status != tasks_[i].status) {
                    structureChanged = true;
                    changed = true;
                    break;
                }
                if (next[i].completedBytes != tasks_[i].completedBytes ||
                    next[i].speedBytesPerSecond !=
                        tasks_[i].speedBytesPerSecond ||
                    next[i].peers != tasks_[i].peers) {
                    changed = true;
                    break;
                }
                if (next[i].packagesInstalled !=
                        tasks_[i].packagesInstalled ||
                    next[i].installedBytes != tasks_[i].installedBytes ||
                    next[i].currentPackage != tasks_[i].currentPackage) {
                    changed = true;
                    break;
                }
            }
        }
        if (!changed)
            return;
        float offset = recycler_->getContentOffsetY();
        brls::View* focused = brls::Application::getCurrentFocus();
        bool ownsFocus = focused && recycler_->getParentActivity() &&
                         focused->getParentActivity() ==
                             recycler_->getParentActivity();
        auto* focusedCell = ownsFocus
            ? dynamic_cast<brls::RecyclerCell*>(focused)
            : nullptr;
        std::string focusedTaskId;
        if (focusedCell)
            focusedTaskId =
                dataSource_->taskIdAt(focusedCell->getIndexPath());
        tasks_ = std::move(next);
        settingsGeneration_ = settingsGeneration;
        initialized_ = true;
        dataSource_->setTasks(tasks_);
        recycler_->setDefaultCellFocus(
            dataSource_->indexForTask(focusedTaskId));
        if (ownsFocus) {
            recycler_->setFocusable(true);
            brls::Application::giveFocus(recycler_);
        }
        recycler_->reloadData();
        if (ownsFocus) {
            recycler_->setFocusable(false);
            brls::Application::giveFocus(recycler_);
        }
        if (!structureChanged)
            recycler_->setContentOffsetY(offset, false);
    }

    DownloadManager* manager_;
    GameMetadataService* metadata_;
    AppSettings* settings_;
    brls::RecyclerFrame* recycler_;
    DownloadDataSource* dataSource_;
    brls::RepeatingTimer timer_;
    std::vector<DownloadTask> tasks_;
    bool initialized_ = false;
    bool fastRefresh_ = false;
    uint64_t settingsGeneration_ = 0;
};

void DownloadDataSource::setTasks(std::vector<DownloadTask> tasks) {
    sections_.clear();
    const struct {
        const char* title;
        bool (*matches)(DownloadStatus);
    } groups[] = {
        {"Active", [](DownloadStatus s) {
            return s == DownloadStatus::Checking ||
                   s == DownloadStatus::Downloading ||
                   s == DownloadStatus::Installing ||
                   s == DownloadStatus::Committing ||
                   s == DownloadStatus::Verifying;
        }},
        {"Queue", [](DownloadStatus s) {
            return s == DownloadStatus::Queued;
        }},
        {"Paused", [](DownloadStatus s) {
            return s == DownloadStatus::Paused;
        }},
        {"Completed", [](DownloadStatus s) {
            return s == DownloadStatus::Completed ||
                   s == DownloadStatus::Installed;
        }},
        {"Errors", [](DownloadStatus s) {
            return s == DownloadStatus::Error;
        }},
    };
    for (const auto& group : groups) {
        Section section;
        section.title = group.title;
        for (const auto& task : tasks)
            if (group.matches(task.status))
                section.tasks.push_back(task);
        if (!section.tasks.empty())
            sections_.push_back(std::move(section));
    }
    if (sections_.empty())
        sections_.push_back({"", {}});
}

std::string DownloadDataSource::taskIdAt(brls::IndexPath index) const {
    if (index.section >= sections_.size())
        return {};
    const Section& section = sections_[index.section];
    if (index.row < 0 ||
        static_cast<size_t>(index.row) >= section.tasks.size())
        return {};
    return section.tasks[static_cast<size_t>(index.row)].id;
}

brls::IndexPath DownloadDataSource::indexForTask(
    const std::string& taskId) const {
    if (!taskId.empty()) {
        for (size_t section = 0; section < sections_.size(); ++section) {
            for (size_t row = 0; row < sections_[section].tasks.size(); ++row) {
                if (sections_[section].tasks[row].id == taskId)
                    return brls::IndexPath(section, row);
            }
        }
    }
    return brls::IndexPath(0, 0);
}

int DownloadDataSource::numberOfSections(brls::RecyclerFrame*) {
    return static_cast<int>(sections_.size());
}

int DownloadDataSource::numberOfRows(brls::RecyclerFrame*, int section) {
    return sections_[section].tasks.empty()
        ? 1
        : static_cast<int>(sections_[section].tasks.size());
}

std::string DownloadDataSource::titleForHeader(brls::RecyclerFrame*,
                                                int section) {
    return sections_[section].title;
}

brls::RecyclerCell* DownloadDataSource::cellForRow(
    brls::RecyclerFrame* recycler, brls::IndexPath index) {
    Section& section = sections_[index.section];
    if (section.tasks.empty())
        return recycler->dequeueReusableCell("Message");
    auto* cell = static_cast<DownloadCell*>(
        recycler->dequeueReusableCell("Download"));
    cell->setTask(section.tasks[index.row], owner_->metadataService());
    return cell;
}

void DownloadDataSource::didSelectRowAt(brls::RecyclerFrame*,
                                         brls::IndexPath index) {
    Section& section = sections_[index.section];
    if (!section.tasks.empty())
        owner_->openDetails(section.tasks[index.row].id);
    else
        owner_->openFilePicker();
}

// ---------------------------------------------------------------------------
// RuTracker catalog tab
// ---------------------------------------------------------------------------

std::string formatCatalogDate(int64_t timestamp) {
    if (timestamp <= 0)
        return "Unknown date";
    std::time_t value = static_cast<std::time_t>(timestamp);
    std::tm result{};
    localtime_r(&value, &result);
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d", &result);
    return buffer;
}

class CatalogCell : public brls::RecyclerCell {
public:
    CatalogCell() {
        setFocusable(true);
        setAxis(brls::Axis::ROW);
        setAlignItems(brls::AlignItems::CENTER);
        setPadding(10, 18, 10, 18);
        setHeight(96);

        mark_ = new brls::Label();
        mark_->setWidth(0);
        mark_->setFontSize(21);
        mark_->setTextColor(nvgRGB(0, 195, 227));
        addView(mark_);

        // Left: rounded box-art thumbnail. The box's background color is the
        // placeholder shown until art loads (or for entries without metadata).
        thumb_ = new brls::Box();
        thumb_->setWidth(64);
        thumb_->setHeight(64);
        thumb_->setCornerRadius(6);
        thumb_->setBackgroundColor(nvgRGB(58, 58, 66));
        thumb_->setMarginRight(16);
        thumb_->setAlignItems(brls::AlignItems::CENTER);
        thumb_->setJustifyContent(brls::JustifyContent::CENTER);
        placeholder_ = new brls::Label();
        placeholder_->setFontSize(28);
        placeholder_->setTextColor(nvgRGB(185, 185, 195));
        thumb_->addView(placeholder_);
        image_ = new AsyncRgbaImage();
        image_->setWidth(64);
        image_->setHeight(64);
        image_->setPositionType(brls::PositionType::ABSOLUTE);
        image_->setPositionTop(0);
        image_->setPositionLeft(0);
        image_->setCornerRadius(6);
        image_->setScalingType(brls::ImageScalingType::FILL);
        thumb_->addView(image_);
        addView(thumb_);

        // Right: title + state on top, content classification underneath.
        auto* right = new brls::Box(brls::Axis::COLUMN);
        right->setGrow(1);
        right->setJustifyContent(brls::JustifyContent::CENTER);

        auto* top = new brls::Box(brls::Axis::ROW);
        top->setAlignItems(brls::AlignItems::CENTER);
        title_ = new brls::Label();
        title_->setSingleLine(true);
        title_->setFontSize(21);
        title_->setGrow(1);
        badge_ = new brls::Label();
        badge_->setSingleLine(true);
        badge_->setFontSize(16);
        badge_->setMarginLeft(12);
        badge_->setTextColor(nvgRGB(0, 195, 227));
        top->addView(title_);
        top->addView(badge_);

        sub_ = new brls::Label();
        sub_->setSingleLine(true);
        sub_->setFontSize(16);
        sub_->setMarginTop(6);
        sub_->setTextColor(nvgRGB(160, 160, 170));

        right->addView(top);
        right->addView(sub_);
        addView(right);
    }

    void setEntry(const CatalogEntry& entry, const std::string& stateBadge,
                  const std::string& iconUrl, GameMetadataService* service,
                  bool selectionMode, bool selected, bool selectable) {
        mark_->setWidth(selectionMode ? 42 : 0);
        mark_->setMarginRight(selectionMode ? 8 : 0);
        mark_->setText(!selectionMode ? "" : !selectable ? "[-]"
                                             : selected ? "[x]" : "[ ]");
        mark_->setTextColor(selectable ? nvgRGB(0, 195, 227)
                                       : nvgRGB(115, 115, 125));
        title_->setText(entry.title);
        placeholder_->setText(placeholderLetter(entry.title));
        badge_->setText(stateBadge);
        std::string sub = entry.size ? formatBytes(entry.size) : "Unknown size";
        sub += "   " + formatCatalogDate(entry.publishedAt);
        sub_->setText(sub);
        setArtworkUrl(image_, service, iconUrl, currentIconUrl_, imageState_);
    }

private:
    brls::Label* mark_;
    brls::Box* thumb_;
    brls::Label* placeholder_;
    AsyncRgbaImage* image_;
    brls::Label* title_;
    brls::Label* badge_;
    brls::Label* sub_;
    std::string currentIconUrl_;
    std::shared_ptr<ImageRequestState> imageState_ =
        std::make_shared<ImageRequestState>();
};

class TextMessageCell : public brls::RecyclerCell {
public:
    TextMessageCell() {
        setFocusable(true);
        setHeight(100);
        setPadding(24);
        label_ = new brls::Label();
        label_->setFontSize(20);
        addView(label_);
    }
    void setMessage(const std::string& text) { label_->setText(text); }

private:
    brls::Label* label_;
};

class CatalogView;

class CatalogDataSource : public brls::RecyclerDataSource {
public:
    explicit CatalogDataSource(CatalogView* owner) : owner_(owner) {}

    void setEntries(std::vector<CatalogEntry> entries,
                    std::vector<std::string> stateBadges,
                    std::vector<std::string> gameNames,
                    std::vector<std::string> iconUrls,
                    std::vector<uint8_t> selected,
                    std::vector<uint8_t> selectable,
                    GameMetadataService* metadata,
                    bool selectionMode) {
        entries_ = std::move(entries);
        stateBadges_ = std::move(stateBadges);
        gameNames_ = std::move(gameNames);
        iconUrls_ = std::move(iconUrls);
        selected_ = std::move(selected);
        selectable_ = std::move(selectable);
        metadata_ = metadata;
        selectionMode_ = selectionMode;
    }
    void setMessage(const std::string& message) { message_ = message; }
    const CatalogEntry* entryAt(int row) const {
        if (row < 0 || static_cast<size_t>(row) >= entries_.size())
            return nullptr;
        return &entries_[static_cast<size_t>(row)];
    }
    const std::vector<CatalogEntry>& entries() const { return entries_; }
    bool selectableAt(int row) const {
        return row >= 0 && static_cast<size_t>(row) < selectable_.size() &&
               selectable_[static_cast<size_t>(row)] != 0;
    }

    int numberOfRows(brls::RecyclerFrame*, int) override {
        return entries_.empty() ? 1 : static_cast<int>(entries_.size());
    }
    brls::RecyclerCell* cellForRow(brls::RecyclerFrame* recycler,
                                    brls::IndexPath index) override {
        if (entries_.empty()) {
            auto* cell = static_cast<TextMessageCell*>(
                recycler->dequeueReusableCell("Message"));
            cell->setMessage(message_);
            return cell;
        }
        auto* cell = static_cast<CatalogCell*>(
            recycler->dequeueReusableCell("Catalog"));
        size_t row = static_cast<size_t>(index.row);
        CatalogEntry display = entries_[row];
        if (row < gameNames_.size() && !gameNames_[row].empty())
            display.title = gameNames_[row];
        cell->setEntry(display,
                       row < stateBadges_.size() ? stateBadges_[row]
                                                 : std::string(),
                       row < iconUrls_.size() ? iconUrls_[row] : std::string(),
                       metadata_, selectionMode_,
                       row < selected_.size() && selected_[row] != 0,
                       row < selectable_.size() && selectable_[row] != 0);
        return cell;
    }
    void didSelectRowAt(brls::RecyclerFrame*, brls::IndexPath index) override;

private:
    CatalogView* owner_;
    std::vector<CatalogEntry> entries_;
    std::vector<std::string> stateBadges_;
    std::vector<std::string> gameNames_;
    std::vector<std::string> iconUrls_;
    std::vector<uint8_t> selected_;
    std::vector<uint8_t> selectable_;
    GameMetadataService* metadata_ = nullptr;
    std::string message_;
    bool selectionMode_ = false;
};

// ---------------------------------------------------------------------------
// Shared catalog helpers (used by both the list and the detail page)
// ---------------------------------------------------------------------------

std::string catalogLower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) {
                       return static_cast<char>(std::tolower(c));
                   });
    return value;
}

std::string classifyResolveFailure(const std::string& error) {
    std::string lower = catalogLower(error);
    if (lower.find("not registered") != std::string::npos ||
        lower.find("stale") != std::string::npos)
        return "Stale";
    if (lower.find("metadata") != std::string::npos)
        return "No metadata";
    if (lower.find("no usable peers") != std::string::npos ||
        lower.find("no peers") != std::string::npos)
        return "No peers";
    return "Resolve failed";
}

std::string badgeForCatalogHealth(const CatalogEntry& entry) {
    switch (entry.health) {
        case pipensx::CatalogHealth::Ok:
            return entry.metadataOk ? "Fresh" : "Checked";
        case pipensx::CatalogHealth::NoPeers:
            return "No peers";
        case pipensx::CatalogHealth::MetadataTimeout:
            return "No metadata";
        case pipensx::CatalogHealth::TrackerNotRegistered:
        case pipensx::CatalogHealth::Dead:
            return "Dead";
        case pipensx::CatalogHealth::Replaced:
            return "Replaced";
        case pipensx::CatalogHealth::Unknown:
            break;
    }
    return entry.catalogGeneratedAt || entry.sourceUpdatedAt
         ? "Unchecked" : std::string();
}

std::string joinStrings(const std::vector<std::string>& values,
                        const char* separator) {
    std::string out;
    for (const std::string& value : values) {
        if (value.empty())
            continue;
        if (!out.empty())
            out += separator;
        out += value;
    }
    return out;
}

std::string shortDescription(const std::string& value) {
    if (value.size() <= 900)
        return value;
    return value.substr(0, 900) + "...";
}

// Append a freshly created async image to a box (banner / screenshot on the
// detail page). Reuses loadImageInto for the disk-cached fetch.
void appendAsyncImage(brls::Box* parent, GameMetadataService* service,
                      const std::string& url, float height) {
    if (!service || url.empty())
        return;
    auto* image = new AsyncRgbaImage();
    image->setHeight(height);
    image->setMarginBottom(12);
    image->setAlignSelf(brls::AlignSelf::CENTER);
    image->setScalingType(brls::ImageScalingType::FIT);
    loadImageInto(image, service, url);
    parent->addView(image);
}

struct TorrentSelectionEntry {
    std::string path;
    uint64_t length = 0;
    bool package = false;
    bool selected = true;
};

class TorrentSelectionCell : public brls::RecyclerCell {
public:
    TorrentSelectionCell() {
        setFocusable(true);
        setAxis(brls::Axis::ROW);
        setAlignItems(brls::AlignItems::CENTER);
        setPadding(12, 20, 12, 20);
        setHeight(82);

        mark_ = new brls::Label();
        mark_->setWidth(34);
        mark_->setFontSize(21);
        mark_->setTextColor(nvgRGB(0, 195, 227));
        mark_->setMarginRight(10);
        addView(mark_);

        auto* body = new brls::Box(brls::Axis::COLUMN);
        body->setGrow(1);
        body->setJustifyContent(brls::JustifyContent::CENTER);

        title_ = new brls::Label();
        title_->setSingleLine(true);
        title_->setFontSize(18);
        body->addView(title_);

        meta_ = new brls::Label();
        meta_->setFontSize(14);
        meta_->setMarginTop(4);
        meta_->setTextColor(nvgRGB(160, 160, 170));
        body->addView(meta_);

        addView(body);
    }

    void setEntry(const TorrentSelectionEntry& entry) {
        mark_->setText(entry.selected ? "[x]" : "[ ]");
        title_->setText(entry.path);
        std::string meta = formatBytes(entry.length);
        meta += "   ";
        meta += entry.package ? "Package file" : "Other file";
        meta += entry.selected ? "   Install" : "   Skip";
        meta_->setText(meta);
    }

private:
    brls::Label* mark_;
    brls::Label* title_;
    brls::Label* meta_;
};

class TorrentSelectionActivity;

class TorrentSelectionDataSource : public brls::RecyclerDataSource {
public:
    explicit TorrentSelectionDataSource(TorrentSelectionActivity* owner)
        : owner_(owner) {}

    void setEntries(std::vector<TorrentSelectionEntry> entries) {
        entries_ = std::move(entries);
    }

    void setAll(bool selected) {
        for (auto& entry : entries_)
            entry.selected = selected;
    }

    size_t selectedCount() const {
        size_t count = 0;
        for (const auto& entry : entries_)
            if (entry.selected)
                ++count;
        return count;
    }

    size_t selectedPackageCount() const {
        size_t count = 0;
        for (const auto& entry : entries_)
            if (entry.selected && entry.package)
                ++count;
        return count;
    }

    std::vector<uint8_t> selectionMask() const {
        std::vector<uint8_t> mask;
        bool allSelected = true;
        mask.reserve(entries_.size());
        for (const auto& entry : entries_) {
            mask.push_back(entry.selected ? 1 : 0);
            if (!entry.selected)
                allSelected = false;
        }
        return allSelected ? std::vector<uint8_t>() : mask;
    }

    int numberOfSections(brls::RecyclerFrame*) override {
        return entries_.empty() ? 1 : 1;
    }

    int numberOfRows(brls::RecyclerFrame*, int) override {
        return entries_.empty() ? 1 : static_cast<int>(entries_.size());
    }

    brls::RecyclerCell* cellForRow(brls::RecyclerFrame* recycler,
                                   brls::IndexPath index) override;

    void didSelectRowAt(brls::RecyclerFrame*, brls::IndexPath index) override;

    const TorrentSelectionEntry* entryAt(int row) const {
        if (row < 0 || static_cast<size_t>(row) >= entries_.size())
            return nullptr;
        return &entries_[static_cast<size_t>(row)];
    }

private:
    TorrentSelectionActivity* owner_;
    std::vector<TorrentSelectionEntry> entries_;
};

class TorrentSelectionActivity : public brls::Activity {
public:
    friend class TorrentSelectionDataSource;

    TorrentSelectionActivity(DownloadManager* manager, std::string path,
                             pipensx::TorrentPreview preview,
                             TransferMode preferred,
                             StreamSelection initialSelection)
        : manager_(manager), path_(std::move(path)),
          preview_(std::move(preview)), preferred_(preferred),
          initialSelection_(initialSelection) {
        auto* content = new brls::Box(brls::Axis::COLUMN);
        content->setGrow(1);
        content->setPadding(24, 38, 24, 34);
        content->setBackgroundColor(nvgRGBA(35, 35, 40, 235));
        content->setCornerRadius(12);

        title_ = new brls::Label();
        title_->setFontSize(26);
        title_->setText("Choose torrent files");
        content->addView(title_);

        summary_ = new brls::Label();
        summary_->setFontSize(15);
        summary_->setMarginTop(8);
        summary_->setMarginBottom(14);
        summary_->setTextColor(nvgRGB(170, 170, 180));
        summary_->setText("A toggles a file. Selection follows Settings.");
        content->addView(summary_);

        recycler_ = new brls::RecyclerFrame();
        recycler_->setGrow(1);
        recycler_->setPadding(6, 0, 6, 0);
        recycler_->estimatedRowHeight = 82;
        recycler_->registerCell("FileSelect",
                               [] { return new TorrentSelectionCell(); });
        dataSource_ = new TorrentSelectionDataSource(this);
        recycler_->setDataSource(dataSource_);
        content->addView(recycler_);

        auto* buttons = new brls::Box(brls::Axis::COLUMN);
        buttons->setMarginTop(16);

        auto* row = new brls::Box(brls::Axis::ROW);
        row->setMarginBottom(10);

        selectAll_ = new brls::Button();
        selectAll_->setStyle(&brls::BUTTONSTYLE_DEFAULT);
        selectAll_->setFontSize(18);
        selectAll_->setHeight(52);
        selectAll_->setMarginRight(10);
        selectAll_->setGrow(1);
        selectAll_->setText("Select all");
        selectAll_->registerClickAction([this](brls::View*) {
            setAllSelected(true);
            return true;
        });
        row->addView(selectAll_);

        clearAll_ = new brls::Button();
        clearAll_->setStyle(&brls::BUTTONSTYLE_DEFAULT);
        clearAll_->setFontSize(18);
        clearAll_->setHeight(52);
        clearAll_->setGrow(1);
        clearAll_->setText("Clear");
        clearAll_->registerClickAction([this](brls::View*) {
            setAllSelected(false);
            return true;
        });
        row->addView(clearAll_);

        buttons->addView(row);

        installSelected_ = new brls::Button();
        installSelected_->setStyle(&brls::BUTTONSTYLE_PRIMARY);
        installSelected_->setFontSize(21);
        installSelected_->setHeight(60);
        installSelected_->setText("Continue");
        installSelected_->registerClickAction([this](brls::View*) {
            confirmSelection();
            return true;
        });
        buttons->addView(installSelected_);

        content->addView(buttons);
        frame_ = new brls::AppletFrame(content);
        frame_->setTitle(preview_.name.empty() ? "Select files"
                                              : preview_.name);

        populateEntries();
        refreshSummary();
    }

    ~TorrentSelectionActivity() override {
        if (!finished_ && !path_.empty())
            ::unlink(path_.c_str());
    }

    brls::View* createContentView() override {
        return frame_;
    }

    void onContentAvailable() override {
        registerAction("Select all", brls::BUTTON_X, [this](brls::View*) {
            setAllSelected(true);
            return true;
        });
        registerAction("Clear", brls::BUTTON_Y, [this](brls::View*) {
            setAllSelected(false);
            return true;
        });
        registerAction("Install", brls::BUTTON_RB,
                       [this](brls::View*) {
                           confirmSelection();
                           return true;
                       });
    }

private:
    void populateEntries() {
        std::vector<TorrentSelectionEntry> entries;
        entries.reserve(preview_.files.size());
        for (const auto& file : preview_.files) {
            TorrentSelectionEntry entry;
            entry.path = file.path;
            entry.length = file.length;
            entry.package = file.package;
            entry.selected = initialSelection_ == StreamSelection::AllFiles ||
                             preferred_ != TransferMode::StreamInstall ||
                             file.package;
            entries.push_back(std::move(entry));
        }
        dataSource_->setEntries(std::move(entries));
        recycler_->reloadData();
    }

    void setAllSelected(bool selected) {
        dataSource_->setAll(selected);
        recycler_->reloadData();
        refreshSummary();
    }

    void refreshSummary() {
        size_t selected = dataSource_->selectedCount();
        size_t selectedPackages = dataSource_->selectedPackageCount();
        std::vector<uint8_t> mask = dataSource_->selectionMask();
        const TransferMode mode = selectedPackages > 0 &&
                                  preferred_ == TransferMode::StreamInstall
            ? TransferMode::StreamInstall
            : TransferMode::DownloadOnly;
        const auto estimate = pipensx::estimateInstallSpace(preview_, mask,
                                                            mode);
        const StorageSpaceSnapshot storage =
            pipensx::queryStorageSpace(manager_->rootPath());
        const auto check = pipensx::assessInstallSpace(estimate, storage);
        std::string text = std::to_string(selected) + " / " +
                           std::to_string(preview_.files.size()) +
                           " files   " + formatBytes(estimate.requiredBytes);
        text += storage.available
            ? "   SD free: " + formatBytes(storage.freeBytes)
            : "   SD free: unavailable";
        if (selectedPackages > 0) {
            text += "   " + std::to_string(selectedPackages) +
                    " package(s)";
        } else {
            text += "   Download-only selection";
        }
        if (estimate.certainty ==
            SpaceEstimateCertainty::CompressedUnknown) {
            text += "   NSZ may expand";
        }
        if (check.status == InstallSpaceCheckStatus::Insufficient)
            text += "   Need " + formatBytes(check.shortfallBytes) + " more";
        summary_->setText(text);
        installSelected_->setState(selected == 0 || estimate.overflow ||
                                    check.status ==
                                        InstallSpaceCheckStatus::Insufficient
            ? brls::ButtonState::DISABLED
            : brls::ButtonState::ENABLED);
    }

    void confirmSelection() {
        std::vector<uint8_t> mask = dataSource_->selectionMask();
        if (mask.empty() && preview_.files.empty()) {
            brls::Application::notify("No files found in this torrent.");
            return;
        }
        size_t selectedPackages = dataSource_->selectedPackageCount();
        if (!mask.empty()) {
            bool anySelected = false;
            for (uint8_t value : mask) {
                if (value) {
                    anySelected = true;
                    break;
                }
            }
            if (!anySelected) {
                brls::Application::notify("Select at least one file.");
                return;
            }
        }

        TransferMode mode = (selectedPackages > 0 &&
                             preferred_ == TransferMode::StreamInstall)
            ? TransferMode::StreamInstall
            : TransferMode::DownloadOnly;
        const auto estimate = pipensx::estimateInstallSpace(preview_, mask,
                                                            mode);
        const StorageSpaceSnapshot storage =
            pipensx::queryStorageSpace(manager_->rootPath());
        if (pipensx::assessInstallSpace(estimate, storage).status ==
            InstallSpaceCheckStatus::Insufficient) {
            refreshSummary();
            brls::Application::notify("Not enough free space on SD.");
            return;
        }
        std::string id;
        std::string error;
        if (!manager_->importTorrent(path_, mode, mask, id, error)) {
            brls::Application::notify(error);
            return;
        }
        ::unlink(path_.c_str());
        finished_ = true;
        brls::Application::notify(mode == TransferMode::StreamInstall
            ? "Added. Installing to SD..."
            : "Added to downloads.");
        brls::Application::popActivity();
    }

    DownloadManager* manager_;
    std::string path_;
    pipensx::TorrentPreview preview_;
    TransferMode preferred_;
    StreamSelection initialSelection_;
    brls::AppletFrame* frame_ = nullptr;
    brls::Label* title_ = nullptr;
    brls::Label* summary_ = nullptr;
    brls::RecyclerFrame* recycler_ = nullptr;
    TorrentSelectionDataSource* dataSource_ = nullptr;
    brls::Button* selectAll_ = nullptr;
    brls::Button* clearAll_ = nullptr;
    brls::Button* installSelected_ = nullptr;
    bool finished_ = false;
};

brls::RecyclerCell* TorrentSelectionDataSource::cellForRow(
    brls::RecyclerFrame* recycler, brls::IndexPath index) {
    if (entries_.empty()) {
        return recycler->dequeueReusableCell("FileSelect");
    }
    auto* cell = static_cast<TorrentSelectionCell*>(
        recycler->dequeueReusableCell("FileSelect"));
    cell->setEntry(entries_[static_cast<size_t>(index.row)]);
    return cell;
}

void TorrentSelectionDataSource::didSelectRowAt(brls::RecyclerFrame* recycler,
                                                brls::IndexPath index) {
    if (entries_.empty() ||
        index.row < 0 || static_cast<size_t>(index.row) >= entries_.size())
        return;
    entries_[static_cast<size_t>(index.row)].selected =
        !entries_[static_cast<size_t>(index.row)].selected;
    recycler->reloadData();
    if (owner_)
        owner_->refreshSummary();
}

// ---------------------------------------------------------------------------
// Full-screen game page (eShop-style detail + one-tap install)
// ---------------------------------------------------------------------------

class GameDetailActivity : public brls::Activity {
public:
    // hashLower + failure ("" clears) recorded back into the catalog; onChange
    // asks the catalog list to re-badge.
    using FailureCallback =
        std::function<void(const std::string&, const std::string&)>;
    using ChangeCallback = std::function<void()>;

    GameDetailActivity(CatalogEntry entry, std::string lastFailure,
                       DownloadManager* manager, GameMetadataService* metadata,
                       InstalledTitleService* installed, AppSettings* settings,
                       FailureCallback onFailure, ChangeCallback onChange)
        : entry_(std::move(entry)), lastFailure_(std::move(lastFailure)),
          manager_(manager), metadata_(metadata), installed_(installed),
          settings_(settings),
          onFailure_(std::move(onFailure)), onChange_(std::move(onChange)),
          alive_(std::make_shared<std::atomic<bool>>(true)),
          cancelled_(std::make_shared<std::atomic<bool>>(false)) {
        const GameMetadata* found = metadata_->findByInfoHash(entry_.infoHash);
        titleId_ = found ? found->titleId : std::string();

        auto* content = new brls::Box(brls::Axis::COLUMN);
        content->setPadding(24, 40, 24, 40);
        buildHeader(content, found);
        buildActions(content);
        buildBody(content, found);

        auto* scroll = new brls::ScrollingFrame();
        scroll->setGrow(1);
        scroll->setContentView(content);

        frame_ = new brls::AppletFrame(scroll);
        frame_->setTitle(found ? found->name : entry_.title);
    }

    ~GameDetailActivity() override {
        alive_->store(false);
        cancelled_->store(true);
        timer_.stop();
        if (onChange_)
            onChange_();  // refresh the row badge on the way back
    }

    brls::View* createContentView() override { return frame_; }

    void onContentAvailable() override {
        registerAction("Cancel", brls::BUTTON_Y, [this](brls::View*) {
            if (busy_)
                cancelled_->store(true);
            return true;
        });
        refreshButtons();
        timer_.setCallback([this] { refreshButtons(); });
        timer_.start(500);
        brls::Button* focus = primary_;
        if (focus)
            brls::Application::giveFocus(focus);
    }

private:
    void buildHeader(brls::Box* content, const GameMetadata* found) {
        if (found)
            appendAsyncImage(content, metadata_,
                             !found->bannerUrl.empty() ? found->bannerUrl
                                                       : found->iconUrl,
                             220);
        auto* title = new brls::Label();
        title->setFontSize(28);
        title->setText(found ? found->name : entry_.title);
        content->addView(title);

        if (found) {
            std::string facts;
            if (!found->publisher.empty())
                facts += found->publisher;
            if (!found->releaseDate.empty())
                facts += (facts.empty() ? "" : "   ") + found->releaseDate;
            std::string categories = joinStrings(found->categories, ", ");
            if (!categories.empty())
                facts += (facts.empty() ? "" : "   ") + categories;
            if (!facts.empty()) {
                auto* factLabel = new brls::Label();
                factLabel->setFontSize(16);
                factLabel->setMarginTop(10);
                factLabel->setTextColor(nvgRGB(190, 190, 200));
                factLabel->setText(facts);
                content->addView(factLabel);
            }
        }
    }

    void buildActions(brls::Box* content) {
        auto* actions = new brls::Box(brls::Axis::COLUMN);
        actions->setMarginTop(20);
        actions->setMarginBottom(8);

        auto* row = new brls::Box(brls::Axis::ROW);

        secondary_ = new brls::Button();
        secondary_->setStyle(&brls::BUTTONSTYLE_DEFAULT);
        secondary_->setFontSize(21);
        secondary_->setHeight(64);
        secondary_->setGrow(1);
        secondary_->setMarginRight(12);
        secondary_->setText("Options");
        secondary_->registerClickAction([this](brls::View*) {
            onSecondary();
            return true;
        });
        row->addView(secondary_);

        primary_ = new brls::Button();
        primary_->setStyle(&brls::BUTTONSTYLE_PRIMARY);
        primary_->setFontSize(21);
        primary_->setHeight(64);
        primary_->setGrow(1);
        primary_->setText("Install");
        primary_->registerClickAction([this](brls::View*) {
            onPrimary();
            return true;
        });
        row->addView(primary_);
        actions->addView(row);

        statusLabel_ = new brls::Label();
        statusLabel_->setFontSize(16);
        statusLabel_->setMarginTop(12);
        statusLabel_->setTextColor(nvgRGB(0, 195, 227));
        statusLabel_->setText("Install adds this game to your console.");
        actions->addView(statusLabel_);

        content->addView(actions);
    }

    void buildBody(brls::Box* content, const GameMetadata* found) {
        auto* release = new brls::Label();
        release->setFontSize(15);
        release->setMarginTop(4);
        release->setTextColor(nvgRGB(150, 150, 160));
        release->setText("Release: " + entry_.title);
        content->addView(release);

        if (found) {
            std::string text = !found->description.empty() ? found->description
                                                           : found->intro;
            if (!text.empty()) {
                auto* desc = new brls::Label();
                desc->setFontSize(17);
                desc->setMarginTop(16);
                desc->setText(shortDescription(text));
                content->addView(desc);
            }
        } else {
            auto* missing = new brls::Label();
            missing->setFontSize(17);
            missing->setMarginTop(16);
            missing->setText("No game artwork match yet. Install still works "
                             "from the catalog release.");
            content->addView(missing);
        }

        std::vector<std::string> screenshots =
            pipensx::mergeScreenshotUrls(found, entry_, 6);
        if (!screenshots.empty()) {
            auto* shots = new brls::Label();
            shots->setFontSize(16);
            shots->setMarginTop(18);
            shots->setMarginBottom(8);
            shots->setTextColor(nvgRGB(190, 190, 200));
            shots->setText("Screenshots");
            content->addView(shots);

            auto* rail = new brls::Box(brls::Axis::ROW);
            rail->setHeight(180);
            for (const std::string& url : screenshots) {
                auto* image = new AsyncRgbaImage();
                image->setWidth(300);
                image->setHeight(170);
                image->setMarginRight(12);
                image->setCornerRadius(6);
                image->setFocusable(true);
                image->setScalingType(brls::ImageScalingType::FIT);
                loadImageInto(image, metadata_, url);
                rail->addView(image);
            }
            auto* gallery = new brls::HScrollingFrame();
            gallery->setHeight(190);
            gallery->setContentView(rail);
            content->addView(gallery);
        }

        auto* size = new brls::Label();
        size->setFontSize(15);
        size->setMarginTop(16);
        size->setTextColor(nvgRGB(150, 150, 160));
        size->setText("Download size: " +
                      (entry_.size ? formatBytes(entry_.size)
                                   : std::string("Unknown")));
        content->addView(size);

        std::string warn;
        if (!lastFailure_.empty()) {
            warn = "Last attempt: " + lastFailure_;
        } else {
            std::string health = badgeForCatalogHealth(entry_);
            if (!health.empty() && health != "Fresh") {
                warn = "Catalog health: " + health;
                if (!entry_.healthReason.empty())
                    warn += " (" + entry_.healthReason + ")";
            }
        }
        if (!warn.empty()) {
            auto* warning = new brls::Label();
            warning->setFontSize(15);
            warning->setMarginTop(10);
            warning->setTextColor(nvgRGB(230, 150, 80));
            warning->setText(warn);
            content->addView(warning);
        }
    }

    // Find the managed task for this game, if any.
    const DownloadTask* currentTask() {
        cache_ = manager_->snapshot();
        std::string hash = catalogLower(entry_.infoHash);
        for (const DownloadTask& task : cache_)
            if (catalogLower(task.id) == hash)
                return &task;
        return nullptr;
    }

    static std::string installButtonLabel(const DownloadTask& task) {
        switch (task.status) {
            case DownloadStatus::Queued: return "In queue";
            case DownloadStatus::Checking:
            case DownloadStatus::Downloading: {
                return "Downloading " +
                       std::to_string(percentOf(progressOf(task))) + "%";
            }
            case DownloadStatus::Installing:
            case DownloadStatus::Committing: {
                return "Installing " +
                       std::to_string(percentOf(installProgressOf(task))) + "%";
            }
            case DownloadStatus::Verifying:
                return "Verifying " +
                       std::to_string(percentOf(progressOf(task))) + "%";
            case DownloadStatus::Paused: {
                int pct = percentOf(progressOf(task));
                return pct > 0 ? "Paused " + std::to_string(pct) + "%"
                               : "Paused";
            }
            case DownloadStatus::Completed:
                return "Downloaded 100%";
            case DownloadStatus::Installed:
                return "Installed 100%";
            case DownloadStatus::Error: {
                int pct = percentOf(progressOf(task));
                return pct > 0 ? "Error " + std::to_string(pct) + "%"
                               : "Error";
            }
            case DownloadStatus::Removing:
                return "Removing";
        }
        return "Install";
    }

    // Reflect live task state on the buttons. Skipped while resolving so the
    // inline progress text isn't clobbered.
    void refreshButtons() {
        if (busy_)
            return;
        const DownloadTask* task = currentTask();
        if (task) {
            operationMessage_.clear();
            primary_->setText(installButtonLabel(*task));
            primary_->setState(
                task->status == DownloadStatus::Paused ||
                task->status == DownloadStatus::Error
                ? brls::ButtonState::ENABLED
                : brls::ButtonState::DISABLED);
            if (task->status == DownloadStatus::Paused ||
                task->status == DownloadStatus::Error)
                primary_->setText("Resume");
            secondary_->setText("View download");
            secondary_->setState(brls::ButtonState::ENABLED);
            if (task->status == DownloadStatus::Error && !task->error.empty())
                statusLabel_->setText(task->error);
        } else {
            primary_->setText("Install");
            primary_->setState(brls::ButtonState::ENABLED);
            secondary_->setText("Options");
            secondary_->setState(brls::ButtonState::ENABLED);
            if (!operationMessage_.empty())
                statusLabel_->setText(operationMessage_);
            else if (installed_ && installed_->contains(titleId_))
                statusLabel_->setText(
                    "Installed on this console. You can still install updates or DLC.");
            else
                statusLabel_->setText(
                    "Install adds this game to your console.");
        }
    }

    void onPrimary() {
        if (busy_)
            return;
        const DownloadTask* task = currentTask();
        if (task) {
            if (task->status == DownloadStatus::Paused ||
                task->status == DownloadStatus::Error)
                manager_->resume(task->id);
            return;
        }
        // One-tap install: resolve, then queue silently (picker only on Options).
        startInstall(false);
    }

    void onSecondary() {
        if (busy_)
            return;
        const DownloadTask* task = currentTask();
        if (task) {
            brls::Application::pushActivity(
                new DetailsActivity(task->id, manager_));
            return;
        }
        // Options: always open the per-file picker after resolve.
        startInstall(true);
    }

    // One-tap: resolve the magnet inline, then import immediately (no second
    // dialog) unless forcePicker is set (the "Options" path), which always
    // opens the per-file selection screen after resolve.
    void startInstall(bool forcePicker) {
        if (busy_)
            return;
        busy_ = true;
        operationMessage_.clear();
        cancelled_->store(false);
        primary_->setState(brls::ButtonState::DISABLED);
        secondary_->setState(brls::ButtonState::DISABLED);
        primary_->setText("Resolving...");
        statusLabel_->setText("Finding peers...   (Y to cancel)");

        auto alive = alive_;
        auto cancelled = cancelled_;
        uint32_t serial = gCatalogTempSerial.fetch_add(1);
        std::string tmp = manager_->rootPath() + "/_catalog_tmp_" +
                          catalogLower(entry_.infoHash) + "_" +
                          std::to_string(serial) + ".torrent";
        std::string magnet = entry_.magnetUri;
        std::string telemetryTag = catalogLower(entry_.infoHash);
        uint64_t startedMs = now_ms();
        brls::async([this, alive, cancelled, magnet, tmp, forcePicker,
                     telemetryTag, startedMs] {
            std::string err;
            MagnetResolver resolver;
            auto progress = [this, alive, last = std::string()](
                                const pipensx::MagnetProgress& p) mutable {
                std::string text;
                switch (p.stage) {
                    case pipensx::MagnetProgress::Stage::FindingPeers:
                        text = "Finding peers...";
                        break;
                    case pipensx::MagnetProgress::Stage::Connecting:
                        text = "Contacting peer " +
                               std::to_string(p.peerIndex) + "/" +
                               std::to_string(p.peerCount) + "...";
                        break;
                    case pipensx::MagnetProgress::Stage::FetchingMetadata:
                        text = "Fetching metadata " +
                               std::to_string(p.completedPieces) + "/" +
                               std::to_string(p.totalPieces) + "...";
                        break;
                    case pipensx::MagnetProgress::Stage::Validating:
                        text = "Validating...";
                        break;
                }
                if (text == last)
                    return;
                last = text;
                brls::sync([this, alive, text] {
                    if (alive->load() && busy_)
                        statusLabel_->setText(text + "   (Y to cancel)");
                });
            };
            bool ok = resolver.resolveToFile(magnet, tmp, *cancelled,
                                             progress, err);
            telemetry_log("magnet", telemetryTag.c_str(),
                          "event=resolve ok=%d cancelled=%d duration_ms=%llu",
                          ok ? 1 : 0, cancelled->load() ? 1 : 0,
                          (unsigned long long)(now_ms() - startedMs));
            brls::sync([this, alive, ok, err, tmp, forcePicker] {
                if (!alive->load()) {
                    ::unlink(tmp.c_str());
                    return;
                }
                busy_ = false;
                std::string hash = catalogLower(entry_.infoHash);
                if (!ok) {
                    std::string reason = classifyResolveFailure(err);
                    if (onFailure_)
                        onFailure_(hash, reason);
                    diagnostic_error("magnet", hash.c_str(), "error=%s",
                                     err.c_str());
                    operationMessage_ = reason;
                    refreshButtons();
                    brls::Application::notify(err);
                    ::unlink(tmp.c_str());
                    return;
                }
                if (onFailure_)
                    onFailure_(hash, "");  // clear stale failure
                finishImport(tmp, forcePicker);
            });
        });
    }

    void finishImport(const std::string& path, bool forcePicker) {
        pipensx::TorrentPreview preview;
        std::string error;
        if (!DownloadManager::previewTorrent(path, preview, error)) {
            diagnostic_error("catalog", "preview", "error=%s",
                             error.c_str());
            operationMessage_ = error;
            refreshButtons();
            brls::Application::notify(error);
            ::unlink(path.c_str());
            return;
        }

        StreamSelection selection = settings_
            ? settings_->get().streamSelection : StreamSelection::AllFiles;

        // Options path: always hand off to the per-file picker. The picker owns
        // the temp file (unlinks it on cancel) and derives the mode from the
        // selection, so no further work here.
        if (forcePicker) {
            brls::Application::pushActivity(new TorrentSelectionActivity(
                manager_, path, std::move(preview),
                TransferMode::StreamInstall, selection));
            return;
        }

        // One-tap path. No installable packages -> nothing to silently install.
        if (preview.packageCount == 0) {
            operationMessage_ = preview.cartridgeCount > 0
                ? "Cartridge dump (XCI) — open Options to download it."
                : "No installable game files. Open Options to download.";
            refreshButtons();
            brls::Application::notify(operationMessage_);
            ::unlink(path.c_str());
            return;
        }

        // Packages present. Install them silently. On a mixed release (anything
        // that is not an install package) auto-select packages only; on a clean
        // package-only release an empty mask means "all files".
        uint32_t extras = preview.fileCount - preview.packageCount;
        std::vector<uint8_t> mask;
        if (extras > 0) {
            mask.reserve(preview.files.size());
            for (const auto& file : preview.files)
                mask.push_back(file.package ? 1 : 0);
        }

        std::string id;
        std::string err;
        if (manager_->importTorrent(path, TransferMode::StreamInstall, mask,
                                    id, err)) {
            log_msg("[catalog] imported torrent %s\n", id.c_str());
            if (extras > 0) {
                statusLabel_->setText("Installing game files. Extra files "
                                      "skipped — use Options to include them.");
                brls::Application::notify(
                    "Installing game files. Extra files skipped.");
            } else {
                statusLabel_->setText("Added. Installing to SD...");
            }
            if (onChange_)
                onChange_();
        } else if (catalogLower(err).find("already in the download manager") !=
                   std::string::npos) {
            statusLabel_->setText("Already in downloads.");
            if (onChange_)
                onChange_();
        } else {
            log_msg("[catalog] import failed from '%s': %s\n",
                    path.c_str(), err.c_str());
            diagnostic_error("catalog", "import", "error=%s", err.c_str());
            operationMessage_ = err;
            refreshButtons();
            brls::Application::notify(err);
        }
        ::unlink(path.c_str());
        refreshButtons();
    }

    CatalogEntry entry_;
    std::string lastFailure_;
    DownloadManager* manager_;
    GameMetadataService* metadata_;
    InstalledTitleService* installed_;
    AppSettings* settings_;
    std::string titleId_;
    std::string operationMessage_;
    FailureCallback onFailure_;
    ChangeCallback onChange_;
    std::shared_ptr<std::atomic<bool>> alive_;
    std::shared_ptr<std::atomic<bool>> cancelled_;
    brls::AppletFrame* frame_ = nullptr;
    brls::Button* primary_ = nullptr;
    brls::Button* secondary_ = nullptr;
    brls::Label* statusLabel_ = nullptr;
    brls::RepeatingTimer timer_;
    std::vector<DownloadTask> cache_;
    bool busy_ = false;
};

class BatchInstallActivity;

class BatchInstallCell : public brls::RecyclerCell {
public:
    BatchInstallCell() {
        setFocusable(true);
        setAxis(brls::Axis::ROW);
        setAlignItems(brls::AlignItems::CENTER);
        setPadding(12, 20, 12, 20);
        setHeight(82);

        mark_ = new brls::Label();
        mark_->setWidth(38);
        mark_->setFontSize(21);
        mark_->setTextColor(nvgRGB(0, 195, 227));
        mark_->setMarginRight(10);
        addView(mark_);

        auto* body = new brls::Box(brls::Axis::COLUMN);
        body->setGrow(1);
        body->setJustifyContent(brls::JustifyContent::CENTER);
        title_ = new brls::Label();
        title_->setSingleLine(true);
        title_->setFontSize(18);
        meta_ = new brls::Label();
        meta_->setSingleLine(true);
        meta_->setFontSize(14);
        meta_->setMarginTop(4);
        meta_->setTextColor(nvgRGB(160, 160, 170));
        body->addView(title_);
        body->addView(meta_);
        addView(body);
    }

    void setReady(const PreparedCatalogInstall& item) {
        mark_->setText(item.selected ? "[x]" : "[ ]");
        mark_->setTextColor(nvgRGB(0, 195, 227));
        title_->setText(item.entry.title);
        std::string meta = formatBytes(item.space.requiredBytes) + " selected";
        if (item.space.packageFiles)
            meta += "   " + std::to_string(item.space.packageFiles) +
                    " package(s)";
        if (item.space.certainty ==
            SpaceEstimateCertainty::CompressedUnknown) {
            meta += "   NSZ: installed size may be larger";
        }
        meta_->setText(meta);
    }

    void setFailure(const pipensx::BatchItemFailure& failure) {
        mark_->setText("!");
        mark_->setTextColor(nvgRGB(235, 105, 105));
        title_->setText(failure.entry.title);
        meta_->setText(failure.error);
    }

private:
    brls::Label* mark_ = nullptr;
    brls::Label* title_ = nullptr;
    brls::Label* meta_ = nullptr;
};

class BatchInstallDataSource : public brls::RecyclerDataSource {
public:
    explicit BatchInstallDataSource(BatchInstallActivity* owner)
        : owner_(owner) {}

    void setPreparation(std::shared_ptr<BatchPreparation> prepared) {
        prepared_ = std::move(prepared);
    }

    int numberOfRows(brls::RecyclerFrame*, int) override {
        if (!prepared_)
            return 0;
        return static_cast<int>(prepared_->items().size() +
                                prepared_->failures().size());
    }

    brls::RecyclerCell* cellForRow(brls::RecyclerFrame* recycler,
                                   brls::IndexPath index) override {
        auto* cell = static_cast<BatchInstallCell*>(
            recycler->dequeueReusableCell("BatchItem"));
        const size_t row = static_cast<size_t>(index.row);
        if (row < prepared_->items().size())
            cell->setReady(prepared_->items()[row]);
        else
            cell->setFailure(
                prepared_->failures()[row - prepared_->items().size()]);
        return cell;
    }

    void didSelectRowAt(brls::RecyclerFrame*, brls::IndexPath index) override;

private:
    BatchInstallActivity* owner_ = nullptr;
    std::shared_ptr<BatchPreparation> prepared_;
};

class BatchInstallActivity : public brls::Activity {
public:
    using CompletionCallback =
        std::function<void(const std::unordered_set<std::string>&)>;

    BatchInstallActivity(DownloadManager* manager,
                         std::vector<CatalogEntry> entries,
                         StreamSelection selection,
                         CompletionCallback completion,
                         std::function<void()> viewDownloads)
        : manager_(manager), entries_(std::move(entries)),
          selection_(selection), completion_(std::move(completion)),
          viewDownloads_(std::move(viewDownloads)),
          alive_(std::make_shared<std::atomic<bool>>(true)),
          cancelled_(std::make_shared<std::atomic<bool>>(false)) {
        for (const CatalogEntry& entry : entries_)
            remaining_.insert(catalogLower(entry.infoHash));

        auto resolver = [](const std::string& magnet, const std::string& path,
                           std::atomic<bool>& cancelled,
                           const MagnetResolver::ProgressCallback& progress,
                           std::string& error) {
            MagnetResolver instance;
            return instance.resolveToFile(magnet, path, cancelled, progress,
                                          error);
        };
        installer_ = std::make_shared<CatalogBatchInstaller>(
            manager_->rootPath(), std::move(resolver));

        auto* content = new brls::Box(brls::Axis::COLUMN);
        content->setGrow(1);
        content->setPadding(24, 38, 24, 34);
        content->setBackgroundColor(nvgRGBA(35, 35, 40, 235));
        content->setCornerRadius(12);

        status_ = new brls::Label();
        status_->setFontSize(17);
        status_->setMarginBottom(12);
        status_->setTextColor(nvgRGB(180, 180, 190));
        status_->setText("Preparing selected games...");
        content->addView(status_);

        recycler_ = new brls::RecyclerFrame();
        recycler_->setGrow(1);
        recycler_->setPadding(6, 0, 6, 0);
        recycler_->estimatedRowHeight = 82;
        recycler_->registerCell("BatchItem",
                                [] { return new BatchInstallCell(); });
        dataSource_ = new BatchInstallDataSource(this);
        recycler_->setDataSource(dataSource_);
        recycler_->setVisibility(brls::Visibility::GONE);
        content->addView(recycler_);

        controls_ = new brls::Box(brls::Axis::COLUMN);
        controls_->setMarginTop(14);
        controls_->setVisibility(brls::Visibility::GONE);
        auto* row = new brls::Box(brls::Axis::ROW);
        row->setMarginBottom(10);
        auto* selectAll = new brls::Button();
        selectAll->setStyle(&brls::BUTTONSTYLE_DEFAULT);
        selectAll->setGrow(1);
        selectAll->setHeight(48);
        selectAll->setMarginRight(10);
        selectAll->setText("Select ready");
        selectAll->registerClickAction([this](brls::View*) {
            setAllPrepared(true);
            return true;
        });
        row->addView(selectAll);
        auto* clear = new brls::Button();
        clear->setStyle(&brls::BUTTONSTYLE_DEFAULT);
        clear->setGrow(1);
        clear->setHeight(48);
        clear->setText("Clear");
        clear->registerClickAction([this](brls::View*) {
            setAllPrepared(false);
            return true;
        });
        row->addView(clear);
        controls_->addView(row);

        enqueue_ = new brls::Button();
        enqueue_->setStyle(&brls::BUTTONSTYLE_PRIMARY);
        enqueue_->setHeight(58);
        enqueue_->setText("Add to queue");
        enqueue_->registerClickAction([this](brls::View*) {
            enqueuePrepared();
            return true;
        });
        controls_->addView(enqueue_);
        content->addView(controls_);

        resultControls_ = new brls::Box(brls::Axis::ROW);
        resultControls_->setMarginTop(14);
        resultControls_->setVisibility(brls::Visibility::GONE);
        auto* downloads = new brls::Button();
        downloads->setStyle(&brls::BUTTONSTYLE_PRIMARY);
        downloads->setGrow(1);
        downloads->setHeight(54);
        downloads->setMarginRight(10);
        downloads->setText("View downloads");
        downloads->registerClickAction([this](brls::View*) {
            auto callback = viewDownloads_;
            brls::delay(100, [callback] {
                if (callback)
                    callback();
            });
            brls::Application::popActivity();
            return true;
        });
        resultControls_->addView(downloads);
        resultBack_ = new brls::Button();
        resultBack_->setStyle(&brls::BUTTONSTYLE_DEFAULT);
        resultBack_->setGrow(1);
        resultBack_->setHeight(54);
        resultBack_->setText("Back to catalog");
        resultBack_->registerClickAction([](brls::View*) {
            brls::Application::popActivity();
            return true;
        });
        resultControls_->addView(resultBack_);
        content->addView(resultControls_);

        frame_ = new brls::AppletFrame(content);
        frame_->setTitle("Batch install");
    }

    ~BatchInstallActivity() override {
        alive_->store(false);
        cancelled_->store(true);
    }

    brls::View* createContentView() override { return frame_; }

    void onContentAvailable() override {
        cancelAction_ = registerAction("Cancel", brls::BUTTON_Y,
                                       [this](brls::View*) {
            cancelled_->store(true);
            status_->setText("Cancelling preparation...");
            return true;
        });
        startPreparation();
    }

    void togglePrepared(size_t index) {
        if (!prepared_ || index >= prepared_->items().size())
            return;
        prepared_->items()[index].selected =
            !prepared_->items()[index].selected;
        recycler_->reloadData();
        refreshSummary();
    }

private:
    void startPreparation() {
        auto alive = alive_;
        auto cancelled = cancelled_;
        auto installer = installer_;
        auto entries = entries_;
        StreamSelection selection = selection_;
        brls::async([this, alive, cancelled, installer, entries, selection] {
            auto progress = [this, alive](
                                const pipensx::BatchPrepareProgress& value) {
                std::string stage;
                switch (value.magnet.stage) {
                    case pipensx::MagnetProgress::Stage::FindingPeers:
                        stage = "Finding peers";
                        break;
                    case pipensx::MagnetProgress::Stage::Connecting:
                        stage = "Connecting";
                        break;
                    case pipensx::MagnetProgress::Stage::FetchingMetadata:
                        stage = "Fetching metadata";
                        break;
                    case pipensx::MagnetProgress::Stage::Validating:
                        stage = "Validating";
                        break;
                }
                std::string text = "Preparing " +
                    std::to_string(value.index) + "/" +
                    std::to_string(value.total) + ": " + value.title +
                    "\n" + stage + "...   (Y to cancel)";
                brls::sync([this, alive, text] {
                    if (alive->load())
                        status_->setText(text);
                });
            };
            auto prepared = std::make_shared<BatchPreparation>(
                installer->prepare(entries, selection, *cancelled, progress));
            brls::sync([this, alive, prepared] {
                if (!alive->load())
                    return;
                prepared_ = prepared;
                if (prepared_->cancelled()) {
                    status_->setText("Preparation cancelled. Press B to return.");
                    return;
                }
                dataSource_->setPreparation(prepared_);
                recycler_->setVisibility(brls::Visibility::VISIBLE);
                controls_->setVisibility(brls::Visibility::VISIBLE);
                recycler_->reloadData();
                if (cancelAction_ != ACTION_NONE) {
                    unregisterAction(cancelAction_);
                    cancelAction_ = ACTION_NONE;
                }
                queueAction_ = registerAction(
                    "Add to queue", brls::BUTTON_RB,
                    [this](brls::View*) {
                        enqueuePrepared();
                        return true;
                    });
                refreshSummary();
            });
        });
    }

    void setAllPrepared(bool selected) {
        if (!prepared_)
            return;
        for (PreparedCatalogInstall& item : prepared_->items())
            item.selected = selected;
        recycler_->reloadData();
        refreshSummary();
    }

    void refreshSummary() {
        if (!prepared_)
            return;
        size_t selected = 0;
        for (const PreparedCatalogInstall& item : prepared_->items())
            selected += item.selected ? 1 : 0;
        const auto estimate = prepared_->selectedSpace();
        storage_ = pipensx::queryStorageSpace(manager_->rootPath());
        const auto check = pipensx::assessInstallSpace(estimate, storage_);

        std::string text = std::to_string(selected) + " ready selected";
        if (!prepared_->failures().empty())
            text += "   " + std::to_string(prepared_->failures().size()) +
                    " failed";
        text += "\nSelected: " + formatBytes(estimate.requiredBytes);
        if (storage_.available)
            text += "   SD free: " + formatBytes(storage_.freeBytes);
        else
            text += "   SD free: unavailable";
        if (estimate.certainty ==
            SpaceEstimateCertainty::CompressedUnknown) {
            text += "\nNSZ is compressed; exact installed size is checked per NCA.";
        }
        if (check.status == InstallSpaceCheckStatus::Insufficient)
            text += "\nNot enough space. Free " +
                    formatBytes(check.shortfallBytes) + " more.";
        else if (!storage_.available && !storage_.error.empty())
            text += "\n" + storage_.error;
        status_->setText(text);

        const bool enabled = selected > 0 && !estimate.overflow &&
            check.status != InstallSpaceCheckStatus::Insufficient;
        enqueue_->setState(enabled ? brls::ButtonState::ENABLED
                                   : brls::ButtonState::DISABLED);
        enqueue_->setText("Add " + std::to_string(selected) + " to queue");
    }

    void enqueuePrepared() {
        if (!prepared_ || enqueueFinished_)
            return;
        const auto estimate = prepared_->selectedSpace();
        if (estimate.selectedFiles == 0 || estimate.overflow) {
            refreshSummary();
            brls::Application::notify("Select at least one ready game.");
            return;
        }
        storage_ = pipensx::queryStorageSpace(manager_->rootPath());
        const auto check = pipensx::assessInstallSpace(estimate, storage_);
        if (check.status == InstallSpaceCheckStatus::Insufficient) {
            refreshSummary();
            brls::Application::notify("Not enough free space on SD.");
            return;
        }

        pipensx::BatchEnqueueResult result =
            installer_->enqueue(*prepared_, *manager_);
        for (const std::string& hash : result.queuedInfoHashes)
            remaining_.erase(catalogLower(hash));
        if (completion_)
            completion_(remaining_);
        enqueueFinished_ = true;
        std::string message = "Added " +
            std::to_string(result.taskIds.size()) + " to the queue.";
        const size_t failures = prepared_->failures().size() +
                                result.failures.size();
        if (failures)
            message += " " + std::to_string(failures) + " failed.";
        if (result.skipped)
            message += " " + std::to_string(result.skipped) + " skipped.";
        if (!remaining_.empty())
            message += "\nUnfinished games stay selected for retry.";
        size_t shown = 0;
        for (const pipensx::BatchItemFailure& failure :
             prepared_->failures()) {
            if (shown++ == 3)
                break;
            message += "\n" + failure.entry.title + ": " + failure.error;
        }
        for (const pipensx::BatchItemFailure& failure : result.failures) {
            if (shown++ == 3)
                break;
            message += "\n" + failure.entry.title + ": " + failure.error;
        }
        status_->setText(message);
        recycler_->setVisibility(brls::Visibility::GONE);
        controls_->setVisibility(brls::Visibility::GONE);
        resultControls_->setVisibility(brls::Visibility::VISIBLE);
        resultBack_->setText(remaining_.empty() ? "Back to catalog"
                                                : "Back to selected");
        if (queueAction_ != ACTION_NONE) {
            unregisterAction(queueAction_);
            queueAction_ = ACTION_NONE;
        }
    }

    DownloadManager* manager_ = nullptr;
    std::vector<CatalogEntry> entries_;
    StreamSelection selection_ = StreamSelection::AllFiles;
    CompletionCallback completion_;
    std::function<void()> viewDownloads_;
    std::unordered_set<std::string> remaining_;
    std::shared_ptr<std::atomic<bool>> alive_;
    std::shared_ptr<std::atomic<bool>> cancelled_;
    std::shared_ptr<CatalogBatchInstaller> installer_;
    std::shared_ptr<BatchPreparation> prepared_;
    StorageSpaceSnapshot storage_;
    brls::AppletFrame* frame_ = nullptr;
    brls::Label* status_ = nullptr;
    brls::RecyclerFrame* recycler_ = nullptr;
    BatchInstallDataSource* dataSource_ = nullptr;
    brls::Box* controls_ = nullptr;
    brls::Button* enqueue_ = nullptr;
    brls::Box* resultControls_ = nullptr;
    brls::Button* resultBack_ = nullptr;
    brls::ActionIdentifier cancelAction_ = ACTION_NONE;
    brls::ActionIdentifier queueAction_ = ACTION_NONE;
    bool enqueueFinished_ = false;
};

void BatchInstallDataSource::didSelectRowAt(brls::RecyclerFrame*,
                                            brls::IndexPath index) {
    if (!prepared_ || index.row < 0)
        return;
    const size_t row = static_cast<size_t>(index.row);
    if (row < prepared_->items().size())
        owner_->togglePrepared(row);
}

class CatalogView : public brls::Box {
public:
    CatalogView(DownloadManager* manager, CatalogService* catalog,
                GameMetadataService* metadata,
                InstalledTitleService* installed, AppSettings* settings,
                std::function<void()> openDownloads)
        : brls::Box(brls::Axis::COLUMN), manager_(manager), catalog_(catalog),
          metadata_(metadata), installed_(installed), settings_(settings),
          openDownloads_(std::move(openDownloads)),
          alive_(std::make_shared<std::atomic<bool>>(true)),
          cancelled_(std::make_shared<std::atomic<bool>>(false)) {
        recycler_ = new brls::RecyclerFrame();
        recycler_->setGrow(1);
        recycler_->setPadding(6, 32, 6, 32);
        recycler_->estimatedRowHeight = 82;
        recycler_->registerCell("Catalog", [] { return new CatalogCell(); });
        recycler_->registerCell("Message",
            [] { return new TextMessageCell(); });
        dataSource_ = new CatalogDataSource(this);
        recycler_->setDataSource(dataSource_);

        status_ = new brls::Label();
        status_->setFontSize(15);
        status_->setMarginTop(10);
        status_->setMarginLeft(34);
        status_->setMarginBottom(2);
        status_->setTextColor(nvgRGB(140, 140, 150));

        batchControls_ = new brls::Box(brls::Axis::ROW);
        batchControls_->setMarginTop(8);
        batchControls_->setMarginLeft(34);
        batchControls_->setMarginRight(34);
        batchControls_->setVisibility(brls::Visibility::GONE);
        auto* selectVisible = new brls::Button();
        selectVisible->setStyle(&brls::BUTTONSTYLE_DEFAULT);
        selectVisible->setGrow(1);
        selectVisible->setHeight(44);
        selectVisible->setMarginRight(8);
        selectVisible->setText("Select visible");
        selectVisible->registerClickAction([this](brls::View*) {
            selectVisibleEntries();
            return true;
        });
        batchControls_->addView(selectVisible);
        auto* clearSelection = new brls::Button();
        clearSelection->setStyle(&brls::BUTTONSTYLE_DEFAULT);
        clearSelection->setGrow(1);
        clearSelection->setHeight(44);
        clearSelection->setMarginRight(8);
        clearSelection->setText("Clear");
        clearSelection->registerClickAction([this](brls::View*) {
            selectedHashes_.clear();
            rebuildEntries();
            return true;
        });
        batchControls_->addView(clearSelection);
        prepareBatch_ = new brls::Button();
        prepareBatch_->setStyle(&brls::BUTTONSTYLE_PRIMARY);
        prepareBatch_->setGrow(1);
        prepareBatch_->setHeight(44);
        prepareBatch_->setText("Prepare");
        prepareBatch_->registerClickAction([this](brls::View*) {
            prepareSelectedEntries();
            return true;
        });
        batchControls_->addView(prepareBatch_);

        addView(status_);
        addView(batchControls_);
        addView(recycler_);
        rebuildEntries();

        registerAction("Search", brls::BUTTON_X, [this](brls::View*) {
            openSearchKeyboard();
            return true;
        });
        registerAction("Sort", brls::BUTTON_Y, [this](brls::View*) {
            if (busy_)
                cancelled_->store(true);
            else
                cycleSort();
            return true;
        });
        registerAction("Refresh", brls::BUTTON_RB, [this](brls::View*) {
            if (batchMode_)
                prepareSelectedEntries();
            else
                refreshCatalog();
            return true;
        });
        registerAction("Batch install", brls::BUTTON_LB,
                       [this](brls::View*) {
            toggleBatchMode();
            return true;
        });
        observedSettingsGeneration_ = settings_ ? settings_->generation() : 0;
        taskSignature_ = taskSignature();
        timer_.setCallback([this] { refreshLiveState(); });
        timer_.start(1000);
        if (settings_ && settings_->get().refreshCatalogOnLaunch)
            refreshCatalog();
    }

    ~CatalogView() override {
        alive_->store(false);
        cancelled_->store(true);
        timer_.stop();
    }

    void openSearchKeyboard() {
        if (busy_)
            return;
        brls::Application::getImeManager()->openForText(
            [this](std::string text) {
                query_ = std::move(text);
                rebuildEntries();
            },
            "Search catalog", "", 256, query_,
            brls::KEYBOARD_DISABLE_NONE);
    }

    void onEntrySelected(int row) {
        const CatalogEntry* picked = dataSource_->entryAt(row);
        if (!picked || busy_)
            return;
        if (batchMode_) {
            if (!dataSource_->selectableAt(row)) {
                brls::Application::notify("This item is already in Downloads.");
                return;
            }
            const std::string hash = lowerAscii(picked->infoHash);
            if (selectedHashes_.erase(hash) == 0)
                selectedHashes_.insert(hash);
            rebuildEntries();
            return;
        }
        CatalogEntry entry = *picked;
        auto it = catalogFailures_.find(lowerAscii(entry.infoHash));
        std::string lastFailure =
            it != catalogFailures_.end() ? it->second : std::string();
        auto onFailure = [this](const std::string& hashLower,
                                const std::string& failure) {
            if (failure.empty())
                catalogFailures_.erase(hashLower);
            else
                catalogFailures_[hashLower] = failure;
        };
        auto onChange = [this] { rebuildEntries(); };
        brls::Application::pushActivity(new GameDetailActivity(
            std::move(entry), std::move(lastFailure), manager_, metadata_,
            installed_, settings_,
            std::move(onFailure), std::move(onChange)));
    }

private:
    enum class SortMode { Latest, Alphabetical, Largest };

    static std::string lowerAscii(std::string value) {
        std::transform(value.begin(), value.end(), value.begin(),
                       [](unsigned char c) {
                           return static_cast<char>(std::tolower(c));
                       });
        return value;
    }

    void toggleBatchMode() {
        if (busy_)
            return;
        batchMode_ = !batchMode_;
        batchControls_->setVisibility(batchMode_ ? brls::Visibility::VISIBLE
                                                 : brls::Visibility::GONE);
        updateActionHint(brls::BUTTON_LB,
                         batchMode_ ? "Close batch" : "Batch install");
        updateActionHint(brls::BUTTON_RB,
                         batchMode_ ? "Prepare" : "Refresh");
        rebuildEntries();
    }

    void selectVisibleEntries() {
        if (!batchMode_)
            return;
        for (size_t row = 0; row < dataSource_->entries().size(); ++row) {
            if (dataSource_->selectableAt(static_cast<int>(row)))
                selectedHashes_.insert(
                    lowerAscii(dataSource_->entries()[row].infoHash));
        }
        rebuildEntries();
    }

    void prepareSelectedEntries() {
        if (!batchMode_ || selectedHashes_.empty() || busy_)
            return;

        std::unordered_set<std::string> managed;
        for (const DownloadTask& task : manager_->snapshot())
            managed.insert(lowerAscii(task.id));

        std::vector<CatalogEntry> entries;
        for (const CatalogEntry& entry : catalog_->entries()) {
            const std::string hash = lowerAscii(entry.infoHash);
            if (selectedHashes_.count(hash) && !managed.count(hash))
                entries.push_back(entry);
        }
        if (sort_ == SortMode::Alphabetical) {
            std::stable_sort(entries.begin(), entries.end(),
                [](const CatalogEntry& left, const CatalogEntry& right) {
                    return lowerAscii(left.title) < lowerAscii(right.title);
                });
        } else if (sort_ == SortMode::Largest) {
            std::stable_sort(entries.begin(), entries.end(),
                [](const CatalogEntry& left, const CatalogEntry& right) {
                    return left.size > right.size;
                });
        } else {
            std::stable_sort(entries.begin(), entries.end(),
                [](const CatalogEntry& left, const CatalogEntry& right) {
                    return left.publishedAt > right.publishedAt;
                });
        }
        for (const std::string& hash : managed)
            selectedHashes_.erase(hash);
        if (entries.empty()) {
            rebuildEntries();
            brls::Application::notify("Select at least one available game.");
            return;
        }

        auto alive = alive_;
        auto completion = [this, alive](
                              const std::unordered_set<std::string>& remaining) {
            if (!alive->load())
                return;
            selectedHashes_ = remaining;
            rebuildEntries();
        };
        const StreamSelection selection = settings_
            ? settings_->get().streamSelection
            : StreamSelection::AllFiles;
        brls::Application::pushActivity(new BatchInstallActivity(
            manager_, std::move(entries), selection, std::move(completion),
            openDownloads_));
    }

    void refreshBatchStatus() {
        uint64_t bytes = 0;
        size_t unknown = 0;
        for (const CatalogEntry& entry : catalog_->entries()) {
            if (!selectedHashes_.count(lowerAscii(entry.infoHash)))
                continue;
            if (!entry.size) {
                ++unknown;
                continue;
            }
            if (entry.size > std::numeric_limits<uint64_t>::max() - bytes)
                bytes = std::numeric_limits<uint64_t>::max();
            else
                bytes += entry.size;
        }
        const StorageSpaceSnapshot storage =
            pipensx::queryStorageSpace(manager_->rootPath());
        std::string text = std::to_string(selectedHashes_.size()) +
                           " selected   Catalog size: " + formatBytes(bytes);
        if (unknown)
            text += " + " + std::to_string(unknown) + " unknown";
        text += storage.available
            ? "   SD free: " + formatBytes(storage.freeBytes)
            : "   SD free: unavailable";
        status_->setText(text);
        const bool available = !selectedHashes_.empty();
        prepareBatch_->setState(available ? brls::ButtonState::ENABLED
                                          : brls::ButtonState::DISABLED);
        prepareBatch_->setText("Prepare " +
                               std::to_string(selectedHashes_.size()));
        setActionAvailable(brls::BUTTON_RB, available);
    }

    void rebuildEntries() {
        // Info-hash (lower-case hex) -> status for anything already managed,
        // so rows can be badged. Task ids are lower-case hex; catalog info
        // hashes are upper-case, hence the case fold on both sides.
        std::unordered_map<std::string, DownloadStatus> added;
        for (const DownloadTask& task : manager_->snapshot())
            added[lowerAscii(task.id)] = task.status;

        std::vector<CatalogEntry> visible;
        std::string needle = lowerAscii(query_);
        for (const CatalogEntry& entry : catalog_->entries()) {
            if (entry.isHiddenByDefault())
                continue;
            const GameMetadata* meta = metadata_->findByInfoHash(entry.infoHash);
            if (settings_ &&
                settings_->get().catalogFilter == CatalogFilter::Games &&
                !meta)
                continue;
            bool matches = needle.empty() ||
                lowerAscii(entry.title).find(needle) != std::string::npos ||
                (meta && lowerAscii(meta->name).find(needle) !=
                             std::string::npos);
            if (!matches)
                continue;
            visible.push_back(entry);
        }
        if (sort_ == SortMode::Alphabetical) {
            std::stable_sort(visible.begin(), visible.end(),
                [](const CatalogEntry& left, const CatalogEntry& right) {
                    return lowerAscii(left.title) < lowerAscii(right.title);
                });
        } else if (sort_ == SortMode::Largest) {
            std::stable_sort(visible.begin(), visible.end(),
                [](const CatalogEntry& left, const CatalogEntry& right) {
                    return left.size > right.size;
                });
        } else {
            std::stable_sort(visible.begin(), visible.end(),
                [](const CatalogEntry& left, const CatalogEntry& right) {
                    return left.publishedAt > right.publishedAt;
                });
        }

        std::vector<std::string> stateBadges;
        std::vector<std::string> gameNames;
        std::vector<std::string> iconUrls;
        std::vector<uint8_t> selected;
        std::vector<uint8_t> selectable;
        stateBadges.reserve(visible.size());
        gameNames.reserve(visible.size());
        iconUrls.reserve(visible.size());
        selected.reserve(visible.size());
        selectable.reserve(visible.size());
        for (const CatalogEntry& entry : visible) {
            std::string hash = lowerAscii(entry.infoHash);
            auto it = added.find(hash);
            const bool canSelect = it == added.end();
            if (it != added.end()) {
                stateBadges.push_back(badgeForStatus(it->second));
                selectedHashes_.erase(hash);
            } else
                stateBadges.emplace_back();
            const GameMetadata* meta = metadata_->findByInfoHash(entry.infoHash);
            if (it == added.end() && meta && installed_ &&
                installed_->contains(meta->titleId))
                stateBadges.back() = "Installed";
            gameNames.push_back(meta ? meta->name : std::string());
            iconUrls.push_back(meta ? meta->iconUrl : std::string());
            selected.push_back(selectedHashes_.count(hash) ? 1 : 0);
            selectable.push_back(canSelect ? 1 : 0);
        }

        size_t count = visible.size();
        dataSource_->setEntries(std::move(visible), std::move(stateBadges),
                                std::move(gameNames), std::move(iconUrls),
                                std::move(selected), std::move(selectable),
                                metadata_, batchMode_);
        dataSource_->setMessage(query_.empty()
            ? "Catalog is empty. Press R to refresh."
            : "Nothing found. Press X to change the search.");
        recycler_->reloadData();

        countText_ = query_.empty()
            ? withThousands(count) + (count == 1 ? " release" : " releases")
            : withThousands(count) + (count == 1 ? " match" : " matches");
        if (!busy_ && batchMode_) {
            refreshBatchStatus();
        } else if (!busy_) {
            std::string filter = settings_ &&
                settings_->get().catalogFilter == CatalogFilter::Games
                ? "Games" : "All";
            status_->setText(countText_ + "   Filter: " + filter);
            setActionAvailable(brls::BUTTON_RB, true);
        }
    }

    // While busy (catalog refresh) Y cancels instead of sorting; reflect that
    // in the bottom-bar hint so the label always matches the action.
    void setBusy(bool busy) {
        busy_ = busy;
        updateActionHint(brls::BUTTON_Y, busy ? "Stop" : "Sort");
    }

    uint64_t taskSignature() const {
        uint64_t signature = 1469598103934665603ULL;
        for (const DownloadTask& task : manager_->snapshot()) {
            for (unsigned char c : task.id)
                signature = (signature ^ c) * 1099511628211ULL;
            signature = (signature ^ static_cast<uint64_t>(task.status)) *
                        1099511628211ULL;
        }
        return signature;
    }

    void refreshLiveState() {
        if (busy_)
            return;
        bool changed = false;
        if (settings_ && settings_->generation() !=
                             observedSettingsGeneration_) {
            observedSettingsGeneration_ = settings_->generation();
            changed = true;
        }
        uint64_t signature = taskSignature();
        if (signature != taskSignature_) {
            taskSignature_ = signature;
            changed = true;
            bool installedFinished = false;
            for (const DownloadTask& task : manager_->snapshot())
                installedFinished = installedFinished ||
                    task.status == DownloadStatus::Installed;
            if (installedFinished && installed_ &&
                installedRefreshSignature_ != signature) {
                installedRefreshSignature_ = signature;
                refreshInstalledAsync();
            }
        }
        if (changed)
            rebuildEntries();
    }

    void refreshInstalledAsync() {
        if (installedRefreshInFlight_)
            return;
        installedRefreshInFlight_ = true;
        auto alive = alive_;
        InstalledTitleService* installed = installed_;
        brls::async([this, alive, installed] {
            std::string error;
            bool ok = installed->refresh(error);
            brls::sync([this, alive, ok, error] {
                if (!alive->load())
                    return;
                installedRefreshInFlight_ = false;
                if (!ok)
                    diagnostic_error("installed", "auto_refresh", "error=%s",
                                     error.c_str());
                rebuildEntries();
            });
        });
    }

    static std::string withThousands(size_t value) {
        std::string digits = std::to_string(value);
        int insertAt = static_cast<int>(digits.size()) - 3;
        while (insertAt > 0) {
            digits.insert(static_cast<size_t>(insertAt), ",");
            insertAt -= 3;
        }
        return digits;
    }

    static std::string badgeForStatus(DownloadStatus status) {
        switch (status) {
            case DownloadStatus::Queued:      return "In queue";
            case DownloadStatus::Checking:
            case DownloadStatus::Downloading:
            case DownloadStatus::Verifying:   return "Downloading";
            case DownloadStatus::Paused:      return "Paused";
            case DownloadStatus::Installing:
            case DownloadStatus::Committing:  return "Installing";
            case DownloadStatus::Completed:   return "Downloaded";
            case DownloadStatus::Installed:   return "Installed";
            case DownloadStatus::Error:       return "Error";
            case DownloadStatus::Removing:    return "Removing";
        }
        return "";
    }

    void cycleSort() {
        if (sort_ == SortMode::Latest)
            sort_ = SortMode::Alphabetical;
        else if (sort_ == SortMode::Alphabetical)
            sort_ = SortMode::Largest;
        else
            sort_ = SortMode::Latest;
        rebuildEntries();
        brls::Application::notify(
            sort_ == SortMode::Latest ? "Sorted by latest."
          : sort_ == SortMode::Alphabetical ? "Sorted alphabetically."
                                             : "Sorted by size.");
    }

    void refreshCatalog() {
        if (busy_)
            return;
        setBusy(true);
        brls::Application::notify("Updating catalog from GitHub...");
        auto alive = alive_;
        CatalogService* catalog = catalog_;
        uint64_t startedMs = now_ms();
        brls::async([this, alive, catalog, startedMs] {
            std::string err;
            bool ok = catalog->refresh(err);
            telemetry_log("catalog", "-",
                          "event=refresh ok=%d duration_ms=%llu entries=%zu",
                          ok ? 1 : 0,
                          (unsigned long long)(now_ms() - startedMs),
                          catalog->entries().size());
            brls::sync([this, alive, ok, err] {
                if (!alive->load())
                    return;
                setBusy(false);
                if (!ok) {
                    diagnostic_error("catalog", "refresh", "error=%s",
                                     err.c_str());
                    brls::Application::notify(err);
                    return;
                }
                rebuildEntries();
                brls::Application::notify(
                    "Catalog updated: " +
                    std::to_string(catalog_->entries().size()) + " entries.");
            });
        });
    }

    DownloadManager* manager_;
    CatalogService* catalog_;
    GameMetadataService* metadata_;
    InstalledTitleService* installed_;
    AppSettings* settings_;
    std::function<void()> openDownloads_;
    brls::RecyclerFrame* recycler_;
    CatalogDataSource* dataSource_;
    brls::Label* status_;
    brls::Box* batchControls_ = nullptr;
    brls::Button* prepareBatch_ = nullptr;
    std::shared_ptr<std::atomic<bool>> alive_;
    std::shared_ptr<std::atomic<bool>> cancelled_;
    std::unordered_map<std::string, std::string> catalogFailures_;
    std::unordered_set<std::string> selectedHashes_;
    std::string query_;
    std::string countText_;
    SortMode sort_ = SortMode::Latest;
    bool busy_ = false;
    bool batchMode_ = false;
    brls::RepeatingTimer timer_;
    uint64_t observedSettingsGeneration_ = 0;
    uint64_t taskSignature_ = 0;
    uint64_t installedRefreshSignature_ = 0;
    bool installedRefreshInFlight_ = false;
};

void CatalogDataSource::didSelectRowAt(brls::RecyclerFrame*,
                                       brls::IndexPath index) {
    if (!entryAt(index.row))
        owner_->openSearchKeyboard();
    else
        owner_->onEntrySelected(index.row);
}

class InstalledCell : public brls::RecyclerCell {
public:
    InstalledCell() {
        setFocusable(true);
        setAxis(brls::Axis::ROW);
        setAlignItems(brls::AlignItems::CENTER);
        setPadding(10, 18, 10, 18);
        setHeight(92);

        image_ = new AsyncRgbaImage();
        image_->setWidth(64);
        image_->setHeight(64);
        image_->setCornerRadius(8);
        image_->setMarginRight(16);
        image_->setScalingType(brls::ImageScalingType::FILL);
        addView(image_);

        auto* labels = new brls::Box(brls::Axis::COLUMN);
        labels->setGrow(1);
        title_ = new brls::Label();
        title_->setSingleLine(true);
        title_->setFontSize(21);
        subtitle_ = new brls::Label();
        subtitle_->setSingleLine(true);
        subtitle_->setFontSize(15);
        subtitle_->setMarginTop(6);
        subtitle_->setTextColor(nvgRGB(160, 160, 170));
        labels->addView(title_);
        labels->addView(subtitle_);
        addView(labels);
    }

    void setTitle(const InstalledTitle& title,
                  GameMetadataService* metadata) {
        title_->setText(title.name);
        std::string subtitle = title.publisher;
        if (!subtitle.empty())
            subtitle += "   ";
        subtitle += title.titleId;
        subtitle_->setText(subtitle);
        setArtworkUrl(image_, metadata, title.iconPath, currentIconPath_,
                      imageState_);
    }

private:
    AsyncRgbaImage* image_ = nullptr;
    brls::Label* title_ = nullptr;
    brls::Label* subtitle_ = nullptr;
    std::string currentIconPath_;
    std::shared_ptr<ImageRequestState> imageState_ =
        std::make_shared<ImageRequestState>();
};

class InstalledDataSource : public brls::RecyclerDataSource {
public:
    explicit InstalledDataSource(GameMetadataService* metadata)
        : metadata_(metadata) {}

    void setTitles(std::vector<InstalledTitle> titles) {
        titles_ = std::move(titles);
    }

    int numberOfRows(brls::RecyclerFrame*, int) override {
        return titles_.empty() ? 1 : static_cast<int>(titles_.size());
    }

    brls::RecyclerCell* cellForRow(brls::RecyclerFrame* recycler,
                                    brls::IndexPath index) override {
        if (titles_.empty()) {
            auto* cell = static_cast<TextMessageCell*>(
                recycler->dequeueReusableCell("Message"));
            cell->setMessage("No installed applications found. Press R to refresh.");
            return cell;
        }
        auto* cell = static_cast<InstalledCell*>(
            recycler->dequeueReusableCell("Installed"));
        cell->setTitle(titles_[static_cast<size_t>(index.row)], metadata_);
        return cell;
    }

private:
    GameMetadataService* metadata_;
    std::vector<InstalledTitle> titles_;
};

class InstalledView : public brls::Box {
public:
    InstalledView(InstalledTitleService* installed, DownloadManager* manager,
                  GameMetadataService* metadata)
        : brls::Box(brls::Axis::COLUMN), installed_(installed),
          manager_(manager), alive_(std::make_shared<std::atomic<bool>>(true)) {
        status_ = new brls::Label();
        status_->setFontSize(15);
        status_->setMarginTop(10);
        status_->setMarginLeft(34);
        status_->setTextColor(nvgRGB(140, 140, 150));
        addView(status_);

        recycler_ = new brls::RecyclerFrame();
        recycler_->setGrow(1);
        recycler_->setPadding(6, 32, 6, 32);
        recycler_->estimatedRowHeight = 92;
        recycler_->registerCell("Installed", [] { return new InstalledCell(); });
        recycler_->registerCell("Message", [] { return new TextMessageCell(); });
        dataSource_ = new InstalledDataSource(metadata);
        recycler_->setDataSource(dataSource_);
        addView(recycler_);
        reload();

        registerAction("Refresh", brls::BUTTON_RB, [this](brls::View*) {
            refresh();
            return true;
        });
    }

    ~InstalledView() override { alive_->store(false); }

private:
    bool hasActiveStreamInstall() const {
        for (const DownloadTask& task : manager_->snapshot()) {
            if (task.mode != TransferMode::StreamInstall)
                continue;
            if (task.status == DownloadStatus::Queued ||
                task.status == DownloadStatus::Checking ||
                task.status == DownloadStatus::Downloading ||
                task.status == DownloadStatus::Installing ||
                task.status == DownloadStatus::Committing ||
                task.status == DownloadStatus::Verifying)
                return true;
        }
        return false;
    }

    void reload() {
        std::vector<InstalledTitle> titles = installed_->titles();
        size_t count = titles.size();
        dataSource_->setTitles(std::move(titles));
        recycler_->reloadData();
        status_->setText(std::to_string(count) +
            (count == 1 ? " installed application" : " installed applications"));
    }

    void refresh() {
        if (refreshing_)
            return;
        if (hasActiveStreamInstall()) {
            brls::Application::notify(
                "Installed games will refresh after streaming installation finishes.");
            return;
        }
        refreshing_ = true;
        status_->setText("Refreshing installed applications...");
        auto alive = alive_;
        InstalledTitleService* installed = installed_;
        brls::async([this, alive, installed] {
            std::string error;
            bool ok = installed->refresh(error);
            brls::sync([this, alive, ok, error] {
                if (!alive->load())
                    return;
                refreshing_ = false;
                if (!ok) {
                    status_->setText(error);
                    brls::Application::notify(error);
                    return;
                }
                reload();
            });
        });
    }

    InstalledTitleService* installed_;
    DownloadManager* manager_;
    brls::Label* status_ = nullptr;
    brls::RecyclerFrame* recycler_ = nullptr;
    InstalledDataSource* dataSource_ = nullptr;
    std::shared_ptr<std::atomic<bool>> alive_;
    bool refreshing_ = false;
};

class SettingsView : public brls::Box {
public:
    SettingsView(AppSettings* settings, DownloadManager* manager,
                 CatalogService* catalog, GameMetadataService* metadata,
                 InstalledTitleService* installed)
        : brls::Box(brls::Axis::COLUMN), settings_(settings), manager_(manager),
          catalog_(catalog), metadata_(metadata), installed_(installed) {
        auto* content = new brls::Box(brls::Axis::COLUMN);
        content->setPadding(24, 34, 24, 34);

        addSection(content, "Catalog");
        catalogFilter_ = new brls::SelectorCell();
        catalogFilter_->init("Visible releases", {"All", "Games"},
            settings_->get().catalogFilter == CatalogFilter::Games ? 1 : 0,
            [this](int selected) {
                AppSettingsData values = settings_->get();
                CatalogFilter previous = values.catalogFilter;
                values.catalogFilter = selected == 1
                    ? CatalogFilter::Games : CatalogFilter::All;
                if (!persist(values, "catalog_filter"))
                    catalogFilter_->setSelection(
                        previous == CatalogFilter::Games ? 1 : 0, true);
            });
        content->addView(catalogFilter_);

        refreshCatalog_ = new brls::BooleanCell();
        refreshCatalog_->init("Refresh catalog on launch",
            settings_->get().refreshCatalogOnLaunch,
            [this](bool enabled) {
                AppSettingsData values = settings_->get();
                bool previous = values.refreshCatalogOnLaunch;
                values.refreshCatalogOnLaunch = enabled;
                if (!persist(values, "catalog_refresh"))
                    refreshCatalog_->setOn(previous, false);
            });
        content->addView(refreshCatalog_);

        addSection(content, "Downloads");
        streamSelection_ = new brls::SelectorCell();
        streamSelection_->init("Default streaming file selection",
            {"All files", "NSP/NSZ only"},
            settings_->get().streamSelection == StreamSelection::PackagesOnly
                ? 1 : 0,
            [this](int selected) {
                AppSettingsData values = settings_->get();
                StreamSelection previous = values.streamSelection;
                values.streamSelection = selected == 1
                    ? StreamSelection::PackagesOnly
                    : StreamSelection::AllFiles;
                if (!persist(values, "stream_selection"))
                    streamSelection_->setSelection(
                        previous == StreamSelection::PackagesOnly ? 1 : 0,
                        true);
            });
        content->addView(streamSelection_);

        showCompleted_ = new brls::BooleanCell();
        showCompleted_->init("Show completed downloads",
            settings_->get().showCompletedDownloads,
            [this](bool enabled) {
                AppSettingsData values = settings_->get();
                bool previous = values.showCompletedDownloads;
                values.showCompletedDownloads = enabled;
                if (!persist(values, "show_completed"))
                    showCompleted_->setOn(previous, false);
            });
        content->addView(showCompleted_);

        addSection(content, "Diagnostics");
        auto* description = new brls::Label();
        description->setText(
            "Errors are always recorded. Extended mode adds rate-limited "
            "torrent, buffer, decoder, image and NCM metrics every 5 seconds.");
        description->setFontSize(16);
        description->setTextColor(nvgRGB(170, 170, 180));
        description->setMarginBottom(10);
        content->addView(description);

        extendedTelemetry_ = new brls::BooleanCell();
        extendedTelemetry_->init("Extended telemetry",
            settings_->get().extendedTelemetry,
            [this](bool enabled) {
                AppSettingsData values = settings_->get();
                bool previous = values.extendedTelemetry;
                values.extendedTelemetry = enabled;
                if (!persist(values, "extended_telemetry")) {
                    extendedTelemetry_->setOn(previous, false);
                    return;
                }
                telemetry_set_enabled(enabled ? 1 : 0);
                brls::Application::notify(enabled
                    ? "Extended telemetry enabled."
                    : "Extended telemetry disabled.");
            });
        content->addView(extendedTelemetry_);

        content->addView(actionCell("Capture diagnostic snapshot", "Write now",
            [this] { captureSnapshot(); }));
        content->addView(actionCell("Clear log", "32 MB rotation",
            [this] { confirmClearLog(); }));
        content->addView(actionCell("Clear artwork cache", "Downloaded images",
            [this] { confirmClearArtwork(); }));
        content->addView(actionCell("Reset settings", "Restore defaults",
            [this] { confirmReset(); }));

        auto* path = new brls::Label();
        path->setText(std::string("Log: ") + LogPath);
        path->setFontSize(15);
        path->setTextColor(nvgRGB(145, 145, 155));
        path->setMarginTop(18);
        content->addView(path);

        auto* scroll = new brls::ScrollingFrame();
        scroll->setGrow(1);
        scroll->setContentView(content);
        addView(scroll);
    }

private:
    static void addSection(brls::Box* content, const std::string& text) {
        auto* title = new brls::Label();
        title->setText(text);
        title->setFontSize(25);
        title->setMarginTop(14);
        title->setMarginBottom(8);
        content->addView(title);
    }

    static brls::DetailCell* actionCell(const std::string& title,
                                        const std::string& detail,
                                        std::function<void()> callback) {
        auto* cell = new brls::DetailCell();
        cell->setText(title);
        cell->setDetailText(detail);
        cell->registerClickAction(
            [callback = std::move(callback)](brls::View*) {
                callback();
                return true;
            });
        return cell;
    }

    bool persist(const AppSettingsData& values, const char* tag) {
        std::string error;
        if (settings_->update(values, error))
            return true;
        diagnostic_error("settings", tag, "error=%s", error.c_str());
        brls::Application::notify(error);
        return false;
    }

    void applyValues() {
        const AppSettingsData& values = settings_->get();
        catalogFilter_->setSelection(
            values.catalogFilter == CatalogFilter::Games ? 1 : 0, true);
        refreshCatalog_->setOn(values.refreshCatalogOnLaunch, false);
        streamSelection_->setSelection(
            values.streamSelection == StreamSelection::PackagesOnly ? 1 : 0,
            true);
        showCompleted_->setOn(values.showCompletedDownloads, false);
        extendedTelemetry_->setOn(values.extendedTelemetry, false);
        telemetry_set_enabled(values.extendedTelemetry ? 1 : 0);
    }

    void captureSnapshot() {
        size_t active = 0;
        size_t errors = 0;
        for (const DownloadTask& task : manager_->snapshot()) {
            if (task.status == DownloadStatus::Error)
                ++errors;
            else if (task.status != DownloadStatus::Completed &&
                     task.status != DownloadStatus::Installed &&
                     task.status != DownloadStatus::Paused)
                ++active;
        }
        uint64_t freeBytes = 0;
        struct statvfs storage {};
        if (statvfs("sdmc:/", &storage) == 0)
            freeBytes = static_cast<uint64_t>(storage.f_bavail) *
                        static_cast<uint64_t>(storage.f_frsize);
        uint32_t hos = hosversionGet();
        diagnostic_snapshot("system", "manual",
            "version=%s hos=%u.%u.%u operation_mode=%d telemetry=%d "
            "catalog=%zu metadata=%zu installed=%zu active=%zu errors=%zu "
            "sd_free_bytes=%llu",
            PIPENSX_VERSION, HOSVER_MAJOR(hos), HOSVER_MINOR(hos),
            HOSVER_MICRO(hos), static_cast<int>(appletGetOperationMode()),
            telemetry_enabled(), catalog_->entries().size(), metadata_->size(),
            installed_->titles().size(), active, errors,
            static_cast<unsigned long long>(freeBytes));
        brls::Application::notify("Diagnostic snapshot written to pipensx.log.");
    }

    void confirmClearLog() {
        auto* dialog = new brls::Dialog("Clear pipensx.log now?");
        dialog->addButton("Clear log", [] {
            if (!clearApplicationLog())
                brls::Application::notify("Unable to clear pipensx.log.");
            else
                brls::Application::notify("Log cleared.");
        });
        dialog->addButton("Cancel", [] {});
        dialog->open();
    }

    void confirmClearArtwork() {
        auto* dialog = new brls::Dialog("Clear downloaded artwork cache?");
        dialog->addButton("Clear artwork", [this] {
            std::string error;
            if (!metadata_->clearImageCache(error)) {
                diagnostic_error("metadata", "clear_cache", "error=%s",
                                 error.c_str());
                brls::Application::notify(error);
            } else {
                brls::Application::notify("Artwork cache cleared.");
            }
        });
        dialog->addButton("Cancel", [] {});
        dialog->open();
    }

    void confirmReset() {
        auto* dialog = new brls::Dialog("Reset all pipensx settings?");
        dialog->addButton("Reset settings", [this] {
            std::string error;
            if (!settings_->reset(error)) {
                diagnostic_error("settings", "reset", "error=%s",
                                 error.c_str());
                brls::Application::notify(error);
                return;
            }
            applyValues();
            brls::Application::notify("Settings restored to defaults.");
        });
        dialog->addButton("Cancel", [] {});
        dialog->open();
    }

    AppSettings* settings_;
    DownloadManager* manager_;
    CatalogService* catalog_;
    GameMetadataService* metadata_;
    InstalledTitleService* installed_;
    brls::SelectorCell* catalogFilter_ = nullptr;
    brls::BooleanCell* refreshCatalog_ = nullptr;
    brls::SelectorCell* streamSelection_ = nullptr;
    brls::BooleanCell* showCompleted_ = nullptr;
    brls::BooleanCell* extendedTelemetry_ = nullptr;
};

class AboutView : public brls::Box {
public:
    AboutView() : brls::Box(brls::Axis::COLUMN) {
        auto* content = new brls::Box(brls::Axis::COLUMN);
        content->setPadding(30, 50, 30, 50);
        addLine(content, "pipensx", 32, nvgRGB(245, 245, 250));
        addLine(content, std::string("Version ") + PIPENSX_VERSION +
            "   Built " + __DATE__ + " " + __TIME__, 18,
            nvgRGB(180, 180, 190));
        addLine(content,
            "A Nintendo Switch storefront and BitTorrent client for "
            "downloading or streaming NSP/NSZ packages to SD.",
            19, nvgRGB(220, 220, 225));
        addLine(content,
            "Questions or feedback? Message @i3sey on Telegram.",
            19, nvgRGB(0, 195, 227));
        addLine(content,
            "Catalog: cached on SD with a bundled offline fallback.\n"
            "Log: sdmc:/switch/pipensx/pipensx.log\n"
            "Settings: sdmc:/switch/pipensx/settings.json",
            17, nvgRGB(175, 175, 185));
        addLine(content,
            "Built with libnx, Borealis, libcurl, zstd, mbedTLS and "
            "miniupnpc. See THIRD_PARTY_NOTICES.md for licenses.",
            17, nvgRGB(175, 175, 185));
        addLine(content,
            "pipensx is an independent open-source project and is not "
            "affiliated with Nintendo.",
            16, nvgRGB(150, 150, 160));
        auto* scroll = new brls::ScrollingFrame();
        scroll->setGrow(1);
        scroll->setContentView(content);
        addView(scroll);
    }

private:
    static void addLine(brls::Box* content, const std::string& text,
                        float size, NVGcolor color) {
        auto* label = new brls::Label();
        label->setText(text);
        label->setFontSize(size);
        label->setTextColor(color);
        label->setMarginBottom(22);
        content->addView(label);
    }
};

class MainActivity : public brls::Activity {
public:
    MainActivity(DownloadManager* manager, CatalogService* catalog,
                 GameMetadataService* metadata,
                 InstalledTitleService* installed, AppSettings* settings)
        : manager_(manager), catalog_(catalog), metadata_(metadata),
          installed_(installed), settings_(settings) {
        auto* tabs = new brls::TabFrame();
        tabs->addTab("Catalog", [manager, catalog, metadata, installed,
                                  settings, tabs] {
            return new CatalogView(manager, catalog, metadata, installed,
                                   settings, [tabs] { tabs->focusTab(1); });
        });
        tabs->addTab("Downloads", [manager, metadata, settings] {
            return new MainView(manager, metadata, settings);
        });
        tabs->addTab("Installed", [installed, manager, metadata] {
            return new InstalledView(installed, manager, metadata);
        });
        tabs->addTab("Settings", [settings, manager, catalog, metadata,
                                   installed] {
            return new SettingsView(settings, manager, catalog, metadata,
                                    installed);
        });
        tabs->addTab("About", [] {
            return new AboutView();
        });
        frame_ = new brls::AppletFrame(tabs);
        frame_->setTitle("pipensx");
    }

    brls::View* createContentView() override {
        return frame_;
    }

    void onContentAvailable() override {
        registerAction("Exit", brls::BUTTON_START,
            [this](brls::View*) {
                startupStage("quit requested by Plus");
                brls::Application::quit();
                return true;
            });
    }

private:
    DownloadManager* manager_;
    CatalogService* catalog_;
    GameMetadataService* metadata_;
    InstalledTitleService* installed_;
    AppSettings* settings_;
    brls::AppletFrame* frame_;
};

} // namespace

int main(int, char**) {
    switch_crashlog_install();
    switch_crashlog_stage("creating application directories");
    mkdir("sdmc:/switch", 0755);
    mkdir("sdmc:/switch/pipensx", 0755);
    log_init(LogPath);
    AppSettings settings(SettingsPath, TelemetryFlagPath);
    std::string settingsError;
    if (!settings.load(settingsError))
        diagnostic_error("settings", "startup", "error=%s",
                         settingsError.c_str());
    telemetry_set_enabled(settings.get().extendedTelemetry ? 1 : 0);
    log_msg("[telemetry] setting enabled=%d interval_ms=5000 build='%s %s'\n",
            telemetry_enabled(), __DATE__, __TIME__);
    startupStage("entered main");

    openBorealisLog();

    std::set_terminate([] {
        switch_crashlog_stage("uncaught C++ exception");
        log_msg("[crash] std::terminate called\n");
        if (gBorealisLog)
            std::fflush(gBorealisLog);
        std::_Exit(134);
    });

    bool curlReady = false;
    bool ncmReady = false;
    bool nsReady = false;
    bool esReady = false;
    try {
        log_msg("[startup] applet_type=%d operation_mode=%d\n",
                (int)appletGetAppletType(), (int)appletGetOperationMode());

        if (!isApplicationMode()) {
            startupStage("unsupported applet mode");
            showApplicationModeRequired();
            startupStage("applet mode exit");
            if (gBorealisLog) {
                std::fflush(gBorealisLog);
                std::fclose(gBorealisLog);
                gBorealisLog = nullptr;
            }
            log_close();
            return 2;
        }

        startupStage("curl_global_init");
        CURLcode curlResult = curl_global_init(CURL_GLOBAL_DEFAULT);
        if (curlResult != CURLE_OK) {
            log_msg("[startup] curl_global_init failed: %d\n",
                    (int)curlResult);
            throw std::runtime_error("curl_global_init failed");
        }
        curlReady = true;

        startupStage("installer services");
        Result rc = ncmInitialize();
        if (R_FAILED(rc))
            throw std::runtime_error("ncmInitialize failed");
        ncmReady = true;
        rc = nsInitialize();
        if (R_FAILED(rc))
            throw std::runtime_error("nsInitialize failed");
        nsReady = true;
        rc = esInitialize();
        if (R_FAILED(rc))
            throw std::runtime_error("esInitialize failed");
        esReady = true;

        startupStage("Borealis Application::init");
        brls::Platform::APP_LOCALE_DEFAULT = brls::LOCALE_EN_US;
        if (!brls::Application::init())
            throw std::runtime_error("Borealis Application::init failed");

        startupStage("Borealis createWindow");
        brls::Application::createWindow("pipensx");
        brls::Application::setGlobalQuit(false);

        startupStage("CatalogService construction");
        antizapret_init("sdmc:/switch/pipensx");
        antizapret_set_enabled(1);
        log_msg("[startup] image relay: relays-first + disk cache (rev4)\n");
        unlink("sdmc:/switch/pipensx/rutracker.cfg");
        unlink("sdmc:/switch/pipensx/rutracker_cookies.txt");
        CatalogService catalog("sdmc:/switch/pipensx");
        std::string catalogError;
        if (!catalog.load(catalogError))
            log_msg("[catalog] initial load failed: %s\n",
                    catalogError.c_str());
        startupStage("GameMetadataService construction");
        GameMetadataService metadata("sdmc:/switch/pipensx");
        std::string metadataError;
        if (!metadata.load(metadataError))
            log_msg("[metadata] initial load failed: %s\n",
                    metadataError.c_str());

        startupStage("InstalledTitleService refresh");
        InstalledTitleService installed("sdmc:/switch/pipensx");
        std::string installedError;
        if (!installed.refresh(installedError))
            diagnostic_error("installed", "startup", "error=%s",
                             installedError.c_str());

        startupStage("DownloadManager construction");
        SwitchPerformanceController performance;
        DownloadManager manager("sdmc:/switch/pipensx");
        metadata.setImageNetworkPaused(manager.hasActiveTransfer());

        startupStage("MainActivity construction");
        auto* activity = new MainActivity(&manager, &catalog, &metadata,
                                          &installed, &settings);

        startupStage("push MainActivity");
        brls::Application::pushActivity(activity);

        startupStage("first main loop");
        bool firstFrame = true;
        while (true) {
            bool activeTransfer = manager.hasActiveTransfer();
            performance.setActive(activeTransfer);
            metadata.setImageNetworkPaused(activeTransfer);
            if (!brls::Application::mainLoop())
                break;
            if (firstFrame) {
                startupStage("main loop running");
                firstFrame = false;
            }
        }

        startupStage("manager shutdown");
        manager.shutdown();
        performance.setActive(false);
    } catch (const std::exception& error) {
        log_msg("[crash] exception at stage '%s': %s\n",
                "see previous startup marker", error.what());
    } catch (...) {
        log_msg("[crash] unknown exception\n");
    }

    startupStage("cleanup");
    if (esReady)
        esExit();
    if (nsReady)
        nsExit();
    if (ncmReady)
        ncmExit();
    if (curlReady)
        curl_global_cleanup();
    if (gBorealisLog) {
        std::fflush(gBorealisLog);
        std::fclose(gBorealisLog);
        gBorealisLog = nullptr;
    }
    log_close();
    return 0;
}
