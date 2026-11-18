#pragma once

#include <algorithm>
#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#include <borealis.hpp>

#include "app/app_settings.hpp"
#include "app/download_manager.hpp"
#include "app/pending_debrid.hpp"
#include "app/torbox_client.hpp"
#include "ui/common/stick_scrolling_frame.hpp"
#include "ui/common/ui_helpers.hpp"
#include "ui/debrid_ui.hpp"
#include "ui/i18n.hpp"
#include "ui/settings/settings_cells.hpp"
#include "ui/theme.hpp"

namespace pipensx::ui {

inline bool retainOrRemoveDebridTransfer(
    DownloadManager* manager, bool keepUncached, bool reusedExisting,
    DebridProviderKind providerKind, const std::string& key,
    const std::string& debridId, const std::string& infoHash,
    const std::string& title, const DebridInfo& info) {
    if (debridId.empty())
        return false;
    if (manager) {
        if (auto existingTask = manager->snapshotUi(infoHash)) {
            if (existingTask->debridId == debridId)
                return false;
        }
    }
    if (info.phase == DebridInfo::Phase::Failed) {
        if (manager) {
            std::string ignored;
            manager->pendingDebrid().remove(providerKind, infoHash, ignored);
        }
        removeDebridTransferAsync(providerKind, key, debridId);
        return false;
    }
    if ((keepUncached && info.phase != DebridInfo::Phase::Ready) ||
        reusedExisting) {
        if (manager) {
            PendingDebridEntry rec;
            rec.infoHash = infoHash;
            rec.debridId = debridId;
            rec.provider = providerKind;
            rec.title = !info.name.empty() ? info.name : title;
            rec.progress = info.progress;
            rec.ready = info.phase == DebridInfo::Phase::Ready &&
                        !info.files.empty();
            std::string ignored;
            manager->pendingDebrid().upsert(rec, ignored);
        }
        return true;
    }
    removeDebridTransferAsync(providerKind, key, debridId);
    return false;
}

inline void refreshPendingDebridAsync(
    AppSettings* settings, DownloadManager* manager,
    std::function<void(bool ok, std::string error)> onDone = {}) {
    if (!settings || !manager) {
        if (onDone)
            onDone(true, {});
        return;
    }
    manager->pruneExpiredPendingDebrid();
    const std::vector<PendingDebridEntry> entries =
        manager->pendingDebrid().list();
    if (entries.empty()) {
        if (onDone)
            onDone(true, {});
        return;
    }
    const AppSettingsData values = settings->get();
    brls::async([manager, entries, values,
                 onDone = std::move(onDone)]() mutable {
        bool allOk = true;
        std::string firstError;

        std::vector<PendingDebridEntry> torboxEntries;
        std::vector<PendingDebridEntry> otherEntries;
        for (const PendingDebridEntry& entry : entries) {
            if (entry.provider == DebridProviderKind::TorBox)
                torboxEntries.push_back(entry);
            else
                otherEntries.push_back(entry);
        }

        if (!torboxEntries.empty() && !values.torboxApiKey.empty()) {
            TorboxClient client(values.torboxApiKey);
            std::vector<TorboxTorrentInfo> items;
            std::string listError;
            if (client.fetchList(items, listError)) {
                auto rank = [](const TorboxTorrentInfo& item) {
                    return std::tuple<bool, bool, bool, double, uint64_t>(
                        !TorboxClient::isFailedState(item.state),
                        item.ready && !item.files.empty(),
                        !item.files.empty(),
                        item.progress,
                        item.id);
                };
                for (const PendingDebridEntry& entry : torboxEntries) {
                    const TorboxTorrentInfo* matched = nullptr;
                    for (const TorboxTorrentInfo& item : items) {
                        if (std::to_string(item.id) == entry.debridId) {
                            matched = &item;
                            break;
                        }
                    }
                    if (!matched) {
                        for (const TorboxTorrentInfo& item : items) {
                            if (item.hash == entry.infoHash &&
                                (!matched || rank(item) > rank(*matched))) {
                                matched = &item;
                            }
                        }
                    }
                    std::string ignored;
                    if (!matched) {
                        manager->pendingDebrid().remove(
                            entry.provider, entry.infoHash, ignored);
                    } else if (TorboxClient::isFailedState(matched->state)) {
                        client.remove(matched->id, ignored);
                        manager->pendingDebrid().remove(
                            entry.provider, entry.infoHash, ignored);
                    } else {
                        PendingDebridEntry updated = entry;
                        updated.debridId = std::to_string(matched->id);
                        if (!matched->name.empty())
                            updated.title = matched->name;
                        updated.progress = matched->progress;
                        updated.ready = matched->ready &&
                                        !matched->files.empty();
                        manager->pendingDebrid().upsert(updated, ignored);
                    }
                }
            } else {
                allOk = false;
                if (firstError.empty())
                    firstError = listError;
            }
        }

        for (const PendingDebridEntry& entry : otherEntries) {
            const std::string& key = debridKeyFor(values, entry.provider);
            if (key.empty())
                continue;
            DebridInfo info;
            std::string err;
            auto provider = makeDebridProvider(entry.provider, key);
            if (provider->fetchInfo(entry.debridId, info, err)) {
                std::string ignored;
                if (info.phase == DebridInfo::Phase::Failed) {
                    provider->remove(entry.debridId, ignored);
                    manager->pendingDebrid().remove(
                        entry.provider, entry.infoHash, ignored);
                } else {
                    PendingDebridEntry updated = entry;
                    if (!info.name.empty())
                        updated.title = info.name;
                    updated.progress = info.progress;
                    updated.ready = info.phase == DebridInfo::Phase::Ready &&
                                    !info.files.empty();
                    manager->pendingDebrid().upsert(updated, ignored);
                }
            } else if (isDebridNotFoundError(err)) {
                std::string ignored;
                manager->pendingDebrid().remove(
                    entry.provider, entry.infoHash, ignored);
            } else {
                allOk = false;
                if (firstError.empty())
                    firstError = err;
            }
        }

        brls::sync([allOk, firstError = std::move(firstError),
                    onDone = std::move(onDone)]() mutable {
            if (onDone)
                onDone(allOk, std::move(firstError));
        });
    });
}

class PendingDebridView : public brls::Box {
public:
    PendingDebridView(AppSettings* settings, DownloadManager* manager,
                      std::function<void()> onChanged = {})
        : brls::Box(brls::Axis::COLUMN), settings_(settings),
          manager_(manager), onChanged_(std::move(onChanged)),
          alive_(std::make_shared<std::atomic<bool>>(true)) {
        setGrow(1);

        auto* scroll = new StickScrollingFrame();
        scroll->setGrow(1);

        content_ = new theme::SettingsContent();
        content_->setPadding(24, 40, 24, 40);

        statusLabel_ = new brls::Label();
        statusLabel_->setFontSize(theme::kFontSmall);
        statusLabel_->setSingleLine(false);
        statusLabel_->setMarginBottom(12);
        statusLabel_->setFocusable(false);
        content_->addView(statusLabel_);

        listBox_ = new brls::Box(brls::Axis::COLUMN);
        listBox_->setFocusable(false);
        content_->addView(listBox_);

        scroll->setContentView(content_);
        addView(scroll);

        registerAction(tr("pipensx/common/refresh"), brls::BUTTON_X,
                       [this](brls::View*) {
            refresh();
            return true;
        });

        rebuild();
        refresh();
    }

    ~PendingDebridView() override {
        alive_->store(false);
    }

    void willAppear(bool resetState) override {
        brls::Box::willAppear(resetState);
        ensureValidFocus();
    }

    static void push(AppSettings* settings, DownloadManager* manager,
                     std::function<void()> onChanged = {}) {
        auto* view = new PendingDebridView(settings, manager,
                                           std::move(onChanged));
        auto* frame = new brls::AppletFrame(view);
        frame->setTitle(tr("pipensx/settings/pending_debrid_title"));
        brls::Application::pushActivity(new brls::Activity(frame));
    }

private:
    void refresh() {
        if (!settings_ || !manager_ || refreshing_ || removing_)
            return;
        refreshing_ = true;
        lastError_.clear();
        updateStatusBanner();
        auto alive = alive_;
        refreshPendingDebridAsync(
            settings_, manager_,
            [this, alive](bool ok, std::string error) {
                if (!alive->load())
                    return;
                refreshing_ = false;
                if (!ok && !error.empty())
                    lastError_ = formatDebridValidateError(error);
                if (onChanged_)
                    onChanged_();
                rebuild();
            });
    }

    void updateStatusBanner() {
        if (!statusLabel_)
            return;
        if (refreshing_) {
            statusLabel_->setText(tr("pipensx/debrid/pending_refreshing"));
            statusLabel_->setTextColor(theme::textSecondary());
            statusLabel_->setVisibility(brls::Visibility::VISIBLE);
        } else if (!lastError_.empty()) {
            statusLabel_->setText(lastError_);
            statusLabel_->setTextColor(theme::color(theme::Tok::ChipError));
            statusLabel_->setVisibility(brls::Visibility::VISIBLE);
        } else if (!manager_ || manager_->pendingDebrid().list().empty()) {
            statusLabel_->setText(tr("pipensx/debrid/pending_empty"));
            statusLabel_->setTextColor(theme::textSecondary());
            statusLabel_->setVisibility(brls::Visibility::VISIBLE);
        } else {
            statusLabel_->setVisibility(brls::Visibility::GONE);
        }
    }

    void rebuild() {
        if (!listBox_)
            return;
        const bool hadFocus = viewContains(
            this, brls::Application::getCurrentFocus());
        firstFocusable_ = nullptr;
        listBox_->clearViews();

        const auto entries = manager_
            ? manager_->pendingDebrid().list()
            : std::vector<PendingDebridEntry>{};
        updateStatusBanner();

        if (entries.empty()) {
            auto* refreshCell = actionCell(
                tr("pipensx/common/refresh"), "", [this] { refresh(); });
            listBox_->addView(refreshCell);
            firstFocusable_ = refreshCell;
            if (hadFocus)
                ensureValidFocus();
            return;
        }

        for (const PendingDebridEntry& entry : entries) {
            const std::string displayTitle =
                entry.title.empty() ? entry.infoHash : entry.title;
            const char* providerName = debridProviderName(entry.provider);
            const std::string detail = entry.ready
                ? tr("pipensx/debrid/pending_item_ready", providerName)
                : tr("pipensx/debrid/pending_item_caching", providerName,
                     percentOf(static_cast<float>(entry.progress)));
            auto* cell = actionCell(displayTitle, detail,
                                    [this, entry, displayTitle] {
                confirmRemoveOne(entry, displayTitle);
            });
            if (!firstFocusable_)
                firstFocusable_ = cell;
            listBox_->addView(cell);
        }

        auto* clearAllCell = actionCell(
            tr("pipensx/debrid/pending_clear_all"), "",
            [this, count = entries.size()] {
                confirmClearAll(count);
            });
        clearAllCell->setMarginTop(12);
        listBox_->addView(clearAllCell);

        if (hadFocus)
            ensureValidFocus();
    }

    void ensureValidFocus() {
        if (!firstFocusable_)
            return;
        brls::View* current = brls::Application::getCurrentFocus();
        if (!viewContains(this, current))
            brls::Application::giveFocus(firstFocusable_);
    }

    void confirmRemoveOne(const PendingDebridEntry& entry,
                          const std::string& displayTitle) {
        if (removing_)
            return;
        auto* dialog = new brls::Dialog(
            tr("pipensx/debrid/pending_remove_confirm", displayTitle));
        dialog->addButton(tr("pipensx/debrid/pending_remove_one"),
                          [this, entry] { removeOne(entry); });
        dialog->addButton(tr("pipensx/common/cancel"), [] {});
        dialog->open();
    }

    void confirmClearAll(size_t count) {
        if (removing_ || count == 0)
            return;
        auto* dialog = new brls::Dialog(
            tr("pipensx/debrid/pending_clear_all_confirm", count));
        dialog->addButton(tr("pipensx/common/clear"),
                          [this] { clearAll(); });
        dialog->addButton(tr("pipensx/common/cancel"), [] {});
        dialog->open();
    }

    void removeOne(const PendingDebridEntry& entry) {
        if (!settings_ || !manager_ || removing_)
            return;
        removing_ = true;
        auto alive = alive_;
        const AppSettingsData values = settings_->get();
        DownloadManager* manager = manager_;
        brls::async([this, alive, manager, values, entry] {
            const std::string& key = debridKeyFor(values, entry.provider);
            std::string error;
            bool ok = true;
            if (!key.empty()) {
                ok = makeDebridProvider(entry.provider, key)
                         ->remove(entry.debridId, error);
            }
            const bool removeLocal = ok || isDebridNotFoundError(error);
            if (removeLocal) {
                std::string storeErr;
                if (!manager->pendingDebrid().remove(
                        entry.provider, entry.infoHash, storeErr)) {
                    error = storeErr;
                }
            }
            brls::sync([this, alive, removeLocal,
                        error = std::move(error)]() mutable {
                if (!alive->load())
                    return;
                removing_ = false;
                if (removeLocal) {
                    lastError_.clear();
                    brls::Application::notify(
                        tr("pipensx/debrid/pending_removed_notify"));
                    if (onChanged_)
                        onChanged_();
                    rebuild();
                } else {
                    lastError_ = formatDebridValidateError(error);
                    updateStatusBanner();
                    brls::Application::notify(lastError_);
                }
            });
        });
    }

    void clearAll() {
        if (!settings_ || !manager_ || removing_)
            return;
        const auto entries = manager_->pendingDebrid().list();
        if (entries.empty())
            return;
        removing_ = true;
        auto alive = alive_;
        const AppSettingsData values = settings_->get();
        DownloadManager* manager = manager_;
        brls::async([this, alive, manager, values, entries] {
            size_t removedCount = 0;
            std::string firstError;
            for (const PendingDebridEntry& entry : entries) {
                const std::string& key = debridKeyFor(values, entry.provider);
                std::string error;
                bool ok = true;
                if (!key.empty()) {
                    ok = makeDebridProvider(entry.provider, key)
                             ->remove(entry.debridId, error);
                }
                if (ok || isDebridNotFoundError(error)) {
                    std::string storeErr;
                    if (manager->pendingDebrid().remove(
                            entry.provider, entry.infoHash, storeErr)) {
                        ++removedCount;
                    } else if (firstError.empty()) {
                        firstError = storeErr;
                    }
                } else if (firstError.empty()) {
                    firstError = error;
                }
            }
            brls::sync([this, alive, removedCount,
                        firstError = std::move(firstError)]() mutable {
                if (!alive->load())
                    return;
                removing_ = false;
                if (removedCount > 0 && onChanged_)
                    onChanged_();
                if (!firstError.empty()) {
                    lastError_ = formatDebridValidateError(firstError);
                    brls::Application::notify(lastError_);
                } else {
                    lastError_.clear();
                    brls::Application::notify(
                        tr("pipensx/debrid/pending_removed_notify"));
                }
                rebuild();
            });
        });
    }

    AppSettings* settings_;
    DownloadManager* manager_;
    std::function<void()> onChanged_;
    std::shared_ptr<std::atomic<bool>> alive_;
    brls::Box* content_ = nullptr;
    brls::Label* statusLabel_ = nullptr;
    brls::Box* listBox_ = nullptr;
    brls::View* firstFocusable_ = nullptr;
    bool refreshing_ = false;
    bool removing_ = false;
    std::string lastError_;
};

} // namespace pipensx::ui
