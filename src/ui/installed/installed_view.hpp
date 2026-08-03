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
    // req #4: current version always, found version when an update is known.
    void updateSubtitle() {
        std::string subtitle = publisher_;
        if (!subtitle.empty())
            subtitle += "   ";
        subtitle += titleId_;
        if (!version_.empty()) {
            subtitle += "  v" + version_;
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
        return titles_.empty() ? 1 : static_cast<int>(titles_.size());
    }

    brls::RecyclerCell* cellForRow(brls::RecyclerFrame* recycler,
                                    brls::IndexPath index) override {
        if (titles_.empty()) {
            auto* cell = static_cast<TextMessageCell*>(
                recycler->dequeueReusableCell("Message"));
            cell->setMessage(tr("pipensx/installed/empty_cell"));
            return cell;
        }
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
                  CatalogService* catalog)
        : brls::Box(brls::Axis::COLUMN), installed_(installed),
          manager_(manager), metadata_(metadata), settings_(settings),
          catalog_(catalog),
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
        recycler_->registerCell("Message", [] { return new TextMessageCell(); });
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

        registerAction(tr("pipensx/common/refresh"), brls::BUTTON_RB,
                       [this](brls::View*) {
            refresh();
            return true;
        });
        registerAction(tr("pipensx/installed/update_check_all"),
                       brls::BUTTON_LB, [this](brls::View*) {
            checkAllTitles();
            return true;
        });
    }

    ~InstalledView() override { alive_->store(false); }

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
        // brls::Dialog fits at most three buttons, so a title with several
        // bundles pages through them: two bundle buttons + "Other bundles…",
        // which opens the next page. Entries arrive newest-first.
        std::vector<GameMetadata> bundles;
        bundles.reserve(entries.size());
        for (const GameMetadata* entry : entries)
            bundles.push_back(*entry);
        chooseBundle(std::move(bundles), 0);
    }

    void chooseBundle(std::vector<GameMetadata> bundles, size_t start) {
        auto* dialog = new brls::Dialog(
            tr("pipensx/installed/update_choose_bundle"));
        size_t shown = 0;
        for (size_t i = start; i < bundles.size() && shown < 2; ++i, ++shown)
            dialog->addButton(bundleLabel(bundles[i]),
                              [this, entry = bundles[i]] {
                confirmUpdateInstall(std::move(entry));
            });
        const size_t remaining = bundles.size() - start - shown;
        if (remaining > 0)
            dialog->addButton(
                tr("pipensx/installed/update_choose_more", remaining),
                [this, bundles = std::move(bundles), start = start + shown] {
                    chooseBundle(std::move(bundles), start);
                });
        else
            dialog->addButton(tr("pipensx/common/later"), [] {});
        dialog->open();
    }

    static std::string bundleLabel(const GameMetadata& entry) {
        std::string label = entry.name;
        if (!entry.latestVersion.empty())
            label += "  v" + entry.latestVersion;
        // Dialog buttons hold one line; a 60-char cap keeps the label from
        // overflowing the dialog width on long game names.
        constexpr size_t kMaxLabel = 60;
        if (label.size() > kMaxLabel)
            label.resize(kMaxLabel - 1);
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
            beginUpdateInstall(std::move(entry));
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
        brls::async([this, alive, cancelled, magnet, tmp,
                     infoDict = std::move(infoDict)] {
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
                        initialPeers = std::move(initialPeers)]() mutable {
                if (!alive->load()) {
                    ::unlink(tmp.c_str());
                    return;
                }
                updateInFlight_ = false;
                if (!ok) {
                    ::unlink(tmp.c_str());
                    diagnostic_error("game_updates", "resolve",
                                     "title error=%s", err.c_str());
                    brls::Application::notify(err);
                    reload();
                    return;
                }
                finishUpdateImport(tmp, std::move(initialPeers));
            });
        });
    }

    void finishUpdateImport(const std::string& path,
                            std::vector<uint8_t> initialPeers) {
        TorrentPreview preview;
        std::string err;
        if (!manager_->previewTorrent(path, preview, err)) {
            ::unlink(path.c_str());
            brls::Application::notify(err);
            reload();
            return;
        }
        const std::vector<uint8_t> actions = selectUpdateFiles(preview);
        std::string id;
        if (manager_->importTorrentActions(path, actions, id, err,
                                           initialPeers)) {
            log_msg("[game_updates] imported update torrent %s\n", id.c_str());
            brls::Application::notify(
                tr("pipensx/installed/update_added"));
        } else if (err.find("already in the download manager") !=
                   std::string::npos) {
            brls::Application::notify(
                tr("pipensx/detail/already_in_downloads"));
        } else {
            diagnostic_error("game_updates", "import", "error=%s",
                             err.c_str());
            brls::Application::notify(err);
        }
        ::unlink(path.c_str());
        reload();
    }

    void reload() {
        std::vector<InstalledTitle> titles = installed_->titles();
        size_t count = titles.size();
        dataSource_->setTitles(std::move(titles));
        recycler_->reloadData();
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
    bool refreshing_ = false;
    bool updateInFlight_ = false;
};

}  // namespace pipensx::ui
