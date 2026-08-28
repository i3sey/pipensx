#pragma once

#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <borealis.hpp>

#include "app/download_manager.hpp"
#include "app/install_space.hpp"
#include "ui/common/storage_meter.hpp"
#include "ui/common/ui_helpers.hpp"
#include "ui/detail/torrent_selection.hpp"
#include "ui/i18n.hpp"
#include "ui/theme.hpp"

namespace pipensx::ui {

// Multi-select chooser for "which of these packages is the update?" A
// release bundle can carry several packages with the update's [vN] tag — a
// mod reusing the version of the release it patches is the classic lookalike
// — so every update offer now lands here with the recommended packages
// preselected, and the user tunes the selection before importing.
//
// The previous implementation paged candidates through a brls::Dialog two at
// a time. The dialog's third slot is a full-width button on top, so the
// auxiliary "more/later" button outranked both candidates, and the two
// half-width bottom slots (~330 px each) cut long file names — exactly when
// the deep directory prefixes that tell the candidates apart matter. A
// compact list replaces it: TorrentSelectionCell draws the directory dimmed,
// the name readable and the byte size right-aligned, so deep paths and
// same-name files (e.g. "update.nsp" vs "mods/update.nsp", usually different
// sizes) resolve visually. A on a row toggles it; Continue imports the final
// mask.
class UpdateFileChooserActivity : public brls::Activity {
public:
    // `actions` is the full per-file recommendation mask from
    // selectUpdateFiles, parallel to preview.files; only package rows are
    // shown, preselected where the mask says Install. `initialPeers` are the
    // bootstrap peers from the magnet resolve — on a network where the
    // tracker is unreachable they are the only way an import can start — so
    // onPick hands them back untouched for the import.
    //
    // onPick fires with the final mask and those peers; onCancel fires when
    // the user backs out. The caller owns the tmp torrent and unlinks it in
    // both callbacks.
    UpdateFileChooserActivity(
        DownloadManager* manager, pipensx::TorrentPreview preview,
        std::vector<uint8_t> actions, std::vector<uint8_t> initialPeers,
        std::function<void(std::vector<uint8_t>, std::vector<uint8_t>)> onPick,
        std::function<void()> onCancel)
        : manager_(manager), preview_(std::move(preview)),
          actions_(std::move(actions)),
          initialPeers_(std::move(initialPeers)), onPick_(std::move(onPick)),
          onCancel_(std::move(onCancel)),
          alive_(std::make_shared<std::atomic<bool>>(true)) {
        auto* content = new brls::Box(brls::Axis::COLUMN);
        content->setGrow(1);
        content->setPadding(18, 38, 18, 34);
        content->setBackgroundColor(theme::overlay());
        content->setCornerRadius(12);

        title_ = new brls::Label();
        title_->setFontSize(26);
        title_->setText(tr("pipensx/installed/update_choose_file"));
        content->addView(title_);

        meter_ = new StorageMeter();
        meter_->setHeader(storageMeterHeader(
            manager_ ? manager_->installTarget()
                     : pipensx::install::InstallStorageTarget::SdCard));
        meter_->setLegendVisible(true);
        meter_->setMarginBottom(10);
        content->addView(meter_);

        recycler_ = new brls::RecyclerFrame();
        recycler_->setGrow(1);
        recycler_->setPadding(6, 0, 6, 0);
        recycler_->estimatedRowHeight = 82;
        recycler_->registerCell("FileSelect",
                                [] { return new TorrentSelectionCell(); });
        dataSource_ = new ChooserDataSource(this);
        dataSource_->setPreview(preview_, actions_);
        recycler_->setDataSource(dataSource_);
        content->addView(recyclerHost(recycler_));

        auto* buttons = new brls::Box(brls::Axis::ROW);
        buttons->setMarginTop(12);

        confirm_ = new brls::Button();
        confirm_->setStyle(&brls::BUTTONSTYLE_PRIMARY);
        confirm_->setFontSize(18);
        confirm_->setHeight(46);
        confirm_->setGrow(1);
        confirm_->setText(tr("pipensx/common/continue"));
        confirm_->registerClickAction([this](brls::View*) {
            confirm();
            return true;
        });
        buttons->addView(confirm_);

        auto* cancel = new brls::Button();
        cancel->setStyle(&brls::BUTTONSTYLE_DEFAULT);
        cancel->setFontSize(18);
        cancel->setHeight(46);
        cancel->setGrow(1);
        cancel->setMarginLeft(12);
        cancel->setText(tr("pipensx/common/cancel"));
        cancel->registerClickAction([this](brls::View*) {
            backOut();
            return true;
        });
        buttons->addView(cancel);
        content->addView(buttons);

        frame_ = new brls::AppletFrame(content);
        frame_->setTitle(preview_.name.empty()
                             ? tr("pipensx/torrent/frame_title")
                             : preview_.name);

        updateConfirm();
    }

    ~UpdateFileChooserActivity() override {
        alive_->store(false);
        // Backing out with B pops the activity without touching backOut(),
        // so the cancel contract is enforced here, like TorrentSelectionActivity.
        if (!finished_ && onCancel_)
            onCancel_();
    }

    brls::View* createContentView() override { return frame_; }

    void onContentAvailable() override {
        recycler_->setDefaultCellFocus(brls::IndexPath(0, 0));
        brls::Application::giveFocus(recycler_);
        registerAction(tr("pipensx/common/cancel"), brls::BUTTON_B,
                       [this](brls::View*) {
            backOut();
            return true;
        });
        refreshStorageSnapshots();
    }

    // Golden harness: the current per-file mask the rows show.
    const std::vector<uint8_t>& selection() const { return actions_; }

    // Golden harness: toggle by package-row index.
    void toggleRowForTest(size_t row) { toggle(row); }

private:
    void toggle(size_t row) {
        if (finished_ || validationInFlight_)
            return;
        const size_t fileIndex = dataSource_->fileIndexAt(row);
        if (fileIndex >= actions_.size())
            return;
        const bool installing =
            actions_[fileIndex] == static_cast<uint8_t>(FileAction::Install);
        actions_[fileIndex] =
            static_cast<uint8_t>(installing ? FileAction::Skip
                                            : FileAction::Install);
        dataSource_->setRowAction(
            row, static_cast<FileAction>(actions_[fileIndex]));
        for (auto* cell : visibleCells<TorrentSelectionCell>(recycler_))
            if (cell->getIndexPath().row == static_cast<int>(row))
                if (const auto* entry = dataSource_->entryAt(row))
                    cell->setEntry(*entry);
        updateConfirm();
    }

    void confirm() {
        if (finished_ || validationInFlight_ || !storageReady_)
            return;
        bool any = false;
        for (const uint8_t action : actions_)
            if (action == static_cast<uint8_t>(FileAction::Install)) {
                any = true;
                break;
            }
        if (!any) {
            brls::Application::notify(
                tr("pipensx/installed/update_nothing_selected"));
            return;
        }
        if (!manager_) {
            finishConfirm();
            return;
        }
        validationInFlight_ = true;
        updateConfirm();
        auto alive = alive_;
        const std::string root = manager_->rootPath();
        const auto target = manager_->installTarget();
        const auto estimate = pipensx::estimateInstallSpace(
            preview_, actions_, TransferMode::StreamInstall);
        brls::async([this, alive, root, target, estimate] {
            const auto downloadStorage = pipensx::queryStorageSpace(root);
            const auto packageStorage =
                target == pipensx::install::InstallStorageTarget::SdCard
                    ? downloadStorage
                    : pipensx::queryInstallStorageSpace(target, root);
            brls::sync([this, alive, estimate, downloadStorage,
                        packageStorage] {
                if (!alive->load() || finished_)
                    return;
                validationInFlight_ = false;
                downloadStorage_ = downloadStorage;
                packageStorage_ = packageStorage;
                storageReady_ = true;
                updateConfirm();
                if (pipensx::assessTransferSpace(
                        estimate, downloadStorage_, packageStorage_)
                        .status == InstallSpaceCheckStatus::Insufficient) {
                    brls::Application::notify(
                        tr("pipensx/torrent/no_space"));
                    return;
                }
                finishConfirm();
            });
        });
    }

    void finishConfirm() {
        finished_ = true;
        onPick_(std::move(actions_), std::move(initialPeers_));
        brls::Application::popActivity();
    }

    void backOut() {
        if (finished_)
            return;
        finished_ = true;
        onCancel_();
        brls::Application::popActivity();
    }

    void updateConfirm() {
        size_t installs = 0;
        for (const uint8_t action : actions_)
            if (action == static_cast<uint8_t>(FileAction::Install))
                ++installs;
        const auto estimate = pipensx::estimateInstallSpace(
            preview_, actions_, TransferMode::StreamInstall);
        const auto check = pipensx::assessTransferSpace(
            estimate, downloadStorage_, packageStorage_);
        const auto target = manager_ ? manager_->installTarget()
            : pipensx::install::InstallStorageTarget::SdCard;
        meter_->setHeader(storageMeterHeader(target));
        if (storageReady_ && packageStorage_.available)
            meter_->setEstimate(
                packageStorage_.totalBytes, packageStorage_.freeBytes,
                estimate.packageBytes,
                check.status == InstallSpaceCheckStatus::Insufficient,
                estimate.certainty == SpaceEstimateCertainty::CompressedUnknown);
        else
            meter_->setUnavailable();
        if (installs == 0) {
            confirm_->setText(tr("pipensx/common/continue"));
            confirm_->setState(brls::ButtonState::DISABLED);
            return;
        }
        confirm_->setText(tr(
            "pipensx/torrent/cta_install", installs,
            installDestinationLabel(target),
            formatBytes(estimate.requiredBytes)));
        confirm_->setState(
            !storageReady_ || validationInFlight_ || estimate.overflow ||
                    check.status == InstallSpaceCheckStatus::Insufficient
                ? brls::ButtonState::DISABLED
                : brls::ButtonState::ENABLED);
    }

    void refreshStorageSnapshots() {
        if (storageQueryInFlight_)
            return;
        if (!manager_) {
            storageReady_ = true;
            updateConfirm();
            return;
        }
        storageQueryInFlight_ = true;
        auto alive = alive_;
        const std::string root = manager_->rootPath();
        const auto target = manager_->installTarget();
        brls::async([this, alive, root, target] {
            const auto downloadStorage = pipensx::queryStorageSpace(root);
            const auto packageStorage =
                target == pipensx::install::InstallStorageTarget::SdCard
                    ? downloadStorage
                    : pipensx::queryInstallStorageSpace(target, root);
            brls::sync([this, alive, downloadStorage, packageStorage] {
                if (!alive->load())
                    return;
                storageQueryInFlight_ = false;
                downloadStorage_ = downloadStorage;
                packageStorage_ = packageStorage;
                storageReady_ = true;
                updateConfirm();
            });
        });
    }

    // Rows are the torrent's package files only; A on a row toggles it
    // between Install and Skip instead of making the whole choice.
    class ChooserDataSource : public brls::RecyclerDataSource {
    public:
        explicit ChooserDataSource(UpdateFileChooserActivity* owner)
            : owner_(owner) {}

        void setPreview(const pipensx::TorrentPreview& preview,
                        const std::vector<uint8_t>& actions) {
            entries_.clear();
            fileIndex_.clear();
            for (size_t index = 0; index < preview.files.size(); ++index) {
                const auto& file = preview.files[index];
                if (!file.package)
                    continue;
                TorrentSelectionEntry entry;
                entry.path = file.path;
                entry.length = file.length;
                entry.package = file.package;
                entry.compressed = file.compressed;
                entry.cartridge = file.cartridge;
                entry.action =
                    index < actions.size() &&
                            actions[index] ==
                                static_cast<uint8_t>(FileAction::Install)
                        ? FileAction::Install
                        : FileAction::Skip;
                entries_.push_back(std::move(entry));
                fileIndex_.push_back(index);
            }
        }

        // Row -> preview.files index, so a toggle lands on the right slot of
        // the import mask (which stays parallel to preview.files, gaps
        // included).
        size_t fileIndexAt(size_t row) const {
            return row < fileIndex_.size() ? fileIndex_[row] : SIZE_MAX;
        }

        void setRowAction(size_t row, FileAction action) {
            if (row < entries_.size())
                entries_[row].action = action;
        }

        const TorrentSelectionEntry* entryAt(size_t row) const {
            return row < entries_.size() ? &entries_[row] : nullptr;
        }

        int numberOfSections(brls::RecyclerFrame*) override { return 1; }

        int numberOfRows(brls::RecyclerFrame*, int) override {
            return static_cast<int>(entries_.size());
        }

        brls::RecyclerCell* cellForRow(brls::RecyclerFrame* recycler,
                                       brls::IndexPath index) override {
            auto* cell = static_cast<TorrentSelectionCell*>(
                recycler->dequeueReusableCell("FileSelect"));
            if (index.row >= 0 &&
                static_cast<size_t>(index.row) < entries_.size())
                cell->setEntry(entries_[static_cast<size_t>(index.row)]);
            return cell;
        }

        void didSelectRowAt(brls::RecyclerFrame*, brls::IndexPath index)
            override {
            if (index.row >= 0)
                owner_->toggle(static_cast<size_t>(index.row));
        }

    private:
        UpdateFileChooserActivity* owner_;
        std::vector<TorrentSelectionEntry> entries_;
        std::vector<size_t> fileIndex_;
    };

    DownloadManager* manager_ = nullptr;
    pipensx::TorrentPreview preview_;
    std::vector<uint8_t> actions_;
    std::vector<uint8_t> initialPeers_;
    std::function<void(std::vector<uint8_t>, std::vector<uint8_t>)> onPick_;
    std::function<void()> onCancel_;
    std::shared_ptr<std::atomic<bool>> alive_;
    StorageSpaceSnapshot downloadStorage_;
    StorageSpaceSnapshot packageStorage_;
    brls::AppletFrame* frame_ = nullptr;
    brls::Label* title_ = nullptr;
    StorageMeter* meter_ = nullptr;
    brls::RecyclerFrame* recycler_ = nullptr;
    ChooserDataSource* dataSource_ = nullptr;
    brls::Button* confirm_ = nullptr;
    bool storageQueryInFlight_ = false;
    bool storageReady_ = false;
    bool validationInFlight_ = false;
    bool finished_ = false;
};

}  // namespace pipensx::ui
