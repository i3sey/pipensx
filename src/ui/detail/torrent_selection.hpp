#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <borealis.hpp>

#include "app/download_manager.hpp"
#include "app/install_space.hpp"
#include "app/nx_file_types.hpp"
#include "app/port_selection.hpp"
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

struct TorrentFolderGroup {
    std::string name;    // single path segment, e.g. "wallpapers"
    std::string prefix;  // "bonus/wallpapers/" — empty means root files bucket
    int depth = 0;
    std::vector<size_t> indices;   // all descendant file entries
    std::vector<size_t> children;  // child group indices (nested folders)
    std::vector<size_t> files;     // direct child file entry indices
    bool expanded = false;
};

constexpr float kThreadStep = 20.0f;

// Reddit-style nest guides: one vertical rule per ancestor depth.
class ThreadGuide : public brls::View {
public:
    ThreadGuide() {
        setFocusable(false);
        setHeight(82);
    }

    void setDepth(int depth) {
        depth_ = std::max(0, depth);
        setWidth(depth_ * kThreadStep);
        setVisibility(depth_ == 0 ? brls::Visibility::GONE
                                 : brls::Visibility::VISIBLE);
    }

    void draw(NVGcontext* vg, float x, float y, float width, float height,
              brls::Style, brls::FrameContext*) override {
        if (depth_ <= 0)
            return;
        NVGcolor color = theme::textTertiary();
        color.a *= 0.55f;
        nvgStrokeColor(vg, color);
        nvgStrokeWidth(vg, 1.5f);
        nvgLineCap(vg, NVG_BUTT);
        for (int i = 0; i < depth_; ++i) {
            const float px = x + (i + 0.5f) * kThreadStep;
            nvgBeginPath(vg);
            nvgMoveTo(vg, px, y + 4.0f);
            nvgLineTo(vg, px, y + height - 4.0f);
            nvgStroke(vg);
        }
    }

private:
    int depth_ = 0;
};

class TorrentSelectionCell : public brls::RecyclerCell {
public:
    explicit TorrentSelectionCell(
        std::function<bool()> inputBlocked = {})
        : inputBlocked_(std::move(inputBlocked)) {
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
            if (inputBlocked_ && inputBlocked_())
                return true;
            auto* recycler =
                dynamic_cast<brls::RecyclerFrame*>(getParent()->getParent());
            if (recycler && recycler->getDataSource())
                recycler->getDataSource()->didSelectRowAt(recycler,
                                                          getIndexPath());
            return true;
        });

        thread_ = new ThreadGuide();
        addView(thread_);

        icon_ = new ActionIcon();
        icon_->setMarginRight(14);
        addView(icon_);

        auto* body = new brls::Box(brls::Axis::COLUMN);
        body->setGrow(1);
        body->setJustifyContent(brls::JustifyContent::CENTER);

        name_ = new brls::Label();
        name_->setSingleLine(true);
        name_->setAutoAnimate(false);
        name_->setFontSize(18);
        body->addView(name_);

        meta_ = new brls::Label();
        meta_->setSingleLine(true);
        meta_->setAutoAnimate(false);
        meta_->setFontSize(14);
        meta_->setMarginTop(4);
        meta_->setTextColor(theme::textTertiary());
        body->addView(meta_);
        addView(body);

        // Fixed width, not auto: the sizes have to right-align as a column, and
        // an auto-width label would also spill under the scroll bar.
        size_ = new brls::Label();
        size_->setSingleLine(true);
        size_->setAutoAnimate(false);
        size_->setFontSize(17);
        size_->setWidth(110);
        size_->setMarginLeft(16);
        size_->setHorizontalAlign(brls::HorizontalAlign::RIGHT);
        size_->setTextColor(theme::textSecondary());
        addView(size_);
    }

    void setEntry(const TorrentSelectionEntry& entry, int depth = 0) {
        thread_->setDepth(depth);
        const bool skipped = entry.action == FileAction::Skip;
        icon_->setKind(entry.action == FileAction::Install
                           ? ActionIconKind::Install
                           : entry.action == FileAction::Download
                                 ? ActionIconKind::Download
                                 : ActionIconKind::Skip);

        const auto [directory, name] = splitPath(entry.path);
        (void)directory;
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
        thread_->setDepth(0);
        icon_->setKind(ActionIconKind::Skip);
        name_->setText(tr("pipensx/torrent/empty"));
        name_->setTextColor(theme::textDisabled());
        meta_->setText("");
        size_->setText("");
    }

    void onFocusGained() override {
        brls::RecyclerCell::onFocusGained();
        name_->setAnimated(true);
    }

    void onFocusLost() override {
        brls::RecyclerCell::onFocusLost();
        name_->setAnimated(false);
    }

private:
    std::function<bool()> inputBlocked_;
    ThreadGuide* thread_;
    ActionIcon* icon_;
    brls::Label* name_;
    brls::Label* meta_;
    brls::Label* size_;
};

// Collapsible folder row in a nested tree. Y expands/collapses; A cycles
// Skip/Download/(Install if every descendant is a package). Starts collapsed.
class TorrentFolderCell : public brls::RecyclerCell {
public:
    explicit TorrentFolderCell(
        std::function<bool()> inputBlocked = {})
        : inputBlocked_(std::move(inputBlocked)) {
        setFocusable(true);
        setAxis(brls::Axis::ROW);
        setAlignItems(brls::AlignItems::CENTER);
        setPadding(12, 20, 12, 20);
        setHeight(82);

        registerAction(tr("pipensx/common/toggle"), brls::BUTTON_A,
                       [this](brls::View*) {
            if (inputBlocked_ && inputBlocked_())
                return true;
            auto* recycler =
                dynamic_cast<brls::RecyclerFrame*>(getParent()->getParent());
            if (recycler && recycler->getDataSource())
                recycler->getDataSource()->didSelectRowAt(recycler,
                                                          getIndexPath());
            return true;
        });

        thread_ = new ThreadGuide();
        addView(thread_);

        icon_ = new ActionIcon();
        icon_->setMarginRight(10);
        addView(icon_);

        folderIcon_ = new ActionIcon(ActionIconKind::Folder);
        folderIcon_->setMarginRight(10);
        addView(folderIcon_);

        auto* body = new brls::Box(brls::Axis::COLUMN);
        body->setGrow(1);
        body->setJustifyContent(brls::JustifyContent::CENTER);

        name_ = new brls::Label();
        name_->setSingleLine(true);
        name_->setAutoAnimate(false);
        name_->setFontSize(18);
        body->addView(name_);

        meta_ = new brls::Label();
        meta_->setSingleLine(true);
        meta_->setAutoAnimate(false);
        meta_->setFontSize(14);
        meta_->setMarginTop(4);
        meta_->setTextColor(theme::textTertiary());
        body->addView(meta_);
        addView(body);

        size_ = new brls::Label();
        size_->setSingleLine(true);
        size_->setAutoAnimate(false);
        size_->setFontSize(17);
        size_->setWidth(110);
        size_->setMarginLeft(16);
        size_->setHorizontalAlign(brls::HorizontalAlign::RIGHT);
        size_->setTextColor(theme::textSecondary());
        addView(size_);
    }

    void setGroup(const TorrentFolderGroup& group,
                  const std::vector<TorrentSelectionEntry>& entries) {
        thread_->setDepth(group.depth);
        uint64_t bytes = 0;
        bool allInstall = true, allDownload = true, allSkip = true;
        for (size_t index : group.indices) {
            if (index >= entries.size())
                continue;
            bytes += entries[index].length;
            const FileAction action = entries[index].action;
            if (action != FileAction::Install)
                allInstall = false;
            if (action != FileAction::Download)
                allDownload = false;
            if (action != FileAction::Skip)
                allSkip = false;
        }
        icon_->setKind(allInstall ? ActionIconKind::Install
                                  : allDownload ? ActionIconKind::Download
                                                : ActionIconKind::Skip);
        folderIcon_->setKind(ActionIconKind::Folder);
        name_->setText(group.name);
        meta_->setText(tr("pipensx/torrent/folder_files", group.indices.size()));
        size_->setText(formatBytes(bytes));
        const bool dimmed = allSkip;
        name_->setTextColor(dimmed ? theme::textDisabled()
                                   : theme::textPrimary());
        meta_->setTextColor(dimmed ? theme::textDisabled()
                                   : theme::textTertiary());
        size_->setTextColor(dimmed ? theme::textDisabled()
                                   : theme::textSecondary());
    }

    void onFocusGained() override {
        brls::RecyclerCell::onFocusGained();
        name_->setAnimated(true);
    }

    void onFocusLost() override {
        brls::RecyclerCell::onFocusLost();
        name_->setAnimated(false);
    }

private:
    std::function<bool()> inputBlocked_;
    ThreadGuide* thread_;
    ActionIcon* icon_;
    ActionIcon* folderIcon_;
    brls::Label* name_;
    brls::Label* meta_;
    brls::Label* size_;
};

class TorrentSelectionActivity;

class TorrentSelectionDataSource : public brls::RecyclerDataSource {
public:
    enum class VisibleKind { File, Folder };

    using FolderGroup = TorrentFolderGroup;

    struct VisibleRow {
        VisibleKind kind = VisibleKind::File;
        size_t groupIndex = 0;
        size_t entryIndex = 0;  // File only
        int depth = 0;
    };

    explicit TorrentSelectionDataSource(TorrentSelectionActivity* owner)
        : owner_(owner) {}

    void setEntries(std::vector<TorrentSelectionEntry> entries) {
        entries_ = std::move(entries);
        rebuildGroups();
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
        const std::vector<uint8_t> mask =
            pipensx::selectPortPayloadActions(preview, root);
        for (size_t i = 0; i < entries_.size() && i < mask.size(); ++i)
            entries_[i].action = static_cast<FileAction>(mask[i]);
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
        return visibleRowCount();
    }

    brls::RecyclerCell* cellForRow(brls::RecyclerFrame* recycler,
                                   brls::IndexPath index) override;

    void didSelectRowAt(brls::RecyclerFrame*, brls::IndexPath index) override;

    const TorrentSelectionEntry* entryAt(int entryIndex) const {
        if (entryIndex < 0 ||
            static_cast<size_t>(entryIndex) >= entries_.size())
            return nullptr;
        return &entries_[static_cast<size_t>(entryIndex)];
    }

    const VisibleRow* visibleAt(int row) const;
    const FolderGroup* groupAt(size_t groupIndex) const;
    const std::vector<TorrentSelectionEntry>& entries() const {
        return entries_;
    }
    int visibleRowCount() const;
    bool anyFolderCollapsed() const;
    bool toggleFolderAtVisibleRow(int row);
    void setAllFoldersExpanded(bool expanded);
    void cycleFolder(size_t groupIndex);
    void cycleRow(int row);
    void cycleEntry(int entryIndex);
    void rebuildVisible();

private:
    void rebuildGroups();
    void emitVisible(size_t groupIndex);
    static bool isCollapsibleFolder(const FolderGroup& group) {
        return !group.prefix.empty();
    }

    TorrentSelectionActivity* owner_;
    std::vector<TorrentSelectionEntry> entries_;
    std::vector<FolderGroup> groups_;
    struct RootItem {
        bool folder = false;
        size_t index = 0;
    };
    std::vector<RootItem> rootOrder_;
    std::vector<VisibleRow> visible_;
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
          debridImport_(std::move(debridImport)), abandon_(std::move(abandon)),
          alive_(std::make_shared<std::atomic<bool>>(true)) {
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

        portRoot_ = pipensx::candidatePortRoot(preview_);
        const bool portLayout =
            pipensx::torrentPortLayoutDetected(preview_);
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
        recycler_->registerCell("FileSelect", [this] {
            return new TorrentSelectionCell(
                [this] { return validationInFlight_; });
        });
        recycler_->registerCell("FolderSelect", [this] {
            return new TorrentFolderCell(
                [this] { return validationInFlight_; });
        });
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
        refreshStorageSnapshots();
    }

    ~TorrentSelectionActivity() override {
        alive_->store(false);
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
        // Clear stays on the on-screen button; Y expands/collapses folders so
        // mod trees do not own the whole list.
        registerAction(tr("pipensx/torrent/expand"), brls::BUTTON_Y,
                       [this](brls::View*) {
            toggleFolderAtFocus();
            return true;
        });
        registerAction(tr("pipensx/common/continue"), brls::BUTTON_RB,
                       [this](brls::View*) {
                           confirmSelection();
                           return true;
                       });
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

    bool selectedPortTransaction(const std::vector<uint8_t>& actions,
                                 size_t& packageCount) const {
        packageCount = 0;
        bool payload = false;
        for (size_t i = 0; i < actions.size() && i < preview_.files.size(); ++i) {
            if (actions[i] == static_cast<uint8_t>(FileAction::Skip))
                continue;
            const auto& file = preview_.files[i];
            if (file.package) {
                ++packageCount;
            } else if (!file.cartridge &&
                       (hasNroExtension(file.path) ||
                        isPortArchiveName(file.path))) {
                payload = true;
            }
        }
        return payload;
    }

    void populateEntries() {
        std::vector<uint8_t> portDefaults;
        if (preferred_ == TransferMode::StreamInstall &&
            initialSelection_ == StreamSelection::PackagesOnly &&
            torrentPortLayoutDetected(preview_))
            portDefaults = selectPortInstallActions(preview_);

        std::vector<TorrentSelectionEntry> entries;
        entries.reserve(preview_.files.size());
        for (size_t i = 0; i < preview_.files.size(); ++i) {
            const auto& file = preview_.files[i];
            TorrentSelectionEntry entry;
            entry.path = file.path;
            entry.length = file.length;
            entry.package = file.package;
            entry.compressed = file.compressed;
            entry.cartridge = file.cartridge;
            if (!portDefaults.empty()) {
                entry.action = static_cast<FileAction>(portDefaults[i]);
            } else if (preferred_ == TransferMode::StreamInstall &&
                       file.package) {
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
        if (validationInFlight_)
            return;
        mutate();
        repaintVisible();
        refreshSummary();
    }

    void setAllSelected(bool selected) {
        if (validationInFlight_)
            return;
        dataSource_->setAll(selected);
        repaintVisible();
        refreshSummary();
    }

    void cycleFolderAtRow(int row) {
        if (validationInFlight_)
            return;
        const auto* vr = dataSource_->visibleAt(row);
        if (!vr ||
            vr->kind != TorrentSelectionDataSource::VisibleKind::Folder)
            return;
        dataSource_->cycleFolder(vr->groupIndex);
        repaintVisible();
        refreshSummary();
    }

    void repaintVisible() {
        for (auto* cell : visibleCells<TorrentSelectionCell>(recycler_))
            repaint(cell);
        for (auto* cell : visibleCells<TorrentFolderCell>(recycler_)) {
            const auto* vr =
                dataSource_->visibleAt(cell->getIndexPath().row);
            if (!vr ||
                vr->kind != TorrentSelectionDataSource::VisibleKind::Folder)
                continue;
            if (const auto* group = dataSource_->groupAt(vr->groupIndex))
                cell->setGroup(*group, dataSource_->entries());
        }
    }

    void toggleFolderAtFocus() {
        auto* cell = dynamic_cast<brls::RecyclerCell*>(
            brls::Application::getCurrentFocus());
        const int row = cell ? cell->getIndexPath().row : 0;
        toggleFolderAtRow(row);
    }

    void toggleFolderAtRow(int row) {
        if (validationInFlight_)
            return;
        std::string folderPrefix;
        if (const auto* before = dataSource_->visibleAt(row)) {
            if (before->kind ==
                TorrentSelectionDataSource::VisibleKind::Folder) {
                if (const auto* group =
                        dataSource_->groupAt(before->groupIndex))
                    folderPrefix = group->prefix;
            }
        }
        if (!dataSource_->toggleFolderAtVisibleRow(row))
            return;
        int focusRow = std::max(
            0, std::min(row, dataSource_->visibleRowCount() - 1));
        if (!folderPrefix.empty())
            for (int i = 0; i < dataSource_->visibleRowCount(); ++i) {
                const auto* candidate = dataSource_->visibleAt(i);
                if (!candidate ||
                    candidate->kind !=
                        TorrentSelectionDataSource::VisibleKind::Folder)
                    continue;
                const auto* group =
                    dataSource_->groupAt(candidate->groupIndex);
                if (group && group->prefix == folderPrefix) {
                    focusRow = i;
                    break;
                }
            }
        recycler_->setDefaultCellFocus(brls::IndexPath(0, focusRow));
        recycler_->reloadData();
        // reloadData re-homes focus; nudge back onto the folder row once cells
        // exist. Golden and Switch both need a frame of layout first.
        brls::sync([this, focusRow] {
            for (brls::View* child : recycler_->getChildren()) {
                auto* box = dynamic_cast<brls::Box*>(child);
                if (!box)
                    continue;
                for (brls::View* view : box->getChildren()) {
                    auto* cell = dynamic_cast<brls::RecyclerCell*>(view);
                    if (cell && cell->getIndexPath().row == focusRow) {
                        brls::Application::giveFocus(cell);
                        return;
                    }
                }
            }
        });
    }

    void setAllFoldersExpanded(bool expanded) {
        if (validationInFlight_)
            return;
        dataSource_->setAllFoldersExpanded(expanded);
        recycler_->reloadData();
    }

    int visibleRowCount() const { return dataSource_->visibleRowCount(); }

    // Repainting the one row that changed keeps the cursor and the scroll
    // offset exactly where they were. reloadData() would recycle every cell,
    // snap the scroll to 0 and re-home focus on defaultCellFocus.
    void repaintVisibleRow(int row) {
        for (auto* cell : visibleCells<TorrentSelectionCell>(recycler_)) {
            if (cell->getIndexPath().row == row)
                repaint(cell);
        }
    }

    void repaint(TorrentSelectionCell* cell) {
        const auto* vr = dataSource_->visibleAt(cell->getIndexPath().row);
        if (vr && vr->kind == TorrentSelectionDataSource::VisibleKind::File) {
            if (const auto* entry =
                    dataSource_->entryAt(static_cast<int>(vr->entryIndex))) {
                cell->setEntry(*entry, vr->depth);
                return;
            }
        }
        cell->setEmpty();
    }

    void refreshStorageSnapshots() {
        if (storageQueryInFlight_)
            return;
        storageQueryInFlight_ = true;
        auto alive = alive_;
        const std::string root = manager_->rootPath();
        const auto target = manager_->installTarget();
        brls::async([this, alive, root, target] {
            const uint64_t startedUs =
                telemetry_enabled() ? now_us() : 0;
            const auto downloadStorage =
                pipensx::queryStorageSpace(root);
            const auto packageStorage =
                target == pipensx::install::InstallStorageTarget::SdCard
                    ? downloadStorage
                    : pipensx::queryInstallStorageSpace(target, root);
            if (startedUs) {
                telemetry_log(
                    "ui", "torrent_selection",
                    "event=storage duration_us=%llu",
                    static_cast<unsigned long long>(now_us() - startedUs));
            }
            brls::sync([this, alive, downloadStorage, packageStorage] {
                if (!alive->load())
                    return;
                storageQueryInFlight_ = false;
                downloadStorage_ = downloadStorage;
                packageStorage_ = packageStorage;
                storageReady_ = true;
                refreshSummary();
            });
        });
    }

    void refreshSummary() {
        size_t selected = dataSource_->selectedCount();
        size_t installs = dataSource_->installCount();
        size_t downloads = dataSource_->downloadCount();
        std::vector<uint8_t> actions = dataSource_->fileActions();
        TransferMode mode = installs > 0
            ? TransferMode::StreamInstall
            : TransferMode::DownloadOnly;
        size_t portPackages = 0;
        if (selectedPortTransaction(actions, portPackages)) {
            mode = TransferMode::PortInstall;
            installs = portPackages;
            downloads = 0;
            for (size_t i = 0; i < actions.size() &&
                               i < preview_.files.size(); ++i) {
                if (preview_.files[i].package &&
                    actions[i] != static_cast<uint8_t>(FileAction::Skip)) {
                    actions[i] = static_cast<uint8_t>(FileAction::Download);
                } else if (actions[i] ==
                           static_cast<uint8_t>(FileAction::Download)) {
                    ++downloads;
                }
            }
        }
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
        if (storageReady_ && meterStorage.available)
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
        installSelected_->setState(!storageReady_ || validationInFlight_ ||
                                    selected == 0 || estimate.overflow ||
                                    check.status ==
                                        InstallSpaceCheckStatus::Insufficient
            ? brls::ButtonState::DISABLED
            : brls::ButtonState::ENABLED);
        const brls::ButtonState toggleState = validationInFlight_
            ? brls::ButtonState::DISABLED
            : brls::ButtonState::ENABLED;
        selectPackages_->setState(toggleState);
        selectDownloadAll_->setState(toggleState);
        clearAll_->setState(toggleState);
        if (selectPort_)
            selectPort_->setState(toggleState);
    }

    void confirmSelection() {
        if (!storageReady_ || validationInFlight_)
            return;
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

        size_t installs = dataSource_->installCount();
        TransferMode mode = installs > 0
            ? TransferMode::StreamInstall
            : TransferMode::DownloadOnly;
        size_t portPackages = 0;
        if (selectedPortTransaction(actions, portPackages)) {
            mode = TransferMode::PortInstall;
            installs = portPackages;
            for (size_t i = 0; i < actions.size() &&
                               i < preview_.files.size(); ++i)
                if (preview_.files[i].package &&
                    actions[i] != static_cast<uint8_t>(FileAction::Skip))
                    actions[i] = static_cast<uint8_t>(FileAction::Download);
        }
        const auto estimate = pipensx::estimateInstallSpace(preview_, actions,
                                                            mode);
        validationInFlight_ = true;
        refreshSummary();
        auto alive = alive_;
        const std::string root = manager_->rootPath();
        const auto target = manager_->installTarget();
        brls::async([this, alive, root, target, estimate, mode, installs,
                     actions = std::move(actions)]() mutable {
            const uint64_t startedUs =
                telemetry_enabled() ? now_us() : 0;
            const auto downloadStorage =
                pipensx::queryStorageSpace(root);
            const auto packageStorage =
                target == pipensx::install::InstallStorageTarget::SdCard
                    ? downloadStorage
                    : pipensx::queryInstallStorageSpace(target, root);
            if (startedUs) {
                telemetry_log(
                    "ui", "torrent_selection",
                    "event=storage duration_us=%llu",
                    static_cast<unsigned long long>(now_us() - startedUs));
            }
            brls::sync([this, alive, estimate, mode, installs,
                        actions = std::move(actions), downloadStorage,
                        packageStorage]() mutable {
                if (!alive->load() || finished_)
                    return;
                validationInFlight_ = false;
                downloadStorage_ = downloadStorage;
                packageStorage_ = packageStorage;
                storageReady_ = true;
                refreshSummary();
                if (pipensx::assessTransferSpace(
                        estimate, downloadStorage_, packageStorage_)
                        .status == InstallSpaceCheckStatus::Insufficient) {
                    brls::Application::notify(tr("pipensx/torrent/no_space"));
                    return;
                }
                continueValidated(std::move(actions), mode, installs);
            });
        });
    }

    void continueValidated(std::vector<uint8_t> actions, TransferMode mode,
                           size_t installs) {
        size_t portPackages = 0;
        if (selectedPortTransaction(actions, portPackages)) {
            mode = TransferMode::PortInstall;
            installs = portPackages;
            for (size_t i = 0; i < actions.size() &&
                               i < preview_.files.size(); ++i)
                if (preview_.files[i].package &&
                    actions[i] != static_cast<uint8_t>(FileAction::Skip))
                    actions[i] = static_cast<uint8_t>(FileAction::Download);
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
            import.packageCount = static_cast<uint32_t>(installs);
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
    std::shared_ptr<std::atomic<bool>> alive_;
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
    bool storageQueryInFlight_ = false;
    bool storageReady_ = false;
    bool validationInFlight_ = false;
    bool finished_ = false;
};

}  // namespace pipensx::ui
