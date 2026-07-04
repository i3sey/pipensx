#pragma once

#include <functional>
#include <string>
#include <unordered_set>
#include <vector>

#include <borealis.hpp>

#include "app/app_settings.hpp"
#include "app/catalog_presentation.hpp"
#include "app/catalog_service.hpp"
#include "app/download_manager.hpp"
#include "app/game_metadata_service.hpp"
#include "app/installed_title_service.hpp"
#include "ui/catalog/batch_install.hpp"
#include "ui/catalog/catalog_cells.hpp"
#include "ui/catalog/catalog_helpers.hpp"
#include "ui/common/message_cells.hpp"
#include "ui/common/ui_helpers.hpp"
#include "ui/detail/game_detail.hpp"

namespace pipensx::ui {

class CatalogView;

class CatalogDataSource : public brls::RecyclerDataSource {
public:
    explicit CatalogDataSource(CatalogView* owner) : owner_(owner) {}

    void setEntries(std::vector<CatalogEntry> entries,
                    std::vector<std::string> stateBadges,
                    std::vector<std::string> gameNames,
                    std::vector<std::string> iconUrls,
                    std::vector<uint8_t> selected,
                    std::vector<uint8_t> selectable,
                    GameMetadataService* metadata,
                    bool selectionMode) {
        entries_ = std::move(entries);
        stateBadges_ = std::move(stateBadges);
        gameNames_ = std::move(gameNames);
        iconUrls_ = std::move(iconUrls);
        selected_ = std::move(selected);
        selectable_ = std::move(selectable);
        metadata_ = metadata;
        selectionMode_ = selectionMode;
    }
    void setMessage(const std::string& message) { message_ = message; }
    const CatalogEntry* entryAt(int row) const {
        if (row < 0 || static_cast<size_t>(row) >= entries_.size())
            return nullptr;
        return &entries_[static_cast<size_t>(row)];
    }
    const std::vector<CatalogEntry>& entries() const { return entries_; }
    bool selectableAt(int row) const {
        return row >= 0 && static_cast<size_t>(row) < selectable_.size() &&
               selectable_[static_cast<size_t>(row)] != 0;
    }

    int numberOfRows(brls::RecyclerFrame*, int) override {
        return entries_.empty() ? 1 : static_cast<int>(entries_.size());
    }
    brls::RecyclerCell* cellForRow(brls::RecyclerFrame* recycler,
                                    brls::IndexPath index) override {
        if (entries_.empty()) {
            auto* cell = static_cast<TextMessageCell*>(
                recycler->dequeueReusableCell("Message"));
            cell->setMessage(message_);
            return cell;
        }
        auto* cell = static_cast<CatalogCell*>(
            recycler->dequeueReusableCell("Catalog"));
        size_t row = static_cast<size_t>(index.row);
        CatalogEntry display = entries_[row];
        if (row < gameNames_.size() && !gameNames_[row].empty())
            display.title = gameNames_[row];
        cell->setEntry(display,
                       row < stateBadges_.size() ? stateBadges_[row]
                                                 : std::string(),
                       row < iconUrls_.size() ? iconUrls_[row] : std::string(),
                       metadata_, selectionMode_,
                       row < selected_.size() && selected_[row] != 0,
                       row < selectable_.size() && selectable_[row] != 0);
        return cell;
    }
    void didSelectRowAt(brls::RecyclerFrame*, brls::IndexPath index) override;

private:
    CatalogView* owner_;
    std::vector<CatalogEntry> entries_;
    std::vector<std::string> stateBadges_;
    std::vector<std::string> gameNames_;
    std::vector<std::string> iconUrls_;
    std::vector<uint8_t> selected_;
    std::vector<uint8_t> selectable_;
    GameMetadataService* metadata_ = nullptr;
    std::string message_;
    bool selectionMode_ = false;
};
class CatalogView : public brls::Box {
public:
    CatalogView(DownloadManager* manager, CatalogService* catalog,
                GameMetadataService* metadata,
                InstalledTitleService* installed, AppSettings* settings,
                std::function<void()> openDownloads)
        : brls::Box(brls::Axis::COLUMN), manager_(manager), catalog_(catalog),
          metadata_(metadata), installed_(installed), settings_(settings),
          openDownloads_(std::move(openDownloads)),
          alive_(std::make_shared<std::atomic<bool>>(true)),
          cancelled_(std::make_shared<std::atomic<bool>>(false)) {
        recycler_ = new brls::RecyclerFrame();
        recycler_->setGrow(1);
        recycler_->setPadding(6, 32, 6, 32);
        recycler_->estimatedRowHeight = 82;
        recycler_->registerCell("Catalog", [] { return new CatalogCell(); });
        recycler_->registerCell("Message",
            [] { return new TextMessageCell(); });
        dataSource_ = new CatalogDataSource(this);
        recycler_->setDataSource(dataSource_);

        status_ = new brls::Label();
        status_->setFontSize(15);
        status_->setMarginTop(10);
        status_->setMarginLeft(34);
        status_->setMarginBottom(2);
        status_->setTextColor(nvgRGB(140, 140, 150));

        batchControls_ = new brls::Box(brls::Axis::ROW);
        batchControls_->setMarginTop(8);
        batchControls_->setMarginLeft(34);
        batchControls_->setMarginRight(34);
        batchControls_->setVisibility(brls::Visibility::GONE);
        auto* selectVisible = new brls::Button();
        selectVisible->setStyle(&brls::BUTTONSTYLE_DEFAULT);
        selectVisible->setGrow(1);
        selectVisible->setHeight(44);
        selectVisible->setMarginRight(8);
        selectVisible->setText("Select visible");
        selectVisible->registerClickAction([this](brls::View*) {
            selectVisibleEntries();
            return true;
        });
        batchControls_->addView(selectVisible);
        auto* clearSelection = new brls::Button();
        clearSelection->setStyle(&brls::BUTTONSTYLE_DEFAULT);
        clearSelection->setGrow(1);
        clearSelection->setHeight(44);
        clearSelection->setMarginRight(8);
        clearSelection->setText("Clear");
        clearSelection->registerClickAction([this](brls::View*) {
            selectedHashes_.clear();
            rebuildEntries();
            return true;
        });
        batchControls_->addView(clearSelection);
        prepareBatch_ = new brls::Button();
        prepareBatch_->setStyle(&brls::BUTTONSTYLE_PRIMARY);
        prepareBatch_->setGrow(1);
        prepareBatch_->setHeight(44);
        prepareBatch_->setText("Prepare");
        prepareBatch_->registerClickAction([this](brls::View*) {
            prepareSelectedEntries();
            return true;
        });
        batchControls_->addView(prepareBatch_);

        addView(status_);
        addView(batchControls_);
        addView(recycler_);
        rebuildEntries();

        registerAction("Search", brls::BUTTON_X, [this](brls::View*) {
            openSearchKeyboard();
            return true;
        });
        registerAction("Sort", brls::BUTTON_Y, [this](brls::View*) {
            if (busy_)
                cancelled_->store(true);
            else
                cycleSort();
            return true;
        });
        registerAction("Refresh", brls::BUTTON_RB, [this](brls::View*) {
            if (batchMode_)
                prepareSelectedEntries();
            else
                refreshCatalog();
            return true;
        });
        registerAction("Batch install", brls::BUTTON_LB,
                       [this](brls::View*) {
            toggleBatchMode();
            return true;
        });
        observedSettingsGeneration_ = settings_ ? settings_->generation() : 0;
        taskSignature_ = taskSignature();
        timer_.setCallback([this] { refreshLiveState(); });
        timer_.start(1000);
        if (settings_ && settings_->get().refreshCatalogOnLaunch)
            refreshCatalog();
    }

    ~CatalogView() override {
        alive_->store(false);
        cancelled_->store(true);
        timer_.stop();
    }

    void openSearchKeyboard() {
        if (busy_)
            return;
        brls::Application::getImeManager()->openForText(
            [this](std::string text) {
                query_ = std::move(text);
                rebuildEntries();
            },
            "Search catalog", "", 256, query_,
            brls::KEYBOARD_DISABLE_NONE);
    }

    void onEntrySelected(int row) {
        const CatalogEntry* picked = dataSource_->entryAt(row);
        if (!picked || busy_)
            return;
        if (batchMode_) {
            if (!dataSource_->selectableAt(row)) {
                brls::Application::notify("This item is already in Downloads.");
                return;
            }
            const std::string hash = lowerAscii(picked->infoHash);
            if (selectedHashes_.erase(hash) == 0)
                selectedHashes_.insert(hash);
            rebuildEntries();
            return;
        }
        CatalogEntry entry = *picked;
        auto it = catalogFailures_.find(lowerAscii(entry.infoHash));
        std::string lastFailure =
            it != catalogFailures_.end() ? it->second : std::string();
        auto onFailure = [this](const std::string& hashLower,
                                const std::string& failure) {
            if (failure.empty())
                catalogFailures_.erase(hashLower);
            else
                catalogFailures_[hashLower] = failure;
        };
        auto onChange = [this] { rebuildEntries(); };
        brls::Application::pushActivity(new GameDetailActivity(
            std::move(entry), std::move(lastFailure), manager_, metadata_,
            installed_, settings_,
            std::move(onFailure), std::move(onChange)));
    }

private:
    enum class SortMode { Latest, Alphabetical, Largest };

    static std::string lowerAscii(std::string value) {
        std::transform(value.begin(), value.end(), value.begin(),
                       [](unsigned char c) {
                           return static_cast<char>(std::tolower(c));
                       });
        return value;
    }

    void toggleBatchMode() {
        if (busy_)
            return;
        batchMode_ = !batchMode_;
        batchControls_->setVisibility(batchMode_ ? brls::Visibility::VISIBLE
                                                 : brls::Visibility::GONE);
        updateActionHint(brls::BUTTON_LB,
                         batchMode_ ? "Close batch" : "Batch install");
        updateActionHint(brls::BUTTON_RB,
                         batchMode_ ? "Prepare" : "Refresh");
        rebuildEntries();
    }

    void selectVisibleEntries() {
        if (!batchMode_)
            return;
        for (size_t row = 0; row < dataSource_->entries().size(); ++row) {
            if (dataSource_->selectableAt(static_cast<int>(row)))
                selectedHashes_.insert(
                    lowerAscii(dataSource_->entries()[row].infoHash));
        }
        rebuildEntries();
    }

    void prepareSelectedEntries() {
        if (!batchMode_ || selectedHashes_.empty() || busy_)
            return;

        std::unordered_set<std::string> managed;
        for (const DownloadTask& task : manager_->snapshot())
            managed.insert(lowerAscii(task.id));

        std::vector<CatalogEntry> entries;
        for (const CatalogEntry& entry : catalog_->entries()) {
            const std::string hash = lowerAscii(entry.infoHash);
            if (selectedHashes_.count(hash) && !managed.count(hash))
                entries.push_back(entry);
        }
        if (sort_ == SortMode::Alphabetical) {
            std::stable_sort(entries.begin(), entries.end(),
                [](const CatalogEntry& left, const CatalogEntry& right) {
                    return lowerAscii(left.title) < lowerAscii(right.title);
                });
        } else if (sort_ == SortMode::Largest) {
            std::stable_sort(entries.begin(), entries.end(),
                [](const CatalogEntry& left, const CatalogEntry& right) {
                    return left.size > right.size;
                });
        } else {
            std::stable_sort(entries.begin(), entries.end(),
                [](const CatalogEntry& left, const CatalogEntry& right) {
                    return left.publishedAt > right.publishedAt;
                });
        }
        for (const std::string& hash : managed)
            selectedHashes_.erase(hash);
        if (entries.empty()) {
            rebuildEntries();
            brls::Application::notify("Select at least one available game.");
            return;
        }

        auto alive = alive_;
        auto completion = [this, alive](
                              const std::unordered_set<std::string>& remaining) {
            if (!alive->load())
                return;
            selectedHashes_ = remaining;
            rebuildEntries();
        };
        const StreamSelection selection = settings_
            ? settings_->get().streamSelection
            : StreamSelection::AllFiles;
        brls::Application::pushActivity(new BatchInstallActivity(
            manager_, std::move(entries), selection, std::move(completion),
            openDownloads_));
    }

    void refreshBatchStatus() {
        uint64_t bytes = 0;
        size_t unknown = 0;
        for (const CatalogEntry& entry : catalog_->entries()) {
            if (!selectedHashes_.count(lowerAscii(entry.infoHash)))
                continue;
            if (!entry.size) {
                ++unknown;
                continue;
            }
            if (entry.size > std::numeric_limits<uint64_t>::max() - bytes)
                bytes = std::numeric_limits<uint64_t>::max();
            else
                bytes += entry.size;
        }
        const StorageSpaceSnapshot storage =
            pipensx::queryStorageSpace(manager_->rootPath());
        std::string text = std::to_string(selectedHashes_.size()) +
                           " selected   Catalog size: " + formatBytes(bytes);
        if (unknown)
            text += " + " + std::to_string(unknown) + " unknown";
        text += storage.available
            ? "   SD free: " + formatBytes(storage.freeBytes)
            : "   SD free: unavailable";
        status_->setText(text);
        const bool available = !selectedHashes_.empty();
        prepareBatch_->setState(available ? brls::ButtonState::ENABLED
                                          : brls::ButtonState::DISABLED);
        prepareBatch_->setText("Prepare " +
                               std::to_string(selectedHashes_.size()));
        setActionAvailable(brls::BUTTON_RB, available);
    }

    void rebuildEntries() {
        // Info-hash (lower-case hex) -> status for anything already managed,
        // so rows can be badged. Task ids are lower-case hex; catalog info
        // hashes are upper-case, hence the case fold on both sides.
        std::unordered_map<std::string, DownloadStatus> added;
        for (const DownloadTask& task : manager_->snapshot())
            added[lowerAscii(task.id)] = task.status;

        std::vector<CatalogEntry> visible;
        std::string needle = lowerAscii(query_);
        for (const CatalogEntry& entry : catalog_->entries()) {
            if (entry.isHiddenByDefault())
                continue;
            const GameMetadata* meta = metadata_->findByInfoHash(entry.infoHash);
            if (settings_ &&
                settings_->get().catalogFilter == CatalogFilter::Games &&
                !meta)
                continue;
            bool matches = needle.empty() ||
                lowerAscii(entry.title).find(needle) != std::string::npos ||
                (meta && lowerAscii(meta->name).find(needle) !=
                             std::string::npos);
            if (!matches)
                continue;
            visible.push_back(entry);
        }
        if (sort_ == SortMode::Alphabetical) {
            std::stable_sort(visible.begin(), visible.end(),
                [](const CatalogEntry& left, const CatalogEntry& right) {
                    return lowerAscii(left.title) < lowerAscii(right.title);
                });
        } else if (sort_ == SortMode::Largest) {
            std::stable_sort(visible.begin(), visible.end(),
                [](const CatalogEntry& left, const CatalogEntry& right) {
                    return left.size > right.size;
                });
        } else {
            std::stable_sort(visible.begin(), visible.end(),
                [](const CatalogEntry& left, const CatalogEntry& right) {
                    return left.publishedAt > right.publishedAt;
                });
        }

        std::vector<std::string> stateBadges;
        std::vector<std::string> gameNames;
        std::vector<std::string> iconUrls;
        std::vector<uint8_t> selected;
        std::vector<uint8_t> selectable;
        stateBadges.reserve(visible.size());
        gameNames.reserve(visible.size());
        iconUrls.reserve(visible.size());
        selected.reserve(visible.size());
        selectable.reserve(visible.size());
        for (const CatalogEntry& entry : visible) {
            std::string hash = lowerAscii(entry.infoHash);
            auto it = added.find(hash);
            const bool canSelect = it == added.end();
            if (it != added.end()) {
                stateBadges.push_back(badgeForStatus(it->second));
                selectedHashes_.erase(hash);
            } else
                stateBadges.emplace_back();
            const GameMetadata* meta = metadata_->findByInfoHash(entry.infoHash);
            if (it == added.end() && meta && installed_ &&
                installed_->contains(meta->titleId))
                stateBadges.back() = "Installed";
            gameNames.push_back(meta ? meta->name : std::string());
            iconUrls.push_back(meta ? meta->iconUrl : std::string());
            selected.push_back(selectedHashes_.count(hash) ? 1 : 0);
            selectable.push_back(canSelect ? 1 : 0);
        }

        size_t count = visible.size();
        dataSource_->setEntries(std::move(visible), std::move(stateBadges),
                                std::move(gameNames), std::move(iconUrls),
                                std::move(selected), std::move(selectable),
                                metadata_, batchMode_);
        dataSource_->setMessage(query_.empty()
            ? "Catalog is empty. Press R to refresh."
            : "Nothing found. Press X to change the search.");
        recycler_->reloadData();

        countText_ = query_.empty()
            ? withThousands(count) + (count == 1 ? " release" : " releases")
            : withThousands(count) + (count == 1 ? " match" : " matches");
        if (!busy_ && batchMode_) {
            refreshBatchStatus();
        } else if (!busy_) {
            std::string filter = settings_ &&
                settings_->get().catalogFilter == CatalogFilter::Games
                ? "Games" : "All";
            status_->setText(countText_ + "   Filter: " + filter);
            setActionAvailable(brls::BUTTON_RB, true);
        }
    }

    // While busy (catalog refresh) Y cancels instead of sorting; reflect that
    // in the bottom-bar hint so the label always matches the action.
    void setBusy(bool busy) {
        busy_ = busy;
        updateActionHint(brls::BUTTON_Y, busy ? "Stop" : "Sort");
    }

    uint64_t taskSignature() const {
        uint64_t signature = 1469598103934665603ULL;
        for (const DownloadTask& task : manager_->snapshot()) {
            for (unsigned char c : task.id)
                signature = (signature ^ c) * 1099511628211ULL;
            signature = (signature ^ static_cast<uint64_t>(task.status)) *
                        1099511628211ULL;
        }
        return signature;
    }

    void refreshLiveState() {
        if (busy_)
            return;
        bool changed = false;
        if (settings_ && settings_->generation() !=
                             observedSettingsGeneration_) {
            observedSettingsGeneration_ = settings_->generation();
            changed = true;
        }
        uint64_t signature = taskSignature();
        if (signature != taskSignature_) {
            taskSignature_ = signature;
            changed = true;
            bool installedFinished = false;
            for (const DownloadTask& task : manager_->snapshot())
                installedFinished = installedFinished ||
                    task.status == DownloadStatus::Installed;
            if (installedFinished && installed_ &&
                installedRefreshSignature_ != signature) {
                installedRefreshSignature_ = signature;
                refreshInstalledAsync();
            }
        }
        if (changed)
            rebuildEntries();
    }

    void refreshInstalledAsync() {
        if (installedRefreshInFlight_)
            return;
        installedRefreshInFlight_ = true;
        auto alive = alive_;
        InstalledTitleService* installed = installed_;
        brls::async([this, alive, installed] {
            std::string error;
            bool ok = installed->refresh(error);
            brls::sync([this, alive, ok, error] {
                if (!alive->load())
                    return;
                installedRefreshInFlight_ = false;
                if (!ok)
                    diagnostic_error("installed", "auto_refresh", "error=%s",
                                     error.c_str());
                rebuildEntries();
            });
        });
    }

    static std::string withThousands(size_t value) {
        std::string digits = std::to_string(value);
        int insertAt = static_cast<int>(digits.size()) - 3;
        while (insertAt > 0) {
            digits.insert(static_cast<size_t>(insertAt), ",");
            insertAt -= 3;
        }
        return digits;
    }

    static std::string badgeForStatus(DownloadStatus status) {
        switch (status) {
            case DownloadStatus::Queued:      return "In queue";
            case DownloadStatus::Checking:
            case DownloadStatus::Downloading:
            case DownloadStatus::Verifying:   return "Downloading";
            case DownloadStatus::Paused:      return "Paused";
            case DownloadStatus::Installing:
            case DownloadStatus::Committing:  return "Installing";
            case DownloadStatus::Completed:   return "Downloaded";
            case DownloadStatus::Installed:   return "Installed";
            case DownloadStatus::Error:       return "Error";
            case DownloadStatus::Removing:    return "Removing";
        }
        return "";
    }

    void cycleSort() {
        if (sort_ == SortMode::Latest)
            sort_ = SortMode::Alphabetical;
        else if (sort_ == SortMode::Alphabetical)
            sort_ = SortMode::Largest;
        else
            sort_ = SortMode::Latest;
        rebuildEntries();
        brls::Application::notify(
            sort_ == SortMode::Latest ? "Sorted by latest."
          : sort_ == SortMode::Alphabetical ? "Sorted alphabetically."
                                             : "Sorted by size.");
    }

    void refreshCatalog() {
        if (busy_)
            return;
        setBusy(true);
        brls::Application::notify("Updating catalog from GitHub...");
        auto alive = alive_;
        CatalogService* catalog = catalog_;
        uint64_t startedMs = now_ms();
        brls::async([this, alive, catalog, startedMs] {
            std::string err;
            bool ok = catalog->refresh(err);
            telemetry_log("catalog", "-",
                          "event=refresh ok=%d duration_ms=%llu entries=%zu",
                          ok ? 1 : 0,
                          (unsigned long long)(now_ms() - startedMs),
                          catalog->entries().size());
            brls::sync([this, alive, ok, err] {
                if (!alive->load())
                    return;
                setBusy(false);
                if (!ok) {
                    diagnostic_error("catalog", "refresh", "error=%s",
                                     err.c_str());
                    brls::Application::notify(err);
                    return;
                }
                rebuildEntries();
                brls::Application::notify(
                    "Catalog updated: " +
                    std::to_string(catalog_->entries().size()) + " entries.");
            });
        });
    }

    DownloadManager* manager_;
    CatalogService* catalog_;
    GameMetadataService* metadata_;
    InstalledTitleService* installed_;
    AppSettings* settings_;
    std::function<void()> openDownloads_;
    brls::RecyclerFrame* recycler_;
    CatalogDataSource* dataSource_;
    brls::Label* status_;
    brls::Box* batchControls_ = nullptr;
    brls::Button* prepareBatch_ = nullptr;
    std::shared_ptr<std::atomic<bool>> alive_;
    std::shared_ptr<std::atomic<bool>> cancelled_;
    std::unordered_map<std::string, std::string> catalogFailures_;
    std::unordered_set<std::string> selectedHashes_;
    std::string query_;
    std::string countText_;
    SortMode sort_ = SortMode::Latest;
    bool busy_ = false;
    bool batchMode_ = false;
    brls::RepeatingTimer timer_;
    uint64_t observedSettingsGeneration_ = 0;
    uint64_t taskSignature_ = 0;
    uint64_t installedRefreshSignature_ = 0;
    bool installedRefreshInFlight_ = false;
};

}  // namespace pipensx::ui
