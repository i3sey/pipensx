#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <vector>

#include <borealis.hpp>

#include "app/app_settings.hpp"
#include "app/catalog_service.hpp"
#include "app/download_manager.hpp"
#include "app/game_metadata_service.hpp"
#include "app/game_update_install.hpp"
#include "app/game_update_service.hpp"
#include "app/installed_title_service.hpp"
#include "app/magnet_resolver.hpp"
#include "ui/catalog/catalog_helpers.hpp"
#include "ui/common/async_image.hpp"
#include "ui/common/ui_helpers.hpp"
#include "ui/i18n.hpp"
#include "ui/installed/update_file_chooser.hpp"
#include "ui/theme.hpp"

namespace pipensx::ui {

class InstalledCell : public brls::RecyclerCell {
public:
    using OpenMenu = std::function<void(InstalledTitle)>;
    using InstallOne = std::function<void(const std::string&)>;

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
            if (titleId_.empty())
                return true;
            if (!ignored_ &&
                currentState_ == GameUpdateState::UpdateAvailable &&
                onInstallOne_)
                onInstallOne_(titleId_);
            else if (onOpenMenu_)
                onOpenMenu_(storedTitle_);
            return true;
        });
        addGestureRecognizer(new brls::TapGestureRecognizer(this));

        registerAction(tr("pipensx/common/more"), brls::BUTTON_Y,
                       [this](brls::View*) {
            if (!titleId_.empty() && onOpenMenu_)
                onOpenMenu_(storedTitle_);
            return true;
        });
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
                   OpenMenu onOpenMenu, InstallOne onInstallOne) {
        onOpenMenu_ = std::move(onOpenMenu);
        onInstallOne_ = std::move(onInstallOne);
        ignored_ = ignored;
        const GameUpdateState state =
            result ? result->state : GameUpdateState::NotChecked;
        currentState_ = state;
        currentFoundVersion_ = result ? result->foundVersion : std::string();
        updateSubtitle();
        // The base click action is registered with a generic "OK" hint; A
        // means something different per state, so the hint bar must say so.
        // updateActionHint rewrites the text of the existing action (the
        // click still routes through the tap handler), then repaints the bar.
        const bool available =
            !ignored && state == GameUpdateState::UpdateAvailable;
        updateActionHint(brls::BUTTON_A,
                         available ? tr("pipensx/common/install")
                                   : tr("pipensx/common/more"));
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
    InstallOne onInstallOne_;
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
    void setInstallOne(InstalledCell::InstallOne onInstallOne) {
        onInstallOne_ = std::move(onInstallOne);
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
                        onOpenMenu_, onInstallOne_);
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
    InstalledCell::InstallOne onInstallOne_;
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
                  bool checkOnEntry = true)
        : brls::Box(brls::Axis::COLUMN), installed_(installed),
          manager_(manager), metadata_(metadata), settings_(settings),
          catalog_(catalog), updates_(updates), checkOnEntry_(checkOnEntry),
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
        dataSource_->setInstallOne(
            [this](const std::string& titleId) { installUpdate(titleId); });
        reload();
        recheckTimer_.setCallback([this] { pollUpdateRecheck(); });
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
    }

    void willDisappear(bool resetState) override {
        setAncestorActionHidden(this, brls::BUTTON_BACK, false);
        brls::Box::willDisappear(resetState);
    }

    void setOnUpdateCount(std::function<void(size_t)> callback) {
        onUpdateCount_ = std::move(callback);
        if (onUpdateCount_)
            onUpdateCount_(dataSource_->updateTitles().size());
    }

    ~InstalledView() override {
        recheckTimer_.stop();
        alive_->store(false);
        // Abort an in-flight magnet resolve: without this, tearing the tab
        // down leaves the resolver hammering the network to completion.
        cancelled_->store(true);
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
            title.name, labels, [runners](int selected) {
                if (selected < 0 ||
                    selected >= static_cast<int>(runners->size()))
                    return;
                auto run = (*runners)[selected];
                brls::sync([run] { run(); });
            });
        brls::Application::pushActivity(new brls::Activity(dropdown));
    }

    // A-тап по строке "Update available": скачать и установить апдейт.
    void installUpdate(const std::string& titleId) {
        if (refreshing_ || updateInFlight_ || uninstallInFlight_)
            return;
        if (updates_->isIgnored(titleId))
            return;
        std::vector<const GameMetadata*> entries;
        if (!metadata_ || !metadata_->findByTitleId(titleId, entries)) {
            brls::Application::notify(
                tr("pipensx/installed/update_no_bundle"));
            return;
        }
        if (entries.size() == 1) {
            confirmUpdateInstall(GameMetadata(*entries.front()));
            return;
        }
        // A title with several bundles pages through them one at a time —
        // see chooseBundle. Entries arrive newest-first.
        std::vector<GameMetadata> bundles;
        bundles.reserve(entries.size());
        for (const GameMetadata* entry : entries)
            bundles.push_back(*entry);
        chooseBundle(std::move(bundles), 0);
    }

    void confirmUninstall(InstalledTitle title) {
        if (refreshing_ || updateInFlight_ || uninstallInFlight_)
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

    // brls::Dialog's third button claims a full-width top slot, so paging
    // two bundles at a time put the auxiliary "more" button above both
    // candidates. One bundle per page keeps the hierarchy honest: candidate
    // in the left half, "more"/"later" in the right. Entries arrive
    // newest-first, so the first candidate is the newest release.
    void chooseBundle(std::vector<GameMetadata> bundles, size_t start) {
        auto* dialog = new brls::Dialog(
            tr("pipensx/installed/update_choose_bundle"));
        if (start < bundles.size()) {
            const GameMetadata& candidate = bundles[start];
            // Two bundles of the same title can share a version (different
            // builds); a bare version would then make the buttons identical,
            // so pin the short info-hash suffix onto each twin.
            bool twin = false;
            for (size_t i = 0; i < bundles.size(); ++i)
                if (i != start &&
                    bundles[i].latestVersion == candidate.latestVersion) {
                    twin = true;
                    break;
                }
            dialog->addButton(bundleLabel(candidate, twin),
                              [this, entry = bundles[start]] {
                confirmUpdateInstall(entry);
            });
        }
        const size_t remaining = bundles.size() - start - 1;
        if (remaining > 0)
            dialog->addButton(
                tr("pipensx/installed/update_choose_more", remaining),
                [this, bundles = std::move(bundles), start = start + 1] {
                    chooseBundle(std::move(bundles), start);
                });
        else
            dialog->addButton(tr("pipensx/common/later"), [] {});
        dialog->open();
    }

    // Dialog buttons hold one line and half the dialog width, so a full game
    // name cannot fit reliably: the old byte-capped "name  vN" label still
    // ran over into the neighbouring button, because Cyrillic and Latin
    // letters have different widths and a byte cap has nothing to do with
    // pixels. The dialog is already about one title, so the version alone
    // identifies the candidate — and a short info-hash suffix tells two
    // same-version bundles apart.
    static std::string bundleLabel(const GameMetadata& entry, bool twin) {
        std::string label = "v" + entry.latestVersion;
        if (twin && entry.infoHash.size() >= 8)
            label += " (" + entry.infoHash.substr(0, 8) + ")";
        return label;
    }

    void confirmUpdateInstall(GameMetadata entry) {
        const std::string foundVersion =
            entry.latestVersion.empty() ? std::string("?") : entry.latestVersion;
        auto* dialog = new brls::Dialog(tr(
            "pipensx/installed/update_install_confirm", entry.name,
            foundVersion));
        dialog->addButton(tr("pipensx/installed/update_download"),
                          [this, entry = std::move(entry)] {
            beginUpdateInstall(entry);
        });
        dialog->addButton(tr("pipensx/common/later"), [] {});
        dialog->open();
    }

    // Резолв magnet'а в .torrent (паттерн из game_detail), затем импорт
    // торрента с установкой только файлов-апдейтов.
    void beginUpdateInstall(GameMetadata entry) {
        if (updateInFlight_)
            return;
        updateInFlight_ = true;
        cancelled_->store(false);
        status_->setText(tr("pipensx/installed/update_resolving"));
        // While the resolve is in flight no other action can start, so Y is
        // the cancel: it flips the flag the resolver polls, and the
        // completion path unlinks the tmp torrent and says so in a toast.
        // Re-registering in a later beginUpdateInstall replaces the action;
        // unregistering on completion removes the hint from the bar.
        updateCancelAction_ = registerAction(
            tr("pipensx/installed/update_cancel"), brls::BUTTON_Y,
            [this](brls::View*) {
                cancelled_->store(true);
                return true;
            });
        const std::string hash = entry.infoHash;
        const CatalogEntry* catalogEntry =
            catalog_ ? catalog_->findByInfoHash(hash) : nullptr;
        const std::string magnet = updateMagnetFor(hash, catalogEntry);
        std::vector<uint8_t> infoDict =
            catalogEntry ? catalogEntry->infoDict : std::vector<uint8_t>();
        const std::string tmp = manager_->rootPath() + "/_update_tmp_" +
                                catalogLower(hash) + "_" +
                                std::to_string(updateTempSerial_.fetch_add(1)) +
                                ".torrent";
        auto alive = alive_;
        auto cancelled = cancelled_;
        const std::string latestVersion = entry.latestVersion;
        const std::string titleId = entry.titleId;
        brls::async([this, alive, cancelled, magnet, tmp,
                     infoDict = std::move(infoDict), latestVersion, titleId] {
            std::string err;
            MagnetResolver resolver;
            auto progress = [this, alive](const pipensx::MagnetProgress& p) {
                std::string text;
                switch (p.stage) {
                    case pipensx::MagnetProgress::Stage::FindingPeers:
                        text = tr("pipensx/detail/finding_peers");
                        break;
                    case pipensx::MagnetProgress::Stage::Connecting:
                        text = tr("pipensx/detail/contacting_peer",
                                  p.peerIndex, p.peerCount);
                        break;
                    case pipensx::MagnetProgress::Stage::FetchingMetadata:
                        text = tr("pipensx/detail/fetching_metadata",
                                  p.completedPieces, p.totalPieces);
                        break;
                    case pipensx::MagnetProgress::Stage::Validating:
                        text = tr("pipensx/detail/validating");
                        break;
                }
                brls::sync([this, alive, text] {
                    if (alive->load())
                        status_->setText(text);
                });
            };
            std::vector<uint8_t> initialPeers;
            const bool ok = resolver.resolveToFile(
                magnet, tmp, *cancelled, progress, err, &initialPeers,
                infoDict.empty() ? nullptr : &infoDict);
            brls::sync([this, alive, ok, err = std::move(err), tmp,
                        initialPeers = std::move(initialPeers),
                        latestVersion, titleId]() mutable {
                if (!alive->load()) {
                    ::unlink(tmp.c_str());
                    return;
                }
                updateInFlight_ = false;
                if (updateCancelAction_ != ACTION_NONE) {
                    unregisterAction(updateCancelAction_);
                    updateCancelAction_ = ACTION_NONE;
                }
                if (!ok) {
                    ::unlink(tmp.c_str());
                    reload();
                    // A user cancel is not an error: the resolver reports it
                    // as a failure, so distinguish it from a genuine one
                    // before the diagnostic and the toast.
                    if (cancelled_->load()) {
                        brls::Application::notify(
                            tr("pipensx/installed/update_cancelled"));
                        return;
                    }
                    diagnostic_error("game_updates", "resolve",
                                     "title error=%s", err.c_str());
                    brls::Application::notify(resolveErrorToast(err));
                    return;
                }
                finishUpdateImport(tmp, std::move(initialPeers),
                                   latestVersion, titleId);
            });
        });
    }

    // The resolver's errors are English diagnostic strings; what the user
    // sees must be localized. Classify by the failure modes it actually
    // emits (magnet_resolver.cpp resolveToFile) and keep the raw string in
    // the diagnostic log either way.
    static std::string resolveErrorToast(const std::string& err) {
        const auto has = [&err](const char* needle) {
            return err.find(needle) != std::string::npos;
        };
        if (has("not registered anymore"))
            return tr("pipensx/installed/update_error_unregistered");
        if (has("no usable peers") || has("could not connect to any of them") ||
            has("none returned"))
            return tr("pipensx/installed/update_error_no_peers");
        if (has("rejected"))
            return tr("pipensx/installed/update_error_rejected");
        return tr("pipensx/installed/update_error_failed");
    }

    void finishUpdateImport(const std::string& path,
                            std::vector<uint8_t> initialPeers,
                            const std::string& latestVersion,
                            const std::string& titleId) {
        TorrentPreview preview;
        std::string err;
        if (!manager_->previewTorrent(path, preview, err)) {
            ::unlink(path.c_str());
            diagnostic_error("game_updates", "preview", "error=%s",
                             err.c_str());
            brls::Application::notify(
                tr("pipensx/installed/update_error_preview"));
            reload();
            return;
        }
        std::vector<uint8_t> actions =
            selectUpdateFiles(preview, latestVersion, titleId);
        // Every update offer lands in the chooser with the recommended
        // packages preselected. The old shortcut — importing straight away
        // when exactly one package carried the update's version — is gone:
        // the user always gets to see (and tune) what an update would pull.
        chooseUpdateFile(preview, path, std::move(initialPeers),
                         std::move(actions));
    }

    // The tmp torrent stays alive until the choice lands; the chooser hands
    // the bootstrap peers straight back into the import, so a resolved
    // torrent never loses its only way to start where the tracker is
    // unreachable. Both exits (confirm and cancel) come back here, where the
    // tmp torrent is owned. `actions` is the recommendation mask from
    // selectUpdateFiles — the rows open with it preselected.
    void chooseUpdateFile(const TorrentPreview& preview,
                          const std::string& path,
                          std::vector<uint8_t> initialPeers,
                          std::vector<uint8_t> actions) {
        auto alive = alive_;
        brls::Application::pushActivity(new UpdateFileChooserActivity(
            manager_, preview, std::move(actions), std::move(initialPeers),
            [this, alive, preview, path](std::vector<uint8_t> mask,
                                         std::vector<uint8_t> peers) {
                if (!alive->load()) {
                    ::unlink(path.c_str());
                    return;
                }
                importUpdateTorrent(preview, path, std::move(peers),
                                    std::move(mask));
            },
            [this, alive, path] {
                ::unlink(path.c_str());
                if (alive->load())
                    reload();
            }));
    }

    void importUpdateTorrent(const TorrentPreview& preview,
                             const std::string& path,
                             std::vector<uint8_t> initialPeers,
                             std::vector<uint8_t> actions) {
        std::string id;
        std::string err;
        if (manager_->importTorrentActions(path, actions, id, err,
                                           initialPeers)) {
            log_msg("[game_updates] imported update torrent %s\n", id.c_str());
            brls::Application::notify(
                tr("pipensx/installed/update_added"));
            // Once the task settles (installed, failed or removed) refresh
            // the installed list and re-check, so the row flips to Latest
            // without another manual press.
            pendingRecheckTaskId_ = catalogLower(id);
            recheckTimer_.start(1000);
        } else if (err.find("already in the download manager") !=
                   std::string::npos) {
            brls::Application::notify(
                tr("pipensx/detail/already_in_downloads"));
        } else {
            diagnostic_error("game_updates", "import", "error=%s",
                             err.c_str());
            brls::Application::notify(
                tr("pipensx/installed/update_error_import"));
        }
        ::unlink(path.c_str());
        reload();
    }

    // UI-thread tick while an update task we started is in flight: wait for
    // a terminal state, then refresh installed titles and re-check them.
    void pollUpdateRecheck() {
        if (pendingRecheckTaskId_.empty())
            return;
        bool found = false;
        DownloadStatus status = DownloadStatus::Queued;
        for (const DownloadTask& candidate : manager_->snapshotUi()) {
            if (catalogLower(candidate.id) == pendingRecheckTaskId_) {
                found = true;
                status = candidate.status;
                break;
            }
        }
        if (!updateRecheckSettled(found, status))
            return;
        // Another stream install still running: the installed scan would
        // race it (same reason RB refresh refuses), keep polling.
        if (hasActiveStreamInstall() || refreshing_)
            return;
        recheckTimer_.stop();
        pendingRecheckTaskId_.clear();
        recheckAfterInstall();
    }

    // Refresh the installed list, then re-check every title. Mirrors
    // refresh() but always re-checks on success; callers ensure no stream
    // install is active and no other refresh is in flight.
    void recheckAfterInstall() {
        refreshing_ = true;
        status_->setText(tr("pipensx/installed/refreshing"));
        auto alive = alive_;
        InstalledTitleService* installed = installed_;
        brls::async([this, alive, installed] {
            std::string error;
            const bool ok = installed->refresh(error);
            brls::sync([this, alive, ok, error] {
                if (!alive->load())
                    return;
                refreshing_ = false;
                if (!ok) {
                    status_->setText(error);
                    brls::Application::notify(error);
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
    bool checkOnEntry_ = true;
    std::function<void(size_t)> onUpdateCount_;
    brls::Label* status_ = nullptr;
    EmptyStateView* emptyState_ = nullptr;
    brls::RecyclerFrame* recycler_ = nullptr;
    brls::Box* recyclerHost_ = nullptr;
    InstalledDataSource* dataSource_ = nullptr;
    std::shared_ptr<std::atomic<bool>> alive_;
    std::shared_ptr<std::atomic<bool>> cancelled_ =
        std::make_shared<std::atomic<bool>>(false);
    std::atomic<uint32_t> updateTempSerial_{0};
    brls::RepeatingTimer recheckTimer_;
    std::string pendingRecheckTaskId_;
    bool refreshing_ = false;
    bool updateInFlight_ = false;
    bool uninstallInFlight_ = false;
    brls::ActionIdentifier updateCancelAction_ = ACTION_NONE;
};

}  // namespace pipensx::ui
