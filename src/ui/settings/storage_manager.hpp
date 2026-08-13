#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <borealis.hpp>

#include "app/download_manager.hpp"
#include "app/game_metadata_service.hpp"
#include "app/storage_manager.hpp"
#include "ui/common/storage_meter.hpp"
#include "ui/common/ui_helpers.hpp"
#include "ui/i18n.hpp"
#include "ui/settings/settings_cells.hpp"
#include "ui/theme.hpp"

namespace pipensx::ui {

// Storage Manager: an SD-card breakdown of pipensx-managed directories plus
// explicit, confirm-before-deleting cleanup actions. Each action reports the
// space it can recover before the confirmation dialog, and the breakdown is
// re-scanned after every change.
class StorageManagerActivity : public brls::Activity {
public:
    StorageManagerActivity(DownloadManager* manager,
                           GameMetadataService* metadata)
        : manager_(manager), metadata_(metadata),
          alive_(std::make_shared<std::atomic<bool>>(true)) {
        buildContent();
    }

    ~StorageManagerActivity() override { alive_->store(false); }

    brls::View* createContentView() override { return frame_; }

    void willAppear(bool resetState) override {
        brls::Activity::willAppear(resetState);
        refresh();
    }

private:
    void buildContent() {
        auto* content = new brls::Box(brls::Axis::COLUMN);
        content->setPadding(24, 34, 24, 34);

        meter_ = new StorageMeter();
        meter_->setHeader(tr("pipensx/storage/title_sd_card"));
        content->addView(meter_);

        addSection(content, tr("pipensx/storage/breakdown"));
        breakdown_ = new brls::Box(brls::Axis::COLUMN);
        breakdown_->setMarginBottom(10);
        content->addView(breakdown_);

        addSection(content, tr("pipensx/storage/cleanup"));
        clearCompleted_ = actionCell(
            tr("pipensx/storage/clear_completed"), "", [this] {
                confirmClearCompleted();
            });
        content->addView(clearCompleted_);
        clearImages_ = actionCell(
            tr("pipensx/storage/clear_images"), "", [this] {
                confirmClearImages();
            });
        content->addView(clearImages_);
        clearTorrents_ = actionCell(
            tr("pipensx/storage/clear_torrents"), "", [this] {
                confirmClearTorrents();
            });
        content->addView(clearTorrents_);
        clearTemporary_ = actionCell(
            tr("pipensx/storage/clear_temporary"), "", [this] {
                confirmClearTemporary();
            });
        content->addView(clearTemporary_);

        auto* scroll = new brls::ScrollingFrame();
        scroll->setGrow(1);
        scroll->setContentView(content);
        frame_ = new brls::AppletFrame(scroll);
        frame_->setTitle(tr("pipensx/storage/title"));
    }

    // Re-reads the breakdown and repaints the meter, the per-directory rows and
    // each action's recoverable-space detail. Runs on the UI thread only.
    void refresh() {
        if (!manager_)
            return;
        snapshot_ = scanStorageBreakdown(manager_->rootPath());

        if (meter_) {
            if (snapshot_.available)
                meter_->setStorage(snapshot_.totalBytes, snapshot_.freeBytes);
            else
                meter_->setUnavailable();
        }

        rebuildRows();

        if (clearCompleted_)
            clearCompleted_->setDetailText(completedDownloadsDetail());
        if (clearImages_)
            clearImages_->setDetailText(
                recoverableDetail(snapshot_.imageCacheBytes));
        if (clearTorrents_)
            clearTorrents_->setDetailText(
                recoverableDetail(orphanTorrentBytes()));
        if (clearTemporary_)
            clearTemporary_->setDetailText(
                recoverableDetail(snapshot_.temporaryBytes));
    }

    void rebuildRows() {
        if (!breakdown_)
            return;
        breakdown_->clearViews();
        addRow(tr("pipensx/storage/cat_downloads"), snapshot_.downloadsBytes);
        addRow(tr("pipensx/storage/cat_torrents"), snapshot_.torrentBytes);
        addRow(tr("pipensx/storage/cat_images"), snapshot_.imageCacheBytes);
        addRow(tr("pipensx/storage/cat_metadata"), snapshot_.metadataCacheBytes);
        addRow(tr("pipensx/storage/cat_temporary"), snapshot_.temporaryBytes);
        addRow(tr("pipensx/storage/cat_icons"), snapshot_.iconsBytes);
        addRow(tr("pipensx/storage/cat_other"), snapshot_.otherBytes);
    }

    void addRow(const std::string& label, uint64_t bytes) {
        auto* row = new brls::Box(brls::Axis::ROW);
        row->setFocusable(false);
        row->setAlignItems(brls::AlignItems::CENTER);
        row->setMarginBottom(4);

        auto* name = new brls::Label();
        name->setSingleLine(true);
        name->setGrow(1);
        name->setFontSize(17);
        name->setTextColor(theme::textSecondary());
        name->setText(label);
        row->addView(name);

        auto* value = new brls::Label();
        value->setSingleLine(true);
        value->setFontSize(17);
        value->setTextColor(theme::textPrimary());
        value->setText(formatBytes(bytes));
        row->addView(value);

        breakdown_->addView(row);
    }

    std::string recoverableDetail(uint64_t bytes) {
        return bytes == 0 ? tr("pipensx/storage/nothing_to_recover")
                          : tr("pipensx/storage/recoverable", formatBytes(bytes));
    }

    // Sum of the on-disk bytes held by completed (finished but not yet
    // removed) downloads — the target of "Clear completed downloads".
    uint64_t completedDownloadsBytes() const {
        uint64_t total = 0;
        for (const DownloadTask& task : manager_->snapshot()) {
            if (task.status != DownloadStatus::Completed)
                continue;
            uint64_t size = 0;
            if (directorySize(task.dataPath, size))
                total = size > UINT64_MAX - total ? UINT64_MAX : total + size;
        }
        return total;
    }

    std::string completedDownloadsDetail() {
        uint64_t bytes = completedDownloadsBytes();
        if (bytes == 0)
            return tr("pipensx/storage/nothing_to_recover");
        return tr("pipensx/storage/recoverable", formatBytes(bytes));
    }

    // Size of `.torrent` files not backed by a live task.
    uint64_t orphanTorrentBytes() const {
        std::vector<DownloadTask> tasks = manager_->snapshot();
        std::vector<std::string> active;
        active.reserve(tasks.size());
        for (const DownloadTask& task : tasks)
            active.push_back(task.id);
        return pipensx::orphanTorrentBytes(manager_->torrentRoot(), active);
    }

    void confirmClearCompleted() {
        const uint64_t bytes = completedDownloadsBytes();
        if (bytes == 0) {
            brls::Application::notify(
                tr("pipensx/storage/nothing_to_recover"));
            return;
        }
        confirm(tr("pipensx/storage/clear_completed_confirm",
                   formatBytes(bytes)), [this] { clearCompleted(); });
    }

    void confirmClearImages() {
        confirmAction(snapshot_.imageCacheBytes, [this] { clearImages(); });
    }

    void confirmClearTorrents() {
        confirmAction(orphanTorrentBytes(), [this] { clearTorrents(); });
    }

    void confirmClearTemporary() {
        confirmAction(snapshot_.temporaryBytes, [this] { clearTemporary(); });
    }

    void confirmAction(uint64_t bytes, const std::function<void()>& action) {
        if (bytes == 0) {
            brls::Application::notify(
                tr("pipensx/storage/nothing_to_recover"));
            return;
        }
        confirm(tr("pipensx/storage/confirm_recover", formatBytes(bytes)),
                action);
    }

    void confirm(const std::string& message,
                 const std::function<void()>& action) {
        auto* dialog = new brls::Dialog(message);
        dialog->addButton(tr("pipensx/common/clear"), [action] { action(); });
        dialog->addButton(tr("pipensx/common/cancel"), [] {});
        dialog->open();
    }

    void clearCompleted() {
        std::vector<DownloadTask> tasks = manager_->snapshot();
        std::vector<std::string> ids;
        for (const DownloadTask& task : tasks)
            if (task.status == DownloadStatus::Completed)
                ids.push_back(task.id);
        for (const std::string& id : ids) {
            std::string error;
            if (!manager_->remove(id, true, error))
                diagnostic_error("storage", id.c_str(), "error=%s",
                                 error.c_str());
        }
        brls::Application::notify(tr("pipensx/storage/cleared"));
        refresh();
    }

    void clearImages() {
        std::string error;
        if (!metadata_->clearImageCache(error)) {
            diagnostic_error("storage", "images", "error=%s", error.c_str());
            brls::Application::notify(error);
            return;
        }
        brls::Application::notify(tr("pipensx/storage/cleared"));
        refresh();
    }

    void clearTorrents() {
        std::vector<DownloadTask> tasks = manager_->snapshot();
        std::vector<std::string> active;
        active.reserve(tasks.size());
        for (const DownloadTask& task : tasks)
            active.push_back(task.id);
        std::string error;
        uint64_t recovered = 0;
        if (!clearOrphanTorrents(manager_->torrentRoot(), active, error,
                                 recovered)) {
            diagnostic_error("storage", "torrents", "error=%s",
                             error.c_str());
            brls::Application::notify(error);
            return;
        }
        brls::Application::notify(tr("pipensx/storage/cleared"));
        refresh();
    }

    void clearTemporary() {
        std::string error;
        uint64_t recovered = 0;
        if (!clearTemporaryFiles(manager_->rootPath(), error, recovered)) {
            diagnostic_error("storage", "temporary", "error=%s",
                             error.c_str());
            brls::Application::notify(error);
            return;
        }
        brls::Application::notify(tr("pipensx/storage/cleared"));
        refresh();
    }

    DownloadManager* manager_;
    GameMetadataService* metadata_;
    std::shared_ptr<std::atomic<bool>> alive_;
    brls::AppletFrame* frame_ = nullptr;
    StorageMeter* meter_ = nullptr;
    brls::Box* breakdown_ = nullptr;
    brls::DetailCell* clearCompleted_ = nullptr;
    brls::DetailCell* clearImages_ = nullptr;
    brls::DetailCell* clearTorrents_ = nullptr;
    brls::DetailCell* clearTemporary_ = nullptr;
    StorageBreakdown snapshot_;
};

} // namespace pipensx::ui
