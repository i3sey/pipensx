#pragma once

#include <algorithm>
#include <functional>
#include <memory>
#include <numeric>
#include <string>
#include <thread>
#include <ctime>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <borealis.hpp>

#include "app/app_settings.hpp"
#include "app/catalog_presentation.hpp"
#include "app/catalog_refresh.hpp"
#include "app/catalog_service.hpp"
#include "app/switch_deploy.hpp"
#include "app/download_manager.hpp"
#include "app/favorites_service.hpp"
#include "app/game_metadata_service.hpp"
#include "app/install_space.hpp"
#include "app/installed_title_service.hpp"
#include "ui/catalog/catalog_grid.hpp"
#include "ui/catalog/catalog_helpers.hpp"
#include "ui/i18n.hpp"
#include "ui/common/message_cells.hpp"
#include "ui/common/ui_helpers.hpp"
#include "ui/detail/game_detail.hpp"
#include "ui/theme.hpp"

namespace pipensx::ui {

class CatalogView;

class CatalogDataSource : public brls::RecyclerDataSource {
public:
    explicit CatalogDataSource(CatalogView* owner) : owner_(owner) {}

    void setEntries(std::shared_ptr<const std::vector<CatalogEntry>> snapshot,
                    std::vector<int> indices,
                    std::vector<std::string> stateBadges,
                    std::vector<std::string> gameNames,
                    std::vector<std::string> iconUrls,
                    std::vector<uint8_t> iconPreserveAspect,
                    std::vector<uint8_t> selected,
                    std::vector<uint8_t> selectable,
                    std::vector<uint8_t> favorite,
                    GameMetadataService* metadata) {
        snapshot_ = snapshot ? std::move(snapshot)
                             : std::make_shared<const std::vector<CatalogEntry>>();
        indices_ = std::move(indices);
        stateBadges_ = std::move(stateBadges);
        gameNames_ = std::move(gameNames);
        iconUrls_ = std::move(iconUrls);
        iconPreserveAspect_ = std::move(iconPreserveAspect);
        selected_ = std::move(selected);
        selectable_ = std::move(selectable);
        favorite_ = std::move(favorite);
        metadata_ = metadata;
        selectionMode_ = false;
    }
    void updateStateBadges(std::vector<std::string> stateBadges) {
        stateBadges_ = std::move(stateBadges);
    }
    bool sameStructure(
        const std::shared_ptr<const std::vector<CatalogEntry>>& snapshot,
        const std::vector<int>& indices) const {
        auto hashAt = [](const std::shared_ptr<
                             const std::vector<CatalogEntry>>& entries,
                         const std::vector<int>& picks, int row)
            -> const std::string* {
            if (!entries || row < 0 ||
                static_cast<size_t>(row) >= picks.size())
                return nullptr;
            const int index = picks[static_cast<size_t>(row)];
            if (index < 0 || static_cast<size_t>(index) >= entries->size())
                return nullptr;
            return &(*entries)[static_cast<size_t>(index)].infoHash;
        };
        if (indices_.size() != indices.size())
            return false;
        for (size_t row = 0; row < indices.size(); ++row) {
            const std::string* before =
                hashAt(snapshot_, indices_, static_cast<int>(row));
            const std::string* after =
                hashAt(snapshot, indices, static_cast<int>(row));
            if (!before || !after || *before != *after)
                return false;
        }
        return true;
    }
    GridCardInfo cardInfo(int index) const { return makeInfo(index); }
    void setMessage(const std::string& message) { message_ = message; }
    const CatalogEntry* entryAt(int row) const {
        if (!snapshot_ || row < 0 ||
            static_cast<size_t>(row) >= indices_.size())
            return nullptr;
        const int index = indices_[static_cast<size_t>(row)];
        if (index < 0 ||
            static_cast<size_t>(index) >= snapshot_->size())
            return nullptr;
        return &(*snapshot_)[static_cast<size_t>(index)];
    }
    size_t entryCount() const { return indices_.size(); }
    bool selectableAt(int row) const {
        return row >= 0 && static_cast<size_t>(row) < selectable_.size() &&
               selectable_[static_cast<size_t>(row)] != 0;
    }

    // Recycler row layout: [top inset][grid rows of cards].
    int headerRowCount() const { return 1; }
    int rowForEntry(int index) const {
        return headerRowCount() + index / grid::kColumns;
    }
    int columnForEntry(int index) const { return index % grid::kColumns; }

    int numberOfRows(brls::RecyclerFrame*, int) override {
        if (indices_.empty())
            return 2;
        const int gridRows =
            (static_cast<int>(indices_.size()) + grid::kColumns - 1) /
            grid::kColumns;
        return headerRowCount() + gridRows;
    }
    float heightForRow(brls::RecyclerFrame*, brls::IndexPath index) override {
        if (index.row == 0)
            return grid::kTopInsetHeight;
        if (indices_.empty())
            return 100;
        return grid::kRowHeight;
    }
    brls::RecyclerCell* cellForRow(brls::RecyclerFrame* recycler,
                                    brls::IndexPath index) override;
    void repaintCell(brls::RecyclerCell* cell);
    void didSelectRowAt(brls::RecyclerFrame*, brls::IndexPath index) override;

private:
    GridCardInfo makeInfo(int index) const {
        const size_t row = static_cast<size_t>(index);
        const CatalogEntry* entry = entryAt(index);
        GridCardInfo info;
        info.entryIndex = index;
        info.infoHash = entry ? entry->infoHash : std::string();
        info.title = row < gameNames_.size() && !gameNames_[row].empty()
            ? gameNames_[row]
            : (entry ? entry->title : std::string());
        const std::string badge =
            row < stateBadges_.size() ? stateBadges_[row] : std::string();
        info.subIsBadge = !badge.empty();
        const uint64_t size = entry ? entry->size : 0;
        info.sub = info.subIsBadge
            ? badge
            : size ? formatBytes(size)
                   : tr("pipensx/catalog/unknown_size");
        info.iconUrl = row < iconUrls_.size() ? iconUrls_[row] : std::string();
        info.iconPreserveAspect = row < iconPreserveAspect_.size() &&
                                  iconPreserveAspect_[row] != 0;
        info.selectionMode = selectionMode_;
        info.selected = row < selected_.size() && selected_[row] != 0;
        info.selectable = row < selectable_.size() && selectable_[row] != 0;
        info.favorite = row < favorite_.size() && favorite_[row] != 0;
        return info;
    }

    // UI_PLAN F6: warm the memory cache for a grid row about to scroll in,
    // so its covers paint in their first frame instead of re-decoding.
    void prefetchGridRow(int row) const {
        if (!metadata_ || row < headerRowCount())
            return;
        const int start = (row - headerRowCount()) * grid::kColumns;
        const int end = std::min(start + grid::kColumns,
                                 static_cast<int>(iconUrls_.size()));
        for (int i = start; i < end; ++i)
            metadata_->prefetchImage(iconUrls_[static_cast<size_t>(i)],
                                     GameMetadataService::kImageDimGrid);
    }

    CatalogView* owner_;
    std::shared_ptr<const std::vector<CatalogEntry>> snapshot_ =
        std::make_shared<const std::vector<CatalogEntry>>();
    std::vector<int> indices_;
    std::vector<std::string> stateBadges_;
    std::vector<std::string> gameNames_;
    std::vector<std::string> iconUrls_;
    std::vector<uint8_t> iconPreserveAspect_;
    std::vector<uint8_t> selected_;
    std::vector<uint8_t> selectable_;
    std::vector<uint8_t> favorite_;
    GameMetadataService* metadata_ = nullptr;
    std::string message_;
    bool selectionMode_ = false;
};
// Compact magnifier button for the O2 header. The icon is vector-drawn
// (circle + handle) so it stays crisp and theme-aware without relying on
// icon-font glyphs that the bundled fonts may not cover.
class SearchIconButton : public brls::Button {
public:
    void setIconColor(NVGcolor color) { iconColor_ = color; }

    void draw(NVGcontext* vg, float x, float y, float width, float height,
              brls::Style style, brls::FrameContext* ctx) override {
        brls::Button::draw(vg, x, y, width, height, style, ctx);
        // Glass centered slightly up-left so the handle stays inside.
        const float r = 6.5f;
        const float cx = x + width / 2.0f - 2.5f;
        const float cy = y + height / 2.0f - 2.5f;
        nvgStrokeColor(vg, iconColor_);
        nvgStrokeWidth(vg, 2.0f);
        nvgLineCap(vg, NVG_ROUND);
        nvgBeginPath(vg);
        nvgCircle(vg, cx, cy, r);
        nvgStroke(vg);
        const float k = 0.7071f; // 45° handle
        nvgBeginPath(vg);
        nvgMoveTo(vg, cx + r * k, cy + r * k);
        nvgLineTo(vg, cx + r * k + 5.5f, cy + r * k + 5.5f);
        nvgStroke(vg);
    }

private:
    NVGcolor iconColor_ = theme::textPrimary();
};
class CatalogView : public brls::Box {
public:
    CatalogView(DownloadManager* manager, CatalogService* catalog,
                GameMetadataService* metadata,
                InstalledTitleService* installed, AppSettings* settings,
                 std::function<void()> openDownloads,
                 FavoritesService* favorites = nullptr,
                 SwitchDeployService* deploy = nullptr,
                CatalogSection section = CatalogSection::Games,
                std::function<void()> openGames = {},
                std::function<void()> onSourcesRefreshed = {})
        : brls::Box(brls::Axis::COLUMN), manager_(manager), catalog_(catalog),
          metadata_(metadata), installed_(installed), settings_(settings),
          favorites_(favorites), deploy_(deploy),
          openDownloads_(std::move(openDownloads)),
          openGames_(std::move(openGames)),
          onSourcesRefreshed_(std::move(onSourcesRefreshed)), section_(section),
          freshnessBadge_(new CatalogFreshnessBadge()),
          alive_(std::make_shared<std::atomic<bool>>(true)),
          cancelled_(std::make_shared<std::atomic<bool>>(false)) {
        recycler_ = new brls::RecyclerFrame();
        recycler_->setGrow(1);
        recycler_->setPadding(6, 28, 6, 28);
        recycler_->estimatedRowHeight = grid::kRowHeight;
        recycler_->registerCell("TopInset", [] { return new TopInsetCell(); });
        recycler_->registerCell("GridRow", [column = focusColumn_] {
            return new GridRowCell(column);
        });
        recycler_->registerCell("Message",
            [] { return new TextMessageCell(); });
        dataSource_ = new CatalogDataSource(this);
        recycler_->setDataSource(dataSource_);

        header_ = new brls::Box(brls::Axis::ROW);
        header_->setHeight(44);
        header_->setShrink(0.0f);
        header_->setMarginTop(6);
        header_->setMarginBottom(10);
        header_->setMarginLeft(28);
        header_->setMarginRight(28);
        header_->setAlignItems(brls::AlignItems::CENTER);
        sortButton_ = makeChip(tr("pipensx/catalog/sort_latest"),
                               [this] { openSortSheet(); });
        sortButton_->setMarginLeft(0);
        header_->addView(sortButton_);
        auto* split = new brls::Box();
        split->setWidth(1);
        split->setHeight(22);
        split->setFocusable(false);
        split->setBackgroundColor(theme::track());
        split->setMarginLeft(8);
        split->setMarginRight(8);
        split->setShrink(0.0f);
        header_->addView(split);
        auto* filters = new brls::Box(brls::Axis::ROW);
        filters->setAlignItems(brls::AlignItems::CENTER);
        filters->setPadding(4, 4, 4, 4);
        filters->setCornerRadius(10);
        filters->setBackgroundColor(theme::panel());
        filters->setShrink(0.0f);
        filterFavorites_ = makeChip(tr("pipensx/catalog/filter_favorites"),
                                    [this] {
            favoritesOnly_ = !favoritesOnly_;
            rebuildEntries();
        });
        filterFavorites_->setMarginLeft(0);
        filterFits_ = makeChip(tr("pipensx/catalog/filter_fits"), [this] {
            fitsOnly_ = !fitsOnly_;
            rebuildEntries();
            if (fitsOnly_)
                scheduleStorageRefresh();
        });
        filterPlayers_ = makeChip(tr("pipensx/catalog/filter_players"),
                                  [this] { openPlayerFilterMenu(); });
        filterGenres_ = makeChip(tr("pipensx/catalog/filter_genres"),
                                 [this] { openGenreSheet(); });
        filters->addView(filterFavorites_);
        filters->addView(filterFits_);
        filters->addView(filterPlayers_);
        filters->addView(filterGenres_);
        header_->addView(filters);
        if (!favorites_)
            filterFavorites_->setVisibility(brls::Visibility::GONE);
        updatePlayerChipVisibility();
        auto* headerSpacer = new brls::Box();
        headerSpacer->setGrow(1.0f);
        headerSpacer->setShrink(1.0f);
        header_->addView(headerSpacer);
        freshnessBadge_->setMarginLeft(8);
        header_->addView(freshnessBadge_);
        count_ = new brls::Label();
        count_->setFontSize(theme::kFontCaption);
        count_->setTextColor(theme::textTertiary());
        count_->setMarginLeft(8);
        count_->setShrink(0.0f);
        header_->addView(count_);
        clearSearch_ = makeChip(tr("pipensx/catalog/filter_reset"), [this] {
            resetFilters();
        });
        clearSearch_->setMarginLeft(8);
        header_->addView(clearSearch_);
        searchField_ = new SearchIconButton();
        searchField_->setStyle(&brls::BUTTONSTYLE_DEFAULT);
        searchField_->setWidth(44);
        searchField_->setHeight(44);
        searchField_->setShrink(0.0f);
        searchField_->setMarginLeft(8);
        searchField_->setText("");
        searchField_->registerAction("", brls::BUTTON_A, [this](brls::View*) {
            openSearchKeyboard();
            return true;
        }, /*hidden=*/true);
        searchField_->addGestureRecognizer(
            new brls::TapGestureRecognizer(searchField_));
        header_->addView(searchField_);

        addView(header_);
        recyclerHost_ = recyclerHost(recycler_);
        addView(recyclerHost_);
        if (catalogRefreshInFlight())
            setBusy(true);
        rebuildEntries();
        updateFreshnessLabel();

        // X hotkey: toggle the "fits on SD" filter. Same predicate as the
        // header chip, so the button and the chip never disagree.
        registerAction(tr("pipensx/catalog/filter_fits"), brls::BUTTON_X,
                       [this](brls::View*) {
            fitsOnly_ = !fitsOnly_;
            rebuildEntries();
            if (fitsOnly_)
                scheduleStorageRefresh();
            return true;
        }, /*hidden=*/false);
        registerYAction(false);
        if (favorites_) {
            registerAction(tr("pipensx/catalog/action_favorite"),
                           brls::BUTTON_RT, [this](brls::View*) {
                toggleFocusedFavorite();
                return true;
            });
        }
        observedSettingsGeneration_ = settings_ ? settings_->generation() : 0;
        {
            const auto tasks = manager_->snapshotUi();
            uint64_t ids = 0;
            bool installed = false;
            hashTasks(tasks, ids, taskSignature_, installed);
            taskIdSignature_ = ids;
        }
        timer_.setCallback([this] { refreshLiveState(); });
        timer_.start(1000);
        scheduleStorageRefresh();
        refreshCatalogIfDue();
    }

    void willAppear(bool resetState) override {
        brls::Box::willAppear(resetState);
        timer_.start(1000);
        if (pendingRebuild_)
            rebuildEntries();
    }

    void willDisappear(bool resetState) override {
        timer_.stop();
        brls::Box::willDisappear(resetState);
    }

    ~CatalogView() override {
        alive_->store(false);
        cancelled_->store(true);
        timer_.stop();
        freshnessBadge_->setBusy(false);
    }

    // Golden behavior seams: stable identity survives recycling, and tests
    // can trigger the same rebuild path as live refreshes.
    std::string focusedInfoHashForTest() const {
        brls::View* focus = brls::Application::getCurrentFocus();
        bool ownsFocus = false;
        for (brls::View* view = focus; view; view = view->getParent()) {
            if (view == this) {
                ownsFocus = true;
                break;
            }
        }
        if (!ownsFocus)
            return {};
        if (auto* card = dynamic_cast<GameCard*>(focus))
            return card->infoHash();
        return {};
    }

    void rebuildEntriesForTest() { rebuildEntries(); }

    void openSearchKeyboard() {
        if (busy_)
            return;
        brls::Application::getImeManager()->openForText(
            [this](std::string text) {
                query_ = std::move(text);
                rebuildEntries();
            },
            tr("pipensx/catalog/search_placeholder"), "", 256, query_,
            brls::KEYBOARD_DISABLE_NONE);
    }

    void onEntrySelected(int row) {
        const CatalogEntry* picked = dataSource_->entryAt(row);
        if (!picked || busy_)
            return;
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
        // O12: the detail page fires this from its destructor (re-badge the
        // list). Running the rebuild synchronously there recycles the grid
        // cells mid-pop, so borealis restores focus to a stale cell and
        // smooth-scrolls to it for one frame before onDetailClosed re-seats —
        // the visible "scroll a hair and snap back". Defer it so the original
        // focused card survives the pop; the rebuild + focus re-seat then land
        // together in the next frame's sync pass, before any draw.
        auto onChange = [this, alive = alive_] {
            brls::sync([this, alive] {
                if (alive->load())
                    rebuildEntries();
            });
        };
        // O12: remember which card opened the page. On the way back the
        // borealis focus stack may point at a recycled cell (live rebuilds
        // while the page is open reload the recycler with the focus away in
        // the detail activity, resetting the scroll), so the catalog re-seats
        // scroll + focus itself when the page closes.
        returnFocusHash_ = picked->infoHash;
        returnFocusShelf_ = -1;
        brls::View* focus = brls::Application::getCurrentFocus();
        if (auto* card = dynamic_cast<GameCard*>(focus)) {
            if (card->entryIndex() == row)
                returnFocusShelf_ = card->shelfRow();
        }
        auto onClose = [this, alive = alive_] {
            if (alive->load())
                onDetailClosed();
        };
        brls::Application::pushActivity(new GameDetailActivity(
            std::move(entry), std::move(lastFailure), manager_, metadata_,
            installed_, settings_,
            std::move(onFailure), std::move(onChange), std::move(onClose),
            favorites_, deploy_, false,
            section_ == CatalogSection::Ports));
    }

private:
    enum class SortMode { Latest, Popular, Alphabetical, Largest };

    static std::string lowerAscii(std::string value) {
        std::transform(value.begin(), value.end(), value.begin(),
                       [](unsigned char c) {
                           return static_cast<char>(std::tolower(c));
                       });
        return value;
    }

    static std::string upperAscii(std::string value) {
        std::transform(value.begin(), value.end(), value.begin(),
                       [](unsigned char c) {
                           return static_cast<char>(std::toupper(c));
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

    // ZR on the grid: star/unstar whatever card the focus is on. Resolved the
    // same way rebuildEntries() resolves focus — the hero banner is a card too.
    void toggleFocusedFavorite() {
        if (!favorites_ || busy_)
            return;
        brls::View* focus = brls::Application::getCurrentFocus();
        std::string hash;
        if (auto* card = dynamic_cast<GameCard*>(focus))
            hash = card->infoHash();
        if (hash.empty())
            return;

        const CatalogEntry* entry = nullptr;
        for (size_t i = 0; i < dataSource_->entryCount(); ++i) {
            const CatalogEntry* candidate =
                dataSource_->entryAt(static_cast<int>(i));
            if (candidate && candidate->infoHash == hash) {
                entry = candidate;
                break;
            }
        }
        if (!entry)
            return;
        const GameMetadata* meta =
            metadata_ ? metadata_->findByInfoHash(entry->infoHash) : nullptr;
        CatalogPresentation presentation =
            resolveCatalogPresentation(*entry, meta, catalogTextPreference());

        // The service reports the cap through `error` too, but the cap is the
        // one case worth phrasing for the user rather than surfacing raw.
        if (!favorites_->contains(hash) &&
            favorites_->items().size() >= FavoritesService::kMaxFavorites) {
            brls::Application::notify(
                tr("pipensx/catalog/favorites_full",
                   FavoritesService::kMaxFavorites));
            return;
        }
        std::string error;
        favorites_->toggle(hash, presentation.title, error);
        if (!error.empty()) {
            brls::Application::notify(
                tr("pipensx/catalog/favorites_failed", error));
            return;
        }
        rebuildEntries();
    }

    void rebuildEntries() {
        // reloadData() recycles every cell, so remember where the focus was
        // (F2 "done when": focus survives reloadData) and restore it after.
        brls::View* focus = brls::Application::getCurrentFocus();
        bool ownsFocus = false;
        bool focusInRecycler = false;
        for (brls::View* view = focus; view; view = view->getParent()) {
            if (view == recycler_)
                focusInRecycler = true;
            if (view == this) {
                ownsFocus = true;
                break;
            }
        }
        // Same focusStack UAF as downloads: don't free cells while a dialog
        // (e.g. deploy offer) is on top of the activity stack.
        if (activityStackHasOverlay() && !ownsFocus) {
            pendingRebuild_ = true;
            return;
        }
        pendingRebuild_ = false;
        const uint64_t startedUs = telemetry_enabled() ? now_us() : 0;

        std::string focusHash;
        int focusShelf = -1;
        if (focusInRecycler) {
            if (auto* card = dynamic_cast<GameCard*>(focus)) {
                focusHash = card->infoHash();
                focusShelf = card->shelfRow();
            }
        }

        // Info-hash (lower-case hex) -> status for anything already managed,
        // so rows can be badged. Task ids are lower-case hex; catalog info
        // hashes are upper-case, hence the case fold on both sides.
        std::unordered_map<std::string, DownloadStatus> added;
        for (const DownloadTask& task : manager_->snapshotUi())
            added[lowerAscii(task.id)] = task.status;

        auto snapshot = catalog_->sharedEntries();
        const std::vector<CatalogEntry>& all = *snapshot;
        std::vector<int> visible;
        std::vector<const GameMetadata*> metadataByEntry(all.size(), nullptr);
        // Fold once per rebuild: the predicate folds each haystack, so a
        // Russian query matches titles in either case (ASCII-only folding
        // broke this) and also matches the catalogue's own genre string,
        // which half the entries need (no metadata join).
        const std::string needle = catalogFoldForSearch(query_);
        const bool searching = !needle.empty();
        const bool favoritesOnly = favoritesOnly_ && favorites_;
        const uint64_t filterStartedUs = startedUs ? now_us() : 0;
        visible.reserve(all.size());
        for (size_t i = 0; i < all.size(); ++i) {
            const CatalogEntry& entry = all[i];
            if (entry.isHiddenByDefault())
                continue;
            if (favoritesOnly && !favorites_->contains(entry.infoHash))
                continue;
            if (fitsOnly_ && !catalogEntryFitsFreeSpace(entry.size, storage_))
                continue;
            const GameMetadata* meta =
                metadata_ ? metadata_->findByInfoHash(entry.infoHash) : nullptr;
            metadataByEntry[i] = meta;
            if (!catalogEntryInSection(entry, meta, section_))
                continue;
            // Unlike the Games filter above, this one also narrows a search:
            // "which racing game can we play together" is exactly the question
            // it exists for.
            if (playerFilter_ != PlayerFilter::Any &&
                !catalogEntryMatchesPlayerFilter(meta, playerFilter_))
                continue;
            if (!genreFilters_.empty() &&
                !entryMatchesGenres(entry, meta))
                continue;
            bool matches = !searching ||
                catalogEntryMatchesSearch(entry, meta, needle);
            if (!matches)
                continue;
            visible.push_back(static_cast<int>(i));
        }
        const uint64_t filterDurationUs =
            filterStartedUs ? now_us() - filterStartedUs : 0;
        const uint64_t sortStartedUs = startedUs ? now_us() : 0;
        if (sort_ == SortMode::Alphabetical) {
            std::vector<std::string> keys(visible.size());
            for (size_t i = 0; i < visible.size(); ++i)
                keys[i] = catalogFoldForSearch(
                    all[static_cast<size_t>(visible[i])].title);
            std::vector<int> order(visible.size());
            std::iota(order.begin(), order.end(), 0);
            std::stable_sort(order.begin(), order.end(),
                [&keys](int left, int right) {
                    return keys[static_cast<size_t>(left)] <
                           keys[static_cast<size_t>(right)];
                });
            std::vector<int> sorted;
            sorted.reserve(visible.size());
            for (int index : order)
                sorted.push_back(visible[static_cast<size_t>(index)]);
            visible = std::move(sorted);
        } else if (sort_ == SortMode::Largest) {
            std::stable_sort(visible.begin(), visible.end(),
                [&all](int left, int right) {
                    return all[static_cast<size_t>(left)].size >
                           all[static_cast<size_t>(right)].size;
                });
        } else if (sort_ == SortMode::Popular) {
            bool popularFallback = false;
            const std::vector<int> order =
                catalogPopularityOrder(all, visible, popularFallback);
            std::vector<int> sorted;
            sorted.reserve(order.size());
            for (int index : order)
                sorted.push_back(visible[static_cast<size_t>(index)]);
            visible = std::move(sorted);
            (void)popularFallback;
        } else {
            std::stable_sort(visible.begin(), visible.end(),
                [&all](int left, int right) {
                    return all[static_cast<size_t>(left)].publishedAt >
                           all[static_cast<size_t>(right)].publishedAt;
                });
        }
        applySortDirection(visible);
        const uint64_t sortDurationUs =
            sortStartedUs ? now_us() - sortStartedUs : 0;

        std::vector<std::string> stateBadges;
        std::vector<std::string> gameNames;
        std::vector<std::string> iconUrls;
        std::vector<uint8_t> iconPreserveAspect;
        std::vector<uint8_t> selected;
        std::vector<uint8_t> selectable;
        std::vector<uint8_t> favorite;
        stateBadges.reserve(visible.size());
        gameNames.reserve(visible.size());
        iconUrls.reserve(visible.size());
        iconPreserveAspect.reserve(visible.size());
        selected.reserve(visible.size());
        selectable.reserve(visible.size());
        favorite.reserve(visible.size());
        const uint64_t presentationStartedUs = startedUs ? now_us() : 0;
        const std::unordered_set<std::string> installedIds =
            installed_ ? installed_->titleIds()
                       : std::unordered_set<std::string>();
        for (int snapshotIndex : visible) {
            const CatalogEntry& entry = all[static_cast<size_t>(snapshotIndex)];
            std::string hash = lowerAscii(entry.infoHash);
            auto it = added.find(hash);
            const bool canSelect = it == added.end();
            if (it != added.end()) {
                stateBadges.push_back(badgeForStatus(it->second));
                selectedHashes_.erase(hash);
            } else
                stateBadges.emplace_back();
            const GameMetadata* meta =
                metadataByEntry[static_cast<size_t>(snapshotIndex)];
            CatalogRowPresentation row =
                resolveCatalogRow(entry, meta);
            if (it == added.end() && !row.titleId.empty() &&
                installedIds.count(upperAscii(row.titleId)))
                stateBadges.back() = tr("pipensx/catalog/badge_installed");
            gameNames.push_back(std::move(row.title));
            iconUrls.push_back(std::move(row.iconUrl));
            iconPreserveAspect.push_back(row.iconPreserveAspect ? 1 : 0);
            selected.push_back(selectedHashes_.count(hash) ? 1 : 0);
            selectable.push_back(canSelect ? 1 : 0);
            favorite.push_back(favorites_ && favorites_->contains(hash) ? 1
                                                                       : 0);
        }
        const uint64_t presentationDurationUs =
            presentationStartedUs ? now_us() - presentationStartedUs : 0;

        const size_t count = visible.size();
        const bool structureChanged =
            !dataSource_->sameStructure(snapshot, visible);
        observedCatalog_ = snapshot;
        observedMetadataGeneration_ =
            metadata_ ? metadata_->generation() : 0;
        dataSource_->setEntries(std::move(snapshot), std::move(visible),
                                std::move(stateBadges),
                                std::move(gameNames), std::move(iconUrls),
                                std::move(iconPreserveAspect),
                                std::move(selected), std::move(selectable),
                                std::move(favorite),
                                metadata_);
        dataSource_->setMessage(query_.empty()
            ? tr("pipensx/catalog/empty_inline")
            : tr("pipensx/catalog/nothing_found_inline"));
        const uint64_t reloadStartedUs = startedUs ? now_us() : 0;
        if (structureChanged) {
            recycler_->reloadData();
            if (!ownsFocus)
                recycler_->setContentOffsetY(0, false);
        } else {
            for (auto* cell : visibleCells<brls::RecyclerCell>(recycler_))
                dataSource_->repaintCell(cell);
        }
        const uint64_t reloadDurationUs =
            reloadStartedUs ? now_us() - reloadStartedUs : 0;
        const bool empty = count == 0;
        if (empty) {
            if (query_.empty()) {
                if (section_ == CatalogSection::Ports && openGames_ &&
                    catalogHasVisibleEntries()) {
                    ensureEmptyState()->setContent(
                        tr("pipensx/catalog/empty_ports_title"),
                        tr("pipensx/catalog/empty_ports_body"),
                        tr("pipensx/catalog/empty_ports_action"),
                        [this] { openGames_(); });
                } else {
                    ensureEmptyState()->setContent(
                        tr("pipensx/catalog/empty_title"),
                        tr("pipensx/catalog/empty_body"),
                        tr("pipensx/catalog/empty_action"),
                        [this] { refreshCatalog(); });
                }
            } else {
                ensureEmptyState()->setContent(
                    tr("pipensx/catalog/no_match_title"),
                    tr("pipensx/catalog/no_match_body"),
                    tr("pipensx/catalog/no_match_action"), [this] {
                        query_.clear();
                        rebuildEntries();
                    });
            }
        }
        if (empty)
            ensureEmptyState()->setVisibility(brls::Visibility::VISIBLE);
        else if (emptyState_)
            emptyState_->setVisibility(brls::Visibility::GONE);
        recyclerHost_->setVisibility(empty ? brls::Visibility::GONE
                                           : brls::Visibility::VISIBLE);
        if (focusInRecycler && structureChanged)
            restoreFocus(focusHash, focusShelf);

        countText_ = query_.empty()
            ? tr("pipensx/catalog/count_releases", withThousands(count))
            : tr("pipensx/catalog/count_matches", withThousands(count));
        updateHeader();
        if (startedUs) {
            telemetry_log("ui", "catalog",
                          "event=filter duration_us=%llu",
                          static_cast<unsigned long long>(filterDurationUs));
            telemetry_log("ui", "catalog",
                          "event=sort duration_us=%llu",
                          static_cast<unsigned long long>(sortDurationUs));
            telemetry_log(
                "ui", "catalog", "event=presentation duration_us=%llu",
                static_cast<unsigned long long>(presentationDurationUs));
            telemetry_log(
                "ui", "catalog",
                "event=reload duration_us=%llu structural=%d",
                static_cast<unsigned long long>(reloadDurationUs),
                structureChanged ? 1 : 0);
            telemetry_log(
                "ui", "catalog",
                "event=rebuild duration_us=%llu entries=%zu visible=%zu reload=%d",
                static_cast<unsigned long long>(now_us() - startedUs),
                all.size(), count, structureChanged ? 1 : 0);
        }
    }

    // Sync the O2 header with the current state: search field mirrors the
    // query, the active sort/filter chip is highlighted, counter shows the
    // visible entry count.
    void updateHeader() {
        // The magnifier lights up (primary style, white icon) while a query
        // is active; the Clear chip is the visible reminder + escape hatch.
        searchField_->setIconColor(query_.empty() ? theme::textPrimary()
                                                  : theme::onAccent());
        styleChip(searchField_, !query_.empty());
        clearSearch_->setVisibility(
            filtersDirty() ? brls::Visibility::VISIBLE : brls::Visibility::GONE);
        const bool descending = naturalDescending(sort_) != sortReversed_;
        sortButton_->setText(sortLabel(sort_) + (descending ? " ↓" : " ↑"));
        styleChip(sortButton_, true);
        styleChip(filterFavorites_, favoritesOnly_);
        styleChip(filterFits_, fitsOnly_);
        filterPlayers_->setText(playerFilter_ == PlayerFilter::Any
            ? tr("pipensx/catalog/filter_players")
            : ui::playerFilterLabel(playerFilter_));
        styleChip(filterPlayers_, playerFilter_ != PlayerFilter::Any);
        filterGenres_->setText(genreFilterLabel());
        styleChip(filterGenres_, !genreFilters_.empty());
        count_->setText(countText_);
        updateFreshnessLabel();
    }

    // Header freshness badge, left of the release count. Bundled dumps do
    // not count: only a successful network refresh stamps
    // lastCatalogRefreshWallSec. Green = refreshed today, red = never / not
    // today, orange = in flight. The Kind decision lives in
    // resolveCatalogFreshness (unit-tested); this only renders it.
    void updateFreshnessLabel() {
        freshnessBadge_->update(busy_, catalogRefreshInFlight(), settings_,
                                catalog_);
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
        // Chips must never be squeezed below their text width: the default
        // button side padding (25px) plus yoga shrinking clipped everything
        // after "Popular". Keep intrinsic width, compact the padding, and
        // let the search field absorb the shortage instead (it shrinks).
        chip->setShrink(0.0f);
        chip->setPaddingLeft(14);
        chip->setPaddingRight(14);
        chip->setText(text);
        chip->registerAction("", brls::BUTTON_A,
            [onClick = std::move(onClick)](brls::View*) {
                onClick();
                return true;
            },
            /*hidden=*/true);
        chip->addGestureRecognizer(new brls::TapGestureRecognizer(chip));
        return chip;
    }

    // Picking the sort that is already active flips its direction — a second
    // press on Size is how you ask for smallest-first. Moving to another sort
    // always starts from that sort's natural order rather than carrying the
    // flip across, which would silently re-order a list the user did not ask
    // to reverse.
    void setSort(SortMode mode) {
        if (busy_)
            return;
        if (sort_ == mode) {
            sortReversed_ = !sortReversed_;
        } else {
            sort_ = mode;
            sortReversed_ = false;
        }
        rebuildEntries();
    }

    std::string sortLabel(SortMode mode) const {
        switch (mode) {
            case SortMode::Popular: return tr("pipensx/catalog/sort_popular");
            case SortMode::Alphabetical: return tr("pipensx/catalog/sort_alpha");
            case SortMode::Largest: return tr("pipensx/catalog/sort_size");
            case SortMode::Latest:
            default: return tr("pipensx/catalog/sort_latest");
        }
    }

    bool filtersDirty() const {
        return !query_.empty() || favoritesOnly_ || fitsOnly_ ||
               playerFilter_ != PlayerFilter::Any || !genreFilters_.empty();
    }

    void resetFilters() {
        query_.clear();
        favoritesOnly_ = false;
        fitsOnly_ = false;
        playerFilter_ = PlayerFilter::Any;
        genreFilters_.clear();
        rebuildEntries();
    }

    void openSortSheet() {
        if (busy_)
            return;
        const std::vector<SortMode> modes = {
            SortMode::Latest, SortMode::Popular, SortMode::Alphabetical,
            SortMode::Largest};
        std::vector<std::string> labels;
        int selected = 0;
        for (size_t i = 0; i < modes.size(); ++i) {
            labels.push_back(sortLabel(modes[i]));
            if (sort_ == modes[i])
                selected = static_cast<int>(i);
        }
        auto* dropdown = new brls::Dropdown(
            tr("pipensx/catalog/sort_title"), labels, [](int) {}, selected,
            [this, modes](int index) {
                if (index < 0 || index >= static_cast<int>(modes.size()))
                    return;
                setSort(modes[static_cast<size_t>(index)]);
            });
        brls::Application::pushActivity(new brls::Activity(dropdown));
    }

    bool entryMatchesGenres(const CatalogEntry& entry,
                            const GameMetadata* meta) const {
        if (genreFilters_.empty())
            return true;
        if (meta) {
            for (const std::string& category : meta->categories) {
                if (genreFilters_.count(category))
                    return true;
            }
        }
        return !entry.genre.empty() && genreFilters_.count(entry.genre);
    }

    std::vector<std::string> availableGenres() const {
        std::unordered_set<std::string> names;
        if (!catalog_)
            return {};
        for (const CatalogEntry& entry : catalog_->entries()) {
            if (entry.isHiddenByDefault())
                continue;
            const GameMetadata* meta =
                metadata_ ? metadata_->findByInfoHash(entry.infoHash) : nullptr;
            if (!catalogEntryInSection(entry, meta, section_))
                continue;
            if (meta) {
                for (const std::string& category : meta->categories)
                    if (!category.empty())
                        names.insert(category);
            } else if (!entry.genre.empty()) {
                names.insert(entry.genre);
            }
        }
        std::vector<std::string> out(names.begin(), names.end());
        std::sort(out.begin(), out.end());
        return out;
    }

    std::string genreFilterLabel() const {
        if (genreFilters_.empty())
            return tr("pipensx/catalog/filter_genres");
        if (genreFilters_.size() == 1)
            return *genreFilters_.begin();
        return tr("pipensx/catalog/filter_genres") + " · " +
               std::to_string(genreFilters_.size());
    }

    // Virtualized genre rows: the catalog holds ~2k distinct genre combos
    // and instantiating a cell per genre overflows the 128KB deko3d command
    // buffer mid-frame (atmosphere User Break in renderFlush). RadioCell is
    // display-only (no self-toggle, no event subscriptions), so recycled
    // cells are safe to rebind; A-presses arrive via didSelectRowAt.
    class GenreSheetDataSource : public brls::RecyclerDataSource {
    public:
        explicit GenreSheetDataSource(CatalogView* owner) : owner_(owner) {}

        int numberOfRows(brls::RecyclerFrame*, int) override {
            return static_cast<int>(owner_->genreSheetGenres_.size()) + 1;
        }

        brls::RecyclerCell* cellForRow(brls::RecyclerFrame* recycler,
                                       brls::IndexPath index) override {
            auto* cell = static_cast<brls::RadioCell*>(
                recycler->dequeueReusableCell("GenreRow"));
            bindRow(cell, index.row);
            return cell;
        }

        void didSelectRowAt(brls::RecyclerFrame*,
                            brls::IndexPath index) override {
            owner_->toggleGenreRow(index.row);
        }

        void repaintCell(brls::RecyclerCell* cell) {
            if (!cell)
                return;
            bindRow(static_cast<brls::RadioCell*>(cell),
                    cell->getIndexPath().row);
        }

    private:
        void bindRow(brls::RadioCell* cell, int row) {
            if (row <= 0) {
                cell->title->setText(tr("pipensx/catalog/genres_all"));
                cell->setSelected(owner_->genreFilters_.empty());
                return;
            }
            const size_t i = static_cast<size_t>(row) - 1;
            if (i >= owner_->genreSheetGenres_.size())
                return;
            const std::string& name = owner_->genreSheetGenres_[i];
            cell->title->setText(name);
            cell->setSelected(owner_->genreFilters_.count(name) != 0);
        }

        CatalogView* owner_;
    };

    void toggleGenreRow(int row) {
        if (!genreSheet_)
            return;
        if (row <= 0) {
            genreFilters_.clear();
        } else {
            const size_t i = static_cast<size_t>(row) - 1;
            if (i >= genreSheetGenres_.size())
                return;
            const std::string name = genreSheetGenres_[i];
            if (genreFilters_.count(name))
                genreFilters_.erase(name);
            else
                genreFilters_.insert(name);
        }
        rebuildEntries();
        syncGenreSheet();
    }

    // In-place rebind: reloadData() would recycle the focused row out from
    // under the d-pad, so only the visible checkmarks are repainted.
    void syncGenreSheet() {
        if (!genreRecycler_ || !genreDataSource_)
            return;
        for (auto* cell : visibleCells<brls::RadioCell>(genreRecycler_))
            genreDataSource_->repaintCell(cell);
    }

    void closeGenreSheet() {
        if (!genreSheet_)
            return;
        removeView(genreSheet_);
        genreSheet_ = nullptr;
        genreRecycler_ = nullptr;
        // Owned by the recycler (setDataSource(..., true)): removeView freed
        // both, so deleting here would be a double free.
        genreDataSource_ = nullptr;
        genreSheetGenres_.clear();
        if (filterGenres_)
            brls::Application::giveFocus(filterGenres_);
    }

    void openGenreSheet() {
        if (busy_ || genreSheet_)
            return;
        genreSheet_ = new brls::Box();
        genreSheet_->setPositionType(brls::PositionType::ABSOLUTE);
        genreSheet_->setPositionTop(0);
        genreSheet_->setPositionLeft(0);
        genreSheet_->setWidthPercentage(100);
        genreSheet_->setHeightPercentage(100);
        genreSheet_->setAlignItems(brls::AlignItems::CENTER);
        genreSheet_->setJustifyContent(brls::JustifyContent::CENTER);
        genreSheet_->registerAction(tr("pipensx/common/close"), brls::BUTTON_B,
            [this](brls::View*) {
                closeGenreSheet();
                return true;
            });

        auto* backdrop = new brls::Box();
        backdrop->setPositionType(brls::PositionType::ABSOLUTE);
        backdrop->setPositionTop(0);
        backdrop->setPositionLeft(0);
        backdrop->setWidthPercentage(100);
        backdrop->setHeightPercentage(100);
        backdrop->setBackgroundColor(theme::overlay());
        backdrop->setFocusable(false);
        backdrop->addGestureRecognizer(new brls::TapGestureRecognizer(
            [this](brls::TapGestureStatus status, brls::Sound*) {
                if (status.state == brls::GestureState::END)
                    closeGenreSheet();
            }));
        genreSheet_->addView(backdrop);

        auto* panel = new brls::Box(brls::Axis::COLUMN);
        panel->setWidth(520);
        panel->setHeight(560);
        panel->setBackgroundColor(theme::panel());
        panel->setCornerRadius(theme::kRadiusLarge);
        panel->setPadding(16, 16, 16, 16);
        auto* title = new brls::Label();
        title->setText(tr("pipensx/catalog/genres_title"));
        title->setFontSize(theme::kFontBody);
        title->setMarginBottom(12);
        panel->addView(title);
        genreSheetGenres_ = availableGenres();
        auto* recycler = new brls::RecyclerFrame();
        recycler->setGrow(1);
        recycler->estimatedRowHeight =
            brls::Application::getStyle()["brls/dropdown/listItemHeight"];
        recycler->registerCell("GenreRow",
                               [] { return new brls::RadioCell(); });
        genreDataSource_ = new GenreSheetDataSource(this);
        // Explicit single ownership: the recycler deletes the source on
        // teardown (this matches CatalogDataSource above).
        recycler->setDataSource(genreDataSource_, true);
        panel->addView(recycler);
        genreRecycler_ = recycler;
        genreSheet_->addView(panel);
        addView(genreSheet_);
        genreRecycler_->reloadData();
        genreRecycler_->selectRowAt(brls::IndexPath(0, 0), false);
        brls::Application::giveFocus(genreRecycler_);
    }

    bool playerFilterUsable() const {
        return section_ != CatalogSection::Ports && metadata_ &&
               metadata_->hasPlayerData();
    }

    // Which way each sort runs when it has not been flipped: newest, most
    // seeded and largest first, but A before Z.
    static bool naturalDescending(SortMode mode) {
        return mode != SortMode::Alphabetical;
    }

    // Reverse the sorted vector instead of flipping four comparators, so the
    // sort branches stay the single description of what each sort means. Ties
    // reverse along with everything else, which is what "reversed" should look
    // like anyway.
    template <typename T>
    void applySortDirection(std::vector<T>& order) const {
        if (sortReversed_)
            std::reverse(order.begin(), order.end());
    }

    // The menu lists Any plus every mode the loaded index actually has data
    // for, so it grows on its own as metadata releases add modes — no new app
    // build needed. Local co-op is also offered when the index only carries
    // titledb player counts (the fallback in catalogEntryMatchesPlayerFilter).
    std::vector<PlayerFilter> playerFilterChoices() const {
        std::vector<PlayerFilter> choices = {PlayerFilter::Any};
        if (!metadata_)
            return choices;
        const uint8_t available = metadata_->availablePlayerModes();
        for (const ui::PlayerModeOption& option : ui::playerModeOptions()) {
            const bool byCount = option.filter == PlayerFilter::LocalCoop &&
                metadata_->hasPlayerData();
            if ((available & option.bit) || byCount)
                choices.push_back(option.filter);
        }
        return choices;
    }

    void updatePlayerChipVisibility() {
        if (!filterPlayers_)
            return;
        const bool usable = section_ != CatalogSection::Ports && metadata_ &&
                            metadata_->hasPlayerData();
        filterPlayers_->setVisibility(usable ? brls::Visibility::VISIBLE
                                             : brls::Visibility::GONE);
        if (!usable && playerFilter_ != PlayerFilter::Any) {
            playerFilter_ = PlayerFilter::Any;
            rebuildEntries();
        }
    }

    void openPlayerFilterMenu() {
        if (busy_)
            return;
        const std::vector<PlayerFilter> choices = playerFilterChoices();
        std::vector<std::string> labels;
        labels.reserve(choices.size());
        int selected = 0;
        for (size_t i = 0; i < choices.size(); ++i) {
            labels.push_back(ui::playerFilterLabel(choices[i]));
            if (choices[i] == playerFilter_)
                selected = static_cast<int>(i);
        }
        // Apply on dismiss, not select: didSelectRowAt fires while the
        // dropdown is still on the activity stack, and rebuildEntries()
        // refuses to recycle cells under an overlay (focusStack UAF).
        auto* dropdown = new brls::Dropdown(
            tr("pipensx/catalog/players_title"), labels, [](int) {}, selected,
            [this, choices](int index) {
                if (index < 0 || index >= static_cast<int>(choices.size()))
                    return;
                setPlayerFilter(choices[static_cast<size_t>(index)]);
            });
        brls::Application::pushActivity(new brls::Activity(dropdown));
    }

    void setPlayerFilter(PlayerFilter filter) {
        if (busy_ || playerFilter_ == filter)
            return;
        playerFilter_ = filter;
        rebuildEntries();
    }

    bool catalogHasVisibleEntries() const {
        if (!catalog_)
            return false;
        for (const CatalogEntry& entry : catalog_->entries()) {
            if (!entry.isHiddenByDefault())
                return true;
        }
        return false;
    }

    // Re-focus the card that was focused before reloadData recycled all
    // cells: same shelf if the focus lived on a shelf, otherwise the grid
    // card with the same info-hash (or the closest fallback). selectRowAt()
    // scrolls the row in and marks it as the content box's last-focused
    // child, so giving focus to the recycler lands on the row's
    // getDefaultFocus(), which honors focusColumn_.
    void restoreFocus(const std::string& hash, int) {
        if (dataSource_->entryCount() == 0) {
            brls::Application::giveFocus(ensureEmptyState());
            return;
        }
        int index = 0;
        for (size_t i = 0; i < dataSource_->entryCount(); ++i) {
            const CatalogEntry* entry = dataSource_->entryAt(static_cast<int>(i));
            if (entry && entry->infoHash == hash) {
                index = static_cast<int>(i);
                break;
            }
        }
        *focusColumn_ = dataSource_->columnForEntry(index);
        recycler_->selectRowAt(
            brls::IndexPath(0, dataSource_->rowForEntry(index)), false);
        brls::Application::giveFocus(recycler_);
    }

    // O12: the game page closed (B). Re-seat scroll + focus on the card that
    // opened it — even if live rebuilds recycled the cells or reset the
    // scroll while the page was on top. Deferred to the frame after the pop
    // (GameDetailActivity destructor -> brls::sync), so this runs after
    // popActivity() handed the focus to whatever the focus stack still held.
    void onDetailClosed() {
        if (returnFocusHash_.empty())
            return;
        const std::string hash = returnFocusHash_;
        const int shelfRow = returnFocusShelf_;
        returnFocusHash_.clear();
        returnFocusShelf_ = -1;
        restoreFocus(hash, shelfRow);
    }

    // Y searches while idle and cancels during a refresh. The search half is
    // hidden to keep the bottom bar within its width (see the registration
    // block in the constructor), but the cancel half has to be visible or a
    // refresh looks unstoppable — and Action::hidden is fixed at construction,
    // so the swap re-registers. registerAction replaces the entry for a button
    // it already holds, which is what makes this safe to call repeatedly.
    void registerYAction(bool busy) {
        registerAction(busy ? tr("pipensx/common/stop")
                            : tr("pipensx/common/search"),
                       brls::BUTTON_Y, [this](brls::View*) {
            if (busy_)
                cancelled_->store(true);
            else
                openSearchKeyboard();
            return true;
        }, /*hidden=*/false);
    }

    void setBusy(bool busy) {
        busy_ = busy;
        registerYAction(busy);
        freshnessBadge_->setBusy(busy);
        updateFreshnessLabel();
        // Neither registerAction nor updateActionHint fires this, so without it
        // the bar keeps the stale hint until some unrelated focus change
        // refills it.
        brls::Application::getGlobalHintsUpdateEvent()->fire();
    }

    static void hashTasks(const std::vector<DownloadTask>& tasks,
                          uint64_t& ids, uint64_t& full, bool& anyInstalled) {
        ids = full = 1469598103934665603ULL;
        anyInstalled = false;
        for (const DownloadTask& task : tasks) {
            for (unsigned char c : task.id) {
                ids = (ids ^ c) * 1099511628211ULL;
                full = (full ^ c) * 1099511628211ULL;
            }
            full = (full ^ static_cast<uint64_t>(task.status)) *
                   1099511628211ULL;
            anyInstalled = anyInstalled ||
                           task.status == DownloadStatus::Installed;
        }
    }

    void patchTaskBadges(const std::vector<DownloadTask>& tasks) {
        std::unordered_map<std::string, DownloadStatus> added;
        for (const DownloadTask& task : tasks)
            added[lowerAscii(task.id)] = task.status;
        const size_t count = dataSource_->entryCount();
        std::vector<std::string> badges;
        badges.reserve(count);
        const std::unordered_set<std::string> installedIds =
            installed_ ? installed_->titleIds()
                       : std::unordered_set<std::string>();
        for (size_t i = 0; i < count; ++i) {
            const CatalogEntry* entry = dataSource_->entryAt(static_cast<int>(i));
            if (!entry) {
                badges.emplace_back();
                continue;
            }
            auto it = added.find(lowerAscii(entry->infoHash));
            if (it != added.end()) {
                badges.push_back(badgeForStatus(it->second));
                continue;
            }
            badges.emplace_back();
            const GameMetadata* meta =
                metadata_ ? metadata_->findByInfoHash(entry->infoHash) : nullptr;
            CatalogRowPresentation row = resolveCatalogRow(*entry, meta);
            if (!row.titleId.empty() &&
                installedIds.count(upperAscii(row.titleId)))
                badges.back() = tr("pipensx/catalog/badge_installed");
        }
        dataSource_->updateStateBadges(std::move(badges));
        std::function<void(brls::View*)> walk = [&](brls::View* view) {
            if (!view)
                return;
            if (auto* card = dynamic_cast<GameCard*>(view)) {
                if (card->entryIndex() >= 0) {
                    GridCardInfo info = dataSource_->cardInfo(card->entryIndex());
                    card->setSubLine(info.sub, info.subIsBadge);
                }
                return;
            }
            if (auto* box = dynamic_cast<brls::Box*>(view)) {
                for (brls::View* child : box->getChildren())
                    walk(child);
            }
        };
        walk(recycler_);
    }

    void refreshLiveState() {
        scheduleStorageRefresh();
        if (pendingRebuild_ && !activityStackHasOverlay()) {
            rebuildEntries();
            return;
        }
        const bool inFlight = catalogRefreshInFlight();
        if (inFlight && !busy_)
            setBusy(true);
        else if (!inFlight && busy_)
            setBusy(false);
        if (catalog_ && catalog_->sharedEntries() != observedCatalog_) {
            rebuildEntries();
            return;
        }
        if (metadata_ &&
            metadata_->generation() != observedMetadataGeneration_) {
            rebuildEntries();
            return;
        }
        if (busy_)
            return;
        bool settingsChanged = false;
        if (settings_ && settings_->generation() !=
                             observedSettingsGeneration_) {
            observedSettingsGeneration_ = settings_->generation();
            settingsChanged = true;
        }
        const auto tasks = manager_->snapshotUi();
        uint64_t ids = 0;
        uint64_t signature = 0;
        bool installedFinished = false;
        hashTasks(tasks, ids, signature, installedFinished);
        if (signature != taskSignature_) {
            const bool idsChanged = ids != taskIdSignature_;
            taskSignature_ = signature;
            taskIdSignature_ = ids;
            if (installedFinished && installed_ &&
                installedRefreshSignature_ != signature) {
                installedRefreshSignature_ = signature;
                refreshInstalledAsync();
            }
            if (settingsChanged || idsChanged)
                rebuildEntries();
            else
                patchTaskBadges(tasks);
            return;
        }
        if (settingsChanged)
            rebuildEntries();
    }

    void scheduleStorageRefresh() {
        const uint64_t now = now_ms();
        if (storageRefreshInFlight_ ||
            (lastStorageRefreshMs_ &&
             now - lastStorageRefreshMs_ < kStorageRefreshIntervalMs))
            return;
        storageRefreshInFlight_ = true;
        lastStorageRefreshMs_ = now;
        auto alive = alive_;
        const std::string root = manager_->rootPath();
        brls::async([this, alive, root] {
            const uint64_t startedUs =
                telemetry_enabled() ? now_us() : 0;
            const StorageSpaceSnapshot storage =
                pipensx::queryStorageSpace(root);
            if (startedUs) {
                telemetry_log(
                    "ui", "catalog", "event=storage duration_us=%llu",
                    static_cast<unsigned long long>(now_us() - startedUs));
            }
            brls::sync([this, alive, storage] {
                if (!alive->load())
                    return;
                storageRefreshInFlight_ = false;
                storage_ = storage;
                if (fitsOnly_)
                    rebuildEntries();
            });
        });
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
            case DownloadStatus::Queued:
                return tr("pipensx/detail/status_queued");
            case DownloadStatus::Fetching:
                return tr("pipensx/downloads/status_fetching");
            case DownloadStatus::Checking:
            case DownloadStatus::Downloading:
            case DownloadStatus::Verifying:
                return tr("pipensx/downloads/status_downloading");
            case DownloadStatus::Paused:
                return tr("pipensx/downloads/status_paused");
            case DownloadStatus::Installing:
            case DownloadStatus::Committing:
                return tr("pipensx/downloads/status_installing");
            case DownloadStatus::Completed:
                return tr("pipensx/downloads/status_completed");
            case DownloadStatus::Installed:
                return tr("pipensx/downloads/status_installed");
            case DownloadStatus::Error:
                return tr("pipensx/downloads/status_error");
            case DownloadStatus::Removing:
                return tr("pipensx/downloads/status_removing");
        }
        return "";
    }

    void refreshCatalog() {
        refreshSources(true, metadata_ != nullptr, true);
    }

    void refreshCatalogIfDue() {
        // Bundled dumps do not count. Auto-refresh when this console has never
        // pulled the catalogue, or the last pull was not today.
        const uint64_t wallSec =
            settings_ ? settings_->get().lastCatalogRefreshWallSec : 0;
        const bool catalogDue =
            wallSec == 0 || !isLocalToday(static_cast<int64_t>(wallSec));
        const bool metadataDue = metadata_ && settings_ &&
            dailyRefreshDue(now_ms(), settings_->get().lastMetadataRefreshMs);
        refreshSources(catalogDue, catalogDue || metadataDue, false);
    }

    void notifyRefreshResult(bool fetchCatalog, bool fetchMetadata,
                             bool catalogOk, bool metadataOk,
                             const std::string& catalogError,
                             const std::string& metadataError) {
        const std::string catalogMsg = formatCatalogRefreshError(catalogError);
        const std::string metadataMsg =
            formatCatalogRefreshError(metadataError);
        if (fetchCatalog && fetchMetadata) {
            if (catalogOk && metadataOk) {
                brls::Application::notify(
                    tr("pipensx/catalog/updated_both",
                       catalog_->entries().size()));
            } else if (catalogOk) {
                brls::Application::notify(
                    tr("pipensx/catalog/updated_catalog_artwork_failed",
                       metadataMsg));
            } else if (metadataOk) {
                brls::Application::notify(
                    tr("pipensx/catalog/updated_artwork_catalog_failed",
                       catalogMsg));
            } else {
                brls::Application::notify(
                    tr("pipensx/catalog/updated_both_failed", catalogMsg,
                       metadataMsg));
            }
        } else if (fetchCatalog) {
            brls::Application::notify(catalogOk
                ? tr("pipensx/catalog/updated_catalog",
                     catalog_->entries().size())
                : catalogMsg);
        } else if (fetchMetadata) {
            brls::Application::notify(metadataOk
                ? tr("pipensx/catalog/updated_artwork", metadata_->size())
                : metadataMsg);
        }
    }

    void refreshSources(bool fetchCatalog, bool fetchMetadata, bool notify) {
        fetchMetadata = fetchMetadata && metadata_;
        if (!fetchCatalog && !fetchMetadata)
            return;
        if (busy_)
            return;
        if (!tryBeginCatalogRefresh()) {
            // Another tab/settings already owns the fetch. Follow it so the
            // badge stays orange instead of flipping to red "never/stale".
            setBusy(true);
            return;
        }
        setBusy(true);
        const std::string updating = fetchCatalog && fetchMetadata
            ? tr("pipensx/catalog/updating_both")
            : fetchCatalog ? tr("pipensx/catalog/updating_catalog")
                           : tr("pipensx/catalog/updating_artwork");
        if (notify)
            brls::Application::notify(updating);
        auto alive = alive_;
        CatalogService* catalog = catalog_;
        GameMetadataService* metadata = metadata_;
        const std::string catalogSourceUrl =
            effectiveCatalogSourceUrl(settings_->get().catalogSourceUrl);
        AppSettings* settings = settings_;
        uint64_t startedMs = now_ms();
        brls::async([this, alive, catalog, metadata, settings, startedMs,
                     fetchCatalog, fetchMetadata, notify, catalogSourceUrl] {
            CatalogRefreshBatch batch;
            std::thread metadataFetch;
            if (fetchMetadata) {
                metadataFetch = std::thread([&] {
                    batch.metadataOk = metadata->fetchLatest(
                        batch.metadata, batch.metadataError);
                });
            }
            if (fetchCatalog) {
                batch.catalogOk = catalog->fetchLatest(
                    batch.catalogEntries, batch.catalogError, catalogSourceUrl);
            }
            if (metadataFetch.joinable())
                metadataFetch.join();
            if (fetchCatalog) {
                telemetry_log("catalog", "-",
                              "event=refresh ok=%d duration_ms=%llu entries=%zu",
                              batch.catalogOk ? 1 : 0,
                              (unsigned long long)(now_ms() - startedMs),
                              batch.catalogEntries.size());
            }
            if (fetchMetadata) {
                telemetry_log("metadata", "-",
                              "event=refresh ok=%d duration_ms=%llu entries=%zu",
                              batch.metadataOk ? 1 : 0,
                              (unsigned long long)(now_ms() - startedMs),
                              batch.metadata.items.size());
            }
            brls::sync([this, alive, batch = std::move(batch), fetchCatalog,
                        fetchMetadata, notify, catalogSourceUrl, catalog,
                        metadata, settings]() mutable {
                const bool catalogOk = batch.catalogOk;
                const bool metadataOk = batch.metadataOk;
                const std::string catalogError = batch.catalogError;
                const std::string metadataError = batch.metadataError;
                if (metadata) {
                    adoptCatalogRefresh(*catalog, *metadata, std::move(batch),
                                        catalogSourceUrl);
                } else if (catalogOk) {
                    catalog->adopt(std::move(batch.catalogEntries),
                                   catalogSourceUrl);
                }
                std::string stampError;
                if (!recordCatalogRefreshSuccess(
                        settings, fetchCatalog && catalogOk,
                        fetchMetadata && metadataOk, stampError) &&
                    !stampError.empty()) {
                    diagnostic_error("settings", "refresh_time", "error=%s",
                                     stampError.c_str());
                }
                endCatalogRefresh();
                if (!alive->load())
                    return;
                setBusy(false);
                if (fetchCatalog && !catalogOk) {
                    diagnostic_error("catalog", "refresh", "error=%s",
                                     catalogError.c_str());
                }
                if (fetchMetadata && !metadataOk) {
                    diagnostic_error("metadata", "refresh", "error=%s",
                                     metadataError.c_str());
                }
                // A metadata release can be the first one to carry player
                // data, so the chip appears (or its menu grows) right here,
                // without a new app build.
                if (metadataOk)
                    updatePlayerChipVisibility();
                if (catalogOk || metadataOk)
                    rebuildEntries();
                else
                    updateFreshnessLabel();
                observedSettingsGeneration_ =
                    settings_ ? settings_->generation() : 0;
                if (fetchCatalog && catalogOk)
                    updateFreshnessLabel();
                if (metadataOk && onSourcesRefreshed_)
                    onSourcesRefreshed_();
                if (notify)
                    notifyRefreshResult(fetchCatalog, fetchMetadata, catalogOk,
                                        metadataOk, catalogError,
                                        metadataError);
            });
        });
    }

    DownloadManager* manager_;
    CatalogService* catalog_;
    GameMetadataService* metadata_;
    InstalledTitleService* installed_;
    AppSettings* settings_;
    FavoritesService* favorites_;
    SwitchDeployService* deploy_;
    std::function<void()> openDownloads_;
    std::function<void()> openGames_;
    std::function<void()> onSourcesRefreshed_;
    CatalogSection section_ = CatalogSection::Games;
    brls::RecyclerFrame* recycler_;
    brls::Box* recyclerHost_ = nullptr;
    CatalogDataSource* dataSource_;
    brls::Box* header_ = nullptr;
    CatalogFreshnessBadge* freshnessBadge_ = nullptr;
    SearchIconButton* searchField_ = nullptr;
    brls::Button* clearSearch_ = nullptr;
    brls::Button* sortButton_ = nullptr;
    brls::Button* filterFavorites_ = nullptr;
    brls::Button* filterFits_ = nullptr;
    brls::Button* filterPlayers_ = nullptr;
    brls::Button* filterGenres_ = nullptr;
    brls::Label* count_ = nullptr;
    brls::Box* genreSheet_ = nullptr;
    brls::RecyclerFrame* genreRecycler_ = nullptr;
    GenreSheetDataSource* genreDataSource_ = nullptr;
    std::vector<std::string> genreSheetGenres_;
    EmptyStateView* emptyState_ = nullptr;
    std::shared_ptr<std::atomic<bool>> alive_;
    std::shared_ptr<std::atomic<bool>> cancelled_;
    std::shared_ptr<int> focusColumn_ = std::make_shared<int>(0);
    // O12: where to put the focus back when the game page closes.
    std::string returnFocusHash_;
    int returnFocusShelf_ = -1;
    std::unordered_map<std::string, std::string> catalogFailures_;
    std::unordered_set<std::string> selectedHashes_;
    std::string query_;
    std::string countText_;
    SortMode sort_ = SortMode::Popular;
    // Flips the active sort's natural direction (see naturalDescending).
    // Session-only, like the view filters: a relaunch comes back to the
    // catalog's own idea of order.
    bool sortReversed_ = false;
    bool busy_ = false;
    bool pendingRebuild_ = false;
    // Session-only view filters: deliberately not persisted, so a relaunch
    // always comes back to the full catalog.
    bool favoritesOnly_ = false;
    bool fitsOnly_ = false;
    PlayerFilter playerFilter_ = PlayerFilter::Any;
    std::unordered_set<std::string> genreFilters_;
    brls::RepeatingTimer timer_;
    uint64_t observedSettingsGeneration_ = 0;
    std::shared_ptr<const std::vector<CatalogEntry>> observedCatalog_;
    uint64_t observedMetadataGeneration_ = 0;
    uint64_t taskSignature_ = 0;
    uint64_t taskIdSignature_ = 0;
    uint64_t installedRefreshSignature_ = 0;
    bool installedRefreshInFlight_ = false;
    static constexpr uint64_t kStorageRefreshIntervalMs = 2000;
    StorageSpaceSnapshot storage_;
    uint64_t lastStorageRefreshMs_ = 0;
    bool storageRefreshInFlight_ = false;
};

}  // namespace pipensx::ui
