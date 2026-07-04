#pragma once

#include <functional>
#include <memory>
#include <numeric>
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
#include "ui/catalog/catalog_grid.hpp"
#include "ui/catalog/catalog_helpers.hpp"
#include "ui/common/message_cells.hpp"
#include "ui/common/ui_helpers.hpp"
#include "ui/detail/game_detail.hpp"
#include "ui/theme.hpp"

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
    // Shelf contents (UI_PLAN F2): indices into entries(). Empty vector =
    // shelf hidden.
    void setShelves(std::vector<int> latest, std::vector<int> popular) {
        shelfNew_ = std::move(latest);
        shelfPopular_ = std::move(popular);
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

    // Recycler row layout: [shelves][grid rows of kColumns cards each].
    int shelfRowCount() const {
        return (shelfNew_.empty() ? 0 : 1) + (shelfPopular_.empty() ? 0 : 1);
    }
    int rowForEntry(int index) const {
        return shelfRowCount() + index / grid::kColumns;
    }
    int columnForEntry(int index) const { return index % grid::kColumns; }

    int numberOfRows(brls::RecyclerFrame*, int) override {
        if (entries_.empty())
            return 1;
        const int gridRows =
            (static_cast<int>(entries_.size()) + grid::kColumns - 1) /
            grid::kColumns;
        return shelfRowCount() + gridRows;
    }
    float heightForRow(brls::RecyclerFrame*, brls::IndexPath index) override {
        if (entries_.empty())
            return 100;
        return index.row < shelfRowCount() ? grid::kShelfHeight
                                           : grid::kRowHeight;
    }
    brls::RecyclerCell* cellForRow(brls::RecyclerFrame* recycler,
                                    brls::IndexPath index) override;
    void didSelectRowAt(brls::RecyclerFrame*, brls::IndexPath index) override;

private:
    GridCardInfo makeInfo(int index) const {
        const size_t row = static_cast<size_t>(index);
        GridCardInfo info;
        info.entryIndex = index;
        info.infoHash = entries_[row].infoHash;
        info.title = row < gameNames_.size() && !gameNames_[row].empty()
            ? gameNames_[row]
            : entries_[row].title;
        const std::string badge =
            row < stateBadges_.size() ? stateBadges_[row] : std::string();
        info.subIsBadge = !badge.empty();
        info.sub = info.subIsBadge
            ? badge
            : entries_[row].size ? formatBytes(entries_[row].size)
                                 : "Unknown size";
        info.iconUrl = row < iconUrls_.size() ? iconUrls_[row] : std::string();
        info.selectionMode = selectionMode_;
        info.selected = row < selected_.size() && selected_[row] != 0;
        info.selectable = row < selectable_.size() && selectable_[row] != 0;
        return info;
    }

    CatalogView* owner_;
    std::vector<CatalogEntry> entries_;
    std::vector<std::string> stateBadges_;
    std::vector<std::string> gameNames_;
    std::vector<std::string> iconUrls_;
    std::vector<uint8_t> selected_;
    std::vector<uint8_t> selectable_;
    std::vector<int> shelfNew_;
    std::vector<int> shelfPopular_;
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
        recycler_->estimatedRowHeight = grid::kRowHeight;
        recycler_->registerCell("GridRow", [column = focusColumn_] {
            return new GridRowCell(column);
        });
        recycler_->registerCell("Shelf", [] { return new ShelfCell(); });
        recycler_->registerCell("Message",
            [] { return new TextMessageCell(); });
        dataSource_ = new CatalogDataSource(this);
        recycler_->setDataSource(dataSource_);

        // Persistent catalog header (UI_PLAN O2): tappable search field,
        // sort + filter chips and a result counter. X/Y hotkeys still work;
        // the chips make the current state visible and touch-reachable.
        header_ = new brls::Box(brls::Axis::ROW);
        header_->setMarginTop(10);
        header_->setMarginLeft(34);
        header_->setMarginRight(34);
        searchField_ = new brls::Button();
        searchField_->setStyle(&brls::BUTTONSTYLE_DEFAULT);
        searchField_->setHeight(40);
        searchField_->setGrow(1);
        searchField_->setFontSize(theme::kFontSmall);
        searchField_->setText("Search");
        searchField_->registerClickAction([this](brls::View*) {
            openSearchKeyboard();
            return true;
        });
        header_->addView(searchField_);
        clearSearch_ = makeChip("Clear", [this] {
            if (query_.empty())
                return;
            query_.clear();
            rebuildEntries();
        });
        header_->addView(clearSearch_);
        sortLatest_ = makeChip("Latest", [this] { setSort(SortMode::Latest); });
        sortAlpha_ = makeChip("A-Z", [this] { setSort(SortMode::Alphabetical); });
        sortSize_ = makeChip("Size", [this] { setSort(SortMode::Largest); });
        sortLatest_->setMarginLeft(16);
        header_->addView(sortLatest_);
        header_->addView(sortAlpha_);
        header_->addView(sortSize_);
        filterAll_ = makeChip("All", [this] { setFilter(CatalogFilter::All); });
        filterGames_ = makeChip("Games",
                                [this] { setFilter(CatalogFilter::Games); });
        filterAll_->setMarginLeft(16);
        header_->addView(filterAll_);
        header_->addView(filterGames_);
        if (!settings_) {
            filterAll_->setVisibility(brls::Visibility::GONE);
            filterGames_->setVisibility(brls::Visibility::GONE);
        }
        count_ = new brls::Label();
        count_->setFontSize(theme::kFontCaption);
        count_->setTextColor(theme::textTertiary());
        count_->setMarginLeft(16);
        count_->setMarginTop(12);
        header_->addView(count_);

        // Batch-mode summary line; hidden in normal browsing (the header
        // counter replaces the old always-on status text).
        status_ = new brls::Label();
        status_->setFontSize(15);
        status_->setMarginTop(10);
        status_->setMarginLeft(34);
        status_->setMarginBottom(2);
        status_->setTextColor(theme::textTertiary());
        status_->setVisibility(brls::Visibility::GONE);

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

        addView(header_);
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

    EmptyStateView* ensureEmptyState() {
        if (emptyState_)
            return emptyState_;
        emptyState_ = new EmptyStateView();
        addView(emptyState_);
        return emptyState_;
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
        // reloadData() recycles every cell, so remember where the focus was
        // (F2 "done when": focus survives reloadData) and restore it after.
        brls::View* focus = brls::Application::getCurrentFocus();
        bool focusInCatalog = false;
        for (brls::View* view = focus; view; view = view->getParent()) {
            if (view == recycler_) {
                focusInCatalog = true;
                break;
            }
        }
        std::string focusHash;
        int focusShelf = -1;
        if (focusInCatalog) {
            if (auto* card = dynamic_cast<GameCard*>(focus)) {
                focusHash = card->infoHash();
                focusShelf = card->shelfRow();
            }
        }

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

        // Shelves (UI_PLAN F2): "New" by published_at, "Popular" by
        // peer_count. Only in the default browse state — a search, an active
        // sort context or batch mode wants the plain grid.
        std::vector<int> shelfNew;
        std::vector<int> shelfPopular;
        if (query_.empty() && !batchMode_ && !visible.empty()) {
            const size_t take =
                std::min<size_t>(grid::kShelfItems, visible.size());
            std::vector<int> order(visible.size());
            std::iota(order.begin(), order.end(), 0);
            std::partial_sort(order.begin(),
                order.begin() + static_cast<long>(take), order.end(),
                [&visible](int left, int right) {
                    return visible[static_cast<size_t>(left)].publishedAt >
                           visible[static_cast<size_t>(right)].publishedAt;
                });
            shelfNew.assign(order.begin(),
                            order.begin() + static_cast<long>(take));
            std::iota(order.begin(), order.end(), 0);
            std::partial_sort(order.begin(),
                order.begin() + static_cast<long>(take), order.end(),
                [&visible](int left, int right) {
                    const auto& l = visible[static_cast<size_t>(left)];
                    const auto& r = visible[static_cast<size_t>(right)];
                    if (l.peerCount != r.peerCount)
                        return l.peerCount > r.peerCount;
                    return l.publishedAt > r.publishedAt;
                });
            for (size_t i = 0; i < take; ++i) {
                if (visible[static_cast<size_t>(order[i])].peerCount > 0)
                    shelfPopular.push_back(order[i]);
            }
        }

        size_t count = visible.size();
        dataSource_->setEntries(std::move(visible), std::move(stateBadges),
                                std::move(gameNames), std::move(iconUrls),
                                std::move(selected), std::move(selectable),
                                metadata_, batchMode_);
        dataSource_->setShelves(std::move(shelfNew), std::move(shelfPopular));
        dataSource_->setMessage(query_.empty()
            ? "Catalog is empty. Press R to refresh."
            : "Nothing found. Tap Search or press X to change the query.");
        recycler_->reloadData();
        const bool empty = count == 0;
        if (empty) {
            if (query_.empty()) {
                ensureEmptyState()->setContent(
                    "Catalog is empty",
                    "Refresh the catalog to load the latest releases on this "
                    "console.",
                    "Refresh catalog", [this] { refreshCatalog(); });
            } else {
                ensureEmptyState()->setContent(
                    "Nothing found",
                    "Try another query or clear the current search to see all "
                    "releases again.",
                    "Clear search", [this] {
                        if (query_.empty())
                            return;
                        query_.clear();
                        rebuildEntries();
                    });
            }
        }
        if (empty)
            ensureEmptyState()->setVisibility(brls::Visibility::VISIBLE);
        else if (emptyState_)
            emptyState_->setVisibility(brls::Visibility::GONE);
        recycler_->setVisibility(empty ? brls::Visibility::GONE
                                       : brls::Visibility::VISIBLE);
        if (focusInCatalog)
            restoreFocus(focusHash, focusShelf);

        countText_ = query_.empty()
            ? withThousands(count) + (count == 1 ? " release" : " releases")
            : withThousands(count) + (count == 1 ? " match" : " matches");
        if (!busy_ && batchMode_) {
            refreshBatchStatus();
        } else if (!busy_) {
            setActionAvailable(brls::BUTTON_RB, true);
        }
        status_->setVisibility(batchMode_ ? brls::Visibility::VISIBLE
                                          : brls::Visibility::GONE);
        updateHeader();
    }

    // Sync the O2 header with the current state: search field mirrors the
    // query, the active sort/filter chip is highlighted, counter shows the
    // visible entry count.
    void updateHeader() {
        searchField_->setText(query_.empty() ? "Search" : query_);
        clearSearch_->setVisibility(query_.empty() ? brls::Visibility::GONE
                                                   : brls::Visibility::VISIBLE);
        styleChip(sortLatest_, sort_ == SortMode::Latest);
        styleChip(sortAlpha_, sort_ == SortMode::Alphabetical);
        styleChip(sortSize_, sort_ == SortMode::Largest);
        const bool gamesOnly = settings_ &&
            settings_->get().catalogFilter == CatalogFilter::Games;
        styleChip(filterAll_, !gamesOnly);
        styleChip(filterGames_, gamesOnly);
        count_->setText(countText_);
    }

    static void styleChip(brls::Button* chip, bool active) {
        chip->setStyle(active ? &brls::BUTTONSTYLE_PRIMARY
                              : &brls::BUTTONSTYLE_DEFAULT);
    }

    brls::Button* makeChip(const std::string& text,
                           std::function<void()> onClick) {
        auto* chip = new brls::Button();
        chip->setStyle(&brls::BUTTONSTYLE_DEFAULT);
        chip->setHeight(40);
        chip->setFontSize(theme::kFontCaption);
        chip->setMarginLeft(8);
        chip->setText(text);
        chip->registerClickAction(
            [onClick = std::move(onClick)](brls::View*) {
                onClick();
                return true;
            });
        return chip;
    }

    void setSort(SortMode mode) {
        if (busy_ || sort_ == mode)
            return;
        sort_ = mode;
        rebuildEntries();
    }

    void setFilter(CatalogFilter filter) {
        if (busy_ || !settings_ ||
            settings_->get().catalogFilter == filter)
            return;
        AppSettingsData values = settings_->get();
        values.catalogFilter = filter;
        std::string error;
        if (!settings_->update(values, error)) {
            brls::Application::notify(error);
            return;
        }
        observedSettingsGeneration_ = settings_->generation();
        rebuildEntries();
    }

    // Re-focus the card that was focused before reloadData recycled all
    // cells: same shelf if the focus lived on a shelf, otherwise the grid
    // card with the same info-hash (or the closest fallback). selectRowAt()
    // scrolls the row in and marks it as the content box's last-focused
    // child, so giving focus to the recycler lands on the row's
    // getDefaultFocus(), which honors focusColumn_.
    void restoreFocus(const std::string& hash, int shelfRow) {
        if (dataSource_->entries().empty()) {
            brls::Application::giveFocus(ensureEmptyState());
            return;
        }
        if (shelfRow >= 0 && shelfRow < dataSource_->shelfRowCount()) {
            recycler_->selectRowAt(brls::IndexPath(0, shelfRow), false);
            brls::Application::giveFocus(recycler_);
            return;
        }
        int index = 0;
        const std::vector<CatalogEntry>& list = dataSource_->entries();
        for (size_t i = 0; i < list.size(); ++i) {
            if (list[i].infoHash == hash) {
                index = static_cast<int>(i);
                break;
            }
        }
        *focusColumn_ = dataSource_->columnForEntry(index);
        recycler_->selectRowAt(
            brls::IndexPath(0, dataSource_->rowForEntry(index)), false);
        brls::Application::giveFocus(recycler_);
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

    // Y hotkey: cycle through the sort modes; the header chips (O2) reflect
    // the result, so no toast is needed.
    void cycleSort() {
        setSort(sort_ == SortMode::Latest        ? SortMode::Alphabetical
              : sort_ == SortMode::Alphabetical ? SortMode::Largest
                                                 : SortMode::Latest);
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
    brls::Box* header_ = nullptr;
    brls::Button* searchField_ = nullptr;
    brls::Button* clearSearch_ = nullptr;
    brls::Button* sortLatest_ = nullptr;
    brls::Button* sortAlpha_ = nullptr;
    brls::Button* sortSize_ = nullptr;
    brls::Button* filterAll_ = nullptr;
    brls::Button* filterGames_ = nullptr;
    brls::Label* count_ = nullptr;
    brls::Label* status_;
    brls::Box* batchControls_ = nullptr;
    brls::Button* prepareBatch_ = nullptr;
    EmptyStateView* emptyState_ = nullptr;
    std::shared_ptr<std::atomic<bool>> alive_;
    std::shared_ptr<std::atomic<bool>> cancelled_;
    // Column the user last focused in the grid; grid rows read it in
    // getDefaultFocus() so vertical navigation keeps the column.
    std::shared_ptr<int> focusColumn_ = std::make_shared<int>(0);
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
