#include "retro_view.hpp"
#include "retro_download_view.hpp"
#include "ui/catalog/catalog_grid.hpp"
#include "ui/common/catalog_controls.hpp"
#include "ui/common/message_cells.hpp"
#include "ui/common/ui_helpers.hpp"
#include "ui/i18n.hpp"
#include <algorithm>

namespace pipensx::ui {

namespace {

const RetroConsoleDef& resolveConsoleDef(const std::string& id) {
    if (const auto* c = findRetroConsole(id))
        return *c;
    return retroConsoles().front();
}

} // namespace

class RetroDataSource : public brls::RecyclerDataSource {
public:
    std::vector<RetroRelease> releases;
    std::map<std::string, RetroStats> stats;
    std::string message;
    bool loading = true;
    GameMetadataService* metadata = nullptr;
    std::function<void(const RetroRelease&)> select;

    int numberOfRows(brls::RecyclerFrame*, int) override {
        if (releases.empty() && loading)
            return 3;
        return 1 + std::max<size_t>(1, (releases.size() + grid::kColumns - 1) / grid::kColumns);
    }

    float heightForRow(brls::RecyclerFrame*, brls::IndexPath index) override {
        if (index.row == 0)
            return grid::kTopInsetHeight;
        return (releases.empty() && !loading) ? 140.0f : grid::kRowHeight;
    }

    brls::RecyclerCell* cellForRow(brls::RecyclerFrame* recycler, brls::IndexPath index) override {
        if (index.row == 0)
            return recycler->dequeueReusableCell("TopInset");
        if (releases.empty()) {
            if (loading)
                return recycler->dequeueReusableCell("Skeleton");
            auto* cell = static_cast<TextMessageCell*>(recycler->dequeueReusableCell("Message"));
            cell->setMessage(message);
            return cell;
        }
        std::vector<GridCardInfo> cards;
        const size_t start = static_cast<size_t>(index.row - 1) * grid::kColumns;
        for (size_t i = start; i < std::min(releases.size(), start + grid::kColumns); ++i) {
            const auto& release = releases[i];
            GridCardInfo card;
            card.entryIndex = static_cast<int>(i);
            card.title = release.title;
            std::string sub;
            if (!release.year.empty() && catalogFoldForSearch(release.year) != "unknown") {
                const std::string yr = extractRetroYear(release.year);
                sub = yr.empty() ? release.year : yr;
            }
            if (!release.size.empty())
                sub += (sub.empty() ? "" : " · ") + release.size;
            card.sub = std::move(sub);
            const auto peers = stats.find(release.topicId);
            if (peers != stats.end())
                card.peers = std::to_string(peers->second.seeds) + " ↑";
            card.iconUrl = release.coverUrl;
            card.iconPreserveAspect = false;
            if (release.romset)
                card.diffChips.push_back(tr("pipensx/retro/romset"));
            cards.push_back(std::move(card));
        }
        auto* cell = static_cast<GridRowCell*>(recycler->dequeueReusableCell("Retro"));
        cell->setRow(cards, metadata, [this](int entry) {
            if (entry >= 0 && static_cast<size_t>(entry) < releases.size())
                select(releases[entry]);
        });
        return cell;
    }

    void didSelectRowAt(brls::RecyclerFrame*, brls::IndexPath) override {}
};

RetroView::RetroView(std::string appRoot, std::string sdRoot, AppSettings* settings,
                     GameMetadataService* metadata, DownloadManager* manager)
    : brls::Box(brls::Axis::COLUMN),
      catalog_(std::make_shared<RetroCatalog>(appRoot)),
      installer_(std::make_shared<RetroEmulatorInstaller>(sdRoot)),
      roms_(std::make_shared<RetroRomStore>(std::move(appRoot), std::move(sdRoot))),
      manager_(manager),
      settings_(settings),
      metadata_(metadata),
      alive_(std::make_shared<std::atomic<bool>>(true)),
      cancelled_(std::make_shared<std::atomic<bool>>(false)) {
    setGrow(1);

    std::string ignoredErr;
    roms_->loadSettings(retroSettings_, ignoredErr);
    if (findRetroConsole(retroSettings_.lastConsole))
        activeConsole_ = retroSettings_.lastConsole;

    auto* header = new brls::Box(brls::Axis::ROW);
    header->setShrink(0);
    header->setMarginTop(6);
    header->setMarginBottom(8);
    header->setMarginLeft(28);
    header->setMarginRight(28);
    header->setAlignItems(brls::AlignItems::CENTER);

    consoleBtn_ = catalogChip(resolveConsoleDef(activeConsole_).shortBadge, [this] { pickConsole(); });
    consoleBtn_->setMarginLeft(0);
    consoleBtn_->setStyle(&brls::BUTTONSTYLE_PRIMARY);
    header->addView(consoleBtn_);

    sortBtn_ = catalogChip(sortLabel(sortMode_) + " ↓", [this] { openSortSheet(); });
    sortBtn_->setStyle(&brls::BUTTONSTYLE_PRIMARY);
    header->addView(sortBtn_);

    typeBtn_ = catalogChip(tr("pipensx/retro/type_title"), [this] { openTypeSheet(); });
    header->addView(typeBtn_);

    auto* split = new brls::Box();
    split->setWidth(1);
    split->setHeight(22);
    split->setShrink(0);
    split->setBackgroundColor(theme::track());
    split->setMarginLeft(8);
    split->setMarginRight(8);
    header->addView(split);

    emuBtn_ = catalogChip(tr("pipensx/retro/emulators"), [this] { openEmulators(); });
    emuBtn_->setMarginLeft(0);
    header->addView(emuBtn_);

    status_ = new brls::Label();
    status_->setText(tr("pipensx/retro/loading"));
    status_->setFontSize(theme::kFontCaption);
    status_->setSingleLine(true);
    status_->setClipsToBounds(true);
    status_->setGrow(1);
    status_->setShrink(1);
    status_->setMinWidth(0);
    status_->setMarginLeft(12);
    status_->setMarginRight(8);
    status_->setHorizontalAlign(brls::HorizontalAlign::RIGHT);
    status_->setTextColor(theme::textSecondary());
    header->addView(status_);

    reset_ = catalogChip(tr("pipensx/catalog/filter_reset"), [this] {
        query_.clear();
        sortMode_ = SortMode::SeedsDesc;
        sortReversed_ = false;
        typeFilter_ = TypeFilter::All;
        rebuild();
    });
    reset_->setVisibility(brls::Visibility::GONE);
    header->addView(reset_);

    refresh_ = catalogChip(tr("pipensx/common/refresh"), [this] {
        if (!busy_)
            update(true, true);
    });
    header->addView(refresh_);

    search_ = new SearchIconButton();
    search_->setStyle(&brls::BUTTONSTYLE_DEFAULT);
    search_->setWidth(44);
    search_->setHeight(44);
    search_->setShrink(0);
    search_->setMarginLeft(8);
    search_->setText("");
    search_->registerAction("", brls::BUTTON_A, [this](brls::View*) {
        search();
        return true;
    }, true);
    search_->addGestureRecognizer(new brls::TapGestureRecognizer(search_));
    header->addView(search_);

    addView(header);

    recycler_ = new SmoothRecyclerFrame();
    recycler_->setGrow(1);
    recycler_->setPadding(6, 28, 6, 28);
    recycler_->estimatedRowHeight = grid::kRowHeight;
    const auto column = std::make_shared<int>(0);
    recycler_->registerCell("Retro", [column] { return new GridRowCell(column); });
    recycler_->registerCell("TopInset", [] { return new TopInsetCell(); });
    recycler_->registerCell("Message", [] { return new TextMessageCell(); });
    recycler_->registerCell("Skeleton", [] { return new SkeletonGridRowCell(); });
    data_ = new RetroDataSource();
    data_->metadata = metadata_;
    data_->message = tr("pipensx/retro/loading");
    data_->select = [this](const RetroRelease& release) { openRelease(release); };
    recycler_->setDataSource(data_);
    recyclerHost_ = recyclerHost(recycler_);
    addView(recyclerHost_);

    registerAction(tr("pipensx/common/search"), brls::BUTTON_X, [this](brls::View*) {
        search();
        return true;
    }, /*hidden=*/true);
    registerAction(tr("pipensx/retro/select_console"), brls::BUTTON_Y, [this](brls::View*) {
        pickConsole();
        return true;
    });
}

RetroView::~RetroView() {
    *alive_ = false;
    *cancelled_ = true;
    ++filterGeneration_;
}

void RetroView::willAppear(bool resetState) {
    brls::Box::willAppear(resetState);
    if (!initialized_) {
        initialized_ = true;
        update(true, false);
    }
}

std::string RetroView::consoleId() const {
    return resolveConsoleDef(activeConsole_).id;
}

void RetroView::update(bool forceShared, bool forceNetwork) {
    if (busy_) {
        *cancelled_ = true;
        busy_ = false;
    }
    busy_ = true;
    cancelled_ = std::make_shared<std::atomic<bool>>(false);
    const uint64_t gen = ++fetchGeneration_;
    const std::string plat = consoleId();
    const bool wantShared = forceShared || !sharedLoaded_;

    cached_ = false;
    failure_.clear();
    snapshot_ = RetroCatalogSnapshot{};
    snapshot_.platform = plat;
    if (viewContains(refresh_, brls::Application::getCurrentFocus()))
        brls::Application::giveFocus(consoleBtn_);
    refresh_->setState(brls::ButtonState::DISABLED);
    rebuild(true);

    const auto alive = alive_;
    const auto cancelled = cancelled_;
    const std::string source = settings_ ? settings_->get().catalogSourceUrl : std::string();

    brls::async([this, alive, cancelled, gen, plat, wantShared, forceNetwork, catalog = catalog_, source] {
        RetroCatalogSnapshot snapshot;
        std::string error;
        const bool cached = catalog->load(plat, snapshot, error);
        if (cached) {
            brls::sync([this, alive, gen, forceNetwork, snapshot] {
                if (!*alive || gen != fetchGeneration_)
                    return;
                snapshot_ = snapshot;
                cached_ = true;
                sharedLoaded_ = !snapshot_.stats.empty() || !snapshot_.emulators.empty();
                if (!forceNetwork) {
                    busy_ = false;
                    refresh_->setState(brls::ButtonState::ENABLED);
                }
                rebuild();
            });
            if (!forceNetwork)
                return;
        }
        const bool ok = catalog->refresh(plat, snapshot, source, wantShared, *cancelled, error);
        brls::sync([this, alive, gen, cached, ok, snapshot = std::move(snapshot),
                    error = std::move(error)]() mutable {
            if (!*alive || gen != fetchGeneration_)
                return;
            busy_ = false;
            refresh_->setState(brls::ButtonState::ENABLED);
            if (ok || cached)
                snapshot_ = std::move(snapshot);
            if (ok)
                sharedLoaded_ = true;
            cached_ = !ok && (cached || !snapshot_.releases.empty());
            failure_ = ok ? "" : std::move(error);
            rebuild();
        });
    });
}

void RetroView::rebuild(bool scheduleOnly) {
    const auto& console = resolveConsoleDef(activeConsole_);
    consoleBtn_->setText(console.shortBadge);
    const bool descending = naturalDescending(sortMode_) != sortReversed_;
    sortBtn_->setText(sortLabel(sortMode_) + (descending ? " ↓" : " ↑"));
    sortBtn_->setStyle(&brls::BUTTONSTYLE_PRIMARY);
    typeBtn_->setText(typeFilter_ == TypeFilter::All
        ? tr("pipensx/retro/type_title")
        : typeFilterLabel(typeFilter_));
    typeBtn_->setStyle(typeFilter_ != TypeFilter::All
        ? &brls::BUTTONSTYLE_PRIMARY
        : &brls::BUTTONSTYLE_DEFAULT);

    const bool hasActiveFilter =
        !query_.empty() || sortMode_ != SortMode::SeedsDesc || sortReversed_ ||
        typeFilter_ != TypeFilter::All;
    if (!hasActiveFilter && viewContains(reset_, brls::Application::getCurrentFocus()))
        brls::Application::giveFocus(consoleBtn_);
    reset_->setVisibility(hasActiveFilter ? brls::Visibility::VISIBLE : brls::Visibility::GONE);

    const uint64_t filterGen = ++filterGeneration_;
    if (scheduleOnly || snapshot_.releases.empty()) {
        filtering_ = false;
        applyFiltered({}, filterGen);
        return;
    }

    filtering_ = true;
    if (data_->releases.empty()) {
        data_->loading = true;
        recycler_->reloadData();
        status_->setText(tr("pipensx/retro/loading"));
    }

    const auto alive = alive_;
    const auto releases = snapshot_.releases;
    const auto stats = snapshot_.stats;
    const auto queryRaw = query_;
    const auto sortMode = sortMode_;
    const bool sortReversed = sortReversed_;
    const auto typeFilter = typeFilter_;

    brls::async([this, alive, filterGen, releases, stats, queryRaw, sortMode, sortReversed,
                 typeFilter]() {
        std::vector<RetroRelease> filtered;
        filtered.reserve(releases.size());
        const auto query = catalogFoldForSearch(queryRaw);

        for (const auto& release : releases) {
            if (filterGen != filterGeneration_.load(std::memory_order_relaxed))
                return;
            if (typeFilter == TypeFilter::GamesOnly && release.romset)
                continue;
            if (typeFilter == TypeFilter::RomsetsOnly && !release.romset)
                continue;
            if (!query.empty() &&
                catalogFoldForSearch(release.title + " " + release.genre + " " + release.serial +
                                     " " + release.interfaceLang)
                        .find(query) == std::string::npos)
                continue;
            filtered.push_back(release);
        }

        auto seedsOf = [&](const RetroRelease& r) -> uint64_t {
            const auto it = stats.find(r.topicId);
            return it != stats.end() ? it->second.seeds : 0;
        };
        auto yearOf = [](const RetroRelease& r) -> int {
            const std::string yr = extractRetroYear(r.year);
            return yr.empty() ? 0 : std::atoi(yr.c_str());
        };

        std::stable_sort(filtered.begin(), filtered.end(),
                         [&](const RetroRelease& a, const RetroRelease& b) {
            switch (sortMode) {
                case SortMode::SeedsDesc: {
                    const uint64_t sa = seedsOf(a), sb = seedsOf(b);
                    if (sa != sb) return sa > sb;
                    return catalogFoldForSearch(a.title) < catalogFoldForSearch(b.title);
                }
                case SortMode::TitleAsc:
                    return catalogFoldForSearch(a.title) < catalogFoldForSearch(b.title);
                case SortMode::YearDesc: {
                    const int ya = yearOf(a), yb = yearOf(b);
                    if (ya != yb) return ya > yb;
                    return seedsOf(a) > seedsOf(b);
                }
                case SortMode::SizeDesc: {
                    const uint64_t ba = parseRetroSizeBytes(a.size);
                    const uint64_t bb = parseRetroSizeBytes(b.size);
                    if (ba != bb) return ba > bb;
                    return seedsOf(a) > seedsOf(b);
                }
            }
            return false;
        });
        if (sortReversed)
            std::reverse(filtered.begin(), filtered.end());

        brls::sync([this, alive, filterGen, filtered = std::move(filtered)]() mutable {
            if (!*alive || filterGen != filterGeneration_)
                return;
            filtering_ = false;
            applyFiltered(std::move(filtered), filterGen);
        });
    });
}

void RetroView::applyFiltered(std::vector<RetroRelease> filtered, uint64_t gen) {
    if (gen != filterGeneration_)
        return;
    if (viewContains(recyclerHost_, brls::Application::getCurrentFocus()))
        brls::Application::giveFocus(consoleBtn_);

    const bool emptySnapshot = snapshot_.releases.empty();
    data_->releases = std::move(filtered);
    data_->stats = snapshot_.stats;
    data_->loading = (busy_ || filtering_) && emptySnapshot;
    data_->message = tr((busy_ || filtering_) && emptySnapshot
                            ? "pipensx/retro/loading"
                            : (!failure_.empty() && emptySnapshot ? "pipensx/retro/empty_error"
                                                                  : "pipensx/retro/no_results"));
    recycler_->reloadData();

    std::string statusText;
    if (busy_ || filtering_) {
        statusText = tr(cached_ ? "pipensx/retro/refreshing_cached" : "pipensx/retro/loading");
    } else if (!failure_.empty()) {
        statusText = tr(cached_ ? "pipensx/retro/cached_error" : "pipensx/retro/load_failed", failure_);
    } else {
        statusText = tr("pipensx/retro/count", data_->releases.size());
        if (!query_.empty())
            statusText += " · " + query_;
    }
    status_->setText(statusText);
}

void RetroView::pickConsole() {
    const auto& consoles = retroConsoles();
    std::vector<std::string> labels;
    labels.reserve(consoles.size());
    int selectedIdx = 0;
    for (size_t i = 0; i < consoles.size(); ++i) {
        const auto& c = consoles[i];
        if (c.id == activeConsole_)
            selectedIdx = static_cast<int>(i);
        std::string mark = (c.id == activeConsole_) ? "* " : "";
        labels.push_back(mark + "[" + c.shortBadge + "] " + c.defaultName + " (" + c.year + ")");
    }
    const auto alive = alive_;
    auto* menu = new brls::Dropdown(
        tr("pipensx/retro/select_console"), labels,
        [this, alive, consoles](int index) {
            if (!*alive || index < 0 || static_cast<size_t>(index) >= consoles.size())
                return;
            const std::string chosen = consoles[static_cast<size_t>(index)].id;
            if (chosen == activeConsole_)
                return;
            activeConsole_ = chosen;
            query_.clear();
            retroSettings_.lastConsole = chosen;
            std::string err;
            roms_->saveSettings(retroSettings_, err);
            update(false);
        },
        selectedIdx);
    brls::Application::pushActivity(new brls::Activity(menu));
}

bool RetroView::naturalDescending(SortMode mode) {
    return mode != SortMode::TitleAsc;
}

std::string RetroView::sortLabel(SortMode mode) {
    switch (mode) {
        case SortMode::TitleAsc: return tr("pipensx/retro/sort_title");
        case SortMode::YearDesc: return tr("pipensx/retro/sort_year");
        case SortMode::SizeDesc: return tr("pipensx/retro/sort_size");
        case SortMode::SeedsDesc:
        default: return tr("pipensx/retro/sort_seeds");
    }
}

std::string RetroView::typeFilterLabel(TypeFilter filter) {
    switch (filter) {
        case TypeFilter::GamesOnly: return tr("pipensx/retro/type_games");
        case TypeFilter::RomsetsOnly: return tr("pipensx/retro/type_romsets");
        case TypeFilter::All:
        default: return tr("pipensx/retro/type_all");
    }
}

void RetroView::openSortSheet() {
    const std::vector<SortMode> modes = {
        SortMode::SeedsDesc, SortMode::TitleAsc, SortMode::YearDesc, SortMode::SizeDesc};
    std::vector<std::string> labels;
    labels.reserve(modes.size());
    int selected = 0;
    for (size_t i = 0; i < modes.size(); ++i) {
        labels.push_back(sortLabel(modes[i]));
        if (sortMode_ == modes[i])
            selected = static_cast<int>(i);
    }
    const auto alive = alive_;
    auto* dropdown = new brls::Dropdown(
        tr("pipensx/catalog/sort_title"), labels, [](int) {}, selected,
        [this, alive, modes](int index) {
            if (!*alive || index < 0 || static_cast<size_t>(index) >= modes.size())
                return;
            setSort(modes[static_cast<size_t>(index)]);
        });
    brls::Application::pushActivity(new brls::Activity(dropdown));
}

void RetroView::setSort(SortMode mode) {
    if (sortMode_ == mode) {
        sortReversed_ = !sortReversed_;
    } else {
        sortMode_ = mode;
        sortReversed_ = false;
    }
    rebuild();
}

void RetroView::openTypeSheet() {
    const std::vector<TypeFilter> choices = {
        TypeFilter::All, TypeFilter::GamesOnly, TypeFilter::RomsetsOnly};
    std::vector<std::string> labels;
    labels.reserve(choices.size());
    int selected = 0;
    for (size_t i = 0; i < choices.size(); ++i) {
        labels.push_back(typeFilterLabel(choices[i]));
        if (typeFilter_ == choices[i])
            selected = static_cast<int>(i);
    }
    const auto alive = alive_;
    auto* dropdown = new brls::Dropdown(
        tr("pipensx/retro/type_title"), labels, [](int) {}, selected,
        [this, alive, choices](int index) {
            if (!*alive || index < 0 || static_cast<size_t>(index) >= choices.size())
                return;
            setTypeFilter(choices[static_cast<size_t>(index)]);
        });
    brls::Application::pushActivity(new brls::Activity(dropdown));
}

void RetroView::setTypeFilter(TypeFilter filter) {
    if (typeFilter_ == filter)
        return;
    typeFilter_ = filter;
    rebuild();
}

void RetroView::search() {
    const auto alive = alive_;
    brls::Application::getImeManager()->openForText(
        [this, alive](std::string value) {
            if (!*alive)
                return;
            query_ = std::move(value);
            rebuild();
        },
        tr("pipensx/common/search"), "", 128, query_, brls::KEYBOARD_DISABLE_NONE);
}

void RetroView::openEmulators() {
    openRetroEmulatorsView(activeConsole_, snapshot_.emulators, installer_, roms_, [this] {
        if (*alive_) {
            std::string err;
            roms_->loadSettings(retroSettings_, err);
        }
    });
}

void RetroView::openRelease(const RetroRelease& release) {
    const auto it = snapshot_.stats.find(release.topicId);
    const RetroStats* stats = it != snapshot_.stats.end() ? &it->second : nullptr;
    openRetroReleaseDetail(release, stats, snapshot_.emulators, manager_, settings_, roms_,
                           installer_, metadata_);
}

} // namespace pipensx::ui
