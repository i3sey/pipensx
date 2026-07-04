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
#include "ui/theme.hpp"

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

        // F3: eShop-style two-column page. Left column is fixed (cover +
        // install button + size/status); the right column scrolls on its own.
        auto* content = new brls::Box(brls::Axis::ROW);
        content->setPadding(24, 40, 24, 40);
        buildLeftColumn(content, found);
        buildRightColumn(content, found);

        frame_ = new brls::AppletFrame(content);
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
    static constexpr float kLeftColumnWidth = 320.0f;

    // Left column: cover, full-width Install/Options, size, status (F3).
    void buildLeftColumn(brls::Box* content, const GameMetadata* found) {
        auto* left = new brls::Box(brls::Axis::COLUMN);
        left->setWidth(kLeftColumnWidth);
        left->setMarginRight(32);

        if (found) {
            const std::string& coverUrl = !found->iconUrl.empty()
                ? found->iconUrl : found->bannerUrl;
            if (!coverUrl.empty()) {
                auto* cover = new AsyncRgbaImage();
                cover->setWidth(kLeftColumnWidth);
                cover->setHeight(260);
                cover->setCornerRadius(theme::kRadiusLarge);
                cover->setScalingType(brls::ImageScalingType::FIT);
                loadImageInto(cover, metadata_, coverUrl);
                left->addView(cover);
            }
        }

        primary_ = new brls::Button();
        primary_->setStyle(&brls::BUTTONSTYLE_PRIMARY);
        primary_->setFontSize(theme::kFontBody);
        primary_->setHeight(64);
        primary_->setMarginTop(16);
        primary_->setText("Install");
        primary_->registerClickAction([this](brls::View*) {
            onPrimary();
            return true;
        });
        left->addView(primary_);

        secondary_ = new brls::Button();
        secondary_->setStyle(&brls::BUTTONSTYLE_DEFAULT);
        secondary_->setFontSize(theme::kFontSmall);
        secondary_->setHeight(56);
        secondary_->setMarginTop(12);
        secondary_->setText("Options");
        secondary_->registerClickAction([this](brls::View*) {
            onSecondary();
            return true;
        });
        left->addView(secondary_);

        auto* size = new brls::Label();
        size->setFontSize(theme::kFontCaption);
        size->setMarginTop(16);
        size->setTextColor(theme::textTertiary());
        size->setText("Download size: " +
                      (entry_.size ? formatBytes(entry_.size)
                                   : std::string("Unknown")));
        left->addView(size);

        statusLabel_ = new brls::Label();
        statusLabel_->setFontSize(theme::kFontCaption);
        statusLabel_->setMarginTop(8);
        statusLabel_->setTextColor(theme::accent());
        statusLabel_->setText("Install adds this game to your console.");
        left->addView(statusLabel_);

        content->addView(left);
    }

    // Right column (scrolls): title, fact table, screenshots, description.
    void buildRightColumn(brls::Box* content, const GameMetadata* found) {
        auto* right = new brls::Box(brls::Axis::COLUMN);
        right->setPadding(0, 12, 24, 0);

        auto* title = new brls::Label();
        title->setFontSize(theme::kFontTitle);
        title->setText(found ? found->name : entry_.title);
        right->addView(title);

        buildFactsTable(right, found);

        std::vector<std::string> screenshots =
            pipensx::mergeScreenshotUrls(found, entry_, 6);
        if (!screenshots.empty()) {
            auto* shots = new brls::Label();
            shots->setFontSize(theme::kFontSmall);
            shots->setMarginTop(24);
            shots->setMarginBottom(8);
            shots->setTextColor(theme::textSecondary());
            shots->setText("Screenshots");
            right->addView(shots);

            auto* rail = new brls::Box(brls::Axis::ROW);
            rail->setHeight(180);
            for (const std::string& url : screenshots) {
                auto* image = new AsyncRgbaImage();
                image->setWidth(300);
                image->setHeight(170);
                image->setMarginRight(12);
                image->setCornerRadius(theme::kRadiusSmall);
                image->setFocusable(true);
                image->setScalingType(brls::ImageScalingType::FIT);
                loadImageInto(image, metadata_, url);
                rail->addView(image);
            }
            auto* gallery = new brls::HScrollingFrame();
            gallery->setHeight(190);
            gallery->setContentView(rail);
            right->addView(gallery);
        }

        buildDescription(right, found);

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
            warning->setFontSize(theme::kFontCaption);
            warning->setMarginTop(16);
            warning->setTextColor(theme::warning());
            warning->setText(warn);
            right->addView(warning);
        }

        // Raw catalog release title (moved off the list per F2).
        auto* release = new brls::Label();
        release->setFontSize(theme::kFontCaption);
        release->setMarginTop(16);
        release->setTextColor(theme::textTertiary());
        release->setText("Release: " + entry_.title);
        right->addView(release);

        auto* scroll = new brls::ScrollingFrame();
        scroll->setGrow(1);
        scroll->setContentView(right);
        content->addView(scroll);
    }

    // S4: facts as label/value rows instead of one glued string.
    void buildFactsTable(brls::Box* right, const GameMetadata* found) {
        auto* table = new brls::Box(brls::Axis::COLUMN);
        table->setMarginTop(8);
        if (found) {
            addFactRow(table, "Publisher", found->publisher);
            addFactRow(table, "Release", found->releaseDate);
            addFactRow(table, "Genre", joinStrings(found->categories, ", "));
        }
        addFactRow(table, "Size",
                   entry_.size ? formatBytes(entry_.size)
                               : std::string("Unknown"));
        addFactRow(table, "Title ID", titleId_);
        right->addView(table);
    }

    void addFactRow(brls::Box* table, const std::string& name,
                    const std::string& value) {
        if (value.empty())
            return;
        auto* row = new brls::Box(brls::Axis::ROW);
        row->setMarginTop(8);
        auto* key = new brls::Label();
        key->setFontSize(theme::kFontCaption);
        key->setTextColor(theme::textTertiary());
        key->setWidth(160);
        key->setText(name);
        row->addView(key);
        auto* val = new brls::Label();
        val->setFontSize(theme::kFontCaption);
        val->setTextColor(theme::textSecondary());
        val->setGrow(1);
        val->setText(value);
        row->addView(val);
        table->addView(row);
    }

    // S5: reversible "Show more" instead of a hard cut.
    void buildDescription(brls::Box* right, const GameMetadata* found) {
        if (!found) {
            auto* missing = new brls::Label();
            missing->setFontSize(theme::kFontSmall);
            missing->setMarginTop(24);
            missing->setText("No game artwork match yet. Install still works "
                             "from the catalog release.");
            right->addView(missing);
            return;
        }
        std::string text = !found->description.empty() ? found->description
                                                       : found->intro;
        if (text.empty())
            return;
        auto* desc = new brls::Label();
        desc->setFontSize(theme::kFontSmall);
        desc->setMarginTop(24);
        bool truncated = text.size() > 900;
        desc->setText(truncated ? shortDescription(text) : text);
        right->addView(desc);
        if (truncated) {
            auto* more = new brls::Button();
            more->setStyle(&brls::BUTTONSTYLE_BORDERLESS);
            more->setFontSize(theme::kFontSmall);
            more->setTextColor(theme::accent());
            more->setMarginTop(4);
            more->setText("Show more");
            more->registerClickAction([this, desc, more,
                                       text = std::move(text)](brls::View*) {
                desc->setText(text);
                more->setVisibility(brls::Visibility::GONE);
                if (primary_)
                    brls::Application::giveFocus(primary_);
                return true;
            });
            right->addView(more);
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

        // Options path (O9): let the user pick the install mode before the
        // per-file picker. Each branch hands the temp file to the picker, which
        // owns it (unlinks on cancel).
        if (forcePicker) {
            chooseInstallMode(path, std::move(preview),
                              std::move(initialPeers));
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

    // O9: install-mode choice presented after resolve, before the per-file
    // picker. Carries the resolved temp torrent between the dialog buttons.
    struct PendingImport {
        std::string path;
        pipensx::TorrentPreview preview;
        std::vector<uint8_t> initialPeers;
    };

    // Clear "Stream install" vs "Download to SD" dialog with per-mode notes and
    // an up-front space estimate (InstallSpaceEstimate). The console picker
    // still enforces the hard space check per selection.
    void chooseInstallMode(const std::string& path,
                           pipensx::TorrentPreview preview,
                           std::vector<uint8_t> initialPeers) {
        const StorageSpaceSnapshot storage =
            pipensx::queryStorageSpace(manager_->rootPath());
        const auto estimate = pipensx::estimateInstallSpace(
            preview, {}, TransferMode::DownloadOnly);
        const bool compressed =
            pipensx::estimateInstallSpace(preview, {},
                                          TransferMode::StreamInstall)
                .certainty == SpaceEstimateCertainty::CompressedUnknown;

        std::string message =
            "This release is about " + formatBytes(estimate.requiredBytes) +
            " across " + std::to_string(preview.files.size()) +
            " file(s).\n\n"
            "Stream install — installs the game to your console as it "
            "downloads; the temporary download is cleared afterward.";
        if (compressed)
            message += " NSZ packages may expand further.";
        message +=
            "\n\nDownload to SD — keeps the game files on the SD card without "
            "installing.";
        if (storage.available) {
            message += "\n\nSD free: " + formatBytes(storage.freeBytes);
            const auto check = pipensx::assessInstallSpace(estimate, storage);
            if (check.status == InstallSpaceCheckStatus::Insufficient)
                message += "   Need " + formatBytes(check.shortfallBytes) +
                           " more.";
        } else {
            message += "\n\nSD free: unavailable";
        }

        auto pending = std::make_shared<PendingImport>(PendingImport{
            path, std::move(preview), std::move(initialPeers)});
        auto* dialog = new brls::Dialog(message);
        dialog->addButton("Stream install", [this, pending] {
            openSelection(pending, TransferMode::StreamInstall);
        });
        dialog->addButton("Download to SD", [this, pending] {
            openSelection(pending, TransferMode::DownloadOnly);
        });
        dialog->addButton("Cancel", [pending] {
            ::unlink(pending->path.c_str());
        });
        // Force an explicit choice so B can't dismiss and leak the temp file.
        dialog->setCancelable(false);
        dialog->open();
    }

    void openSelection(const std::shared_ptr<PendingImport>& pending,
                       TransferMode mode) {
        StreamSelection selection = settings_
            ? settings_->get().streamSelection : StreamSelection::AllFiles;
        brls::Application::pushActivity(new TorrentSelectionActivity(
            manager_, pending->path, std::move(pending->preview), mode,
            selection, std::move(pending->initialPeers)));
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
