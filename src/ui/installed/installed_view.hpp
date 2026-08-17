#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <vector>

#include <borealis.hpp>

#include "app/app_settings.hpp"
#include "app/catalog_service.hpp"
#include "app/download_manager.hpp"
#include "app/favorites_service.hpp"
#include "app/game_metadata_service.hpp"
#include "app/game_update_service.hpp"
#include "app/installed_title_service.hpp"
#include "app/switch_deploy.hpp"
#include "ui/catalog/catalog_helpers.hpp"
#include "ui/common/async_image.hpp"
#include "ui/common/ui_helpers.hpp"
#include "ui/detail/game_detail.hpp"
#include "ui/i18n.hpp"
#include "ui/theme.hpp"

namespace pipensx::ui {

class InstalledCell : public brls::RecyclerCell {
public:
    using OpenMenu = std::function<void(InstalledTitle)>;

    InstalledCell() {
        setFocusable(true);
        setAxis(brls::Axis::ROW);
        setAlignItems(brls::AlignItems::CENTER);
        setPadding(10, 18, 10, 14);
        setHeight(92);

        mark_ = new brls::Box();
        mark_->setWidth(4);
        mark_->setHeight(64);
        mark_->setCornerRadius(2);
        mark_->setBackgroundColor(theme::error());
        mark_->setMarginRight(12);
        mark_->setVisibility(brls::Visibility::GONE);
        addView(mark_);

        image_ = new AsyncRgbaImage();
        image_->setWidth(64);
        image_->setHeight(64);
        image_->setCornerRadius(8);
        image_->setMarginRight(16);
        image_->setScalingType(brls::ImageScalingType::FILL);
        addView(image_);

        auto* labels = new brls::Box(brls::Axis::COLUMN);
        labels->setGrow(1);
        title_ = new brls::Label();
        title_->setSingleLine(true);
        title_->setAutoAnimate(false);
        title_->setFontSize(21);
        subtitle_ = new brls::Label();
        subtitle_->setSingleLine(true);
        subtitle_->setAutoAnimate(false);
        subtitle_->setFontSize(15);
        subtitle_->setMarginTop(6);
        subtitle_->setTextColor(theme::textTertiary());
        labels->addView(title_);
        labels->addView(subtitle_);
        addView(labels);

        // Update-state chip (Q10 colours): right-aligned coloured label.
        chip_ = new brls::Label();
        chip_->setSingleLine(true);
        chip_->setAutoAnimate(false);
        chip_->setFontSize(13);
        chip_->setMarginLeft(12);
        chip_->setShrink(0.0f);
        addView(chip_);

        registerClickAction([this](brls::View*) {
            if (!titleId_.empty() && onOpenMenu_)
                onOpenMenu_(storedTitle_);
            return true;
        });
        addGestureRecognizer(new brls::TapGestureRecognizer(this));
        updateActionHint(brls::BUTTON_A, tr("pipensx/common/more"));
    }

    void setTitle(const InstalledTitle& title,
                  GameMetadataService* metadata) {
        storedTitle_ = title;
        title_->setText(title.name);
        titleId_ = title.titleId;
        publisher_ = title.publisher;
        version_ = title.version;
        updateSubtitle();
        setArtworkUrl(image_, metadata, title.iconPath, currentIconPath_,
                      imageState_);
    }

    void setResult(const GameUpdateResult* result, bool ignored,
                   OpenMenu onOpenMenu) {
        onOpenMenu_ = std::move(onOpenMenu);
        ignored_ = ignored;
        const GameUpdateState state =
            result ? result->state : GameUpdateState::NotChecked;
        currentState_ = state;
        currentFoundVersion_ = result ? result->foundVersion : std::string();
        updateSubtitle();
        updateActionHint(brls::BUTTON_A, tr("pipensx/common/more"));
        const bool available =
            !ignored && state == GameUpdateState::UpdateAvailable;
        mark_->setVisibility(available ? brls::Visibility::VISIBLE
                                       : brls::Visibility::GONE);
        if (ignored) {
            chip_->setText(tr("pipensx/installed/update_chip_ignored"));
            chip_->setTextColor(theme::textTertiary());
            chip_->setVisibility(brls::Visibility::VISIBLE);
            return;
        }
        switch (state) {
        case GameUpdateState::UpdateAvailable:
            chip_->setText(tr("pipensx/common/install"));
            chip_->setTextColor(theme::error());
            chip_->setVisibility(brls::Visibility::VISIBLE);
            break;
        case GameUpdateState::CheckError:
            chip_->setText(tr("pipensx/installed/update_chip_error"));
            chip_->setTextColor(theme::error());
            chip_->setVisibility(brls::Visibility::VISIBLE);
            break;
        case GameUpdateState::UpToDate:
        case GameUpdateState::SourceUnknown:
        case GameUpdateState::NotChecked:
        case GameUpdateState::Checking:
        default:
            chip_->setText("");
            chip_->setVisibility(brls::Visibility::GONE);
            break;
        }
    }

    void onFocusGained() override {
        brls::RecyclerCell::onFocusGained();
        title_->setAnimated(true);
    }

    void onFocusLost() override {
        brls::RecyclerCell::onFocusLost();
        title_->setAnimated(false);
    }

private:
    // "Publisher · vX → vY": the version transition is the one fact this line
    // exists for when an update is available, so it must never be pushed off
    // the end. The 16-hex title ID was dropped — nothing on screen can act on
    // it, and on long publishers it was the first thing to get clipped.
    void updateSubtitle() {
        std::string subtitle;
        if (!publisher_.empty())
            subtitle = publisher_ + " · ";
        if (!version_.empty()) {
            subtitle += "v" + version_;
            if (!ignored_ &&
                currentState_ == GameUpdateState::UpdateAvailable &&
                !currentFoundVersion_.empty())
                subtitle += " → v" + currentFoundVersion_;
        }
        subtitle_->setText(subtitle);
    }

    brls::Box* mark_ = nullptr;
    AsyncRgbaImage* image_ = nullptr;
    brls::Label* title_ = nullptr;
    brls::Label* subtitle_ = nullptr;
    brls::Label* chip_ = nullptr;
    InstalledTitle storedTitle_;
    std::string currentIconPath_;
    std::shared_ptr<ImageRequestState> imageState_ =
        std::make_shared<ImageRequestState>();
    std::string titleId_;
    std::string publisher_;
    std::string version_;
    std::string currentFoundVersion_;
    GameUpdateState currentState_ = GameUpdateState::NotChecked;
    bool ignored_ = false;
    OpenMenu onOpenMenu_;
};

class InstalledDataSource : public brls::RecyclerDataSource {
public:
    explicit InstalledDataSource(GameMetadataService* metadata)
        : metadata_(metadata) {}

    void setTitles(std::vector<InstalledTitle> titles) {
        updates_.clear();
        rest_.clear();
        for (InstalledTitle& title : titles) {
            if (isUpdateAvailable(title.titleId))
                updates_.push_back(std::move(title));
            else
                rest_.push_back(std::move(title));
        }
    }

    void setResults(const GameUpdateResults* results) { results_ = results; }
    void setUpdates(GameUpdateService* service) { updateService_ = service; }
    void setOpenMenu(InstalledCell::OpenMenu onOpenMenu) {
        onOpenMenu_ = std::move(onOpenMenu);
    }

    const std::vector<InstalledTitle>& updateTitles() const { return updates_; }

    int numberOfSections(brls::RecyclerFrame*) override {
        if (!updates_.empty() && !rest_.empty())
            return 2;
        return 1;
    }

    int numberOfRows(brls::RecyclerFrame*, int section) override {
        return static_cast<int>(sectionTitles(section).size());
    }

    std::string titleForHeader(brls::RecyclerFrame*, int section) override {
        if (!updates_.empty() && !rest_.empty())
            return section == 0 ? tr("pipensx/nav/updates")
                                : tr("pipensx/installed/section_other");
        if (!updates_.empty() && section == 0)
            return tr("pipensx/nav/updates");
        return "";
    }

    float heightForHeader(brls::RecyclerFrame* recycler, int section) override {
        return titleForHeader(recycler, section).empty() ? 0.0f : 44.0f;
    }

    brls::RecyclerCell* cellForRow(brls::RecyclerFrame* recycler,
                                    brls::IndexPath index) override {
        const std::vector<InstalledTitle>& titles = sectionTitles(index.section);
        const InstalledTitle& title = titles[static_cast<size_t>(index.row)];
        auto* cell = static_cast<InstalledCell*>(
            recycler->dequeueReusableCell("Installed"));
        cell->setTitle(title, metadata_);
        const GameUpdateResult* result = nullptr;
        if (results_) {
            auto it = results_->find(title.titleId);
            if (it != results_->end())
                result = &it->second;
        }
        cell->setResult(result,
                        updateService_ && updateService_->isIgnored(title.titleId),
                        onOpenMenu_);
        return cell;
    }

private:
    bool isUpdateAvailable(const std::string& titleId) const {
        if (updateService_ && updateService_->isIgnored(titleId))
            return false;
        if (!results_)
            return false;
        auto it = results_->find(titleId);
        return it != results_->end() &&
               it->second.state == GameUpdateState::UpdateAvailable;
    }

    const std::vector<InstalledTitle>& sectionTitles(int section) const {
        if (!updates_.empty() && !rest_.empty())
            return section == 0 ? updates_ : rest_;
        return updates_.empty() ? rest_ : updates_;
    }

    GameMetadataService* metadata_;
    GameUpdateService* updateService_ = nullptr;
    std::vector<InstalledTitle> updates_;
    std::vector<InstalledTitle> rest_;
    const GameUpdateResults* results_ = nullptr;
    InstalledCell::OpenMenu onOpenMenu_;
};

inline void setAncestorActionHidden(brls::View* start,
                                    brls::ControllerButton button,
                                    bool hidden) {
    for (brls::View* view = start; view; view = view->getParent()) {
        for (const auto& action : view->getActions()) {
            if (action->getType() != brls::ACTION_GAMEPAD)
                continue;
            if (!(*action == button))
                continue;
            if (action->isHidden() == hidden)
                return;
            view->registerAction(action->getHintText(), button,
                                 action->getActionListener(), hidden);
            return;
        }
    }
}

class InstalledView : public brls::Box {
public:
    InstalledView(InstalledTitleService* installed, DownloadManager* manager,
                  GameMetadataService* metadata, AppSettings* settings,
                  CatalogService* catalog, GameUpdateService* updates,
                  bool checkOnEntry = true,
                  FavoritesService* favorites = nullptr,
                  SwitchDeployService* deploy = nullptr)
        : brls::Box(brls::Axis::COLUMN), installed_(installed),
          manager_(manager), metadata_(metadata), settings_(settings),
          catalog_(catalog), updates_(updates), favorites_(favorites),
          deploy_(deploy), checkOnEntry_(checkOnEntry),
          alive_(std::make_shared<std::atomic<bool>>(true)) {
        status_ = new brls::Label();
        status_->setFontSize(15);
        status_->setMarginTop(10);
        status_->setMarginLeft(34);
        status_->setTextColor(theme::textTertiary());
        addView(status_);

        recycler_ = new brls::RecyclerFrame();
        recycler_->setGrow(1);
        recycler_->setPadding(6, 32, 6, 32);
        recycler_->estimatedRowHeight = 92;
        recycler_->registerCell("Installed", [] { return new InstalledCell(); });
        dataSource_ = new InstalledDataSource(metadata);
        recycler_->setDataSource(dataSource_);
        // Visibility toggles on the host, not the recycler: the host is the
        // grow(1) box, so hiding only the recycler would leave its slot behind.
        recyclerHost_ = recyclerHost(recycler_);
        addView(recyclerHost_);

        dataSource_->setResults(&updates_->results());
        dataSource_->setUpdates(updates_);
        dataSource_->setOpenMenu(
            [this](InstalledTitle title) { openRowMenu(std::move(title)); });
        reload();
        observedUpdateGeneration_ = updates_->generation();
        updatePollTimer_.setCallback([this] { pollUpdateResults(); });
        // Catalog/settings metadata refresh is the scheduled check. A silent
        // pass here only runs when those results no longer match the installed
        // set or the index. Golden pins fixture states with checkOnEntry=false.
        if (checkOnEntry_ && !installed_->titles().empty() &&
            updates_->stale(installed_->generation(),
                            settings_->get().lastMetadataRefreshMs))
            checkAllTitles();
    }

    void willAppear(bool resetState) override {
        brls::Box::willAppear(resetState);
        setAncestorActionHidden(this, brls::BUTTON_BACK, true);
        updatePollTimer_.start(1000);
        pollUpdateResults();
    }

    void willDisappear(bool resetState) override {
        updatePollTimer_.stop();
        setAncestorActionHidden(this, brls::BUTTON_BACK, false);
        brls::Box::willDisappear(resetState);
    }

    void setOnUpdateCount(std::function<void(size_t)> callback) {
        onUpdateCount_ = std::move(callback);
        if (onUpdateCount_)
            onUpdateCount_(dataSource_->updateTitles().size());
    }

    ~InstalledView() override {
        updatePollTimer_.stop();
        alive_->store(false);
    }

private:
    EmptyStateView* ensureEmptyState() {
        if (emptyState_)
            return emptyState_;
        emptyState_ = new EmptyStateView();
            emptyState_->setContent(
                tr("pipensx/installed/empty_title"),
                tr("pipensx/installed/empty_body"),
                tr("pipensx/installed/refresh_action"),
                [this] { refresh(); });
        addView(emptyState_);
        return emptyState_;
    }

    bool hasActiveStreamInstall() const {
        for (const DownloadTask& task : manager_->snapshotUi()) {
            if (task.mode != TransferMode::StreamInstall)
                continue;
            if (task.status == DownloadStatus::Queued ||
                task.status == DownloadStatus::Checking ||
                task.status == DownloadStatus::Fetching ||
                task.status == DownloadStatus::Downloading ||
                task.status == DownloadStatus::Installing ||
                task.status == DownloadStatus::Committing ||
                task.status == DownloadStatus::Verifying)
                return true;
        }
        return false;
    }

    // The main loop re-checks game updates (and refreshes the installed
    // scan) when an install task reaches Installed. That check runs outside
    // this view, so watch the service generation and re-render when it
    // changes — this is what drops an installed update out of the Updates
    // section without a manual refresh.
    void pollUpdateResults() {
        if (updates_->generation() != observedUpdateGeneration_) {
            observedUpdateGeneration_ = updates_->generation();
            reload();
        }
    }

    // "Проверить всё": synchronous in-memory check of every installed
    // title, then persist and re-render. Re-entrancy is guarded by the
    // service itself; the work is microseconds, so no spinner is shown.
    void checkAllTitles() {
        std::string saveError;
        updates_->checkAll(installed_->titles(), installed_->generation(),
                           settings_->get().lastMetadataRefreshMs, saveError);
        if (!saveError.empty())
            diagnostic_error("game_updates", "save", "error=%s",
                             saveError.c_str());
        reload();
    }

    void openRowMenu(InstalledTitle title) {
        std::vector<std::string> labels;
        auto runners =
            std::make_shared<std::vector<std::function<void()>>>();
        auto add = [&](const std::string& label, std::function<void()> run) {
            labels.push_back(label);
            runners->push_back(std::move(run));
        };
        const bool ignored = updates_->isIgnored(title.titleId);
        if (!ignored) {
            auto it = updates_->results().find(title.titleId);
            if (it != updates_->results().end() &&
                it->second.state == GameUpdateState::UpdateAvailable)
                add(tr("pipensx/installed/update_action"),
                    [this, titleId = title.titleId] { installUpdate(titleId); });
        }
        add(tr("pipensx/installed/open_in_catalog"),
            [this, titleId = title.titleId] { openInCatalog(titleId); });
        add(tr(ignored ? "pipensx/installed/unignore_updates"
                       : "pipensx/installed/ignore_updates"),
            [this, titleId = title.titleId, ignored] {
                std::string saveError;
                updates_->setIgnored(titleId, !ignored, saveError);
                if (!saveError.empty())
                    diagnostic_error("game_updates", "save", "error=%s",
                                     saveError.c_str());
                reload();
            });
        add(tr("pipensx/installed/uninstall_action"),
            [this, title] { confirmUninstall(title); });
        auto* dropdown = new brls::Dropdown(
            title.name, labels, [](int) {}, 0,
            [runners](int selected) {
                if (selected < 0 ||
                    selected >= static_cast<int>(runners->size()))
                    return;
                auto run = (*runners)[selected];
                brls::sync([run] { run(); });
            });
        brls::Application::pushActivity(new brls::Activity(dropdown));
    }

    const CatalogEntry* catalogEntryForTitle(const std::string& titleId) const {
        if (!catalog_)
            return nullptr;
        std::vector<const GameMetadata*> entries;
        if (metadata_)
            metadata_->findByTitleId(titleId, entries);
        const std::string want = catalogLower(titleId);
        const CatalogEntry* best = nullptr;
        for (const CatalogEntry& entry : catalog_->entries()) {
            bool match = false;
            if (!entry.titleId.empty() && catalogLower(entry.titleId) == want)
                match = true;
            if (!match) {
                const std::string hash = catalogLower(entry.infoHash);
                for (const GameMetadata* meta : entries) {
                    if (catalogLower(meta->infoHash) == hash) {
                        match = true;
                        break;
                    }
                }
            }
            if (!match)
                continue;
            if (!best || entry.publishedAt > best->publishedAt)
                best = &entry;
        }
        return best;
    }

    void openCatalogPage(const std::string& titleId, bool autoInstall) {
        const CatalogEntry* catalogEntry = catalogEntryForTitle(titleId);
        if (!catalogEntry) {
            brls::Application::notify(
                tr("pipensx/installed/update_no_bundle"));
            return;
        }
        brls::Application::pushActivity(new GameDetailActivity(
            *catalogEntry, "", manager_, metadata_, installed_, settings_,
            [](const std::string&, const std::string&) {},
            [this, alive = alive_] {
                brls::sync([this, alive] {
                    if (alive->load())
                        reload();
                });
            },
            nullptr, favorites_, deploy_, autoInstall));
    }

    void openInCatalog(const std::string& titleId) {
        openCatalogPage(titleId, false);
    }

    void installUpdate(const std::string& titleId) {
        if (refreshing_ || uninstallInFlight_)
            return;
        if (updates_->isIgnored(titleId))
            return;
        openCatalogPage(titleId, true);
    }

    void confirmUninstall(InstalledTitle title) {
        if (refreshing_ || uninstallInFlight_)
            return;
        if (hasActiveStreamInstall()) {
            brls::Application::notify(tr("pipensx/installed/busy"));
            return;
        }
        auto* dialog = new brls::Dialog(
            tr("pipensx/installed/uninstall_confirm", title.name));
        dialog->addButton(tr("pipensx/installed/uninstall_action"),
                          [this, title = std::move(title)] {
            beginUninstall(title);
        });
        dialog->addButton(tr("pipensx/common/cancel"), [] {});
        dialog->open();
    }

    void beginUninstall(const InstalledTitle& title) {
        if (uninstallInFlight_)
            return;
        uninstallInFlight_ = true;
        status_->setText(tr("pipensx/installed/uninstalling", title.name));
        auto alive = alive_;
        InstalledTitleService* installed = installed_;
        const std::string titleId = title.titleId;
        brls::async([this, alive, installed, titleId] {
            std::string error;
            std::string refreshError;
            const bool ok = installed->uninstall(titleId, error, refreshError);
            brls::sync([this, alive, ok, error, refreshError] {
                if (!alive->load())
                    return;
                uninstallInFlight_ = false;
                if (!ok) {
                    status_->setText(error);
                    brls::Application::notify(error);
                    return;
                }
                brls::Application::notify(
                    tr("pipensx/installed/uninstall_done"));
                if (!refreshError.empty()) {
                    status_->setText(refreshError);
                    brls::Application::notify(refreshError);
                    return;
                }
                checkAllTitles();
            });
        });
    }


    void reload() {
        std::vector<InstalledTitle> titles = installed_->titles();
        const size_t count = titles.size();
        dataSource_->setTitles(std::move(titles));
        recycler_->reloadData();
        // reloadData re-renders the focused row, whose A hint may have
        // changed state with it; neither setResult nor updateActionHint fires
        // the hints event, so repaint the bar once here — not per cell on
        // every draw (focus changes repaint it themselves).
        brls::Application::getGlobalHintsUpdateEvent()->fire();
        const bool empty = count == 0;
        if (empty)
            ensureEmptyState()->setVisibility(brls::Visibility::VISIBLE);
        else if (emptyState_)
            emptyState_->setVisibility(brls::Visibility::GONE);
        recyclerHost_->setVisibility(empty ? brls::Visibility::GONE
                                           : brls::Visibility::VISIBLE);
        const size_t updateCount = dataSource_->updateTitles().size();
        std::string text = tr("pipensx/installed/count", count);
        if (updateCount > 0)
            text += "   " + tr("pipensx/updates/count", updateCount);
        if (updates_->stale(installed_->generation(),
                            settings_->get().lastMetadataRefreshMs))
            text += "   " + tr("pipensx/installed/update_stale");
        status_->setText(text);
        if (onUpdateCount_)
            onUpdateCount_(updateCount);
    }

    void refresh() {
        if (refreshing_)
            return;
        if (hasActiveStreamInstall()) {
            brls::Application::notify(
                tr("pipensx/installed/busy"));
            return;
        }
        refreshing_ = true;
        status_->setText(tr("pipensx/installed/refreshing"));
        auto alive = alive_;
        InstalledTitleService* installed = installed_;
        brls::async([this, alive, installed] {
            std::string error;
            bool ok = installed->refresh(error);
            brls::sync([this, alive, ok, error] {
                if (!alive->load())
                    return;
                refreshing_ = false;
                if (!ok) {
                    status_->setText(error);
                    brls::Application::notify(error);
                    return;
                }
                reload();
            });
        });
    }

    InstalledTitleService* installed_;
    DownloadManager* manager_;
    GameMetadataService* metadata_ = nullptr;
    AppSettings* settings_;
    CatalogService* catalog_ = nullptr;
    GameUpdateService* updates_ = nullptr;
    FavoritesService* favorites_ = nullptr;
    SwitchDeployService* deploy_ = nullptr;
    bool checkOnEntry_ = true;
    std::function<void(size_t)> onUpdateCount_;
    brls::Label* status_ = nullptr;
    EmptyStateView* emptyState_ = nullptr;
    brls::RecyclerFrame* recycler_ = nullptr;
    brls::Box* recyclerHost_ = nullptr;
    InstalledDataSource* dataSource_ = nullptr;
    std::shared_ptr<std::atomic<bool>> alive_;
    bool refreshing_ = false;
    bool uninstallInFlight_ = false;
    uint64_t observedUpdateGeneration_ = 0;
    brls::RepeatingTimer updatePollTimer_;
};

}  // namespace pipensx::ui
