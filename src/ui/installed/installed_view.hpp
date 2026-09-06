#pragma once

#include <algorithm>
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
#include "app/game_update_install.hpp"
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
        displayVersion_ = title.displayVersion;
        hasMods_ = title.hasLayeredFsMods;
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
    // B3: raw decimal title versions ("v327680") read as garbage — show
    // the eShop x.y.z form ("v5.0.0"), keeping the raw text when it is
    // not a decimal version.
    // B6: append the NACP display_version in parens ("v5.0.0 (1.26.30)")
    // and a mods marker when LayeredFS mods are installed, so the row
    // states what an update would replace before the user taps it.
    static std::string prettyVersion(const std::string& decimal) {
        const std::string formatted = formatTitleVersion(decimal);
        return formatted.empty() ? decimal : formatted;
    }
    void updateSubtitle() {
        std::string subtitle;
        if (!publisher_.empty())
            subtitle = publisher_ + " · ";
        if (!version_.empty()) {
            subtitle += "v" + prettyVersion(version_);
            if (!displayVersion_.empty())
                subtitle += " (" + displayVersion_ + ")";
            if (!ignored_ &&
                currentState_ == GameUpdateState::UpdateAvailable &&
                !currentFoundVersion_.empty())
                subtitle += " → v" + prettyVersion(currentFoundVersion_);
            if (hasMods_)
                subtitle += " · " + tr("pipensx/installed/mods_suffix");
        } else if (hasMods_) {
            subtitle += tr("pipensx/installed/mods_suffix");
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
    std::string displayVersion_;
    bool hasMods_ = false;
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
    size_t titleCount() const { return updates_.size() + rest_.size(); }

    std::string titleIdAt(brls::IndexPath index) const {
        if (index.section >= sectionCount() || index.row < 0)
            return {};
        const auto& titles = sectionTitles(static_cast<int>(index.section));
        return static_cast<size_t>(index.row) < titles.size()
            ? titles[static_cast<size_t>(index.row)].titleId
            : std::string{};
    }

    bool indexForTitle(const std::string& titleId,
                       brls::IndexPath& result) const {
        if (!titleId.empty()) {
            for (size_t section = 0; section < sectionCount(); ++section) {
                const auto& titles =
                    sectionTitles(static_cast<int>(section));
                for (size_t row = 0; row < titles.size(); ++row)
                    if (titles[row].titleId == titleId) {
                        result = brls::IndexPath(section, row);
                        return true;
                    }
            }
        }
        return false;
    }

    brls::IndexPath fallbackIndex(brls::IndexPath preferred) const {
        if (titleCount() == 0)
            return brls::IndexPath(0, 0);
        const size_t section =
            std::min(preferred.section, sectionCount() - 1);
        const auto& titles = sectionTitles(static_cast<int>(section));
        const int row = std::clamp(
            preferred.row, 0, static_cast<int>(titles.size()) - 1);
        return brls::IndexPath(section, row);
    }

    int numberOfSections(brls::RecyclerFrame*) override {
        return static_cast<int>(sectionCount());
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
    size_t sectionCount() const {
        return !updates_.empty() && !rest_.empty() ? 2 : 1;
    }

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
                  SwitchDeployService* deploy = nullptr,
                  PortUninstallService* portUninstall = nullptr)
        : brls::Box(brls::Axis::COLUMN), installed_(installed),
          manager_(manager), metadata_(metadata), settings_(settings),
          catalog_(catalog), updates_(updates), favorites_(favorites),
          deploy_(deploy), portUninstall_(portUninstall),
          checkOnEntry_(checkOnEntry),
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
            return;
        }
        if (pendingReload_ && !activityStackHasOverlay())
            flushPendingReload();
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
                    [this, titleId = title.titleId,
                     foundVersion = it->second.foundVersion] {
                        installUpdate(titleId, foundVersion);
                    });
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

    const CatalogEntry* catalogEntryForTitle(
        const std::string& titleId,
        const std::string& foundVersion = {}) const {
        if (!catalog_)
            return nullptr;
        std::vector<const GameMetadata*> entries;
        if (metadata_)
            metadata_->findByTitleId(titleId, entries);
        // B3: the update check names the wanted version — open the bundle
        // carrying it, not the newest-published one (usually the base
        // game), so "install update" does not download the whole game.
        if (!foundVersion.empty() && metadata_) {
            if (const GameMetadata* match =
                    GameMetadataService::preferVersionMatch(entries,
                                                           foundVersion)) {
                if (const CatalogEntry* direct =
                        catalog_->findByInfoHash(match->infoHash))
                    return direct;
            }
        }
        const CatalogEntry* best = nullptr;
        for (const GameMetadata* meta : entries) {
            if (!meta)
                continue;
            const CatalogEntry* candidate =
                catalog_->findByInfoHash(meta->infoHash);
            if (candidate &&
                (!best || candidate->publishedAt > best->publishedAt))
                best = candidate;
        }
        if (best)
            return best;

        // Metadata-backed titles use the O(1) info-hash index above. Keep the
        // direct title-id fallback for older/custom catalogues without metadata.
        const std::string want = catalogLower(titleId);
        for (const CatalogEntry& entry : catalog_->entries()) {
            if (entry.titleId.empty() ||
                catalogLower(entry.titleId) != want)
                continue;
            if (!best || entry.publishedAt > best->publishedAt)
                best = &entry;
        }
        return best;
    }

    void openCatalogPage(const std::string& titleId, bool autoInstall,
                         const std::string& foundVersion = {}) {
        const CatalogEntry* catalogEntry =
            catalogEntryForTitle(titleId, foundVersion);
        if (!catalogEntry) {
            brls::Application::notify(
                tr("pipensx/installed/update_no_bundle"));
            return;
        }
        CatalogEntry entry = *catalogEntry;
        if (!titleId.empty())
            entry.titleId = titleId;
        brls::Application::pushActivity(new GameDetailActivity(
            std::move(entry), "", manager_, metadata_, installed_, settings_,
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

    void installUpdate(const std::string& titleId,
                       const std::string& foundVersion = {}) {
        if (refreshing_ || uninstallInFlight_)
            return;
        if (updates_->isIgnored(titleId))
            return;
        // B6: state what is installed before replacing it. The row subtitle
        // already carries the installed version (+ display_version) and the
        // mods badge, and the detail page repeats them in its facts table;
        // here, a title with LayeredFS mods opens the detail page for
        // manual review (no one-tap) with a toast naming the mods folder,
        // so a BOTW/Broforce-style stale-mod break stops being silent.
        // Plain titles keep the catalog one-tap flow the installed-bundles
        // behaviour check pins.
        bool needReview = false;
        if (installed_) {
            for (const InstalledTitle& candidate : installed_->titles()) {
                if (candidate.titleId == titleId) {
                    const UpdatePreflight pre = describeUpdatePreflight(
                        candidate, foundVersion, true);
                    needReview = pre.warnMods();
                    break;
                }
            }
        }
        if (needReview)
            brls::Application::notify(
                tr("pipensx/installed/update_preflight_mods", titleId));
        openCatalogPage(titleId, /*autoInstall=*/!needReview, foundVersion);
    }

    void confirmUninstall(InstalledTitle title) {
        if (refreshing_ || uninstallInFlight_)
            return;
        if (hasActiveStreamInstall()) {
            brls::Application::notify(tr("pipensx/installed/busy"));
            return;
        }
        PortUninstallPlan portPlan;
        if (planPortUninstall(title, portPlan)) {
            // Removing a port means deleting deployed files, so a copy or
            // extraction running against /switch would race the deletion.
            if (deploy_ && deploy_->snapshot().active()) {
                brls::Application::notify(tr("pipensx/installed/busy"));
                return;
            }
            openPortUninstallDialog(std::move(title), std::move(portPlan));
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

    // Port detection: the service matches receipts under
    // appRoot/deployments/ by the recorded title ids (or the forwarder
    // package name in the task manifest for older receipts); the metadata
    // infohashes only back ordinary NSP titles. No match means an ordinary
    // install, and Uninstall keeps its plain behaviour.
    bool planPortUninstall(const InstalledTitle& title,
                           PortUninstallPlan& plan) {
        if (!portUninstall_)
            return false;
        std::vector<std::string> hashes;
        if (metadata_) {
            std::vector<const GameMetadata*> entries;
            metadata_->findByTitleId(title.titleId, entries);
            for (const GameMetadata* meta : entries)
                if (meta && !meta->infoHash.empty())
                    hashes.push_back(meta->infoHash);
        }
        return portUninstall_->plan(title.titleId, hashes, plan);
    }

    // One confirmation dialog with the full breakdown: the ncm shortcut, the
    // deployed files (or, for a v1 receipt whose archive is gone, the folder
    // that will be removed entirely) and the download task with its data.
    void openPortUninstallDialog(InstalledTitle title,
                                 PortUninstallPlan plan) {
        auto* box = new brls::Box(brls::Axis::COLUMN);
        box->setPadding(
            brls::Application::getStyle()["brls/dialog/paddingTopBottom"],
            brls::Application::getStyle()["brls/dialog/paddingLeftRight"],
            brls::Application::getStyle()["brls/dialog/paddingTopBottom"],
            brls::Application::getStyle()["brls/dialog/paddingLeftRight"]);
        auto addLine = [box](const std::string& text, bool primary) {
            auto* label = new brls::Label();
            label->setFontSize(primary
                ? brls::Application::getStyle()["brls/dialog/fontSize"]
                : theme::kFontSmall);
            label->setHorizontalAlign(brls::HorizontalAlign::CENTER);
            label->setSingleLine(false);
            if (!primary) {
                label->setTextColor(theme::textSecondary());
                label->setMarginTop(14);
            }
            label->setText(text);
            box->addView(label);
        };
        addLine(tr("pipensx/installed/uninstall_confirm", title.name), true);
        if (!plan.switchFiles.empty())
            addLine(tr("pipensx/installed/uninstall_port_files",
                       plan.switchFiles.size(),
                       formatBytes(plan.switchBytes)),
                    false);
        if (!plan.sdRootFiles.empty())
            addLine(tr("pipensx/installed/uninstall_layered_files",
                       plan.sdRootFiles.size(),
                       formatBytes(plan.sdRootBytes)),
                    false);
        for (const std::string& folder : plan.wholeFolders)
            addLine(tr("pipensx/installed/uninstall_port_folder", folder),
                    false);
        if (plan.hasTask)
            addLine(tr("pipensx/installed/uninstall_port_task"), false);
        auto* dialog = new brls::Dialog(box);
        dialog->addButton(
            tr("pipensx/installed/uninstall_action"),
            [this, title = std::move(title), plan = std::move(plan)] {
                beginPortUninstall(title, plan);
            });
        dialog->addButton(tr("pipensx/common/cancel"), [] {});
        dialog->open();
    }

    void beginPortUninstall(const InstalledTitle& title,
                            const PortUninstallPlan& plan) {
        if (uninstallInFlight_)
            return;
        uninstallInFlight_ = true;
        status_->setText(
            tr("pipensx/installed/uninstall_port_working", title.name));
        auto alive = alive_;
        InstalledTitleService* installed = installed_;
        PortUninstallService* service = portUninstall_;
        const std::string titleId = title.titleId;
        const std::string titleName = title.name;
        brls::async([this, alive, installed, service, titleId, titleName,
                     plan] {
            PortUninstallReport report;
            const bool ok = service->uninstallPort(
                plan,
                [installed, titleId](std::string& error) {
                    std::string refreshError;
                    return installed->uninstall(titleId, error,
                                                refreshError);
                },
                report);
            brls::sync([this, alive, ok, report, titleName] {
                if (!alive->load())
                    return;
                uninstallInFlight_ = false;
                if (ok) {
                    brls::Application::notify(tr(
                        "pipensx/installed/uninstall_port_done", titleName));
                    checkAllTitles();
                    return;
                }
                const std::string error = report.error.empty()
                    ? tr("pipensx/installed/uninstall_port_failed")
                    : report.error;
                status_->setText(error);
                brls::Application::notify(error);
            });
        });
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

    void reloadRecycler(const std::string& focusedTitleId,
                        brls::IndexPath previousIndex, bool ownsFocus) {
        brls::IndexPath focusedIndex;
        if (!dataSource_->indexForTitle(focusedTitleId, focusedIndex))
            focusedIndex = dataSource_->fallbackIndex(previousIndex);
        recycler_->setDefaultCellFocus(focusedIndex);
        recycler_->reloadData();
        // reloadData re-renders the focused row, whose A hint may have
        // changed state with it; neither setResult nor updateActionHint fires
        // the hints event, so repaint the bar once here — not per cell on
        // every draw (focus changes repaint it themselves).
        brls::Application::getGlobalHintsUpdateEvent()->fire();
        const size_t count = dataSource_->titleCount();
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
        if (ownsFocus) {
            if (empty) {
                brls::Application::giveFocus(ensureEmptyState());
            } else {
                recycler_->selectRowAt(focusedIndex, false);
                brls::Application::giveFocus(recycler_);
            }
        }
    }

    void flushPendingReload() {
        if (!pendingReload_ || activityStackHasOverlay())
            return;
        pendingReload_ = false;
        std::string focusedTitleId = std::move(pendingFocusedTitleId_);
        const brls::IndexPath focusedIndex = pendingFocusedIndex_;
        pendingFocusedTitleId_.clear();
        pendingFocusedIndex_ = brls::IndexPath(0, 0);
        reloadRecycler(focusedTitleId, focusedIndex,
                       viewContains(this,
                                    brls::Application::getCurrentFocus()));
    }

    void reload() {
        brls::View* focused = brls::Application::getCurrentFocus();
        const bool ownsFocus = viewContains(this, focused);
        std::string focusedTitleId = pendingReload_
            ? pendingFocusedTitleId_
            : std::string{};
        brls::IndexPath focusedIndex =
            pendingReload_ ? pendingFocusedIndex_ : brls::IndexPath(0, 0);
        if (focusedTitleId.empty()) {
            brls::View* rowFocus =
                ownsFocus ? focused : recycler_->getDefaultFocus();
            if (auto* cell = dynamic_cast<brls::RecyclerCell*>(rowFocus)) {
                focusedIndex = cell->getIndexPath();
                focusedTitleId =
                    dataSource_->titleIdAt(cell->getIndexPath());
            }
        }

        dataSource_->setTitles(installed_->titles());
        if (activityStackHasOverlay() && !ownsFocus) {
            pendingFocusedTitleId_ = std::move(focusedTitleId);
            pendingFocusedIndex_ = focusedIndex;
            pendingReload_ = true;
            return;
        }

        pendingReload_ = false;
        pendingFocusedTitleId_.clear();
        pendingFocusedIndex_ = brls::IndexPath(0, 0);
        reloadRecycler(focusedTitleId, focusedIndex, ownsFocus);
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
    PortUninstallService* portUninstall_ = nullptr;
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
    bool pendingReload_ = false;
    std::string pendingFocusedTitleId_;
    brls::IndexPath pendingFocusedIndex_{0, 0};
    uint64_t observedUpdateGeneration_ = 0;
    brls::RepeatingTimer updatePollTimer_;
};

}  // namespace pipensx::ui
