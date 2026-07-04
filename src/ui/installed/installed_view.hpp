#pragma once

#include <atomic>
#include <memory>
#include <vector>

#include <borealis.hpp>

#include "app/download_manager.hpp"
#include "app/game_metadata_service.hpp"
#include "app/installed_title_service.hpp"
#include "ui/common/async_image.hpp"
#include "ui/common/ui_helpers.hpp"
#include "ui/theme.hpp"

namespace pipensx::ui {

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
        subtitle_->setTextColor(theme::textTertiary());
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
        status_->setTextColor(theme::textTertiary());
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

}  // namespace pipensx::ui
