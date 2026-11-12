#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <borealis.hpp>
#include "ui/common/smooth_recycler.hpp"

#include "app/app_settings.hpp"
#include "app/download_list_refresh.hpp"
#include "app/download_manager.hpp"
#include "app/game_metadata_service.hpp"
#include "app/storage_manager.hpp"
#include "app/switch_deploy.hpp"
#include "ui/common/message_cells.hpp"
#include "ui/common/ui_helpers.hpp"
#include "ui/i18n.hpp"
#include "ui/theme.hpp"
#include "ui/downloads/details_activity.hpp"
#include "ui/downloads/download_cell.hpp"
#include "ui/downloads/file_picker.hpp"

namespace pipensx::ui {

class MainView;

class DownloadDataSource : public brls::RecyclerDataSource {
public:
    explicit DownloadDataSource(MainView* owner) : owner_(owner) {}

    void setTasks(std::vector<DownloadTask> tasks,
                  std::string activeDeployTask = {});
    const DownloadTask* taskAt(brls::IndexPath index) const;
    std::string taskIdAt(brls::IndexPath index) const;
    brls::IndexPath indexForTask(const std::string& taskId) const;
    int numberOfSections(brls::RecyclerFrame*) override;
    int numberOfRows(brls::RecyclerFrame*, int section) override;
    std::string titleForHeader(brls::RecyclerFrame*, int section) override;
    float heightForHeader(brls::RecyclerFrame*, int section) override;
    brls::RecyclerCell* cellForRow(brls::RecyclerFrame* recycler,
                                    brls::IndexPath index) override;
    void didSelectRowAt(brls::RecyclerFrame*, brls::IndexPath index) override;

private:
    struct Section {
        std::string title;
        std::vector<size_t> rows;
    };
    MainView* owner_;
    std::vector<DownloadTask> tasks_;
    std::vector<Section> sections_;
};

class MainView : public brls::Box {
public:
    MainView(DownloadManager* manager, GameMetadataService* metadata,
             AppSettings* settings, SwitchDeployService* deploy = nullptr,
             CatalogService* catalog = nullptr)
        : brls::Box(brls::Axis::COLUMN), manager_(manager), metadata_(metadata),
          settings_(settings), deploy_(deploy), catalog_(catalog) {
        auto* toolbar = new brls::Box(brls::Axis::ROW);
        toolbar->setPadding(16, 32, 8, 32);
        toolbar->setAlignItems(brls::AlignItems::CENTER);
        auto* heading = new brls::Label();
        heading->setText(tr("pipensx/downloads/workspace_title"));
        heading->setFontSize(theme::kFontHeading);
        heading->setGrow(1);
        toolbar->addView(heading);
        addButton_ = new theme::Button();
        auto* add = addButton_;
        add->setText(tr("pipensx/downloads/add_download"));
        add->setMarginRight(16);
        add->registerClickAction([this](brls::View*) {
            openFilePicker();
            return true;
        });
        toolbar->addView(add);
        auto* actions = new theme::Button();
        actions->setText(tr("pipensx/downloads/manage"));
        actions->registerClickAction([this](brls::View*) {
            openManageMenu();
            return true;
        });
        toolbar->addView(actions);
        addView(toolbar);

        filters_ = new brls::Box(brls::Axis::ROW);
        auto* filters = filters_;
        filters->setPadding(0, 32, 12, 32);
        const char* keys[] = {"filter_all", "filter_active", "filter_queue",
                              "filter_finished", "filter_errors"};
        for (int i = 0; i < 5; ++i) {
            auto* button = new theme::Button();
            button->setFontSize(theme::kFontSmall);
            button->setHeight(40);
            button->setCornerRadius(theme::kRadiusSmall);
            button->setMarginRight(i == 4 ? 0 : 12);
            button->registerClickAction([this, i](brls::View*) {
                selectedFilter_ = i;
                pendingReload_ = true;
                refresh();
                if (!tasks_.empty())
                    brls::Application::giveFocus(recycler_);
                return true;
            });
            filterButtons_.push_back(button);
            filterKeys_.push_back(std::string("pipensx/downloads/") + keys[i]);
            filters->addView(button);
        }
        addView(filters);
        summary_ = new brls::Label();
        summary_->setFontSize(theme::kFontCaption);
        summary_->setMarginLeft(32);
        summary_->setMarginRight(32);
        summary_->setMarginBottom(8);
        summary_->setTextColor(theme::textSecondary());
        addView(summary_);

        recycler_ = new SmoothRecyclerFrame();
        recycler_->setGrow(1);
        recycler_->setPadding(6, 32, 6, 32);
        recycler_->estimatedRowHeight = DownloadCell::kHeight;
        recycler_->registerCell("Download", [] { return new DownloadCell(); });
        recycler_->registerCell("Message", [] { return new MessageCell(); });
        dataSource_ = new DownloadDataSource(this);
        recycler_->setDataSource(dataSource_);
        recyclerHost_ = recyclerHost(recycler_);
        addView(recyclerHost_);
        registerAction(tr("pipensx/downloads/add_download"), brls::BUTTON_X,
                       [this](brls::View*) {
            openFilePicker();
            return true;
        });
        registerAction(tr("pipensx/downloads/manage"), brls::BUTTON_Y,
                       [this](brls::View*) {
            openManageMenu();
            return true;
        });
        refresh();
        timer_.setCallback([this] {
            refresh();
            if (fastRefresh_) {
                fastRefresh_ = false;
                timer_.setPeriod(750);
            }
        });
        startRefreshing();
    }

    brls::View* getDefaultFocus() override {
        if (recycler_ && !tasks_.empty())
            return recycler_->getDefaultFocus();
        return addButton_;
    }

    void willAppear(bool resetState) override {
        brls::Box::willAppear(resetState);
        startRefreshing();
    }

    void willDisappear(bool resetState) override {
        timer_.stop();
        brls::Box::willDisappear(resetState);
    }

    ~MainView() override {
        alive_->store(false);
        timer_.stop();
    }

    void openDetails(const std::string& taskId) {
        brls::Application::pushActivity(
            new DetailsActivity(taskId, manager_, deploy_, settings_));
    }

    void openFilePicker() {
        brls::Application::pushActivity(
            new FilePickerActivity(manager_, settings_));
    }

    // O7: per-row context menu on A. Replaces the old blind Y/X hotkeys — the
    // items are labelled and only the ones valid for the current status appear.
    void openRowMenu(const std::string& taskId) {
        auto found = manager_->snapshotUi(taskId);
        if (!found)
            return;
        DownloadTask task = std::move(*found);
        const SwitchDeploySnapshot deployState = deploy_ ? deploy_->snapshot()
                                                         : SwitchDeploySnapshot{};
        const bool leased = deployState.active() && deployState.taskId == taskId;
        const bool needInspect =
            !leased && (task.status == DownloadStatus::Completed ||
                        task.status == DownloadStatus::Installed) &&
            deploy_ && taskReadyForSwitchDeploy(task);
        if (needInspect) {
            SwitchDeployService* deploy = deploy_;
            auto alive = alive_;
            brls::async([this, alive, task = std::move(task), taskId, deploy,
                         leased]() mutable {
                SwitchDeployInspection inspection = deploy->inspect(taskId);
                brls::sync([this, alive, task = std::move(task), taskId, leased,
                            inspection = std::move(inspection)]() mutable {
                    if (!alive->load())
                        return;
                    showRowMenu(taskId, task, leased, &inspection);
                });
            });
            return;
        }
        showRowMenu(taskId, task, leased, nullptr);
    }

    void showRowMenu(const std::string& taskId, const DownloadTask& task,
                     bool leased, const SwitchDeployInspection* inspection) {
        std::vector<std::string> labels;
        auto runners =
            std::make_shared<std::vector<std::function<void()>>>();
        auto add = [&](const std::string& label, std::function<void()> run) {
            labels.push_back(label);
            runners->push_back(std::move(run));
        };

        add(tr("pipensx/common/details"), [this, taskId] { openDetails(taskId); });
        if (leased)
            add(tr("pipensx/deploy/cancel"), [this] { deploy_->cancel(); });

        const TaskCapabilities caps = taskCapabilities(task, leased);
        if (caps.pause.allowed)
            add(tr("pipensx/common/pause"), [this, taskId] {
                std::string error;
                if (!manager_->pause(taskId, error))
                    brls::Application::notify(taskActionReasonText(error));
                startRefreshing(true);
            });
        if (caps.resume.allowed)
            add(tr("pipensx/common/resume"), [this, taskId] {
                std::string error;
                if (!manager_->resume(taskId, error))
                    brls::Application::notify(taskActionReasonText(error));
                startRefreshing(true);
            });
        if (inspection && switchDeployFullyInstalled(*inspection)) {
            add(tr("pipensx/deploy/installed"), [this, taskId] {
                openDetails(taskId);
            });
        } else if (inspection &&
                   switchDeployOffersCopy(inspection->problem)) {
            add(tr("pipensx/deploy/install_port"),
                [this, inspection = *inspection]() mutable {
                if (!deploy_)
                    return;
                brls::Application::pushActivity(
                    new SwitchDeployPreviewActivity(std::move(inspection),
                                                    deploy_));
            });
        }
        if (task.status == DownloadStatus::Completed &&
            (task.mode == TransferMode::DownloadOnly ||
             task.mode == TransferMode::PortInstall) &&
            taskHasPortOrArchiveFiles(manager_->rootPath(), task)) {
            add(tr("pipensx/port_install/how_to_install"), [this, taskId] {
                openPortNoticeDialog([this, taskId] { openDetails(taskId); });
            });
        }
        if (caps.verify.allowed)
            add(tr("pipensx/common/verify"), [this, taskId] {
                std::string error;
                if (manager_->verify(taskId, error)) {
                    brls::Application::notify(tr("pipensx/downloads/verify_started"));
                } else {
                    brls::Application::notify(taskActionReasonText(error));
                }
                startRefreshing(true);
            });
        if (caps.move.allowed) {
            std::vector<std::string> queuedIds;
            for (const auto& candidate : manager_->snapshotUi())
                if (candidate.status == DownloadStatus::Queued)
                    queuedIds.push_back(candidate.id);
            size_t pos = 0;
            bool foundQueued = false;
            for (size_t i = 0; i < queuedIds.size(); ++i)
                if (queuedIds[i] == taskId) {
                    pos = i;
                    foundQueued = true;
                    break;
                }
            if (foundQueued) {
                if (pos > 0)
                    add(tr("pipensx/downloads/move_up"), [this, taskId] {
                        std::string error;
                        if (!manager_->moveTask(taskId, true, error) &&
                            !error.empty()) {
                            brls::Application::notify(tr(
                                "pipensx/downloads/move_to_top_failed", error));
                        }
                        startRefreshing(true);
                    });
                if (pos + 1 < queuedIds.size())
                    add(tr("pipensx/downloads/move_down"), [this, taskId] {
                        std::string error;
                        if (!manager_->moveTask(taskId, false, error) &&
                            !error.empty()) {
                            brls::Application::notify(tr(
                                "pipensx/downloads/move_to_top_failed", error));
                        }
                        startRefreshing(true);
                    });
                if (pos != 0)
                    add(tr("pipensx/downloads/move_to_top"), [this, taskId] {
                        std::string error;
                        if (!manager_->moveToFront(taskId, error) &&
                            !error.empty()) {
                            brls::Application::notify(tr(
                                "pipensx/downloads/move_to_top_failed", error));
                        }
                        startRefreshing(true);
                    });
            }
        }
        if (caps.remove.allowed)
            add(tr("pipensx/common/remove"),
                    [this, taskId] { openRemoveDialog(taskId); });

        auto* dropdown = new brls::Dropdown(
            task.name, labels, [runners](int selected) {
                if (selected < 0 ||
                    selected >= static_cast<int>(runners->size()))
                    return;
                auto run = (*runners)[selected];
                brls::sync([run] { run(); });
            });
        brls::Application::pushActivity(new brls::Activity(dropdown));
    }

    void openRemoveDialog(const std::string& taskId) {
        auto* dialog =
            new brls::Dialog(tr("pipensx/downloads/remove_question"));
        dialog->addButton(tr("pipensx/downloads/remove_keep"), [this, taskId] {
            std::string error;
            if (!manager_->remove(taskId, false, error))
                brls::Application::notify(error);
            else
                startRefreshing(true);
        });
        dialog->addButton(tr("pipensx/downloads/remove_delete"), [this, taskId] {
            DownloadManager* manager = manager_;
            brls::async([manager, taskId] {
                std::string error;
                const bool ok = manager->remove(taskId, true, error);
                brls::sync([ok, error] {
                    if (!ok)
                        brls::Application::notify(error);
                });
            });
            startRefreshing(true);
        });
        dialog->addButton(tr("pipensx/common/cancel"), [] {});
        dialog->open();
    }

    void startRefreshing(bool fast = false) {
        timer_.stop();
        fastRefresh_ = fast;
        timer_.start(fast ? 100 : 750);
    }

    void stopRefreshing() {
        timer_.stop();
    }

    const std::string& rootPath() const { return manager_->rootPath(); }

    GameMetadataService* metadataService() const { return metadata_; }
    CatalogService* catalogService() const { return catalog_; }
    const SwitchDeploySnapshot& deploySnapshot() const {
        return deploySnapshot_;
    }

private:
    EmptyStateView* ensureEmptyState() {
        if (emptyState_)
            return emptyState_;
        emptyState_ = new EmptyStateView();
        emptyState_->setContent(
            tr(selectedFilter_ == 0 ? "pipensx/downloads/empty_title"
                                    : "pipensx/downloads/filter_empty"),
            tr("pipensx/downloads/empty_body"),
            tr("pipensx/downloads/import_action"),
            [this] { openFilePicker(); });
        addView(emptyState_);
        return emptyState_;
    }

    bool hasPausableTask(const std::vector<DownloadTask>& tasks,
                         const std::string& leasedId) const {
        for (const DownloadTask& task : tasks)
            if (taskCapabilities(task, task.id == leasedId).pause.allowed)
                return true;
        return false;
    }

    bool matchesFilter(const DownloadTask& task,
                       const std::string& leasedId, int filter) const {
        const bool finished = finishedDownloadStatus(task.status);
        switch (filter) {
            case 1:
                return task.id == leasedId || (!finished &&
                    task.status != DownloadStatus::Queued &&
                    task.status != DownloadStatus::Error);
            case 2: return task.status == DownloadStatus::Queued;
            case 3: return finished && task.id != leasedId;
            case 4: return task.status == DownloadStatus::Error;
            default: return true;
        }
    }

    void openManageMenu() {
        const auto tasks = manager_->snapshotUi();
        const bool pausable = hasPausableTask(tasks,
                                             manager_->externalDeployTaskId());
        std::vector<std::string> labels = {
            tr(pausable ? "pipensx/downloads/pause_all"
                        : "pipensx/downloads/resume_all"),
            tr("pipensx/downloads/clear_completed",
               planClearCompleted(tasks, manager_->externalDeployTaskId()).clearable),
            tr("pipensx/downloads/clear_files"),
            tr(settings_ && settings_->get().blankScreenDuringTransfers
                ? "pipensx/downloads/screen_mode_on"
                : "pipensx/downloads/screen_mode_off")};
        auto alive = alive_;
        auto* menu = new brls::Dropdown(tr("pipensx/downloads/manage"), labels,
            [this, alive, pausable](int selected) {
                brls::sync([this, alive, pausable, selected] {
                    if (!alive->load())
                        return;
                    if (selected == 0) {
                        if (pausable) pauseAll(); else resumeAll();
                    } else if (selected == 1) confirmClearEntries();
                    else if (selected == 2) confirmClearFiles();
                    else if (selected == 3 && settings_) {
                        auto values = settings_->get();
                        values.blankScreenDuringTransfers = !values.blankScreenDuringTransfers;
                        std::string error;
                        if (!settings_->update(values, error))
                            brls::Application::notify(error);
                        refresh();
                    }
                });
            });
        brls::Application::pushActivity(new brls::Activity(menu));
    }

    void confirmClearEntries() {
        if (clearInFlight_)
            return;
        const ClearCompletedPlan plan = planClearCompleted(
            manager_->snapshotUi(), manager_->externalDeployTaskId());
        if (plan.clearable == 0) {
            brls::Application::notify(plan.skippedBusy == 0
                ? tr("pipensx/downloads/clear_completed_none")
                : tr("pipensx/downloads/clear_result", size_t{0},
                     plan.skippedBusy));
            return;
        }
        auto* dialog = new brls::Dialog(
            tr("pipensx/downloads/clear_completed_question",
               plan.clearable, plan.skippedBusy));
        dialog->addButton(tr("pipensx/common/clear"), [this] {
            runClear(false);
        });
        dialog->addButton(tr("pipensx/common/cancel"), [] {});
        dialog->open();
    }

    void confirmClearFiles() {
        if (clearInFlight_)
            return;
        const std::string leased = manager_->externalDeployTaskId();
        const std::vector<DownloadTask> tasks = manager_->snapshotUi();
        const ClearCompletedPlan plan = planClearCompleted(tasks, leased);
        if (plan.clearable == 0) {
            brls::Application::notify(plan.skippedBusy == 0
                ? tr("pipensx/downloads/clear_completed_none")
                : tr("pipensx/downloads/clear_files_result", size_t{0},
                     plan.skippedBusy));
            return;
        }
        std::vector<std::string> paths;
        paths.reserve(plan.clearable);
        for (const DownloadTask& task : tasks) {
            if (classifyClearCompleted(task.status, task.id, leased) ==
                ClearCompletedClass::Clear)
                paths.push_back(task.dataPath);
        }
        const size_t clearable = plan.clearable;
        const size_t skipped = plan.skippedBusy;
        auto alive = alive_;
        brls::async([this, alive, paths = std::move(paths), clearable,
                     skipped]() {
            uint64_t bytes = 0;
            for (const std::string& path : paths) {
                uint64_t size = 0;
                if (!directorySize(path, size))
                    continue;
                bytes = size > UINT64_MAX - bytes ? UINT64_MAX : bytes + size;
            }
            brls::sync([this, alive, bytes, clearable, skipped] {
                if (!alive->load() || clearInFlight_)
                    return;
                auto* dialog = new brls::Dialog(
                    tr("pipensx/downloads/clear_files_question",
                       clearable, formatBytes(bytes), skipped));
                dialog->addButton(tr("pipensx/downloads/remove_delete"),
                                  [this] { runClear(true); });
                dialog->addButton(tr("pipensx/common/cancel"), [] {});
                dialog->open();
            });
        });
    }

    void runClear(bool deleteData) {
        if (clearInFlight_)
            return;
        clearInFlight_ = true;
        DownloadManager* manager = manager_;
        auto alive = alive_;
        brls::async([this, alive, manager, deleteData] {
            std::string error;
            ClearCompletedResult result;
            const bool ok = manager->clearCompleted(deleteData, error, &result);
            brls::sync([this, alive, ok, error, result, deleteData] {
                if (!alive->load())
                    return;
                clearInFlight_ = false;
                if (!ok) {
                    if (!error.empty())
                        brls::Application::notify(error);
                    startRefreshing(true);
                    return;
                }
                brls::Application::notify(tr(
                    deleteData ? "pipensx/downloads/clear_files_result"
                               : "pipensx/downloads/clear_result",
                    result.cleared, result.skippedBusy));
                startRefreshing(true);
            });
        });
    }

    void pauseAll() {
        manager_->pauseAll();
        startRefreshing(true);
    }

    void resumeAll() {
        manager_->resumeAll();
        startRefreshing(true);
    }

    // One Y action that flips with the queue: pause the active tasks, or resume
    // the paused/failed ones when nothing is running.
    void pauseResumeAll() {
        const SwitchDeploySnapshot deploy = deploy_ ? deploy_->snapshot()
                                                    : SwitchDeploySnapshot{};
        const std::string leased = deploy.active() ? deploy.taskId
                                                   : std::string();
        if (hasPausableTask(manager_->snapshotUi(), leased))
            pauseAll();
        else
            resumeAll();
    }

    std::string summaryText(const std::vector<DownloadTask>& tasks) const {
        const pipensx::QueueSummary s = summarizeQueue(tasks, now_ms());
        std::string text = tr("pipensx/downloads/summary_uploaded",
                              formatBytes(s.uploadedBytes));
        if (s.uploadSpeedBps)
            text += "   ↑ " + formatSpeed(s.uploadSpeedBps);
        if (s.downloading || s.queued || s.installing)
            text += "   " + tr("pipensx/downloads/summary_counts",
                              s.downloading, s.queued, s.installing);
        if (s.paused)
            text += "   " + tr("pipensx/downloads/summary_paused", s.paused);
        if (s.errors)
            text += "   " + tr("pipensx/downloads/summary_errors", s.errors);
        const uint64_t speed = s.downloadSpeedBps;
        if (speed)
            text += "\n" +
                    tr("pipensx/downloads/summary_speed", formatSpeed(speed));
        if (s.installSpeedBps)
            text += "   " + tr("pipensx/downloads/speed_install",
                                formatSpeed(s.installSpeedBps));
        if (s.etaSeconds)
            text += "   " + tr("pipensx/downloads/summary_eta",
                               formatEtaSeconds(s.etaSeconds));
        return text;
    }

    void refresh() {
        const uint64_t startedUs = telemetry_enabled() ? now_us() : 0;
        std::string persistenceError;
        if (manager_->takePersistenceError(persistenceError))
            brls::Application::notify(persistenceError);
        auto logRefresh = [startedUs](bool structural, bool reload) {
            if (!startedUs)
                return;
            const uint64_t durationUs = now_us() - startedUs;
            if (!structural && durationUs < 1000)
                return;
            telemetry_log(
                "ui", "downloads",
                "event=refresh duration_us=%llu structural=%d reload=%d",
                (unsigned long long)durationUs, structural ? 1 : 0,
                reload ? 1 : 0);
        };
        auto next = manager_->snapshotUi();
        const SwitchDeploySnapshot deployState = deploy_ ? deploy_->snapshot()
                                                         : SwitchDeploySnapshot{};
        const std::string activeDeployTask = deployState.active()
            ? deployState.taskId : std::string();
        std::vector<size_t> filterCounts(filterButtons_.size());
        size_t lastVisibleFilter = 0;
        bool filterVisibilityChanged = false;
        for (size_t i = 0; i < filterButtons_.size(); ++i) {
            filterCounts[i] = std::count_if(next.begin(), next.end(),
                [this, &activeDeployTask, i](const DownloadTask& task) {
                    return matchesFilter(task, activeDeployTask,
                                         static_cast<int>(i));
                });
            if (filterCounts[i] > 0)
                lastVisibleFilter = i;
            const bool visible = filterCounts[i] > 0;
            filterVisibilityChanged |= visible !=
                (filterButtons_[i]->getVisibility() == brls::Visibility::VISIBLE);
        }
        // An overlay may restore focus to one of these buttons on dismissal.
        // Defer hiding it until this view owns focus again.
        brls::View* filterFocus = brls::Application::getCurrentFocus();
        if (filterVisibilityChanged && activityStackHasOverlay() &&
            !viewContains(this, filterFocus)) {
            pendingReload_ = true;
            return;
        }
        if (filterCounts[selectedFilter_] == 0 && selectedFilter_ != 0) {
            selectedFilter_ = 0;
            pendingReload_ = true;
        }
        for (size_t i = 0; i < filterButtons_.size(); ++i) {
            auto* button = filterButtons_[i];
            const bool visible = filterCounts[i] > 0;
            // Move focus before hiding its owner, including the last filter.
            if (!visible && viewContains(button, filterFocus))
                brls::Application::giveFocus(filterCounts[0] > 0
                    ? static_cast<brls::View*>(filterButtons_[0])
                    : static_cast<brls::View*>(addButton_));
            button->setVisibility(visible ? brls::Visibility::VISIBLE
                                          : brls::Visibility::GONE);
            button->setMarginRight(i == lastVisibleFilter ? 0 : 12);
            button->setText(tr(filterKeys_[i].c_str()) + "  " +
                            std::to_string(filterCounts[i]));
            button->setStyle(static_cast<int>(i) == selectedFilter_
                ? &brls::BUTTONSTYLE_BORDERED : &brls::BUTTONSTYLE_BORDERLESS);
            button->setTextColor(static_cast<int>(i) == selectedFilter_
                ? theme::textPrimary() : theme::textSecondary());
        }
        filters_->setVisibility(filterCounts[0] > 0
            ? brls::Visibility::VISIBLE : brls::Visibility::GONE);
        const std::string summary = summaryText(next);
        setTextIfChanged(summary_, summary);
        summary_->setVisibility(summary.empty() ? brls::Visibility::GONE
                                                : brls::Visibility::VISIBLE);
        next.erase(std::remove_if(next.begin(), next.end(),
            [this, &activeDeployTask](const DownloadTask& task) {
                return !matchesFilter(task, activeDeployTask, selectedFilter_);
            }), next.end());
        if (selectedFilter_ == 0 && settings_ && !settings_->get().showCompletedDownloads) {
            next.erase(std::remove_if(next.begin(), next.end(),
                [&activeDeployTask](const DownloadTask& task) {
                    return task.id != activeDeployTask &&
                           (task.status == DownloadStatus::Completed ||
                            task.status == DownloadStatus::Installed);
                }), next.end());
        }
        uint64_t settingsGeneration = settings_ ? settings_->generation() : 0;
        bool settingsChanged = settingsGeneration != settingsGeneration_;
        const DownloadListUpdate listed = downloadListUpdate(
            tasks_, next, deployGeneration_, deployState.generation,
            activeDeployTask_, activeDeployTask);
        const bool structureChanged = pendingReload_ || !initialized_ ||
                                       settingsChanged ||
                                       listed == DownloadListUpdate::Reload;
        const bool progressChanged = listed == DownloadListUpdate::Repaint;
        if (!structureChanged && !progressChanged) {
            for (auto* cell : visibleCells<DownloadCell>(recycler_))
                if (const DownloadTask* task =
                        dataSource_->taskAt(cell->getIndexPath()))
                    cell->refreshArtwork(*task, metadata_, rootPath(), catalog_);
            logRefresh(false, false);
            return;
        }
        // Same rows in the same order, only numbers moved: repaint the cells on
        // screen. reloadData() would recycle every cell, snap the scroll to the
        // focused row and re-home focus — once per tick that reads as a blink.
        if (!structureChanged) {
            tasks_ = std::move(next);
            deploySnapshot_ = deployState;
            deployGeneration_ = deployState.generation;
            dataSource_->setTasks(tasks_, activeDeployTask);
            for (auto* cell : visibleCells<DownloadCell>(recycler_))
                if (const DownloadTask* task =
                        dataSource_->taskAt(cell->getIndexPath()))
                    cell->setTask(*task, metadata_, &deploySnapshot_, rootPath(), catalog_);
            logRefresh(false, false);
            return;
        }
        brls::View* focused = brls::Application::getCurrentFocus();
        bool ownsFocus = viewContains(this, focused);
        // Overlay (deploy offer dialog, details, …) pushed our cell onto
        // focusStack. reloadData() would free it and crash on dismiss/Accept.
        if (activityStackHasOverlay() && !ownsFocus) {
            pendingReload_ = true;
            logRefresh(true, false);
            return;
        }
        auto* focusedCell = ownsFocus
            ? dynamic_cast<brls::RecyclerCell*>(focused)
            : nullptr;
        std::string focusedTaskId;
        if (focusedCell)
            focusedTaskId =
                dataSource_->taskIdAt(focusedCell->getIndexPath());
        tasks_ = std::move(next);
        deploySnapshot_ = deployState;
        deployGeneration_ = deployState.generation;
        activeDeployTask_ = activeDeployTask;
        settingsGeneration_ = settingsGeneration;
        initialized_ = true;
        pendingReload_ = false;
        dataSource_->setTasks(tasks_, activeDeployTask);
        recycler_->setDefaultCellFocus(
            dataSource_->indexForTask(focusedTaskId));
        recycler_->reloadData();
        const bool empty = tasks_.empty();
        if (empty) {
            ensureEmptyState()->setContent(
                tr(selectedFilter_ == 0 ? "pipensx/downloads/empty_title"
                                       : "pipensx/downloads/filter_empty"),
                tr(selectedFilter_ == 0 ? "pipensx/downloads/empty_body"
                                       : "pipensx/downloads/filter_empty_body"),
                tr("pipensx/downloads/add_download"),
                [this] { openFilePicker(); });
            emptyState_->setVisibility(brls::Visibility::VISIBLE);
        }
        else if (emptyState_)
            emptyState_->setVisibility(brls::Visibility::GONE);
        recyclerHost_->setVisibility(empty ? brls::Visibility::GONE
                                           : brls::Visibility::VISIBLE);
        if (ownsFocus && (focusedCell ||
                          (emptyState_ && viewContains(emptyState_, focused)))) {
            if (empty) {
                brls::Application::giveFocus(ensureEmptyState());
            } else {
                recycler_->setFocusable(true);
                brls::Application::giveFocus(recycler_);
                recycler_->setFocusable(false);
                brls::Application::giveFocus(recycler_);
            }
        }
        logRefresh(true, true);
    }

    DownloadManager* manager_;
    GameMetadataService* metadata_;
    AppSettings* settings_;
    SwitchDeployService* deploy_;
    CatalogService* catalog_;
    EmptyStateView* emptyState_ = nullptr;
    brls::Label* summary_ = nullptr;
    brls::RecyclerFrame* recycler_ = nullptr;
    brls::Box* recyclerHost_ = nullptr;
    DownloadDataSource* dataSource_;
    brls::RepeatingTimer timer_;
    std::vector<DownloadTask> tasks_;
    bool initialized_ = false;
    bool pendingReload_ = false;
    bool fastRefresh_ = false;
    uint64_t settingsGeneration_ = 0;
    SwitchDeploySnapshot deploySnapshot_;
    uint64_t deployGeneration_ = 0;
    std::string activeDeployTask_;
    std::shared_ptr<std::atomic<bool>> alive_ =
        std::make_shared<std::atomic<bool>>(true);
    bool clearInFlight_ = false;
    brls::Button* addButton_ = nullptr;
    brls::Box* filters_ = nullptr;
    int selectedFilter_ = 0;
    std::vector<brls::Button*> filterButtons_;
    std::vector<std::string> filterKeys_;
};

}  // namespace pipensx::ui
