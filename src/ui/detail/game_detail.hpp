#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <borealis.hpp>

#include "app/app_settings.hpp"
#include "app/catalog_presentation.hpp"
#include "app/catalog_service.hpp"
#include "app/download_manager.hpp"
#include "app/game_metadata_service.hpp"
#include "app/install_space.hpp"
#include "app/installed_title_service.hpp"
#include "app/magnet_resolver.hpp"
#include "ui/catalog/catalog_helpers.hpp"
#include "ui/common/async_image.hpp"
#include "ui/common/ui_helpers.hpp"
#include "ui/detail/torrent_selection.hpp"
#include "ui/downloads/details_activity.hpp"

namespace pipensx::ui {

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
        std::vector<uint8_t> infoDict = entry_.infoDict;
        std::string telemetryTag = catalogLower(entry_.infoHash);
        uint64_t startedMs = now_ms();
        brls::async([this, alive, cancelled, magnet, infoDict, tmp,
                     forcePicker, telemetryTag, startedMs] {
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
            std::vector<uint8_t> initialPeers;
            bool ok = resolver.resolveToFile(
                magnet, tmp, *cancelled, progress, err, &initialPeers,
                infoDict.empty() ? nullptr : &infoDict);
            telemetry_log("magnet", telemetryTag.c_str(),
                          "event=resolve ok=%d cancelled=%d duration_ms=%llu "
                          "verified_peers=%u",
                          ok ? 1 : 0, cancelled->load() ? 1 : 0,
                          (unsigned long long)(now_ms() - startedMs),
                          static_cast<unsigned>(initialPeers.size() / 6));
            brls::sync([this, alive, ok, err, tmp, forcePicker,
                        initialPeers = std::move(initialPeers)]() mutable {
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
                finishImport(tmp, forcePicker, std::move(initialPeers));
            });
        });
    }

    void finishImport(const std::string& path, bool forcePicker,
                      std::vector<uint8_t> initialPeers) {
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
                TransferMode::StreamInstall, selection,
                std::move(initialPeers)));
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
                                    id, err, initialPeers)) {
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

}  // namespace pipensx::ui
