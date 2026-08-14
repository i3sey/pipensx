#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <borealis.hpp>

#include "app/app_settings.hpp"
#include "app/download_manager.hpp"
#include "app/game_metadata_service.hpp"
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
    brls::RecyclerCell* cellForRow(brls::RecyclerFrame* recycler,
                                    brls::IndexPath index) override;
    void didSelectRowAt(brls::RecyclerFrame*, brls::IndexPath index) override;

private:
    struct Section {
        std::string title;
        std::vector<DownloadTask> tasks;
    };
    MainView* owner_;
    std::vector<Section> sections_;
};

class MainView : public brls::Box {
public:
    MainView(DownloadManager* manager, GameMetadataService* metadata,
             AppSettings* settings, SwitchDeployService* deploy = nullptr)
        : brls::Box(brls::Axis::COLUMN), manager_(manager), metadata_(metadata),
          settings_(settings), deploy_(deploy) {
        summary_ = new brls::Label();
        summary_->setFontSize(15);
        summary_->setMarginTop(10);
        summary_->setMarginBottom(4);
        summary_->setMarginLeft(32);
        summary_->setMarginRight(32);
        summary_->setTextColor(theme::textTertiary());
        summary_->setVisibility(brls::Visibility::GONE);
        addView(summary_);

        recycler_ = new brls::RecyclerFrame();
        recycler_->setGrow(1);
        recycler_->setPadding(6, 32, 6, 32);
        recycler_->estimatedRowHeight = 108;
        recycler_->registerCell("Download", [] { return new DownloadCell(); });
        recycler_->registerCell("Message", [] { return new MessageCell(); });
        dataSource_ = new DownloadDataSource(this);
        recycler_->setDataSource(dataSource_);
        addView(recycler_);
        refresh();
        timer_.setCallback([this] {
            refresh();
            if (fastRefresh_) {
                fastRefresh_ = false;
                timer_.setPeriod(750);
            }
        });
        registerAction(tr("pipensx/downloads/import"), brls::BUTTON_X,
                       [this](brls::View*) {
            openFilePicker();
            return true;
        });
        registerAction(tr("pipensx/downloads/pause_all"), brls::BUTTON_Y,
                       [this](brls::View*) {
            pauseResumeAll();
            return true;
        });
        registerAction(tr("pipensx/downloads/clear_completed"), brls::BUTTON_LB,
                       [this](brls::View*) {
            clearCompleted();
            return true;
        });
        startRefreshing();
    }

    ~MainView() override {
        timer_.stop();
    }

    void openDetails(const std::string& taskId) {
        brls::Application::pushActivity(
            new DetailsActivity(taskId, manager_, deploy_));
    }

    void openFilePicker() {
        brls::Application::pushActivity(
            new FilePickerActivity(manager_, settings_));
    }

    // O7: per-row context menu on A. Replaces the old blind Y/X hotkeys — the
    // items are labelled and only the ones valid for the current status appear.
    void openRowMenu(const std::string& taskId) {
        DownloadTask task;
        bool found = false;
        for (const auto& candidate : manager_->snapshot())
            if (candidate.id == taskId) {
                task = candidate;
                found = true;
                break;
            }
        if (!found)
            return;

        std::vector<std::string> labels;
        auto runners =
            std::make_shared<std::vector<std::function<void()>>>();
        auto add = [&](const std::string& label, std::function<void()> run) {
            labels.push_back(label);
            runners->push_back(std::move(run));
        };

        add(tr("pipensx/common/details"), [this, taskId] { openDetails(taskId); });
        const SwitchDeploySnapshot deployState = deploy_ ? deploy_->snapshot()
                                                         : SwitchDeploySnapshot{};
        const bool leased = deployState.active() && deployState.taskId == taskId;
        if (leased)
            add(tr("pipensx/deploy/cancel"), [this] { deploy_->cancel(); });

        bool active = task.status == DownloadStatus::Queued ||
                      task.status == DownloadStatus::Checking ||
                      task.status == DownloadStatus::Fetching ||
                      task.status == DownloadStatus::Downloading ||
                      task.status == DownloadStatus::Installing ||
                      task.status == DownloadStatus::Committing ||
                      task.status == DownloadStatus::Verifying;
        if (active)
            add(tr("pipensx/common/pause"), [this, taskId] {
                manager_->pause(taskId);
                startRefreshing(true);
            });
        if (task.status == DownloadStatus::Paused ||
            task.status == DownloadStatus::Error)
            add(tr("pipensx/common/resume"), [this, taskId] {
                manager_->resume(taskId);
                startRefreshing(true);
            });
        if (!leased && (task.status == DownloadStatus::Completed ||
                        task.status == DownloadStatus::Installed) &&
            deploy_ && taskReadyForSwitchDeploy(task)) {
            add(tr("pipensx/deploy/copy"), [this, taskId] {
                if (!deploy_)
                    return;
                SwitchDeployInspection inspection = deploy_->inspect(taskId);
                if (inspection.problem != SwitchDeployProblem::None &&
                    inspection.problem != SwitchDeployProblem::Conflict &&
                    inspection.problem != SwitchDeployProblem::NoSpace &&
                    inspection.problem != SwitchDeployProblem::NoRam) {
                    brls::Application::notify(deployProblemText(
                        inspection.problem, inspection.detail));
                    return;
                }
                brls::Application::pushActivity(
                    new SwitchDeployPreviewActivity(std::move(inspection),
                                                    deploy_));
            });
        }
        if (!leased && task.status == DownloadStatus::Completed)
            add(tr("pipensx/common/verify"), [this, taskId] {
                manager_->verify(taskId);
                startRefreshing(true);
            });
        // Queue reordering: only offered when it would change something —
        // the task must be queued and not already at the relevant edge.
        if (task.status == DownloadStatus::Queued) {
            std::vector<std::string> queuedIds;
            for (const auto& candidate : manager_->snapshot())
                if (candidate.status == DownloadStatus::Queued)
                    queuedIds.push_back(candidate.id);
            size_t pos = 0;
            bool found = false;
            for (size_t i = 0; i < queuedIds.size(); ++i)
                if (queuedIds[i] == taskId) {
                    pos = i;
                    found = true;
                    break;
                }
            if (found) {
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
        if (!leased && task.status != DownloadStatus::Removing)
            add(tr("pipensx/common/remove"),
                    [this, taskId] { openRemoveDialog(taskId); });

        // The Dropdown pops itself right after firing the callback, so defer
        // the action a frame — otherwise a pushActivity here would land under
        // that pop.
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
            std::string error;
            if (!manager_->remove(taskId, true, error))
                brls::Application::notify(error);
            else
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

    GameMetadataService* metadataService() const { return metadata_; }
    const SwitchDeploySnapshot& deploySnapshot() const {
        return deploySnapshot_;
    }

private:
    bool containsFocus(brls::View* focused) const {
        for (brls::View* view = focused; view; view = view->getParent())
            if (view == this)
                return true;
        return false;
    }

    EmptyStateView* ensureEmptyState() {
        if (emptyState_)
            return emptyState_;
        emptyState_ = new EmptyStateView();
        emptyState_->setContent(
            tr("pipensx/downloads/empty_title"),
            tr("pipensx/downloads/empty_body"),
            tr("pipensx/downloads/import_action"),
            [this] { openFilePicker(); });
        addView(emptyState_);
        return emptyState_;
    }

    bool isPausable(DownloadStatus status) const {
        return status == DownloadStatus::Queued ||
               status == DownloadStatus::Checking ||
               status == DownloadStatus::Fetching ||
               status == DownloadStatus::Downloading ||
               status == DownloadStatus::Installing ||
               status == DownloadStatus::Verifying;
    }

    bool hasPausableTask(const std::vector<DownloadTask>& tasks) const {
        for (const DownloadTask& task : tasks)
            if (isPausable(task.status))
                return true;
        return false;
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
        if (hasPausableTask(manager_->snapshot()))
            pauseAll();
        else
            resumeAll();
    }

    void clearCompleted() {
        bool any = false;
        for (const DownloadTask& task : manager_->snapshot())
            if (task.status == DownloadStatus::Completed ||
                task.status == DownloadStatus::Installed) {
                any = true;
                break;
            }
        if (!any) {
            brls::Application::notify(
                tr("pipensx/downloads/clear_completed_none"));
            return;
        }
        auto* dialog =
            new brls::Dialog(tr("pipensx/downloads/clear_completed_question"));
        auto run = [this](bool deleteData) {
            std::string error;
            if (!manager_->clearCompleted(deleteData, error) && !error.empty())
                brls::Application::notify(error);
            startRefreshing(true);
        };
        dialog->addButton(tr("pipensx/downloads/remove_keep"),
                          [run] { run(false); });
        dialog->addButton(tr("pipensx/downloads/remove_delete"),
                          [run] { run(true); });
        dialog->addButton(tr("pipensx/common/cancel"), [] {});
        dialog->open();
    }

    std::string summaryText(const std::vector<DownloadTask>& tasks) const {
        if (tasks.empty())
            return {};
        const pipensx::QueueSummary s = summarizeQueue(tasks, now_ms());
        std::string text = tr("pipensx/downloads/summary_counts",
                              s.downloading, s.queued, s.installing);
        if (s.paused)
            text += "   " + tr("pipensx/downloads/summary_paused", s.paused);
        if (s.errors)
            text += "   " + tr("pipensx/downloads/summary_errors", s.errors);
        const uint64_t speed = s.downloadSpeedBps + s.installSpeedBps;
        if (speed)
            text += "\n" +
                    tr("pipensx/downloads/summary_speed", formatSpeed(speed));
        if (s.etaSeconds)
            text += "   " + tr("pipensx/downloads/summary_eta",
                               formatEtaSeconds(s.etaSeconds));
        return text;
    }

    void refresh() {
        auto next = manager_->snapshot();
        const SwitchDeploySnapshot deployState = deploy_ ? deploy_->snapshot()
                                                         : SwitchDeploySnapshot{};
        const std::string activeDeployTask = deployState.active()
            ? deployState.taskId : std::string();
        if (settings_ && !settings_->get().showCompletedDownloads) {
            next.erase(std::remove_if(next.begin(), next.end(),
                [&activeDeployTask](const DownloadTask& task) {
                    return task.id != activeDeployTask &&
                           (task.status == DownloadStatus::Completed ||
                            task.status == DownloadStatus::Installed);
                }), next.end());
        }
        setTextIfChanged(summary_, summaryText(next));
        updateActionHint(brls::BUTTON_Y,
                         hasPausableTask(next)
                             ? tr("pipensx/downloads/pause_all")
                             : tr("pipensx/downloads/resume_all"));
        uint64_t settingsGeneration = settings_ ? settings_->generation() : 0;
        bool settingsChanged = settingsGeneration != settingsGeneration_;
        bool structureChanged = !initialized_ || settingsChanged ||
                                 next.size() != tasks_.size() ||
                                 activeDeployTask != activeDeployTask_;
        bool progressChanged = deployState.generation != deployGeneration_;
        if (!structureChanged) {
            // Scan every task: bailing out on the first progress delta used to
            // hide a later task's status change, so a section reshuffle went
            // through the cheap path and the list jumped under the cursor.
            for (size_t i = 0; i < next.size(); ++i) {
                if (next[i].id != tasks_[i].id ||
                    next[i].status != tasks_[i].status) {
                    structureChanged = true;
                    break;
                }
                progressChanged =
                    progressChanged ||
                    next[i].completedBytes != tasks_[i].completedBytes ||
                    next[i].speedBytesPerSecond !=
                        tasks_[i].speedBytesPerSecond ||
                    next[i].installSpeedBytesPerSecond !=
                        tasks_[i].installSpeedBytesPerSecond ||
                    next[i].peers != tasks_[i].peers ||
                    next[i].packagesInstalled != tasks_[i].packagesInstalled ||
                    next[i].installedBytes != tasks_[i].installedBytes ||
                    next[i].currentPackage != tasks_[i].currentPackage ||
                    next[i].status == DownloadStatus::Downloading ||
                    next[i].status == DownloadStatus::Installing;
            }
        }
        if (!structureChanged && !progressChanged)
            return;
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
                    cell->setTask(*task, metadata_, &deploySnapshot_);
            return;
        }
        brls::View* focused = brls::Application::getCurrentFocus();
        bool ownsFocus = containsFocus(focused);
        // Overlay (deploy offer dialog, details, …) pushed our cell onto
        // focusStack. reloadData() would free it and crash on dismiss/Accept.
        if (activityStackHasOverlay() && !ownsFocus) {
            tasks_ = std::move(next);
            deploySnapshot_ = deployState;
            deployGeneration_ = deployState.generation;
            activeDeployTask_ = activeDeployTask;
            settingsGeneration_ = settingsGeneration;
            initialized_ = true;
            dataSource_->setTasks(tasks_, activeDeployTask);
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
        dataSource_->setTasks(tasks_, activeDeployTask);
        recycler_->setDefaultCellFocus(
            dataSource_->indexForTask(focusedTaskId));
        recycler_->reloadData();
        const bool empty = tasks_.empty();
        if (empty)
            ensureEmptyState()->setVisibility(brls::Visibility::VISIBLE);
        else if (emptyState_)
            emptyState_->setVisibility(brls::Visibility::GONE);
        recycler_->setVisibility(empty ? brls::Visibility::GONE
                                       : brls::Visibility::VISIBLE);
        if (ownsFocus) {
            if (empty) {
                brls::Application::giveFocus(ensureEmptyState());
            } else {
                recycler_->setFocusable(true);
                brls::Application::giveFocus(recycler_);
                recycler_->setFocusable(false);
                brls::Application::giveFocus(recycler_);
            }
        }
    }

    DownloadManager* manager_;
    GameMetadataService* metadata_;
    AppSettings* settings_;
    SwitchDeployService* deploy_;
    EmptyStateView* emptyState_ = nullptr;
    brls::Label* summary_ = nullptr;
    brls::RecyclerFrame* recycler_;
    DownloadDataSource* dataSource_;
    brls::RepeatingTimer timer_;
    std::vector<DownloadTask> tasks_;
    bool initialized_ = false;
    bool fastRefresh_ = false;
    uint64_t settingsGeneration_ = 0;
    SwitchDeploySnapshot deploySnapshot_;
    uint64_t deployGeneration_ = 0;
    std::string activeDeployTask_;
};

}  // namespace pipensx::ui
