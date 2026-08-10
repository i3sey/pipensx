#pragma once

#include <atomic>
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
    using CheckOne = std::function<void(const std::string&, const std::string&)>;
    using InstallOne = std::function<void(const std::string&)>;

    InstalledCell() {
        setFocusable(true);
        setAxis(brls::Axis::ROW);
        setAlignItems(brls::AlignItems::CENTER);
        setPadding(10, 18, 10, 18);
        setHeight(92);

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
        title_->setFontSize(21);
        subtitle_ = new brls::Label();
        subtitle_->setSingleLine(true);
        subtitle_->setFontSize(15);
        subtitle_->setMarginTop(6);
        subtitle_->setTextColor(theme::textTertiary());
        labels->addView(title_);
        labels->addView(subtitle_);
        addView(labels);

        // Update-state chip (Q10 colours): right-aligned coloured label.
        chip_ = new brls::Label();
        chip_->setSingleLine(true);
        chip_->setFontSize(13);
        chip_->setMarginLeft(12);
        chip_->setShrink(0.0f);
        addView(chip_);

        registerClickAction([this](brls::View*) {
            if (titleId_.empty())
                return true;
            if (currentState_ == GameUpdateState::UpdateAvailable &&
                onInstallOne_)
                onInstallOne_(titleId_);
            else if (onCheckOne_)
                onCheckOne_(titleId_, version_);
            return true;
        });
        addGestureRecognizer(new brls::TapGestureRecognizer(this));
    }

    void setTitle(const InstalledTitle& title,
                  GameMetadataService* metadata) {
        title_->setText(title.name);
        titleId_ = title.titleId;
        publisher_ = title.publisher;
        version_ = title.version;
        updateSubtitle();
        setArtworkUrl(image_, metadata, title.iconPath, currentIconPath_,
                      imageState_);
    }

    void setResult(const GameUpdateResult* result, CheckOne onCheckOne,
                   InstallOne onInstallOne) {
        onCheckOne_ = std::move(onCheckOne);
        onInstallOne_ = std::move(onInstallOne);
        const GameUpdateState state =
            result ? result->state : GameUpdateState::NotChecked;
        currentState_ = state;
        currentFoundVersion_ = result ? result->foundVersion : std::string();
        updateSubtitle();
        // The base click action is registered with a generic "OK" hint; A
        // means something different per state, so the hint bar must say so.
        // updateActionHint rewrites the text of the existing action (the
        // click still routes through the tap handler), then repaints the bar.
        updateActionHint(
            brls::BUTTON_A,
            state == GameUpdateState::UpdateAvailable
                ? tr("pipensx/common/install")
                : tr("pipensx/installed/action_check"));
        switch (state) {
        case GameUpdateState::UpdateAvailable:
            chip_->setText(tr("pipensx/installed/update_chip_available"));
            chip_->setTextColor(theme::warning());
            break;
        case GameUpdateState::UpToDate:
            chip_->setText(tr("pipensx/installed/update_chip_latest"));
            chip_->setTextColor(theme::success());
            break;
        case GameUpdateState::CheckError:
            chip_->setText(tr("pipensx/installed/update_chip_error"));
            chip_->setTextColor(theme::error());
            break;
        case GameUpdateState::SourceUnknown:
            chip_->setText(tr("pipensx/installed/update_chip_no_source"));
            chip_->setTextColor(theme::textTertiary());
            break;
        case GameUpdateState::NotChecked:
        case GameUpdateState::Checking:
        default:
            chip_->setText(tr("pipensx/installed/update_chip_not_checked"));
            chip_->setTextColor(theme::textTertiary());
            break;
        }
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
            if (currentState_ == GameUpdateState::UpdateAvailable &&
                !currentFoundVersion_.empty())
                subtitle += " → v" + currentFoundVersion_;
        }
        subtitle_->setText(subtitle);
    }

    AsyncRgbaImage* image_ = nullptr;
    brls::Label* title_ = nullptr;
    brls::Label* subtitle_ = nullptr;
    brls::Label* chip_ = nullptr;
    std::string currentIconPath_;
    std::shared_ptr<ImageRequestState> imageState_ =
        std::make_shared<ImageRequestState>();
    std::string titleId_;
    std::string publisher_;
    std::string version_;
    std::string currentFoundVersion_;
    GameUpdateState currentState_ = GameUpdateState::NotChecked;
    CheckOne onCheckOne_;
    InstallOne onInstallOne_;
};

class InstalledDataSource : public brls::RecyclerDataSource {
public:
    explicit InstalledDataSource(GameMetadataService* metadata)
        : metadata_(metadata) {}

    void setTitles(std::vector<InstalledTitle> titles) {
        titles_ = std::move(titles);
    }

    void setResults(const GameUpdateResults* results) { results_ = results; }
    void setCheckOne(InstalledCell::CheckOne onCheckOne) {
        onCheckOne_ = std::move(onCheckOne);
    }
    void setInstallOne(InstalledCell::InstallOne onInstallOne) {
        onInstallOne_ = std::move(onInstallOne);
    }

    int numberOfRows(brls::RecyclerFrame*, int) override {
        return static_cast<int>(titles_.size());
    }

    brls::RecyclerCell* cellForRow(brls::RecyclerFrame* recycler,
                                    brls::IndexPath index) override {
        const InstalledTitle& title = titles_[static_cast<size_t>(index.row)];
        auto* cell = static_cast<InstalledCell*>(
            recycler->dequeueReusableCell("Installed"));
        cell->setTitle(title, metadata_);
        const GameUpdateResult* result = nullptr;
        if (results_) {
            auto it = results_->find(title.titleId);
            if (it != results_->end())
                result = &it->second;
        }
        cell->setResult(result, onCheckOne_, onInstallOne_);
        return cell;
    }

private:
    GameMetadataService* metadata_;
    std::vector<InstalledTitle> titles_;
    const GameUpdateResults* results_ = nullptr;
    InstalledCell::CheckOne onCheckOne_;
    InstalledCell::InstallOne onInstallOne_;
};

class InstalledView : public brls::Box {
public:
    InstalledView(InstalledTitleService* installed, DownloadManager* manager,
                  GameMetadataService* metadata, AppSettings* settings,
                  CatalogService* catalog, bool checkOnEntry = true)
        : brls::Box(brls::Axis::COLUMN), installed_(installed),
          manager_(manager), metadata_(metadata), settings_(settings),
          catalog_(catalog), checkOnEntry_(checkOnEntry),
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

        updates_ = std::make_unique<GameUpdateService>(
            metadata, installed->rootPath() + "/game-updates.json");
        std::string loadError;
        if (!updates_->load(loadError))
            diagnostic_error("game_updates", "load", "error=%s",
                             loadError.c_str());
        dataSource_->setResults(&updates_->results());
        dataSource_->setCheckOne(
            [this](const std::string& titleId, const std::string& version) {
                checkOneTitle(titleId, version);
            });
        dataSource_->setInstallOne(
            [this](const std::string& titleId) { installUpdate(titleId); });
        reload();
        recheckTimer_.setCallback([this] { pollUpdateRecheck(); });
        // Entering the tab re-checks every installed title so verdicts are
        // fresh without pressing L. Skipped when nothing is installed and
        // disabled in the golden runner, which pins planted fixture states.
        if (checkOnEntry_ && !installed_->titles().empty())
            checkAllTitles();

        registerAction(tr("pipensx/installed/update_check_all"),
                       brls::BUTTON_LB, [this](brls::View*) {
            checkAllTitles();
            return true;
        });
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
            tr("pipensx/installed/refresh_action"), [this] { refresh(); });
        addView(emptyState_);
        return emptyState_;
    }

    bool hasActiveStreamInstall() const {
        for (const DownloadTask& task : manager_->snapshot()) {
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

    // "Проверить всё" (LB): synchronous in-memory check of every installed
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

    // "Проверить" на отдельное приложение (A-тап по строке).
    void checkOneTitle(const std::string& titleId,
                       const std::string& version) {
        std::string saveError;
        updates_->checkOne(titleId, version, saveError);
        if (!saveError.empty())
            diagnostic_error("game_updates", "save", "error=%s",
                             saveError.c_str());
        reload();
    }

    // A-тап по строке "Update available": скачать и установить апдейт.
    void installUpdate(const std::string& titleId) {
        if (updateInFlight_)
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
        // Every update offer lands in the chooser with the recommended
        // packages preselected. The old shortcut — importing straight away
        // when exactly one package carried the update's version — is gone:
        // the user always gets to see (and tune) what an update would pull.
        chooseUpdateFile(preview, path, std::move(initialPeers),
                         selectUpdateFiles(preview, latestVersion, titleId));
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
        for (const DownloadTask& candidate : manager_->snapshot()) {
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
        size_t count = titles.size();
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
        std::string text = tr("pipensx/installed/count", count);
        if (updates_->stale(installed_->generation(),
                            settings_->get().lastMetadataRefreshMs))
            text += "   " + tr("pipensx/installed/update_stale");
        status_->setText(text);
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
    brls::Label* status_ = nullptr;
    EmptyStateView* emptyState_ = nullptr;
    brls::RecyclerFrame* recycler_ = nullptr;
    brls::Box* recyclerHost_ = nullptr;
    InstalledDataSource* dataSource_ = nullptr;
    std::unique_ptr<GameUpdateService> updates_;
    std::shared_ptr<std::atomic<bool>> alive_;
    std::shared_ptr<std::atomic<bool>> cancelled_ =
        std::make_shared<std::atomic<bool>>(false);
    std::atomic<uint32_t> updateTempSerial_{0};
    brls::RepeatingTimer recheckTimer_;
    std::string pendingRecheckTaskId_;
    bool checkOnEntry_ = true;
    bool refreshing_ = false;
    bool updateInFlight_ = false;
    brls::ActionIdentifier updateCancelAction_ = ACTION_NONE;
};

}  // namespace pipensx::ui
