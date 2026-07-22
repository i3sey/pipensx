#pragma once

#include <functional>
#include <memory>
#include <unordered_set>
#include <vector>

#include <borealis.hpp>

#include "app/app_settings.hpp"
#include "app/catalog_batch_installer.hpp"
#include "app/catalog_service.hpp"
#include "app/download_manager.hpp"
#include "app/game_metadata_service.hpp"
#include "app/install_space.hpp"
#include "app/installed_title_service.hpp"
#include "ui/catalog/catalog_helpers.hpp"
#include "ui/common/action_icon.hpp"
#include "ui/common/storage_meter.hpp"
#include "ui/common/ui_helpers.hpp"
#include "ui/i18n.hpp"
#include "ui/theme.hpp"

namespace pipensx::ui {

class BatchInstallActivity;

class BatchInstallCell : public brls::RecyclerCell {
public:
    BatchInstallCell() {
        setFocusable(true);
        setAxis(brls::Axis::ROW);
        setAlignItems(brls::AlignItems::CENTER);
        setPadding(12, 20, 12, 20);
        setHeight(82);

        icon_ = new ActionIcon();
        icon_->setMarginRight(14);
        addView(icon_);

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
        meta_->setTextColor(theme::textTertiary());
        body->addView(title_);
        body->addView(meta_);
        addView(body);
    }

    void setReady(const PreparedCatalogInstall& item) {
        icon_->setKind(item.selected ? ActionIconKind::Checked
                                     : ActionIconKind::Unchecked);
        title_->setTextColor(item.selected ? theme::textPrimary()
                                           : theme::textDisabled());
        title_->setText(item.entry.title);
        std::string meta = tr("pipensx/batch/item_selected",
                              formatBytes(item.space.requiredBytes));
        if (item.space.packageFiles)
            meta += tr("pipensx/batch/item_packages", item.space.packageFiles);
        if (item.space.certainty ==
            SpaceEstimateCertainty::CompressedUnknown) {
            meta += tr("pipensx/batch/item_nsz");
        }
        meta_->setText(meta);
    }

    void setFailure(const pipensx::BatchItemFailure& failure) {
        icon_->setKind(ActionIconKind::Error);
        title_->setTextColor(theme::textPrimary());
        title_->setText(failure.entry.title);
        meta_->setText(failure.error);
    }

private:
    ActionIcon* icon_ = nullptr;
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

        auto resolver = [](const CatalogEntry& entry, const std::string& path,
                           std::atomic<bool>& cancelled,
                           const MagnetResolver::ProgressCallback& progress,
                           std::vector<uint8_t>& initialPeers,
                           std::string& error) {
            MagnetResolver instance;
            return instance.resolveToFile(
                entry.magnetUri, path, cancelled, progress, error,
                &initialPeers,
                entry.infoDict.empty() ? nullptr : &entry.infoDict);
        };
        installer_ = std::make_shared<CatalogBatchInstaller>(
            manager_->rootPath(), std::move(resolver));

        auto* content = new brls::Box(brls::Axis::COLUMN);
        content->setGrow(1);
        content->setPadding(24, 38, 24, 34);
        content->setBackgroundColor(theme::overlay());
        content->setCornerRadius(12);

        status_ = new brls::Label();
        status_->setFontSize(17);
        status_->setMarginBottom(12);
        status_->setTextColor(theme::textSecondary());
        status_->setText(tr("pipensx/batch/preparing"));
        content->addView(status_);

        meter_ = new StorageMeter();
        meter_->setHeader(tr("pipensx/batch/sd_card"));
        meter_->setLegendVisible(true);
        meter_->setMarginBottom(12);
        content->addView(meter_);

        recycler_ = new brls::RecyclerFrame();
        recycler_->setGrow(1);
        recycler_->setPadding(6, 0, 6, 0);
        recycler_->estimatedRowHeight = 82;
        recycler_->registerCell("BatchItem",
                                [] { return new BatchInstallCell(); });
        dataSource_ = new BatchInstallDataSource(this);
        recycler_->setDataSource(dataSource_);
        // Visibility is toggled on the host, not the recycler: the host is the
        // grow(1) box, so hiding only the recycler would leave its slot behind.
        recyclerHost_ = recyclerHost(recycler_);
        recyclerHost_->setVisibility(brls::Visibility::GONE);
        content->addView(recyclerHost_);

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
        selectAll->setText(tr("pipensx/batch/select_ready"));
        selectAll->registerClickAction([this](brls::View*) {
            setAllPrepared(true);
            return true;
        });
        row->addView(selectAll);
        auto* clear = new brls::Button();
        clear->setStyle(&brls::BUTTONSTYLE_DEFAULT);
        clear->setGrow(1);
        clear->setHeight(48);
        clear->setText(tr("pipensx/common/clear"));
        clear->registerClickAction([this](brls::View*) {
            setAllPrepared(false);
            return true;
        });
        row->addView(clear);
        controls_->addView(row);

        enqueue_ = new brls::Button();
        enqueue_->setStyle(&brls::BUTTONSTYLE_PRIMARY);
        enqueue_->setHeight(58);
        enqueue_->setText(tr("pipensx/batch/add_to_queue"));
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
        downloads->setText(tr("pipensx/batch/view_downloads"));
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
        resultBack_->setText(tr("pipensx/batch/back_to_catalog"));
        resultBack_->registerClickAction([](brls::View*) {
            brls::Application::popActivity();
            return true;
        });
        resultControls_->addView(resultBack_);
        content->addView(resultControls_);

        frame_ = new brls::AppletFrame(content);
        frame_->setTitle(tr("pipensx/batch/frame_title"));
    }

    ~BatchInstallActivity() override {
        alive_->store(false);
        cancelled_->store(true);
    }

    brls::View* createContentView() override { return frame_; }

    void onContentAvailable() override {
        cancelAction_ = registerAction(tr("pipensx/common/cancel"), brls::BUTTON_Y,
                                       [this](brls::View*) {
            cancelled_->store(true);
            status_->setText(tr("pipensx/batch/cancelling"));
            return true;
        });
        startPreparation();
    }

    void togglePrepared(size_t index) {
        if (!prepared_ || index >= prepared_->items().size())
            return;
        prepared_->items()[index].selected =
            !prepared_->items()[index].selected;
        repaintRow(static_cast<int>(index));
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
                        stage = tr("pipensx/batch/stage_peers");
                        break;
                    case pipensx::MagnetProgress::Stage::Connecting:
                        stage = tr("pipensx/batch/stage_connecting");
                        break;
                    case pipensx::MagnetProgress::Stage::FetchingMetadata:
                        stage = tr("pipensx/batch/stage_metadata");
                        break;
                    case pipensx::MagnetProgress::Stage::Validating:
                        stage = tr("pipensx/batch/stage_validating");
                        break;
                }
                std::string text = tr("pipensx/batch/progress", value.index,
                                      value.total, value.title, stage);
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
                    status_->setText(tr("pipensx/batch/cancelled"));
                    return;
                }
                dataSource_->setPreparation(prepared_);
                recyclerHost_->setVisibility(brls::Visibility::VISIBLE);
                controls_->setVisibility(brls::Visibility::VISIBLE);
                recycler_->reloadData();
                if (cancelAction_ != ACTION_NONE) {
                    unregisterAction(cancelAction_);
                    cancelAction_ = ACTION_NONE;
                }
                queueAction_ = registerAction(
                    tr("pipensx/batch/add_to_queue"), brls::BUTTON_RB,
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
        for (auto* cell : visibleCells<BatchInstallCell>(recycler_))
            repaint(cell);
        refreshSummary();
    }

    // Repaint in place so the cursor and scroll offset survive a toggle;
    // reloadData() would recycle every cell and snap focus back to row 0.
    void repaintRow(int row) {
        for (auto* cell : visibleCells<BatchInstallCell>(recycler_)) {
            if (cell->getIndexPath().row == row)
                repaint(cell);
        }
    }

    void repaint(BatchInstallCell* cell) {
        if (!prepared_)
            return;
        const size_t row = static_cast<size_t>(cell->getIndexPath().row);
        // Failure rows sit after the item rows and are not toggleable.
        if (row < prepared_->items().size())
            cell->setReady(prepared_->items()[row]);
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

        std::string text = tr("pipensx/batch/ready_selected", selected);
        if (!prepared_->failures().empty())
            text += tr("pipensx/batch/failed_count",
                       prepared_->failures().size());
        text += tr("pipensx/batch/selected_bytes",
                   formatBytes(estimate.requiredBytes));
        if (estimate.certainty ==
            SpaceEstimateCertainty::CompressedUnknown) {
            text += tr("pipensx/batch/nsz_note");
        }
        if (!storage_.available && !storage_.error.empty())
            text += "\n" + storage_.error;
        status_->setText(text);

        if (storage_.available)
            meter_->setEstimate(
                storage_.totalBytes, storage_.freeBytes, estimate.requiredBytes,
                check.status == InstallSpaceCheckStatus::Insufficient,
                estimate.certainty == SpaceEstimateCertainty::CompressedUnknown);
        else
            meter_->setUnavailable();

        const bool enabled = selected > 0 && !estimate.overflow &&
            check.status != InstallSpaceCheckStatus::Insufficient;
        enqueue_->setState(enabled ? brls::ButtonState::ENABLED
                                   : brls::ButtonState::DISABLED);
        enqueue_->setText(tr("pipensx/batch/add_n_to_queue", selected));
    }

    void enqueuePrepared() {
        if (!prepared_ || enqueueFinished_)
            return;
        const auto estimate = prepared_->selectedSpace();
        if (estimate.selectedFiles == 0 || estimate.overflow) {
            refreshSummary();
            brls::Application::notify(tr("pipensx/batch/select_one_ready"));
            return;
        }
        storage_ = pipensx::queryStorageSpace(manager_->rootPath());
        const auto check = pipensx::assessInstallSpace(estimate, storage_);
        if (check.status == InstallSpaceCheckStatus::Insufficient) {
            refreshSummary();
            brls::Application::notify(tr("pipensx/batch/no_space"));
            return;
        }

        pipensx::BatchEnqueueResult result =
            installer_->enqueue(*prepared_, *manager_);
        for (const std::string& hash : result.queuedInfoHashes)
            remaining_.erase(catalogLower(hash));
        if (completion_)
            completion_(remaining_);
        enqueueFinished_ = true;
        std::string message = tr("pipensx/batch/added",
                                 result.taskIds.size());
        const size_t failures = prepared_->failures().size() +
                                result.failures.size();
        if (failures)
            message += tr("pipensx/batch/added_failed", failures);
        if (result.skipped)
            message += tr("pipensx/batch/added_skipped", result.skipped);
        if (!remaining_.empty())
            message += tr("pipensx/batch/retry_note");
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
        recyclerHost_->setVisibility(brls::Visibility::GONE);
        controls_->setVisibility(brls::Visibility::GONE);
        resultControls_->setVisibility(brls::Visibility::VISIBLE);
        resultBack_->setText(remaining_.empty()
                                 ? tr("pipensx/batch/back_to_catalog")
                                 : tr("pipensx/batch/back_to_selected"));
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
    StorageMeter* meter_ = nullptr;
    brls::RecyclerFrame* recycler_ = nullptr;
    brls::Box* recyclerHost_ = nullptr;
    BatchInstallDataSource* dataSource_ = nullptr;
    brls::Box* controls_ = nullptr;
    brls::Button* enqueue_ = nullptr;
    brls::Box* resultControls_ = nullptr;
    brls::Button* resultBack_ = nullptr;
    brls::ActionIdentifier cancelAction_ = ACTION_NONE;
    brls::ActionIdentifier queueAction_ = ACTION_NONE;
    bool enqueueFinished_ = false;
};

}  // namespace pipensx::ui
