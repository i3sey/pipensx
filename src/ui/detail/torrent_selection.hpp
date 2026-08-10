#pragma once

#include <cstdint>
#include <functional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <borealis.hpp>

#include "app/download_manager.hpp"
#include "app/install_space.hpp"
#include "app/nx_file_types.hpp"
#include "ui/common/action_icon.hpp"
#include "ui/common/storage_meter.hpp"
#include "ui/common/ui_helpers.hpp"
#include "ui/i18n.hpp"
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
        return entry.cartridge ? tr("pipensx/torrent/kind_cartridge")
                               : tr("pipensx/torrent/kind_other");
    if (entry.compressed)
        return tr("pipensx/torrent/kind_nsz");
    return tr("pipensx/torrent/kind_nsp");
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
        registerAction(tr("pipensx/common/toggle"), brls::BUTTON_A,
                       [this](brls::View*) {
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
        name_->setText(tr("pipensx/torrent/empty"));
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

    void selectPackagesOnly() {
        for (auto& entry : entries_) {
            entry.action = entry.package ? FileAction::Install
                                        : FileAction::Skip;
        }
    }

    void selectDownloadAll() {
        for (auto& entry : entries_)
            entry.action = FileAction::Download;
    }

    void selectPortFiles(const TorrentPreview& preview,
                         const std::string& root) {
        auto lower = [](std::string value) {
            for (char& ch : value)
                if (ch >= 'A' && ch <= 'Z')
                    ch = static_cast<char>(ch - 'A' + 'a');
            return value;
        };
        const std::string prefix = root.empty() ? std::string()
                                                : lower(root) + "/";
        for (size_t i = 0; i < entries_.size() && i < preview.files.size(); ++i) {
            const std::string logical = preview.multi
                ? preview.name + "/" + preview.files[i].path
                : preview.files[i].path;
            const std::string folded = lower(logical);
            const bool underRoot =
                !prefix.empty() && folded.rfind(prefix, 0) == 0 &&
                !entries_[i].package && !entries_[i].cartridge;
            const bool portArchive = isPortArchiveName(logical) &&
                !entries_[i].package && !entries_[i].cartridge;
            entries_[i].action = underRoot || portArchive
                ? FileAction::Download : FileAction::Skip;
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
                             std::vector<uint8_t> initialPeers = {},
                             DebridImport debridImport = {},
                             std::function<void()> abandon = {})
        : manager_(manager), path_(std::move(path)),
          preview_(std::move(preview)), preferred_(preferred),
          initialSelection_(initialSelection),
          initialPeers_(std::move(initialPeers)),
          debridImport_(std::move(debridImport)), abandon_(std::move(abandon)) {
        refreshStorageSnapshots();
        auto* content = new brls::Box(brls::Axis::COLUMN);
        content->setGrow(1);
        content->setPadding(18, 38, 18, 34);
        content->setBackgroundColor(theme::overlay());
        content->setCornerRadius(12);

        title_ = new brls::Label();
        title_->setFontSize(26);
        title_->setText(tr("pipensx/torrent/title"));
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

        addLegendEntry(summaryRow, ActionIconKind::Install,
                       tr("pipensx/torrent/legend_install"));
        addLegendEntry(summaryRow, ActionIconKind::Download,
                       tr("pipensx/torrent/legend_download"));
        addLegendEntry(summaryRow, ActionIconKind::Skip,
                       tr("pipensx/torrent/legend_skip"));
        content->addView(summaryRow);

        meter_ = new StorageMeter();
        meter_->setHeader(storageMeterHeader(manager_->installTarget()));
        meter_->setLegendVisible(true);
        meter_->setMarginBottom(10);
        content->addView(meter_);

        portRoot_ = candidatePortRoot();
        const bool portLayout = !portRoot_.empty() || hasPortArchive();
        if (portLayout) {
            portHint_ = new brls::Label();
            portHint_->setFontSize(theme::kFontCaption);
            portHint_->setTextColor(theme::accent());
            portHint_->setMarginBottom(8);
            portHint_->setText(tr("pipensx/torrent/port_detected"));
            content->addView(portHint_);
        }

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

        selectPackages_ = new brls::Button();
        selectPackages_->setStyle(&brls::BUTTONSTYLE_DEFAULT);
        selectPackages_->setFontSize(16);
        selectPackages_->setHeight(46);
        selectPackages_->setMarginRight(10);
        selectPackages_->setGrow(1);
        selectPackages_->setText(tr("pipensx/torrent/preset_packages"));
        selectPackages_->registerClickAction([this](brls::View*) {
            applyPreset([this] { dataSource_->selectPackagesOnly(); });
            return true;
        });
        row->addView(selectPackages_);

        selectDownloadAll_ = new brls::Button();
        selectDownloadAll_->setStyle(&brls::BUTTONSTYLE_DEFAULT);
        selectDownloadAll_->setFontSize(16);
        selectDownloadAll_->setHeight(46);
        selectDownloadAll_->setGrow(1);
        selectDownloadAll_->setText(tr("pipensx/torrent/preset_download_all"));
        selectDownloadAll_->registerClickAction([this](brls::View*) {
            applyPreset([this] { dataSource_->selectDownloadAll(); });
            return true;
        });
        row->addView(selectDownloadAll_);

        clearAll_ = new brls::Button();
        clearAll_->setStyle(&brls::BUTTONSTYLE_DEFAULT);
        clearAll_->setFontSize(16);
        clearAll_->setHeight(46);
        clearAll_->setMarginLeft(10);
        clearAll_->setGrow(1);
        clearAll_->setText(tr("pipensx/common/clear"));
        clearAll_->registerClickAction([this](brls::View*) {
            setAllSelected(false);
            return true;
        });
        row->addView(clearAll_);

        if (portLayout) {
            selectPort_ = new brls::Button();
            selectPort_->setStyle(&brls::BUTTONSTYLE_DEFAULT);
            selectPort_->setFontSize(16);
            selectPort_->setHeight(46);
            selectPort_->setMarginLeft(10);
            selectPort_->setGrow(1);
            selectPort_->setText(tr("pipensx/torrent/select_port"));
            selectPort_->registerClickAction([this](brls::View*) {
                applyPreset([this] {
                    dataSource_->selectPortFiles(preview_, portRoot_);
                });
                return true;
            });
            row->addView(selectPort_);
        }

        buttons->addView(row);

        installSelected_ = new brls::Button();
        installSelected_->setStyle(&brls::BUTTONSTYLE_PRIMARY);
        installSelected_->setFontSize(21);
        installSelected_->setHeight(54);
        installSelected_->setText(tr("pipensx/common/continue"));
        installSelected_->registerClickAction([this](brls::View*) {
            confirmSelection();
            return true;
        });
        buttons->addView(installSelected_);

        content->addView(buttons);
        frame_ = new brls::AppletFrame(content);
        frame_->setTitle(preview_.name.empty()
                             ? tr("pipensx/torrent/frame_title")
                                              : preview_.name);

        populateEntries();
        refreshSummary();
    }

    ~TorrentSelectionActivity() override {
        if (!finished_ && !path_.empty())
            ::unlink(path_.c_str());
        if (!finished_ && abandon_)
            abandon_();
    }

    brls::View* createContentView() override {
        return frame_;
    }

    void onContentAvailable() override {
        registerAction(tr("pipensx/torrent/preset_packages"), brls::BUTTON_X,
                       [this](brls::View*) {
            applyPreset([this] { dataSource_->selectPackagesOnly(); });
            return true;
        });
        registerAction(tr("pipensx/common/clear"), brls::BUTTON_Y,
                       [this](brls::View*) {
            setAllSelected(false);
            return true;
        });
        registerAction(tr("pipensx/common/continue"), brls::BUTTON_RB,
                       [this](brls::View*) {
                           confirmSelection();
                           return true;
                       });
    }

private:
    std::string candidatePortRoot() const {
        auto lower = [](std::string value) {
            for (char& ch : value)
                if (ch >= 'A' && ch <= 'Z')
                    ch = static_cast<char>(ch - 'A' + 'a');
            return value;
        };
        std::set<std::string> roots;
        for (const TorrentPreview::File& file : preview_.files) {
            const std::string logical = preview_.multi
                ? preview_.name + "/" + file.path : file.path;
            const std::string folded = lower(logical);
            if (folded.size() < 4 ||
                folded.compare(folded.size() - 4, 4, ".nro") != 0)
                continue;
            size_t start = 0;
            while (start < logical.size()) {
                const size_t slash = logical.find('/', start);
                const std::string component = logical.substr(
                    start, slash == std::string::npos ? std::string::npos
                                                       : slash - start);
                if (lower(component) == "switch") {
                    roots.insert(lower(
                        logical.substr(0, start + component.size())));
                    break;
                }
                if (slash == std::string::npos)
                    break;
                start = slash + 1;
            }
        }
        if (roots.size() == 1)
            return *roots.begin();
        return {};
    }

    bool hasPortArchive() const {
        for (const TorrentPreview::File& file : preview_.files) {
            const std::string logical = preview_.multi
                ? preview_.name + "/" + file.path : file.path;
            if (isPortArchiveName(logical))
                return true;
        }
        return false;
    }

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

    void applyPreset(const std::function<void()>& mutate) {
        mutate();
        recycler_->reloadData();
        refreshSummary();
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

    void refreshStorageSnapshots() {
        downloadStorage_ =
            pipensx::queryStorageSpace(manager_->rootPath());
        packageStorage_ = pipensx::queryInstallStorageSpace(
            manager_->installTarget(), manager_->rootPath());
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
        const auto check = pipensx::assessTransferSpace(
            estimate, downloadStorage_, packageStorage_);
        // The meter caption right below already prints the byte totals, so the
        // summary stays on counts.
        std::string text = tr("pipensx/torrent/summary", selected,
                              preview_.files.size());
        if (installs > 0) {
            text += tr("pipensx/torrent/summary_install", installs);
            if (downloads > 0)
                text += tr("pipensx/torrent/summary_download", downloads);
        } else if (downloads > 0) {
            text += tr("pipensx/torrent/summary_download_only");
        }
        summary_->setText(text);

        const auto meterTarget = installs > 0 ? manager_->installTarget()
            : pipensx::install::InstallStorageTarget::SdCard;
        const StorageSpaceSnapshot& meterStorage =
            installs > 0 ? packageStorage_ : downloadStorage_;
        meter_->setHeader(storageMeterHeader(meterTarget));
        if (meterStorage.available)
            meter_->setEstimate(
                meterStorage.totalBytes, meterStorage.freeBytes,
                installs > 0 ? estimate.packageBytes : estimate.downloadBytes,
                check.status == InstallSpaceCheckStatus::Insufficient,
                estimate.certainty == SpaceEstimateCertainty::CompressedUnknown);
        else
            meter_->setUnavailable();

        const std::string destination =
            installDestinationLabel(manager_->installTarget());
        if (selected == 0) {
            installSelected_->setText(tr("pipensx/common/continue"));
        } else if (installs > 0) {
            installSelected_->setText(tr(
                "pipensx/torrent/cta_install", installs, destination,
                formatBytes(estimate.requiredBytes)));
        } else {
            installSelected_->setText(tr(
                "pipensx/torrent/cta_download", downloads,
                formatBytes(estimate.requiredBytes)));
        }
        installSelected_->setState(selected == 0 || estimate.overflow ||
                                    check.status ==
                                        InstallSpaceCheckStatus::Insufficient
            ? brls::ButtonState::DISABLED
            : brls::ButtonState::ENABLED);
    }

    void confirmSelection() {
        std::vector<uint8_t> actions = dataSource_->fileActions();
        if (actions.empty() && preview_.files.empty()) {
            brls::Application::notify(tr("pipensx/torrent/no_files"));
            return;
        }
        size_t selected = dataSource_->selectedCount();
        if (selected == 0) {
            brls::Application::notify(tr("pipensx/torrent/select_one_file"));
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
        refreshStorageSnapshots();
        if (pipensx::assessTransferSpace(estimate, downloadStorage_,
                                         packageStorage_)
                .status == InstallSpaceCheckStatus::Insufficient) {
            refreshSummary();
            brls::Application::notify(tr("pipensx/torrent/no_space"));
            return;
        }
        std::string id;
        std::string error;
        bool imported;
        if (debridImport_.debridId.empty()) {
            imported = manager_->importTorrentActions(path_, actions, id, error,
                                                       initialPeers_);
        } else {
            DebridImport import = debridImport_;
            import.mode = mode;
            import.fileSelection = std::move(actions);
            import.packageCount = static_cast<uint32_t>(
                dataSource_->installCount());
            imported = manager_->importDebrid(import, id, error);
        }
        if (!imported) {
            brls::Application::notify(error);
            return;
        }
        if (!path_.empty())
            ::unlink(path_.c_str());
        finished_ = true;
        const std::string destination =
            installDestinationLabel(manager_->installTarget());
        brls::Application::notify(mode == TransferMode::StreamInstall
            ? tr("pipensx/torrent/added_installing", destination)
            : tr("pipensx/torrent/added"));
        brls::Application::popActivity();
    }

    DownloadManager* manager_;
    std::string path_;
    pipensx::TorrentPreview preview_;
    TransferMode preferred_;
    StreamSelection initialSelection_;
    std::vector<uint8_t> initialPeers_;
    DebridImport debridImport_;
    std::function<void()> abandon_;
    // Queried once at construction instead of once per A press: on Switch this
    // is an nsGetStorageSize IPC. confirmSelection() re-queries before it
    // commits, so a stale reading can never let an oversized install through.
    StorageSpaceSnapshot downloadStorage_;
    StorageSpaceSnapshot packageStorage_;
    brls::AppletFrame* frame_ = nullptr;
    brls::Label* title_ = nullptr;
    brls::Label* summary_ = nullptr;
    StorageMeter* meter_ = nullptr;
    brls::Label* portHint_ = nullptr;
    brls::RecyclerFrame* recycler_ = nullptr;
    TorrentSelectionDataSource* dataSource_ = nullptr;
    brls::Button* selectPackages_ = nullptr;
    brls::Button* selectDownloadAll_ = nullptr;
    brls::Button* clearAll_ = nullptr;
    brls::Button* installSelected_ = nullptr;
    brls::Button* selectPort_ = nullptr;
    std::string portRoot_;
    bool finished_ = false;
};

}  // namespace pipensx::ui
