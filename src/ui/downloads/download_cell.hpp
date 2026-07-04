#pragma once

#include <memory>
#include <string>

#include <borealis.hpp>

#include "app/download_manager.hpp"
#include "ui/common/async_image.hpp"
#include "ui/common/progress_bar.hpp"
#include "ui/common/ui_helpers.hpp"
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
        title_->setText(task.name);
        placeholder_->setText(placeholderLetter(task.name));
        status_->setText(taskStatusText(task));
        status_->setTextColor(statusColor(task.status));
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
        } else if (task.status == DownloadStatus::Downloading) {
            meta += "   " + formatSpeed(task.speedBytesPerSecond) +
                    "   " + std::to_string(task.peers) + " peers";
            if (task.totalBytes > task.completedBytes) {
                std::string eta = formatEta(task.totalBytes - task.completedBytes,
                                            task.speedBytesPerSecond);
                if (!eta.empty())
                    meta += "   ETA " + eta;
            }
        } else if (task.status == DownloadStatus::Queued)
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

}  // namespace pipensx::ui
