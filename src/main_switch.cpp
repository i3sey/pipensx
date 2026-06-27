#include "app/download_manager.hpp"
#include "app/catalog_service.hpp"
#include "app/magnet_resolver.hpp"
#include "app/game_metadata_service.hpp"
#include "core/antizapret.h"
#include "platform/switch_crashlog.h"

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
#include <cstdio>
#include <cstring>
#include <ctime>
#include <exception>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

using pipensx::DownloadManager;
using pipensx::DownloadStatus;
using pipensx::DownloadTask;
using pipensx::CatalogEntry;
using pipensx::CatalogService;
using pipensx::GameMetadata;
using pipensx::GameMetadataService;
using pipensx::MagnetResolver;
using pipensx::TransferMode;

namespace {

FILE* gBorealisLog = nullptr;
std::atomic<uint32_t> gCatalogTempSerial{0};

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
        transfer_->setText("Download speed: " +
                           formatSpeed(task->speedBytesPerSecond));
        recordSpeedSample(*task);
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
            appendSpeedSample(installSpeedSamples_, installSpeed);
        } else {
            hasInstallSample_ = false;
            lastInstallBytes_ = 0;
            lastInstallSampleMs_ = now;
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
    MainView(DownloadManager* manager, GameMetadataService* metadata)
        : brls::Box(brls::Axis::COLUMN), manager_(manager), metadata_(metadata) {
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
        bool structureChanged = !initialized_ || next.size() != tasks_.size();
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
    brls::RecyclerFrame* recycler_;
    DownloadDataSource* dataSource_;
    brls::RepeatingTimer timer_;
    std::vector<DownloadTask> tasks_;
    bool initialized_ = false;
    bool fastRefresh_ = false;
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

        // Right: title + badge on top, size/date underneath.
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

    void setEntry(const CatalogEntry& entry, const std::string& badge,
                  const std::string& iconUrl, GameMetadataService* service) {
        title_->setText(entry.title);
        placeholder_->setText(placeholderLetter(entry.title));
        badge_->setText(badge);
        std::string sub = entry.size ? formatBytes(entry.size) : "Unknown size";
        sub += "   " + formatCatalogDate(entry.publishedAt);
        sub_->setText(sub);
        setArtworkUrl(image_, service, iconUrl, currentIconUrl_, imageState_);
    }

private:
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
                    std::vector<std::string> badges,
                    std::vector<std::string> gameNames,
                    std::vector<std::string> iconUrls,
                    GameMetadataService* metadata) {
        entries_ = std::move(entries);
        badges_ = std::move(badges);
        gameNames_ = std::move(gameNames);
        iconUrls_ = std::move(iconUrls);
        metadata_ = metadata;
    }
    void setMessage(const std::string& message) { message_ = message; }
    const CatalogEntry* entryAt(int row) const {
        if (row < 0 || static_cast<size_t>(row) >= entries_.size())
            return nullptr;
        return &entries_[static_cast<size_t>(row)];
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
                       row < badges_.size() ? badges_[row] : std::string(),
                       row < iconUrls_.size() ? iconUrls_[row] : std::string(),
                       metadata_);
        return cell;
    }
    void didSelectRowAt(brls::RecyclerFrame*, brls::IndexPath index) override;

private:
    CatalogView* owner_;
    std::vector<CatalogEntry> entries_;
    std::vector<std::string> badges_;
    std::vector<std::string> gameNames_;
    std::vector<std::string> iconUrls_;
    GameMetadataService* metadata_ = nullptr;
    std::string message_;
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
                             TransferMode preferred)
        : manager_(manager), path_(std::move(path)),
          preview_(std::move(preview)), preferred_(preferred) {
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
        summary_->setText("A toggles a file. Default keeps everything selected.");
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
        registerAction("Install selected", brls::BUTTON_RB,
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
            entry.selected = true;
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
        std::string text = std::to_string(selected) + " / " +
                           std::to_string(preview_.files.size()) +
                           " files selected";
        if (selectedPackages > 0) {
            text += "   " + std::to_string(selectedPackages) +
                    " package file(s) selected";
        } else {
            text += "   Download-only selection";
        }
        summary_->setText(text);
        installSelected_->setState(selected == 0
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
                       FailureCallback onFailure, ChangeCallback onChange)
        : entry_(std::move(entry)), lastFailure_(std::move(lastFailure)),
          manager_(manager), metadata_(metadata),
          onFailure_(std::move(onFailure)), onChange_(std::move(onChange)),
          alive_(std::make_shared<std::atomic<bool>>(true)),
          cancelled_(std::make_shared<std::atomic<bool>>(false)) {
        const GameMetadata* found = metadata_->findByInfoHash(entry_.infoHash);

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
        if (primary_)
            brls::Application::giveFocus(primary_);
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

        primary_ = new brls::Button();
        primary_->setStyle(&brls::BUTTONSTYLE_PRIMARY);
        primary_->setFontSize(23);
        primary_->setHeight(64);
        primary_->setMarginBottom(12);
        primary_->setText("Stream install");
        primary_->registerClickAction([this](brls::View*) {
            onPrimary();
            return true;
        });
        actions->addView(primary_);

        secondary_ = new brls::Button();
        secondary_->setStyle(&brls::BUTTONSTYLE_DEFAULT);
        secondary_->setFontSize(20);
        secondary_->setHeight(56);
        secondary_->setText("Download only");
        secondary_->registerClickAction([this](brls::View*) {
            if (!busy_)
                startInstall(TransferMode::DownloadOnly);
            return true;
        });
        actions->addView(secondary_);

        statusLabel_ = new brls::Label();
        statusLabel_->setFontSize(16);
        statusLabel_->setMarginTop(12);
        statusLabel_->setTextColor(nvgRGB(0, 195, 227));
        statusLabel_->setText("Stream install adds packages as they arrive.");
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
            if (!found->screenshots.empty()) {
                auto* shots = new brls::Label();
                shots->setFontSize(16);
                shots->setMarginTop(18);
                shots->setMarginBottom(8);
                shots->setTextColor(nvgRGB(190, 190, 200));
                shots->setText("Screenshot");
                content->addView(shots);
                appendAsyncImage(content, metadata_,
                                 found->screenshots.front(), 170);
            }
        } else {
            auto* missing = new brls::Label();
            missing->setFontSize(17);
            missing->setMarginTop(16);
            missing->setText("No game artwork match yet. Install still works "
                             "from the catalog release.");
            content->addView(missing);
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
        return "Stream install";
    }

    // Reflect live task state on the buttons. Skipped while resolving so the
    // inline progress text isn't clobbered.
    void refreshButtons() {
        if (busy_)
            return;
        const DownloadTask* task = currentTask();
        if (task) {
            primary_->setText(installButtonLabel(*task));
            primary_->setState(brls::ButtonState::DISABLED);
            secondary_->setState(brls::ButtonState::DISABLED);
            if (task->status == DownloadStatus::Error && !task->error.empty())
                statusLabel_->setText(task->error);
        } else {
            primary_->setText("Stream install");
            primary_->setState(brls::ButtonState::ENABLED);
            secondary_->setState(brls::ButtonState::ENABLED);
        }
    }

    void onPrimary() {
        if (busy_)
            return;
        // Default to Install; the actual mode is chosen after resolve from the
        // package count, so a base-only torrent gracefully falls back.
        startInstall(TransferMode::StreamInstall);
    }

    // One-tap: resolve the magnet inline, then import immediately (no second
    // dialog). preferred is the mode to use when the torrent has installable
    // packages; package-less torrents always import as DownloadOnly.
    void startInstall(TransferMode preferred) {
        if (busy_)
            return;
        busy_ = true;
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
        brls::async([this, alive, cancelled, magnet, tmp, preferred] {
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
            brls::sync([this, alive, ok, err, tmp, preferred] {
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
                    statusLabel_->setText(reason);
                    primary_->setText("Stream install");
                    primary_->setState(brls::ButtonState::ENABLED);
                    secondary_->setState(brls::ButtonState::ENABLED);
                    brls::Application::notify(err);
                    ::unlink(tmp.c_str());
                    return;
                }
                if (onFailure_)
                    onFailure_(hash, "");  // clear stale failure
                finishImport(tmp, preferred);
            });
        });
    }

    void finishImport(const std::string& path, TransferMode preferred) {
        pipensx::TorrentPreview preview;
        std::string error;
        if (!DownloadManager::previewTorrent(path, preview, error)) {
            statusLabel_->setText(error);
            primary_->setText("Stream install");
            primary_->setState(brls::ButtonState::ENABLED);
            secondary_->setState(brls::ButtonState::ENABLED);
            brls::Application::notify(error);
            ::unlink(path.c_str());
            return;
        }
        if (preview.files.size() > 1 && preview.packageCount > 0) {
            brls::Application::pushActivity(new TorrentSelectionActivity(
                manager_, path, std::move(preview), preferred));
            return;
        }
        TransferMode mode = preview.packageCount ? preferred
                                                 : TransferMode::DownloadOnly;
        std::string id;
        std::string err;
        if (manager_->importTorrent(path, mode, id, err)) {
            log_msg("[catalog] imported torrent %s\n", id.c_str());
            statusLabel_->setText(preview.packageCount
                ? "Added. Installing to SD..."
                : "Added to downloads.");
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
            statusLabel_->setText(err);
            primary_->setText("Stream install");
            primary_->setState(brls::ButtonState::ENABLED);
            secondary_->setState(brls::ButtonState::ENABLED);
            brls::Application::notify(err);
        }
        ::unlink(path.c_str());
        refreshButtons();
    }

    CatalogEntry entry_;
    std::string lastFailure_;
    DownloadManager* manager_;
    GameMetadataService* metadata_;
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

class CatalogView : public brls::Box {
public:
    CatalogView(DownloadManager* manager, CatalogService* catalog,
                GameMetadataService* metadata)
        : brls::Box(brls::Axis::COLUMN), manager_(manager), catalog_(catalog),
          metadata_(metadata),
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
        addView(status_);
        addView(recycler_);
        rebuildEntries();

        registerAction("Search", brls::BUTTON_X, [this](brls::View*) {
            openSearchKeyboard();
            return true;
        });
        registerAction("Sort / Stop", brls::BUTTON_Y, [this](brls::View*) {
            if (busy_)
                cancelled_->store(true);
            else
                cycleSort();
            return true;
        });
        registerAction("Refresh", brls::BUTTON_RB, [this](brls::View*) {
            refreshCatalog();
            return true;
        });
    }

    ~CatalogView() override {
        alive_->store(false);
        cancelled_->store(true);
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
            if (!needle.empty() &&
                lowerAscii(entry.title).find(needle) == std::string::npos)
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

        std::vector<std::string> badges;
        std::vector<std::string> gameNames;
        std::vector<std::string> iconUrls;
        badges.reserve(visible.size());
        gameNames.reserve(visible.size());
        iconUrls.reserve(visible.size());
        for (const CatalogEntry& entry : visible) {
            std::string hash = lowerAscii(entry.infoHash);
            auto it = added.find(hash);
            if (it != added.end()) {
                badges.push_back(badgeForStatus(it->second));
            } else {
                auto fail = catalogFailures_.find(hash);
                badges.push_back(fail == catalogFailures_.end()
                    ? badgeForCatalogHealth(entry) : fail->second);
            }
            const GameMetadata* meta = metadata_->findByInfoHash(entry.infoHash);
            gameNames.push_back(meta ? meta->name : std::string());
            iconUrls.push_back(meta ? meta->iconUrl : std::string());
        }

        size_t count = visible.size();
        dataSource_->setEntries(std::move(visible), std::move(badges),
                                std::move(gameNames), std::move(iconUrls),
                                metadata_);
        dataSource_->setMessage(query_.empty()
            ? "Catalog is empty. Press R to refresh."
            : "Nothing found. Press X to change the search.");
        recycler_->reloadData();

        countText_ = query_.empty()
            ? withThousands(count) + (count == 1 ? " release" : " releases")
            : withThousands(count) + (count == 1 ? " match" : " matches");
        if (!busy_)
            status_->setText(countText_);
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
        busy_ = true;
        brls::Application::notify("Updating catalog from GitHub...");
        auto alive = alive_;
        CatalogService* catalog = catalog_;
        brls::async([this, alive, catalog] {
            std::string err;
            bool ok = catalog->refresh(err);
            brls::sync([this, alive, ok, err] {
                if (!alive->load())
                    return;
                busy_ = false;
                if (!ok) {
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
    brls::RecyclerFrame* recycler_;
    CatalogDataSource* dataSource_;
    brls::Label* status_;
    std::shared_ptr<std::atomic<bool>> alive_;
    std::shared_ptr<std::atomic<bool>> cancelled_;
    std::unordered_map<std::string, std::string> catalogFailures_;
    std::string query_;
    std::string countText_;
    SortMode sort_ = SortMode::Latest;
    bool busy_ = false;
};

void CatalogDataSource::didSelectRowAt(brls::RecyclerFrame*,
                                       brls::IndexPath index) {
    if (!entryAt(index.row))
        owner_->openSearchKeyboard();
    else
        owner_->onEntrySelected(index.row);
}

class MainActivity : public brls::Activity {
public:
    MainActivity(DownloadManager* manager, CatalogService* catalog,
                 GameMetadataService* metadata)
        : manager_(manager), catalog_(catalog), metadata_(metadata) {
        auto* tabs = new brls::TabFrame();
        tabs->addTab("Downloads", [manager, metadata] {
            return new MainView(manager, metadata);
        });
        tabs->addTab("Catalog", [manager, catalog, metadata] {
            return new CatalogView(manager, catalog, metadata);
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
    brls::AppletFrame* frame_;
};

} // namespace

int main(int, char**) {
    switch_crashlog_install();
    switch_crashlog_stage("creating application directories");
    mkdir("sdmc:/switch", 0755);
    mkdir("sdmc:/switch/pipensx", 0755);
    log_init("sdmc:/switch/pipensx/pipensx.log");
    startupStage("entered main");

    gBorealisLog = std::fopen(
        "sdmc:/switch/pipensx/pipensx.log", "a");
    if (gBorealisLog) {
        brls::Logger::setLogOutput(gBorealisLog);
        brls::Logger::setLogLevel(brls::LogLevel::LOG_DEBUG);
    }

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

        startupStage("DownloadManager construction");
        DownloadManager manager("sdmc:/switch/pipensx");

        startupStage("MainActivity construction");
        auto* activity = new MainActivity(&manager, &catalog, &metadata);

        startupStage("push MainActivity");
        brls::Application::pushActivity(activity);

        startupStage("first main loop");
        bool firstFrame = true;
        while (brls::Application::mainLoop()) {
            if (firstFrame) {
                startupStage("main loop running");
                firstFrame = false;
            }
        }

        startupStage("manager shutdown");
        manager.shutdown();
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
