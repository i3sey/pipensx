#pragma once

#include <memory>
#include <string>

#include <borealis.hpp>

#include "app/catalog_presentation.hpp"
#include "app/download_list_refresh.hpp"
#include "app/download_manager.hpp"
#include "app/switch_deploy.hpp"
#include "ui/common/async_image.hpp"
#include "ui/common/progress_bar.hpp"
#include "ui/common/ui_helpers.hpp"
#include "ui/i18n.hpp"
#include "ui/theme.hpp"

namespace pipensx::ui {

class DownloadCell : public brls::RecyclerCell {
public:
    static constexpr float kHeight = 104.0f;

    DownloadCell() {
        setFocusable(true);
        setAxis(brls::Axis::ROW);
        setAlignItems(brls::AlignItems::CENTER);
        setPadding(12, 16, 12, 16);
        setHeight(kHeight);
        setCornerRadius(theme::kRadiusLarge);
        setHighlightCornerRadius(theme::kRadiusLarge);
        if (theme::isCustom()) setLineBottom(0);

        thumb_ = new brls::Box();
        thumb_->setWidth(64);
        thumb_->setHeight(64);
        thumb_->setShrink(0);
        thumb_->setCornerRadius(theme::kRadiusSmall);
        thumb_->setBackgroundColor(theme::surface());
        thumb_->setMarginRight(16);
        thumb_->setAlignItems(brls::AlignItems::CENTER);
        thumb_->setJustifyContent(brls::JustifyContent::CENTER);
        placeholder_ = new brls::Label();
        placeholder_->setFontSize(24);
        placeholder_->setTextColor(theme::textSecondary());
        thumb_->addView(placeholder_);
        image_ = new AsyncRgbaImage();
        image_->setWidth(64);
        image_->setHeight(64);
        image_->setPositionType(brls::PositionType::ABSOLUTE);
        image_->setPositionTop(0);
        image_->setPositionLeft(0);
        image_->setCornerRadius(theme::kRadiusSmall);
        image_->setScalingType(brls::ImageScalingType::FILL);
        thumb_->addView(image_);
        addView(thumb_);

        auto* right = new brls::Box(brls::Axis::COLUMN);
        right->setGrow(1);
        right->setShrink(1);
        right->setJustifyContent(brls::JustifyContent::CENTER);

        auto* top = new brls::Box(brls::Axis::ROW);
        top->setAlignItems(brls::AlignItems::CENTER);
        title_ = new brls::Label();
        title_->setSingleLine(true);
        title_->setAutoAnimate(false);
        title_->setFontSize(22);
        title_->setGrow(1);
        title_->setShrink(1);
        title_->setMarginRight(24);
        status_ = new brls::Label();
        status_->setSingleLine(true);
        status_->setAutoAnimate(false);
        status_->setFontSize(theme::kFontCaption);
        status_->setTextColor(theme::accent());
        status_->setShrink(0);
        status_->setMaxWidth(180);
        top->addView(title_);
        top->addView(status_);

        meta_ = new brls::Label();
        meta_->setSingleLine(true);
        meta_->setAutoAnimate(false);
        meta_->setFontSize(theme::kFontSmall);
        meta_->setTextColor(theme::textSecondary());
        meta_->setMarginTop(4);
        meta_->setMarginBottom(4);

        progress_ = new ProgressBar();
        right->addView(top);
        right->addView(meta_);
        right->addView(progress_);
        addView(right);
        updateActionHint(brls::BUTTON_A, tr("pipensx/downloads/details"));
    }

    void setTask(const DownloadTask& task, GameMetadataService* service,
                 const SwitchDeploySnapshot* deploy = nullptr,
                 const std::string& appRoot = {},
                 CatalogService* catalog = nullptr) {
        updateActionHint(brls::BUTTON_A, tr("pipensx/downloads/details"));
        const uint64_t deployGen = deploy ? deploy->generation : 0;
        const uint64_t deployBytes = deploy && deploy->taskId == task.id
            ? deploy->bytesCopied : 0;
        const DownloadRowPaint nextPaint = DownloadRowPaint::from(task);
        const bool sameTask = paintedId_ == task.id;
        if (sameTask && paintedStatus_ == task.status && painted_ == nextPaint &&
            paintedDeployGen_ == deployGen && paintedDeployBytes_ == deployBytes) {
            refreshArtwork(task, service, appRoot, catalog);
            return;
        }

        paintedId_ = task.id;
        paintedStatus_ = task.status;
        painted_ = nextPaint;
        paintedDeployGen_ = deployGen;
        paintedDeployBytes_ = deployBytes;

        const auto wanted = downloadProgressBytes(task);

        setTextIfChanged(title_, task.name);
        setTextIfChanged(placeholder_, placeholderLetter(task.name));
        setTextIfChanged(status_, taskStatusText(task));
        status_->setTextColor(statusColor(task.status));
        const float progress = task.status == DownloadStatus::Fetching
            ? static_cast<float>(task.fetchProgress)
            : (task.mode == TransferMode::StreamInstall &&
               (task.status == DownloadStatus::Downloading ||
                task.status == DownloadStatus::Installing ||
                task.status == DownloadStatus::Committing ||
                task.status == DownloadStatus::Checking ||
                task.status == DownloadStatus::Verifying))
                ? streamInstallProgressOf(task)
            : (task.status == DownloadStatus::Installing ||
               task.status == DownloadStatus::Committing)
                ? installProgressOf(task) : progressOf(task);
        progress_->setProgress(progress);

        std::string meta = formatBytes(wanted.first) + " / " +
                           formatBytes(wanted.second);
        if (task.status == DownloadStatus::Installing ||
            task.status == DownloadStatus::Committing) {
            meta = tr("pipensx/downloads/cell_package",
                      task.packagesInstalled + 1, task.packageCount);
            if (!task.currentPackage.empty())
                meta += "   " + task.currentPackage;
            if (auto eta = taskEtaSeconds(task, now_ms()))
                meta += tr("pipensx/downloads/cell_eta",
                           formatEtaSeconds(*eta));
        } else if (task.status == DownloadStatus::Installed) {
            meta = tr("pipensx/downloads/cell_installed_packages",
                      task.packagesInstalled);
        } else if (task.status == DownloadStatus::Fetching) {
            meta = tr("pipensx/downloads/cell_fetching",
                      percentOf(static_cast<float>(task.fetchProgress)));
        } else if (task.status == DownloadStatus::Downloading) {
            meta += "   " + formatSpeed(task.speedBytesPerSecond);
            if (task.source == TaskSource::Torrent)
                meta += tr("pipensx/downloads/cell_peers", task.peers);
            if (auto eta = taskEtaSeconds(task, now_ms()))
                meta += tr("pipensx/downloads/cell_eta",
                           formatEtaSeconds(*eta));
        } else if (task.status == DownloadStatus::Queued)
            meta += deploy && deploy->active() &&
                            task.mode == TransferMode::StreamInstall
                ? tr("pipensx/deploy/waiting_stream")
                : tr("pipensx/downloads/cell_waiting");
        else if (task.status == DownloadStatus::Error && !task.error.empty())
            meta += "   " + task.error;
        if (deploy && deploy->taskId == task.id) {
            if (deploy->active()) {
                const char* phaseKey =
                    deploy->phase == SwitchDeployPhase::Preparing
                        ? "pipensx/deploy/phase_preparing"
                        : deploy->phase == SwitchDeployPhase::Extracting
                              ? "pipensx/deploy/phase_extracting"
                              : deploy->phase ==
                                        SwitchDeployPhase::CommittingPackage
                                    ? "pipensx/downloads/status_committing"
                                    : deploy->phase ==
                                              SwitchDeployPhase::InstallingPackages
                                          ? "pipensx/downloads/status_installing"
                                          : "pipensx/deploy/phase_copying";
                setTextIfChanged(status_, tr(phaseKey));
                status_->setTextColor(theme::accent());
                progress_->setProgress(deploy->totalBytes
                    ? static_cast<float>(deploy->bytesCopied) /
                          static_cast<float>(deploy->totalBytes)
                    : 0.0f);
                meta = tr("pipensx/deploy/cell_progress",
                          percentOf(deploy->totalBytes
                                        ? static_cast<float>(deploy->bytesCopied) /
                                              static_cast<float>(deploy->totalBytes)
                                        : 0.0f),
                          deploy->filesCopied, deploy->totalFiles,
                          formatBytes(deploy->bytesCopied),
                          formatBytes(deploy->totalBytes));
                if (!deploy->currentPath.empty())
                    meta += "   " + deploy->currentPath;
            } else if (deploy->phase == SwitchDeployPhase::Completed) {
                setTextIfChanged(status_, tr("pipensx/deploy/completed"));
                status_->setTextColor(theme::success());
            } else if (deploy->phase == SwitchDeployPhase::Failed) {
                setTextIfChanged(status_, tr("pipensx/deploy/failed"));
                status_->setTextColor(theme::error());
                if (!deploy->detail.empty())
                    meta = deploy->detail;
            } else if (deploy->phase == SwitchDeployPhase::Cancelled) {
                setTextIfChanged(status_, tr("pipensx/deploy/cancelled"));
            }
        }
        const bool deployActive = deploy && deploy->taskId == task.id &&
                                  deploy->active();
        const bool finished = task.status == DownloadStatus::Completed ||
                              task.status == DownloadStatus::Installed;
        const bool deployed = deploy && deploy->taskId == task.id &&
                              deploy->phase == SwitchDeployPhase::Completed;
        const bool deployProblem = deploy && deploy->taskId == task.id &&
            (deploy->phase == SwitchDeployPhase::Failed ||
             deploy->phase == SwitchDeployPhase::Cancelled);
        if (!deployActive && finished && !deployProblem) {
            meta = tr(deployed || task.status == DownloadStatus::Installed
                ? "pipensx/downloads/result_installed"
                : "pipensx/downloads/result_saved", formatBytes(wanted.second));
        }
        progress_->setVisibility(finished && !deployActive
            ? brls::Visibility::GONE : brls::Visibility::VISIBLE);
        status_->setVisibility(finished && !deployActive && !deployProblem
            ? brls::Visibility::GONE : brls::Visibility::VISIBLE);
        if (!deployActive && task.status == DownloadStatus::Error &&
            !task.error.empty())
            meta = task.error;
        if (!deployActive && (task.seeding || task.uploadedBytes || task.seedCacheBytes)) {
            meta += "   " + tr("pipensx/seeding/stats",
                formatSpeed(task.uploadSpeedBytesPerSecond), formatBytes(task.uploadedBytes));
            if (task.seeding && !deployProblem) {
                setTextIfChanged(status_, tr("pipensx/seeding/status"));
                status_->setVisibility(brls::Visibility::VISIBLE);
            }
        }
        setTextIfChanged(meta_, meta);

        refreshArtwork(task, service, appRoot, catalog);
    }

    void refreshArtwork(const DownloadTask& task, GameMetadataService* service,
                        const std::string& appRoot,
                        CatalogService* catalog) {
        const GameMetadata* metadata = service
            ? service->findByInfoHash(task.id) : nullptr;
        const CatalogEntry* entry = catalog
            ? catalog->findByInfoHash(task.id) : nullptr;
        std::string iconUrl = entry ? resolveCatalogRow(*entry, metadata).iconUrl
            : metadata ? metadata->iconUrl : std::string{};
        if (iconUrl.empty() && !appRoot.empty())
            iconUrl = deployedPortIconPath(appRoot, task.id);
        setArtworkUrl(image_, service, iconUrl, currentIconUrl_, imageState_,
                      GameMetadataService::kImageDimIcon);
    }

    void onFocusGained() override {
        brls::RecyclerCell::onFocusGained();
        title_->setAnimated(true);
    }

    void onFocusLost() override {
        brls::RecyclerCell::onFocusLost();
        title_->setAnimated(false);
    }

    void draw(NVGcontext* vg, float x, float y, float width, float height,
              brls::Style style, brls::FrameContext* ctx) override {
        if (theme::isCustom())
            theme::drawPlate(vg, x, y + 4, width, height - 8, theme::kRadiusLarge, isFocused());
        brls::RecyclerCell::draw(vg, x, y, width, height, style, ctx);
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
    std::string paintedId_;
    DownloadStatus paintedStatus_ = DownloadStatus::Queued;
    DownloadRowPaint painted_;
    uint64_t paintedDeployGen_ = 0;
    uint64_t paintedDeployBytes_ = 0;
};

}  // namespace pipensx::ui
