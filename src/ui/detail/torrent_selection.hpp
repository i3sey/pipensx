#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <borealis.hpp>

#include "app/download_manager.hpp"
#include "app/install_space.hpp"
#include "ui/common/ui_helpers.hpp"
#include "ui/theme.hpp"

namespace pipensx::ui {

struct TorrentSelectionEntry {
    std::string path;
    uint64_t length = 0;
    bool package = false;
    FileAction action = FileAction::Download;
};

inline const char* actionLabel(FileAction action) {
    switch (action) {
        case FileAction::Install: return "Install";
        case FileAction::Download: return "Download";
        case FileAction::Skip: return "Skip";
    }
    return "Skip";
}

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
        mark_->setTextColor(theme::accent());
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
        meta_->setTextColor(theme::textTertiary());
        body->addView(meta_);

        addView(body);
    }

    void setEntry(const TorrentSelectionEntry& entry) {
        mark_->setText(entry.action == FileAction::Install ? "[I]" :
                       entry.action == FileAction::Download ? "[D]" : "[ ]");
        title_->setText(entry.path);
        std::string meta = formatBytes(entry.length);
        meta += "   ";
        meta += entry.package ? "NSP/NSZ package" : "Other file";
        meta += "   ";
        meta += actionLabel(entry.action);
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
        for (auto& entry : entries_) {
            entry.action = selected
                ? (entry.package ? FileAction::Install : FileAction::Download)
                : FileAction::Skip;
        }
    }

    size_t selectedCount() const {
        size_t count = 0;
        for (const auto& entry : entries_)
            if (entry.action != FileAction::Skip)
                ++count;
        return count;
    }

    size_t installCount() const {
        size_t count = 0;
        for (const auto& entry : entries_)
            if (entry.action == FileAction::Install)
                ++count;
        return count;
    }

    size_t downloadCount() const {
        size_t count = 0;
        for (const auto& entry : entries_)
            if (entry.action == FileAction::Download)
                ++count;
        return count;
    }

    std::vector<uint8_t> fileActions() const {
        std::vector<uint8_t> actions;
        actions.reserve(entries_.size());
        for (const auto& entry : entries_) {
            actions.push_back(static_cast<uint8_t>(entry.action));
        }
        return actions;
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

    void cycleRow(int row);

private:
    TorrentSelectionActivity* owner_;
    std::vector<TorrentSelectionEntry> entries_;
};

class TorrentSelectionActivity : public brls::Activity {
public:
    friend class TorrentSelectionDataSource;

    TorrentSelectionActivity(DownloadManager* manager, std::string path,
                             pipensx::TorrentPreview preview,
                             TransferMode preferred,
                             StreamSelection initialSelection,
                             std::vector<uint8_t> initialPeers = {})
        : manager_(manager), path_(std::move(path)),
          preview_(std::move(preview)), preferred_(preferred),
          initialSelection_(initialSelection),
          initialPeers_(std::move(initialPeers)) {
        auto* content = new brls::Box(brls::Axis::COLUMN);
        content->setGrow(1);
        content->setPadding(24, 38, 24, 34);
        content->setBackgroundColor(theme::overlay());
        content->setCornerRadius(12);

        title_ = new brls::Label();
        title_->setFontSize(26);
        title_->setText("Choose torrent files");
        content->addView(title_);

        summary_ = new brls::Label();
        summary_->setFontSize(15);
        summary_->setMarginTop(8);
        summary_->setMarginBottom(14);
        summary_->setTextColor(theme::textSecondary());
        summary_->setText("A toggles a file. Selection follows Settings.");
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
        registerAction("Continue", brls::BUTTON_RB,
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
            if (preferred_ == TransferMode::StreamInstall && file.package) {
                entry.action = FileAction::Install;
            } else if (initialSelection_ == StreamSelection::AllFiles ||
                       preferred_ != TransferMode::StreamInstall) {
                entry.action = FileAction::Download;
            } else {
                entry.action = FileAction::Skip;
            }
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
        size_t installs = dataSource_->installCount();
        size_t downloads = dataSource_->downloadCount();
        std::vector<uint8_t> actions = dataSource_->fileActions();
        const TransferMode mode = installs > 0
            ? TransferMode::StreamInstall
            : TransferMode::DownloadOnly;
        const auto estimate = pipensx::estimateInstallSpace(preview_, actions,
                                                            mode);
        const StorageSpaceSnapshot storage =
            pipensx::queryStorageSpace(manager_->rootPath());
        const auto check = pipensx::assessInstallSpace(estimate, storage);
        std::string text = std::to_string(selected) + " / " +
                           std::to_string(preview_.files.size()) +
                           " files   " + formatBytes(estimate.requiredBytes);
        text += storage.available
            ? "   SD free: " + formatBytes(storage.freeBytes)
            : "   SD free: unavailable";
        if (installs > 0) {
            text += "   Install: " + std::to_string(installs);
            if (downloads > 0)
                text += "   Download: " + std::to_string(downloads);
        } else {
            text += "   Download-only selection";
        }
        if (estimate.certainty ==
            SpaceEstimateCertainty::CompressedUnknown) {
            text += "   NSZ may expand";
        }
        if (check.status == InstallSpaceCheckStatus::Insufficient)
            text += "   Need " + formatBytes(check.shortfallBytes) + " more";
        summary_->setText(text);
        installSelected_->setState(selected == 0 || estimate.overflow ||
                                    check.status ==
                                        InstallSpaceCheckStatus::Insufficient
            ? brls::ButtonState::DISABLED
            : brls::ButtonState::ENABLED);
    }

    void confirmSelection() {
        std::vector<uint8_t> actions = dataSource_->fileActions();
        if (actions.empty() && preview_.files.empty()) {
            brls::Application::notify("No files found in this torrent.");
            return;
        }
        size_t selected = dataSource_->selectedCount();
        if (selected == 0) {
            brls::Application::notify("Select at least one file.");
            return;
        }

        TransferMode mode = dataSource_->installCount() > 0
            ? TransferMode::StreamInstall
            : TransferMode::DownloadOnly;
        const auto estimate = pipensx::estimateInstallSpace(preview_, actions,
                                                            mode);
        const StorageSpaceSnapshot storage =
            pipensx::queryStorageSpace(manager_->rootPath());
        if (pipensx::assessInstallSpace(estimate, storage).status ==
            InstallSpaceCheckStatus::Insufficient) {
            refreshSummary();
            brls::Application::notify("Not enough free space on SD.");
            return;
        }
        std::string id;
        std::string error;
        if (!manager_->importTorrentActions(path_, actions, id, error,
                                            initialPeers_)) {
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
    StreamSelection initialSelection_;
    std::vector<uint8_t> initialPeers_;
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

}  // namespace pipensx::ui
