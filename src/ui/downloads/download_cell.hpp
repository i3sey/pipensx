#pragma once

#include <memory>
#include <string>

#include <borealis.hpp>

#include "app/download_manager.hpp"
#include "ui/common/async_image.hpp"
#include "ui/common/progress_bar.hpp"
#include "ui/common/ui_helpers.hpp"
#include "ui/i18n.hpp"
#include "ui/theme.hpp"

namespace pipensx::ui {

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
        thumb_->setBackgroundColor(theme::surface());
        thumb_->setMarginRight(16);
        thumb_->setAlignItems(brls::AlignItems::CENTER);
        thumb_->setJustifyContent(brls::JustifyContent::CENTER);
        placeholder_ = new brls::Label();
        placeholder_->setFontSize(30);
        placeholder_->setTextColor(theme::textSecondary());
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
        status_->setTextColor(theme::accent());
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
        setTextIfChanged(title_, task.name);
        setTextIfChanged(placeholder_, placeholderLetter(task.name));
        setTextIfChanged(status_, taskStatusText(task));
        status_->setTextColor(statusColor(task.status));
        const float progress = task.status == DownloadStatus::Fetching
            ? static_cast<float>(task.fetchProgress)
            : (task.status == DownloadStatus::Installing ||
               task.status == DownloadStatus::Committing)
                ? installProgressOf(task) : progressOf(task);
        progress_->setProgress(progress);

        const auto wanted = downloadProgressBytes(task);
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
            meta += "   " + formatSpeed(task.speedBytesPerSecond) +
                    tr("pipensx/downloads/cell_peers", task.peers);
            if (auto eta = taskEtaSeconds(task, now_ms()))
                meta += tr("pipensx/downloads/cell_eta",
                           formatEtaSeconds(*eta));
        } else if (task.status == DownloadStatus::Queued)
            meta += tr("pipensx/downloads/cell_waiting");
        else if (task.status == DownloadStatus::Error && !task.error.empty())
            meta += "   " + task.error;
        setTextIfChanged(meta_, meta);

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

}  // namespace pipensx::ui
