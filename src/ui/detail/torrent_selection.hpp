#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include <borealis.hpp>

#include "app/download_manager.hpp"
#include "app/install_space.hpp"
#include "ui/common/action_icon.hpp"
#include "ui/common/storage_meter.hpp"
#include "ui/common/ui_helpers.hpp"
#include "ui/theme.hpp"

namespace pipensx::ui {

struct TorrentSelectionEntry {
    std::string path;
    uint64_t length = 0;
    bool package = false;
    bool compressed = false;
    bool cartridge = false;
    FileAction action = FileAction::Download;
};

// "bonus/extras/readme.txt" -> {"bonus/extras/", "readme.txt"}. The directory
// half is drawn dimmed so the filename still reads first on deep paths.
inline std::pair<std::string, std::string> splitPath(const std::string& path) {
    const size_t slash = path.find_last_of('/');
    if (slash == std::string::npos)
        return {std::string(), path};
    return {path.substr(0, slash + 1), path.substr(slash + 1)};
}

inline std::string fileKindLabel(const TorrentSelectionEntry& entry) {
    if (!entry.package)
        return entry.cartridge ? "XCI cartridge dump" : "Other file";
    if (entry.compressed)
        return "NSZ package · expands on install";
    return "NSP package";
}

class TorrentSelectionCell : public brls::RecyclerCell {
public:
    TorrentSelectionCell() {
        setFocusable(true);
        setAxis(brls::Axis::ROW);
        setAlignItems(brls::AlignItems::CENTER);
        setPadding(12, 20, 12, 20);
        setHeight(82);

        // The base RecyclerCell registers BUTTON_A as "OK"; re-registering the
        // same button replaces it (View::registerAction), so this only changes
        // the hint text in the applet frame's button bar — the click still
        // routes to the data source exactly as before.
        registerAction("Toggle", brls::BUTTON_A, [this](brls::View*) {
            auto* recycler =
                dynamic_cast<brls::RecyclerFrame*>(getParent()->getParent());
            if (recycler && recycler->getDataSource())
                recycler->getDataSource()->didSelectRowAt(recycler,
                                                          getIndexPath());
            return true;
        });

        icon_ = new ActionIcon();
        icon_->setMarginRight(14);
        addView(icon_);

        auto* body = new brls::Box(brls::Axis::COLUMN);
        body->setGrow(1);
        body->setJustifyContent(brls::JustifyContent::CENTER);

        auto* pathRow = new brls::Box(brls::Axis::ROW);
        pathRow->setFocusable(false);
        pathRow->setAlignItems(brls::AlignItems::BASELINE);

        directory_ = new brls::Label();
        directory_->setSingleLine(true);
        directory_->setFontSize(18);
        directory_->setTextColor(theme::textTertiary());
        pathRow->addView(directory_);

        name_ = new brls::Label();
        name_->setSingleLine(true);
        name_->setFontSize(18);
        name_->setGrow(1);
        pathRow->addView(name_);
        body->addView(pathRow);

        meta_ = new brls::Label();
        meta_->setSingleLine(true);
        meta_->setFontSize(14);
        meta_->setMarginTop(4);
        meta_->setTextColor(theme::textTertiary());
        body->addView(meta_);
        addView(body);

        // Fixed width, not auto: the sizes have to right-align as a column, and
        // an auto-width label would also spill under the scroll bar.
        size_ = new brls::Label();
        size_->setSingleLine(true);
        size_->setFontSize(17);
        size_->setWidth(110);
        size_->setMarginLeft(16);
        size_->setHorizontalAlign(brls::HorizontalAlign::RIGHT);
        size_->setTextColor(theme::textSecondary());
        addView(size_);
    }

    void setEntry(const TorrentSelectionEntry& entry) {
        const bool skipped = entry.action == FileAction::Skip;
        icon_->setKind(entry.action == FileAction::Install
                           ? ActionIconKind::Install
                           : entry.action == FileAction::Download
                                 ? ActionIconKind::Download
                                 : ActionIconKind::Skip);

        const auto [directory, name] = splitPath(entry.path);
        directory_->setText(directory);
        directory_->setVisibility(directory.empty() ? brls::Visibility::GONE
                                                    : brls::Visibility::VISIBLE);
        directory_->setTextColor(skipped ? theme::textDisabled()
                                         : theme::textTertiary());
        name_->setText(name);
        name_->setTextColor(skipped ? theme::textDisabled()
                                    : theme::textPrimary());
        meta_->setText(fileKindLabel(entry));
        meta_->setTextColor(skipped ? theme::textDisabled()
                                    : theme::textTertiary());
        size_->setText(formatBytes(entry.length));
        size_->setTextColor(skipped ? theme::textDisabled()
                                    : theme::textSecondary());
    }

    // What this cell is showing right now, as opposed to what the data source
    // holds. The golden harness uses it to prove a toggle actually repainted
    // the live cell instead of silently doing nothing.
    std::string renderedState() const {
        switch (icon_->kind()) {
            case ActionIconKind::Install: return "install";
            case ActionIconKind::Download: return "download";
            default: return "skip";
        }
    }

    // Torrents with no files at all: one row explaining why the list is empty.
    void setEmpty() {
        icon_->setKind(ActionIconKind::Skip);
        directory_->setVisibility(brls::Visibility::GONE);
        name_->setText("No files in this torrent");
        name_->setTextColor(theme::textDisabled());
        meta_->setText("");
        size_->setText("");
    }

private:
    ActionIcon* icon_;
    brls::Label* directory_;
    brls::Label* name_;
    brls::Label* meta_;
    brls::Label* size_;
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
        return 1;
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
          initialPeers_(std::move(initialPeers)),
          storage_(pipensx::queryStorageSpace(manager->rootPath())) {
        auto* content = new brls::Box(brls::Axis::COLUMN);
        content->setGrow(1);
        content->setPadding(18, 38, 18, 34);
        content->setBackgroundColor(theme::overlay());
        content->setCornerRadius(12);

        title_ = new brls::Label();
        title_->setFontSize(26);
        title_->setText("Choose torrent files");
        content->addView(title_);

        // Counts on the left, icon key on the right. Sharing one row with the
        // summary keeps the key free: the list is the scarce space on this
        // screen, and a legend of its own would cost it another row.
        auto* summaryRow = new brls::Box(brls::Axis::ROW);
        summaryRow->setFocusable(false);
        summaryRow->setAlignItems(brls::AlignItems::CENTER);
        summaryRow->setMarginTop(6);
        summaryRow->setMarginBottom(10);

        summary_ = new brls::Label();
        summary_->setSingleLine(true);
        summary_->setGrow(1);
        summary_->setFontSize(15);
        summary_->setTextColor(theme::textSecondary());
        summaryRow->addView(summary_);

        addLegendEntry(summaryRow, ActionIconKind::Install, "Install");
        addLegendEntry(summaryRow, ActionIconKind::Download, "Download");
        addLegendEntry(summaryRow, ActionIconKind::Skip, "Skip");
        content->addView(summaryRow);

        meter_ = new StorageMeter();
        meter_->setHeader("SD card");
        meter_->setLegendVisible(true);
        meter_->setMarginBottom(10);
        content->addView(meter_);

        recycler_ = new brls::RecyclerFrame();
        recycler_->setGrow(1);
        recycler_->setPadding(6, 0, 6, 0);
        recycler_->estimatedRowHeight = 82;
        recycler_->registerCell("FileSelect",
                               [] { return new TorrentSelectionCell(); });
        dataSource_ = new TorrentSelectionDataSource(this);
        recycler_->setDataSource(dataSource_);
        content->addView(recyclerHost(recycler_));

        auto* buttons = new brls::Box(brls::Axis::COLUMN);
        buttons->setMarginTop(12);

        auto* row = new brls::Box(brls::Axis::ROW);
        row->setMarginBottom(8);

        selectAll_ = new brls::Button();
        selectAll_->setStyle(&brls::BUTTONSTYLE_DEFAULT);
        selectAll_->setFontSize(18);
        selectAll_->setHeight(46);
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
        clearAll_->setHeight(46);
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
        installSelected_->setHeight(54);
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
    // One "<glyph> Label" pair of the icon key. Same glyphs the rows draw, so
    // the key can never drift from what is actually on screen.
    static void addLegendEntry(brls::Box* row, ActionIconKind kind,
                               const std::string& text) {
        auto* icon = new ActionIcon(kind, 18.0f);
        icon->setMarginLeft(16);
        icon->setMarginRight(6);
        row->addView(icon);

        auto* label = new brls::Label();
        label->setSingleLine(true);
        label->setFontSize(theme::kFontCaption);
        label->setTextColor(theme::textTertiary());
        label->setText(text);
        row->addView(label);
    }

    void populateEntries() {
        std::vector<TorrentSelectionEntry> entries;
        entries.reserve(preview_.files.size());
        for (const auto& file : preview_.files) {
            TorrentSelectionEntry entry;
            entry.path = file.path;
            entry.length = file.length;
            entry.package = file.package;
            entry.compressed = file.compressed;
            entry.cartridge = file.cartridge;
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
        for (auto* cell : visibleCells<TorrentSelectionCell>(recycler_))
            repaint(cell);
        refreshSummary();
    }

    // Repainting the one row that changed keeps the cursor and the scroll
    // offset exactly where they were. reloadData() would recycle every cell,
    // snap the scroll to 0 and re-home focus on defaultCellFocus.
    void repaintRow(int row) {
        for (auto* cell : visibleCells<TorrentSelectionCell>(recycler_)) {
            if (cell->getIndexPath().row == row)
                repaint(cell);
        }
    }

    void repaint(TorrentSelectionCell* cell) {
        if (const auto* entry = dataSource_->entryAt(cell->getIndexPath().row))
            cell->setEntry(*entry);
        else
            cell->setEmpty();
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
        const auto check = pipensx::assessInstallSpace(estimate, storage_);
        // The meter caption right below already prints the byte totals, so the
        // summary stays on counts.
        std::string text = std::to_string(selected) + " of " +
                           std::to_string(preview_.files.size()) + " files";
        if (installs > 0) {
            text += "  ·  " + std::to_string(installs) + " install";
            if (downloads > 0)
                text += ", " + std::to_string(downloads) + " download";
        } else if (downloads > 0) {
            text += "  ·  download only";
        }
        summary_->setText(text);

        if (storage_.available)
            meter_->setEstimate(
                storage_.totalBytes, storage_.freeBytes, estimate.requiredBytes,
                check.status == InstallSpaceCheckStatus::Insufficient,
                estimate.certainty == SpaceEstimateCertainty::CompressedUnknown);
        else
            meter_->setUnavailable();
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
        // Authoritative gate: the cached snapshot may be stale if a background
        // download ate into the card while this screen was open, so re-query
        // and keep the fresh reading for the meter.
        storage_ = pipensx::queryStorageSpace(manager_->rootPath());
        if (pipensx::assessInstallSpace(estimate, storage_).status ==
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
    // Queried once at construction instead of once per A press: on Switch this
    // is an nsGetStorageSize IPC. confirmSelection() re-queries before it
    // commits, so a stale reading can never let an oversized install through.
    StorageSpaceSnapshot storage_;
    brls::AppletFrame* frame_ = nullptr;
    brls::Label* title_ = nullptr;
    brls::Label* summary_ = nullptr;
    StorageMeter* meter_ = nullptr;
    brls::RecyclerFrame* recycler_ = nullptr;
    TorrentSelectionDataSource* dataSource_ = nullptr;
    brls::Button* selectAll_ = nullptr;
    brls::Button* clearAll_ = nullptr;
    brls::Button* installSelected_ = nullptr;
    bool finished_ = false;
};

}  // namespace pipensx::ui
