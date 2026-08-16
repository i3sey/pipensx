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
#include "ui/catalog/batch_install.hpp"
#include "ui/catalog/catalog_grid.hpp"
#include "ui/catalog/catalog_helpers.hpp"
#include "ui/i18n.hpp"
#include "ui/common/busy_pulse.hpp"
#include "ui/common/message_cells.hpp"
#include "ui/common/ui_helpers.hpp"
#include "ui/detail/game_detail.hpp"
#include "ui/theme.hpp"

namespace pipensx::ui {

class CatalogView;

// One horizontal shelf above the grid (UI_PLAN F5): title, entry indices,
// optional "See all" action (null = no link).
struct CatalogShelf {
    std::string title;
    std::vector<int> items;
    std::function<void()> seeAll;
};

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
                    GameMetadataService* metadata,
                    bool selectionMode) {
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
        selectionMode_ = selectionMode;
    }
    void updateStateBadges(std::vector<std::string> stateBadges) {
        stateBadges_ = std::move(stateBadges);
    }
    GridCardInfo cardInfo(int index) const { return makeInfo(index); }
    // Shelf contents (UI_PLAN F2/F5): indices into the visible list.
    // heroIndex < 0 = no hero banner; an empty shelf list = plain grid.
    void setShelves(std::vector<CatalogShelf> shelves, int heroIndex,
                    std::string heroImageUrl) {
        shelves_ = std::move(shelves);
        heroIndex_ = heroIndex;
        heroImage_ = std::move(heroImageUrl);
    }
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

    // Recycler row layout: [top inset][hero?][shelves][grid rows of cards].
    int headerRowCount() const {
        return 1 + (heroIndex_ >= 0 ? 1 : 0) +
               static_cast<int>(shelves_.size());
    }
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
        if (heroIndex_ >= 0 && index.row == 1)
            return grid::kHeroHeight;
        return index.row < headerRowCount() ? grid::kShelfHeight
                                            : grid::kRowHeight;
    }
    brls::RecyclerCell* cellForRow(brls::RecyclerFrame* recycler,
                                    brls::IndexPath index) override;
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
            metadata_->prefetchImage(iconUrls_[static_cast<size_t>(i)]);
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
    std::vector<CatalogShelf> shelves_;
    int heroIndex_ = -1;
    std::string heroImage_;
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
    NVGcolor iconColor_ = nvgRGB(255, 255, 255);
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
          alive_(std::make_shared<std::atomic<bool>>(true)),
          cancelled_(std::make_shared<std::atomic<bool>>(false)) {
        recycler_ = new brls::RecyclerFrame();
        recycler_->setGrow(1);
        recycler_->setPadding(6, 32, 6, 32);
        recycler_->estimatedRowHeight = grid::kRowHeight;
        recycler_->registerCell("TopInset", [] { return new TopInsetCell(); });
        recycler_->registerCell("GridRow", [column = focusColumn_] {
            return new GridRowCell(column);
        });
        recycler_->registerCell("Shelf", [hash = shelfFocusHash_] {
            return new ShelfCell(hash);
        });
        recycler_->registerCell("Hero", [] { return new HeroCell(); });
        recycler_->registerCell("Message",
            [] { return new TextMessageCell(); });
        dataSource_ = new CatalogDataSource(this);
        recycler_->setDataSource(dataSource_);

        // Persistent catalog header (UI_PLAN O2): sort + filter chips, a
        // result counter and a compact magnifier button in the right corner.
        // X/Y hotkeys still work; the chips make the current state visible
        // and touch-reachable. Single row: with the search field collapsed
        // to a 40px icon everything fits at 1280 without clipping.
        //
        // Freshness lives ABOVE this row, top-right — never in the chip row,
        // where it shoved the magnifier off-screen once the sidebar folded.
        freshnessRow_ = new brls::Box(brls::Axis::ROW);
        freshnessRow_->setHeight(28);
        freshnessRow_->setShrink(0.0f);
        freshnessRow_->setMarginTop(6);
        freshnessRow_->setMarginBottom(0);
        freshnessRow_->setMarginLeft(34);
        freshnessRow_->setMarginRight(34);
        freshnessRow_->setJustifyContent(brls::JustifyContent::FLEX_END);
        freshnessRow_->setAlignItems(brls::AlignItems::CENTER);
        freshnessRow_->setFocusable(false);
        auto* freshnessSpacer = new brls::Box();
        freshnessSpacer->setGrow(1.0f);
        freshnessSpacer->setFocusable(false);
        freshnessRow_->addView(freshnessSpacer);
        // Pulsing dot while a refresh/batch resolve is in flight — text alone
        // reads as a frozen badge for multi-second waits (#19).
        busyDot_ = new brls::Label();
        busyDot_->setText("●");
        busyDot_->setFontSize(theme::kFontCaption);
        busyDot_->setTextColor(theme::warning());
        busyDot_->setMarginRight(6);
        busyDot_->setFocusable(false);
        busyDot_->setShrink(0.0f);
        busyDot_->setVisibility(brls::Visibility::GONE);
        freshnessRow_->addView(busyDot_);
        freshness_ = new brls::Label();
        freshness_->setFontSize(theme::kFontCaption);
        freshness_->setSingleLine(true);
        freshness_->setFocusable(false);
        freshness_->setShrink(0.0f);
        freshnessRow_->addView(freshness_);

        header_ = new brls::Box(brls::Axis::ROW);
        header_->setHeight(40);
        header_->setShrink(0.0f);
        header_->setMarginTop(6);
        header_->setMarginBottom(10);
        header_->setMarginLeft(34);
        header_->setMarginRight(34);
        sortLatest_ = makeChip(tr("pipensx/catalog/sort_latest"),
                               [this] { setSort(SortMode::Latest); });
        sortPopular_ = makeChip(tr("pipensx/catalog/sort_popular"),
                                [this] { setSort(SortMode::Popular); });
        sortAlpha_ = makeChip(tr("pipensx/catalog/sort_alpha"),
                              [this] { setSort(SortMode::Alphabetical); });
        sortSize_ = makeChip(tr("pipensx/catalog/sort_size"),
                             [this] { setSort(SortMode::Largest); });
        sortLatest_->setMarginLeft(0);
        header_->addView(sortLatest_);
        header_->addView(sortPopular_);
        header_->addView(sortAlpha_);
        header_->addView(sortSize_);
        // Session-only view filters. Sections live in the sidebar, so there
        // is no All/Games pair here.
        filterFavorites_ = makeChip("★", [this] {
            favoritesOnly_ = !favoritesOnly_;
            rebuildEntries();
        });
        filterFits_ = makeChip(tr("pipensx/catalog/filter_fits"), [this] {
            fitsOnly_ = !fitsOnly_;
            rebuildEntries();
        });
        filterPlayers_ = makeChip(tr("pipensx/catalog/filter_players"),
                                  [this] { openPlayerFilterMenu(); });
        filterFavorites_->setMarginLeft(16);
        header_->addView(filterFavorites_);
        header_->addView(filterFits_);
        header_->addView(filterPlayers_);
        if (!favorites_)
            filterFavorites_->setVisibility(brls::Visibility::GONE);
        // Hidden until a metadata index that carries player data is loaded, so
        // the chip never offers a menu that can only answer "nothing found".
        updatePlayerChipVisibility();
        auto* headerSpacer = new brls::Box();
        headerSpacer->setGrow(1.0f);
        headerSpacer->setShrink(1.0f);
        header_->addView(headerSpacer);
        count_ = new brls::Label();
        count_->setFontSize(theme::kFontCaption);
        count_->setTextColor(theme::textTertiary());
        count_->setMarginLeft(16);
        count_->setMarginTop(12);
        count_->setShrink(0.0f);
        header_->addView(count_);
        clearSearch_ = makeChip(tr("pipensx/common/clear"), [this] {
            if (query_.empty() && !shelfDrilldown_)
                return;
            query_.clear();
            shelfDrilldown_ = false;
            rebuildEntries();
        });
        clearSearch_->setMarginLeft(16);
        header_->addView(clearSearch_);
        searchField_ = new SearchIconButton();
        searchField_->setStyle(&brls::BUTTONSTYLE_DEFAULT);
        searchField_->setWidth(40);
        searchField_->setHeight(40);
        searchField_->setShrink(0.0f);
        searchField_->setMarginLeft(8);
        searchField_->setText("");
        searchField_->registerClickAction([this](brls::View*) {
            openSearchKeyboard();
            return true;
        });
        header_->addView(searchField_);

        // Batch-mode summary line; hidden while browsing.
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
        selectVisible->setText(tr("pipensx/catalog/select_visible"));
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
        clearSelection->setText(tr("pipensx/common/clear"));
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
        prepareBatch_->setText(tr("pipensx/catalog/prepare"));
        prepareBatch_->registerClickAction([this](brls::View*) {
            prepareSelectedEntries();
            return true;
        });
        batchControls_->addView(prepareBatch_);

        addView(freshnessRow_);
        addView(header_);
        addView(status_);
        addView(batchControls_);
        // Visibility toggles on the host, not the recycler: the host is the
        // grow(1) box, so hiding only the recycler would leave its slot behind.
        recyclerHost_ = recyclerHost(recycler_);
        addView(recyclerHost_);
        if (catalogRefreshInFlight())
            setBusy(true);
        rebuildEntries();
        updateFreshnessLabel();

        // X and Y carry no hint. This view stacks more gamepad actions than the
        // bottom bar can render: on hardware the bar also holds the frame's
        // Plus action plus the clock, battery and wireless widgets, and the
        // Russian labels then overrun the row and collide with the clock. Both
        // hotkeys keep working, and both are duplicated by focusable header
        // controls — the magnifier button for search, the four sort chips for
        // sort — so nothing here becomes unreachable.
        registerAction(tr("pipensx/common/search"), brls::BUTTON_X,
                       [this](brls::View*) {
            openSearchKeyboard();
            return true;
        }, /*hidden=*/true);
        registerSortAction(false);
        registerAction(tr("pipensx/common/refresh"), brls::BUTTON_RB,
                       [this](brls::View*) {
            if (batchMode_)
                prepareSelectedEntries();
            else
                refreshCatalog();
            return true;
        });
        registerAction(tr("pipensx/catalog/action_batch"), brls::BUTTON_LB,
                       [this](brls::View*) {
            toggleBatchMode();
            return true;
        });
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
        refreshCatalogIfDue();
    }

    ~CatalogView() override {
        alive_->store(false);
        cancelled_->store(true);
        timer_.stop();
        stopBusyPulse(busyDot_);
    }

    void openSearchKeyboard() {
        if (busy_)
            return;
        brls::Application::getImeManager()->openForText(
            [this](std::string text) {
                query_ = std::move(text);
                shelfDrilldown_ = false;
                rebuildEntries();
            },
            tr("pipensx/catalog/search_placeholder"), "", 256, query_,
            brls::KEYBOARD_DISABLE_NONE);
    }

    void onEntrySelected(int row) {
        const CatalogEntry* picked = dataSource_->entryAt(row);
        if (!picked || busy_)
            return;
        if (batchMode_) {
            if (!dataSource_->selectableAt(row)) {
                brls::Application::notify(tr("pipensx/catalog/already_in_downloads"));
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
        } else if (auto* hero = dynamic_cast<HeroCard*>(focus)) {
            if (hero->infoHash() == picked->infoHash)
                returnFocusShelf_ = 1;  // row 0 is the top inset
        }
        auto onClose = [this, alive = alive_] {
            if (alive->load())
                onDetailClosed();
        };
        brls::Application::pushActivity(new GameDetailActivity(
            std::move(entry), std::move(lastFailure), manager_, metadata_,
            installed_, settings_,
            std::move(onFailure), std::move(onChange), std::move(onClose),
            favorites_, deploy_));
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
        else if (auto* hero = dynamic_cast<HeroCard*>(focus))
            hash = hero->infoHash();
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

    void toggleBatchMode() {
        if (busy_)
            return;
        batchMode_ = !batchMode_;
        batchControls_->setVisibility(batchMode_ ? brls::Visibility::VISIBLE
                                                 : brls::Visibility::GONE);
        updateActionHint(brls::BUTTON_LB,
                         batchMode_ ? tr("pipensx/catalog/action_batch_close")
                                    : tr("pipensx/catalog/action_batch"));
        updateActionHint(brls::BUTTON_RB,
                         batchMode_ ? tr("pipensx/catalog/prepare")
                                    : tr("pipensx/common/refresh"));
        rebuildEntries();
    }

    void selectVisibleEntries() {
        if (!batchMode_)
            return;
        for (size_t row = 0; row < dataSource_->entryCount(); ++row) {
            if (dataSource_->selectableAt(static_cast<int>(row))) {
                const CatalogEntry* entry =
                    dataSource_->entryAt(static_cast<int>(row));
                if (entry)
                    selectedHashes_.insert(lowerAscii(entry->infoHash));
            }
        }
        rebuildEntries();
    }

    void prepareSelectedEntries() {
        if (!batchMode_ || selectedHashes_.empty() || busy_)
            return;
        if (debridModeActive(settings_) &&
            !ensureDebridLinked(settings_, manager_))
            return;

        std::unordered_set<std::string> managed;
        for (const DownloadTask& task : manager_->snapshotUi())
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
        } else if (sort_ == SortMode::Popular) {
            std::stable_sort(entries.begin(), entries.end(),
                [](const CatalogEntry& left, const CatalogEntry& right) {
                    if (left.peerCount != right.peerCount)
                        return left.peerCount > right.peerCount;
                    return left.publishedAt > right.publishedAt;
                });
        } else {
            std::stable_sort(entries.begin(), entries.end(),
                [](const CatalogEntry& left, const CatalogEntry& right) {
                    return left.publishedAt > right.publishedAt;
                });
        }
        applySortDirection(entries);
        for (const std::string& hash : managed)
            selectedHashes_.erase(hash);
        if (entries.empty()) {
            rebuildEntries();
            brls::Application::notify(tr("pipensx/catalog/select_one_game"));
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
            manager_, settings_, std::move(entries), selection,
            std::move(completion), openDownloads_));
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
        std::string text = tr("pipensx/catalog/batch_selected",
                              selectedHashes_.size(), formatBytes(bytes));
        if (unknown)
            text += tr("pipensx/catalog/batch_unknown", unknown);
        text += storage.available
            ? tr("pipensx/catalog/batch_sd_free",
                 formatBytes(storage.freeBytes))
            : tr("pipensx/catalog/batch_sd_unavailable");
        status_->setText(text);
        const bool available = !selectedHashes_.empty();
        prepareBatch_->setState(available ? brls::ButtonState::ENABLED
                                          : brls::ButtonState::DISABLED);
        prepareBatch_->setText(tr("pipensx/catalog/prepare_n",
                                  selectedHashes_.size()));
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
        // Same focusStack UAF as downloads: don't free cells while a dialog
        // (e.g. deploy offer) is on top of the activity stack.
        if (activityStackHasOverlay() && !focusInCatalog)
            return;

        std::string focusHash;
        int focusShelf = -1;
        if (focusInCatalog) {
            if (auto* card = dynamic_cast<GameCard*>(focus)) {
                focusHash = card->infoHash();
                focusShelf = card->shelfRow();
            } else if (auto* hero = dynamic_cast<HeroCard*>(focus)) {
                focusHash = hero->infoHash();
                focusShelf = 1;  // row 0 is the top inset
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
        std::string needle = lowerAscii(query_);
        const bool searching = !needle.empty();
        // One syscall per rebuild, and only when the filter is on. Rebuilds are
        // event-driven (refreshLiveState only rebuilds when something changed),
        // so this does not run on a timer.
        StorageSpaceSnapshot storage;
        if (fitsOnly_)
            storage = queryStorageSpace(manager_->rootPath());
        const bool favoritesOnly = favoritesOnly_ && favorites_;
        visible.reserve(all.size());
        for (size_t i = 0; i < all.size(); ++i) {
            const CatalogEntry& entry = all[i];
            if (entry.isHiddenByDefault())
                continue;
            if (favoritesOnly && !favorites_->contains(entry.infoHash))
                continue;
            if (fitsOnly_ && !catalogEntryFitsFreeSpace(entry.size, storage))
                continue;
            const GameMetadata* meta =
                metadata_ ? metadata_->findByInfoHash(entry.infoHash) : nullptr;
            if (!catalogEntryInSection(entry, meta, section_))
                continue;
            // Unlike the Games filter above, this one also narrows a search:
            // "which racing game can we play together" is exactly the question
            // it exists for.
            if (playerFilter_ != PlayerFilter::Any &&
                !catalogEntryMatchesPlayerFilter(meta, playerFilter_))
                continue;
            bool matches = !searching ||
                lowerAscii(entry.title).find(needle) != std::string::npos ||
                (meta && lowerAscii(meta->name).find(needle) !=
                             std::string::npos);
            // Genre shelves (F5) hand off to search, so categories match too.
            if (!matches && meta) {
                for (const std::string& category : meta->categories) {
                    if (lowerAscii(category).find(needle) !=
                            std::string::npos) {
                        matches = true;
                        break;
                    }
                }
            }
            if (!matches)
                continue;
            visible.push_back(static_cast<int>(i));
        }
        if (sort_ == SortMode::Alphabetical) {
            std::vector<std::string> keys(visible.size());
            for (size_t i = 0; i < visible.size(); ++i)
                keys[i] = lowerAscii(all[static_cast<size_t>(visible[i])].title);
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
            // Peer-count sort with the F5 fallback ranking when the source
            // carries no peer data at all.
            bool fallback = false;
            const std::vector<int> order =
                popularityOrder(all, visible, fallback);
            std::vector<int> sorted;
            sorted.reserve(visible.size());
            for (int index : order)
                sorted.push_back(visible[static_cast<size_t>(index)]);
            visible = std::move(sorted);
        } else {
            std::stable_sort(visible.begin(), visible.end(),
                [&all](int left, int right) {
                    return all[static_cast<size_t>(left)].publishedAt >
                           all[static_cast<size_t>(right)].publishedAt;
                });
        }
        applySortDirection(visible);

        std::vector<std::string> stateBadges;
        std::vector<std::string> gameNames;
        std::vector<std::string> iconUrls;
        std::vector<uint8_t> iconPreserveAspect;
        std::vector<uint8_t> selected;
        std::vector<uint8_t> selectable;
        std::vector<uint8_t> favorite;
        std::vector<const GameMetadata*> metas;
        stateBadges.reserve(visible.size());
        gameNames.reserve(visible.size());
        iconUrls.reserve(visible.size());
        iconPreserveAspect.reserve(visible.size());
        selected.reserve(visible.size());
        selectable.reserve(visible.size());
        favorite.reserve(visible.size());
        metas.reserve(visible.size());
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
                metadata_ ? metadata_->findByInfoHash(entry.infoHash) : nullptr;
            CatalogRowPresentation row =
                resolveCatalogRow(entry, meta);
            if (it == added.end() && !row.titleId.empty() &&
                installed_ && installed_->contains(row.titleId))
                stateBadges.back() = tr("pipensx/catalog/badge_installed");
            gameNames.push_back(std::move(row.title));
            iconUrls.push_back(std::move(row.iconUrl));
            iconPreserveAspect.push_back(row.iconPreserveAspect ? 1 : 0);
            selected.push_back(selectedHashes_.count(hash) ? 1 : 0);
            selectable.push_back(canSelect ? 1 : 0);
            favorite.push_back(favorites_ && favorites_->contains(hash) ? 1
                                                                       : 0);
            metas.push_back(meta);
        }

        // Shelves 2.0 (UI_PLAN F5, supersedes the F2 pair): Popular / New /
        // Recently updated / genre shelves. Only in the default browse state —
        // a search or batch mode wants the plain grid.
        // Dedup: a game appears on at most one shelf (Popular wins over New,
        // over later shelves), re-releases of one title collapse by titleId
        // inside a shelf. The grid below always shows everything.
        std::vector<CatalogShelf> shelves;
        int heroIndex = -1;
        std::string heroImage;
        if (query_.empty() && !shelfDrilldown_ && !batchMode_ &&
            !visible.empty()) {
            bool popularFallback = false;
            const std::vector<int> popular =
                popularityOrder(all, visible, popularFallback);
            const size_t withPeers = static_cast<size_t>(std::count_if(
                visible.begin(), visible.end(),
                [&all](int index) {
                    return all[static_cast<size_t>(index)].peerCount > 0;
                }));
            // F5.1 diagnostics: make a silent peer_count outage observable.
            if (shelfDiagTotal_ != visible.size() ||
                shelfDiagPeers_ != withPeers) {
                shelfDiagTotal_ = visible.size();
                shelfDiagPeers_ = withPeers;
                telemetry_log("catalog", "-",
                              "event=popular_shelf entries=%zu with_peers=%zu "
                              "mode=%s",
                              visible.size(), withPeers,
                              popularFallback ? "fallback" : "peers");
            }

            auto visibleEntry = [&](int index) -> const CatalogEntry& {
                return all[static_cast<size_t>(
                    visible[static_cast<size_t>(index)])];
            };
            // Dedup key: titleId when known (metadata or catalogue field),
            // info-hash otherwise.
            auto keyOf = [&](int index) {
                const size_t i = static_cast<size_t>(index);
                const GameMetadata* meta = metas[i];
                if (meta && !meta->titleId.empty())
                    return std::string("t:") + meta->titleId;
                const CatalogEntry& entry = visibleEntry(index);
                if (!entry.titleId.empty())
                    return std::string("t:") + entry.titleId;
                return "h:" + lowerAscii(entry.infoHash);
            };
            std::unordered_set<std::string> used;

            // Take up to kShelfItems unique, not-yet-used titles; commit the
            // keys only when the shelf is actually shown.
            auto buildShelf = [&](const std::vector<int>& candidates,
                                  size_t minItems) {
                std::vector<int> items;
                std::vector<std::string> keys;
                std::unordered_set<std::string> local;
                for (int index : candidates) {
                    if (items.size() >= grid::kShelfItems)
                        break;
                    std::string key = keyOf(index);
                    if (used.count(key) || local.count(key))
                        continue;
                    local.insert(key);
                    keys.push_back(std::move(key));
                    items.push_back(index);
                }
                if (items.size() < minItems)
                    return std::vector<int>();
                used.insert(keys.begin(), keys.end());
                return items;
            };

            // Popular: peered entries, or the full fallback ranking when the
            // source carries no peer data. Never hidden silently (F5.1) —
            // if dedup drained it, refill from the overall ranking.
            std::vector<int> popularCandidates;
            for (int index : popular) {
                if (popularFallback ||
                    visibleEntry(index).peerCount > 0)
                    popularCandidates.push_back(index);
            }
            std::vector<int> items = buildShelf(popularCandidates, 1);
            if (items.empty() && visible.size() > 1)
                items = buildShelf(popular, 1);
            if (!items.empty())
                shelves.push_back({tr("pipensx/catalog/shelf_popular"), std::move(items),
                           [this] {
                    openShelfDrilldown(SortMode::Popular);
                    focusGrid();
                }});

            // New: by published_at, minus everything already shown above.
            std::vector<int> byDate(visible.size());
            std::iota(byDate.begin(), byDate.end(), 0);
            std::stable_sort(byDate.begin(), byDate.end(),
                [&](int left, int right) {
                    return visibleEntry(left).publishedAt >
                           visibleEntry(right).publishedAt;
                });
            items = buildShelf(byDate, grid::kMinShelfItems);
            if (!items.empty())
                shelves.push_back({tr("pipensx/catalog/shelf_new"), std::move(items),
                           [this] {
                    openShelfDrilldown(SortMode::Latest);
                    focusGrid();
                }});

            // Recently updated: source updated after the original release.
            std::vector<int> updated;
            for (size_t i = 0; i < visible.size(); ++i) {
                const CatalogEntry& entry = visibleEntry(static_cast<int>(i));
                if (entry.sourceUpdatedAt > entry.publishedAt)
                    updated.push_back(static_cast<int>(i));
            }
            std::stable_sort(updated.begin(), updated.end(),
                [&](int left, int right) {
                    return visibleEntry(left).sourceUpdatedAt >
                           visibleEntry(right).sourceUpdatedAt;
                });
            items = buildShelf(updated, grid::kMinShelfItems);
            if (!items.empty())
                shelves.push_back({tr("pipensx/catalog/shelf_updated"), std::move(items),
                                   std::function<void()>()});

            // Genre picks from GameMetadataService categories: the two
            // biggest genres that still have enough unused titles. "See all"
            // hands the genre to search (categories match the query above).
            std::unordered_map<std::string, int> genreCounts;
            for (const GameMetadata* meta : metas) {
                if (!meta)
                    continue;
                for (const std::string& category : meta->categories)
                    ++genreCounts[category];
            }
            std::vector<std::pair<std::string, int>> genres(
                genreCounts.begin(), genreCounts.end());
            std::sort(genres.begin(), genres.end(),
                [](const auto& left, const auto& right) {
                    if (left.second != right.second)
                        return left.second > right.second;
                    return left.first < right.first;
                });
            int genreShelves = 0;
            for (const auto& genre : genres) {
                if (genreShelves == 2)
                    break;
                std::vector<int> candidates;
                for (int index : popular) {
                    const GameMetadata* meta =
                        metas[static_cast<size_t>(index)];
                    if (meta && std::find(meta->categories.begin(),
                                          meta->categories.end(),
                                          genre.first) !=
                                    meta->categories.end())
                        candidates.push_back(index);
                }
                items = buildShelf(candidates, grid::kMinShelfItems);
                if (items.empty())
                    continue;
                shelves.push_back({genre.first, std::move(items),
                                   [this, category = genre.first] {
                    query_ = category;
                    rebuildEntries();
                    focusGrid();
                }});
                ++genreShelves;
            }
        }

        size_t count = visible.size();
        observedCatalog_ = snapshot;
        observedMetadataGeneration_ =
            metadata_ ? metadata_->generation() : 0;
        dataSource_->setEntries(std::move(snapshot), std::move(visible),
                                std::move(stateBadges),
                                std::move(gameNames), std::move(iconUrls),
                                std::move(iconPreserveAspect),
                                std::move(selected), std::move(selectable),
                                std::move(favorite),
                                metadata_, batchMode_);
        dataSource_->setShelves(std::move(shelves), heroIndex,
                                std::move(heroImage));
        dataSource_->setMessage(query_.empty()
            ? tr("pipensx/catalog/empty_inline")
            : tr("pipensx/catalog/nothing_found_inline"));
        recycler_->reloadData();
        if (!focusInCatalog)
            recycler_->setContentOffsetY(0, false);
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
                        if (query_.empty() && !shelfDrilldown_)
                            return;
                        query_.clear();
                        shelfDrilldown_ = false;
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
        if (focusInCatalog)
            restoreFocus(focusHash, focusShelf);

        countText_ = query_.empty()
            ? tr("pipensx/catalog/count_releases", withThousands(count))
            : tr("pipensx/catalog/count_matches", withThousands(count));
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
        // The magnifier lights up (primary style, white icon) while a query
        // is active; the Clear chip is the visible reminder + escape hatch.
        searchField_->setIconColor(query_.empty() ? theme::textPrimary()
                                                  : nvgRGB(255, 255, 255));
        styleChip(searchField_, !query_.empty());
        clearSearch_->setVisibility(
            query_.empty() && !shelfDrilldown_ ? brls::Visibility::GONE
                                               : brls::Visibility::VISIBLE);
        styleSortChip(sortLatest_, SortMode::Latest,
                      tr("pipensx/catalog/sort_latest"));
        styleSortChip(sortPopular_, SortMode::Popular,
                      tr("pipensx/catalog/sort_popular"));
        styleSortChip(sortAlpha_, SortMode::Alphabetical,
                      tr("pipensx/catalog/sort_alpha"));
        styleSortChip(sortSize_, SortMode::Largest,
                      tr("pipensx/catalog/sort_size"));
        styleChip(filterFavorites_, favoritesOnly_);
        styleChip(filterFits_, fitsOnly_);
        // The chip carries the active choice, so the header states the filter
        // in force without opening the menu.
        filterPlayers_->setText(playerFilter_ == PlayerFilter::Any
            ? tr("pipensx/catalog/filter_players")
            : ui::playerFilterLabel(playerFilter_));
        styleChip(filterPlayers_, playerFilter_ != PlayerFilter::Any);
        count_->setText(countText_);
        updateFreshnessLabel();
    }

    // Top-right freshness badge. Bundled dumps do not count: only a successful
    // network refresh stamps lastCatalogRefreshWallSec. Green = refreshed
    // today, red = never / not today, orange = in flight.
    void updateFreshnessLabel() {
        if (!freshness_)
            return;
        if (busy_ || catalogRefreshInFlight()) {
            freshness_->setText(tr("pipensx/catalog/freshness_updating"));
            freshness_->setTextColor(theme::warning());
            return;
        }
        const uint64_t wallSec =
            settings_ ? settings_->get().lastCatalogRefreshWallSec : 0;
        if (wallSec == 0) {
            freshness_->setText(tr("pipensx/catalog/freshness_never"));
            freshness_->setTextColor(theme::error());
            return;
        }
        const int64_t epoch = static_cast<int64_t>(wallSec);
        const std::string date = formatEpochDateUtc(epoch);
        if (isLocalToday(epoch)) {
            freshness_->setText(tr("pipensx/catalog/freshness_ok", date));
            freshness_->setTextColor(theme::success());
        } else {
            freshness_->setText(tr("pipensx/catalog/freshness_stale", date));
            freshness_->setTextColor(theme::error());
        }
    }

    // Only the active chip carries an arrow: the direction belongs to the sort
    // in force, and four arrows would read as four independent switches. The
    // arrow is ascending/descending of that chip's own key, so A-Z is up while
    // newest/most-seeded/largest are down.
    void styleSortChip(brls::Button* chip, SortMode mode,
                       const std::string& label) {
        const bool active = sort_ == mode;
        if (active) {
            const bool descending = naturalDescending(mode) != sortReversed_;
            chip->setText(label + (descending ? " ↓" : " ↑"));
        } else {
            chip->setText(label);
        }
        styleChip(chip, active);
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
        chip->registerClickAction(
            [onClick = std::move(onClick)](brls::View*) {
                onClick();
                return true;
            });
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

    void openShelfDrilldown(SortMode mode) {
        if (busy_)
            return;
        query_.clear();
        shelfDrilldown_ = true;
        sort_ = mode;
        sortReversed_ = false;
        rebuildEntries();
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

    // "See all" target (F5): land the focus on the first grid row. In
    // drill-down mode shelves are hidden, so this is the top of the result.
    void focusGrid() {
        if (dataSource_->entryCount() == 0)
            return;
        *focusColumn_ = 0;
        recycler_->selectRowAt(
            brls::IndexPath(0, dataSource_->headerRowCount()), false);
        brls::Application::giveFocus(recycler_);
    }

    // Popularity ranking (F5.1). With peer data: peer_count desc. Without
    // any peers in the whole set (source outage), fall back to a rank sum of
    // freshness and size instead of hiding the shelf.
    static std::vector<int> popularityOrder(
        const std::vector<CatalogEntry>& all, const std::vector<int>& visible,
        bool& usedFallback) {
        std::vector<int> order(visible.size());
        std::iota(order.begin(), order.end(), 0);
        auto entry = [&](int visIndex) -> const CatalogEntry& {
            return all[static_cast<size_t>(
                visible[static_cast<size_t>(visIndex)])];
        };
        usedFallback = std::none_of(visible.begin(), visible.end(),
            [&all](int snapshotIndex) {
                return all[static_cast<size_t>(snapshotIndex)].peerCount > 0;
            });
        if (!usedFallback) {
            std::stable_sort(order.begin(), order.end(),
                [&](int left, int right) {
                    const auto& l = entry(left);
                    const auto& r = entry(right);
                    if (l.peerCount != r.peerCount)
                        return l.peerCount > r.peerCount;
                    return l.publishedAt > r.publishedAt;
                });
            return order;
        }
        std::vector<size_t> score(visible.size(), 0);
        std::vector<int> ranked = order;
        std::stable_sort(ranked.begin(), ranked.end(),
            [&](int left, int right) {
                return entry(left).publishedAt > entry(right).publishedAt;
            });
        for (size_t pos = 0; pos < ranked.size(); ++pos)
            score[static_cast<size_t>(ranked[pos])] += pos;
        std::stable_sort(ranked.begin(), ranked.end(),
            [&](int left, int right) {
                return entry(left).size > entry(right).size;
            });
        for (size_t pos = 0; pos < ranked.size(); ++pos)
            score[static_cast<size_t>(ranked[pos])] += pos;
        std::stable_sort(order.begin(), order.end(),
            [&](int left, int right) {
                if (score[static_cast<size_t>(left)] !=
                    score[static_cast<size_t>(right)])
                    return score[static_cast<size_t>(left)] <
                           score[static_cast<size_t>(right)];
                return entry(left).publishedAt > entry(right).publishedAt;
            });
        return order;
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
    void restoreFocus(const std::string& hash, int shelfRow) {
        if (dataSource_->entryCount() == 0) {
            brls::Application::giveFocus(ensureEmptyState());
            return;
        }
        if (shelfRow >= 0 && shelfRow < dataSource_->headerRowCount()) {
            // O12: the shelf's getDefaultFocus() reads this back, so the
            // focus lands on the same card at its position inside the shelf.
            *shelfFocusHash_ = hash;
            recycler_->selectRowAt(brls::IndexPath(0, shelfRow), false);
            brls::Application::giveFocus(recycler_);
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

    // Y sorts while idle and cancels during a refresh. The sort half is hidden
    // to keep the bottom bar within its width (see the registration block in
    // the constructor), but the cancel half has to be visible or a refresh
    // looks unstoppable — and Action::hidden is fixed at construction, so the
    // swap re-registers. registerAction replaces the entry for a button it
    // already holds, which is what makes this safe to call repeatedly.
    void registerSortAction(bool busy) {
        registerAction(busy ? tr("pipensx/common/stop")
                            : tr("pipensx/common/sort"),
                       brls::BUTTON_Y, [this](brls::View*) {
            if (busy_)
                cancelled_->store(true);
            else
                cycleSort();
            return true;
        }, /*hidden=*/!busy);
    }

    void setBusy(bool busy) {
        busy_ = busy;
        registerSortAction(busy);
        if (busyDot_) {
            if (busy) {
                busyDot_->setVisibility(brls::Visibility::VISIBLE);
                startBusyPulse(busyDot_);
            } else {
                stopBusyPulse(busyDot_);
                busyDot_->setVisibility(brls::Visibility::GONE);
            }
        }
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
            if (!row.titleId.empty() && installed_ &&
                installed_->contains(row.titleId))
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
            if (auto* hero = dynamic_cast<HeroCard*>(view)) {
                if (hero->entryIndex() >= 0) {
                    GridCardInfo info = dataSource_->cardInfo(hero->entryIndex());
                    hero->setSubLine(info.sub, info.subIsBadge);
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

    // Y hotkey: cycle through the sort modes; the header chips (O2) reflect
    // the result, so no toast is needed.
    void cycleSort() {
        setSort(sort_ == SortMode::Latest        ? SortMode::Popular
              : sort_ == SortMode::Popular      ? SortMode::Alphabetical
              : sort_ == SortMode::Alphabetical ? SortMode::Largest
                                                 : SortMode::Latest);
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
        if (fetchCatalog && fetchMetadata) {
            if (catalogOk && metadataOk) {
                brls::Application::notify(
                    tr("pipensx/catalog/updated_both",
                       catalog_->entries().size()));
            } else if (catalogOk) {
                brls::Application::notify(
                    tr("pipensx/catalog/updated_catalog_artwork_failed",
                       metadataError));
            } else if (metadataOk) {
                brls::Application::notify(
                    tr("pipensx/catalog/updated_artwork_catalog_failed",
                       catalogError));
            } else {
                brls::Application::notify(
                    tr("pipensx/catalog/updated_both_failed", catalogError,
                       metadataError));
            }
        } else if (fetchCatalog) {
            brls::Application::notify(catalogOk
                ? tr("pipensx/catalog/updated_catalog",
                     catalog_->entries().size())
                : catalogError);
        } else if (fetchMetadata) {
            brls::Application::notify(metadataOk
                ? tr("pipensx/catalog/updated_artwork", metadata_->size())
                : metadataError);
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
                if (batchMode_)
                    status_->setTextColor(theme::textTertiary());
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
    brls::Box* freshnessRow_ = nullptr;
    brls::Label* busyDot_ = nullptr;
    brls::Label* freshness_ = nullptr;
    SearchIconButton* searchField_ = nullptr;
    brls::Button* clearSearch_ = nullptr;
    brls::Button* sortLatest_ = nullptr;
    brls::Button* sortPopular_ = nullptr;
    brls::Button* sortAlpha_ = nullptr;
    brls::Button* sortSize_ = nullptr;
    brls::Button* filterFavorites_ = nullptr;
    brls::Button* filterFits_ = nullptr;
    brls::Button* filterPlayers_ = nullptr;
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
    // O12: info-hash of the last-focused shelf card, shared with every
    // ShelfCell/HorizontalShelf so it survives cell recycling.
    std::shared_ptr<std::string> shelfFocusHash_ =
        std::make_shared<std::string>();
    // O12: where to put the focus back when the game page closes.
    std::string returnFocusHash_;
    int returnFocusShelf_ = -1;
    std::unordered_map<std::string, std::string> catalogFailures_;
    std::unordered_set<std::string> selectedHashes_;
    std::string query_;
    std::string countText_;
    SortMode sort_ = SortMode::Latest;
    // Flips the active sort's natural direction (see naturalDescending).
    // Session-only, like the view filters: a relaunch comes back to the
    // catalog's own idea of order.
    bool sortReversed_ = false;
    bool busy_ = false;
    bool batchMode_ = false;
    bool shelfDrilldown_ = false;
    // Session-only view filters: deliberately not persisted, so a relaunch
    // always comes back to the full catalog.
    bool favoritesOnly_ = false;
    bool fitsOnly_ = false;
    PlayerFilter playerFilter_ = PlayerFilter::Any;
    brls::RepeatingTimer timer_;
    uint64_t observedSettingsGeneration_ = 0;
    std::shared_ptr<const std::vector<CatalogEntry>> observedCatalog_;
    uint64_t observedMetadataGeneration_ = 0;
    uint64_t taskSignature_ = 0;
    uint64_t taskIdSignature_ = 0;
    // Last logged popular-shelf diagnostics (F5.1): entry/peer counts.
    size_t shelfDiagTotal_ = static_cast<size_t>(-1);
    size_t shelfDiagPeers_ = 0;
    uint64_t installedRefreshSignature_ = 0;
    bool installedRefreshInFlight_ = false;
};

}  // namespace pipensx::ui
