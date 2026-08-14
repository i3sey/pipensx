#pragma once

#include <atomic>
#include <memory>
#include <string>

#include <borealis.hpp>

#include "app/download_manager.hpp"
#include "ui/common/progress_bar.hpp"
#include "ui/common/speed_graph.hpp"
#include "ui/common/ui_helpers.hpp"
#include "ui/downloads/task_files_activity.hpp"
#include "ui/i18n.hpp"
#include "ui/theme.hpp"

namespace pipensx::ui {

// UI_PLAN O8 — download details as eShop-style cards (Progress / Speed /
// Network) with on-screen Pause/Verify/Remove buttons instead of blind X/Y
// hotkeys. Big progress bar + ETA (S1), speed graph kept, network stats
// grouped. All colors/fonts on O1 tokens.
class DetailsActivity : public brls::Activity {
public:
    DetailsActivity(std::string taskId, DownloadManager* manager,
                    SwitchDeployService* deploy = nullptr)
        : taskId_(std::move(taskId)), manager_(manager), deploy_(deploy),
          alive_(std::make_shared<std::atomic<bool>>(true)) {
        auto* root = new brls::Box(brls::Axis::ROW);
        root->setPadding(24, 32, 24, 32);

        auto* left = new brls::Box(brls::Axis::COLUMN);
        left->setAlignItems(brls::AlignItems::STRETCH);
        left->setPadding(0, 16, 0, 0);

        status_ = new brls::Label();
        status_->setFontSize(theme::kFontHeading);
        status_->setMarginBottom(12);
        left->addView(status_);

        auto* progressCard = addCard(left, tr("pipensx/downloads/card_progress"));
        progressBar_ = new ProgressBar();
        progressBar_->setHeight(14);
        progressBar_->setMarginBottom(8);
        progressCard->addView(progressBar_);
        progress_ = addLine(progressCard, theme::kFontBody);
        package_ = addLine(progressCard, theme::kFontSmall);
        package_->setTextColor(theme::textSecondary());
        currentPackage_ = addLine(progressCard, theme::kFontSmall);
        currentPackage_->setSingleLine(true);
        currentPackage_->setAutoAnimate(false);
        eta_ = addLine(progressCard, theme::kFontSmall);
        eta_->setTextColor(theme::textSecondary());

        auto* speedCard = addCard(left, tr("pipensx/downloads/card_speed"));
        auto* speedLegend = new brls::Box(brls::Axis::ROW);
        speedLegend->setAlignItems(brls::AlignItems::CENTER);
        speedLegend->setMarginBottom(6);
        downloadSpeed_ = addSpeedLegend(speedLegend, theme::accent(), nullptr);
        installSpeed_ = addSpeedLegend(speedLegend, theme::success(),
                                       &installSpeedItem_);
        speedCard->addView(speedLegend);
        speedGraph_ = new SpeedGraphView();
        speedGraph_->setHeight(64);
        speedCard->addView(speedGraph_);

        auto* networkCard = addCard(left, tr("pipensx/downloads/card_network"));
        peers_ = addLine(networkCard, theme::kFontBody);
        pieces_ = addLine(networkCard, theme::kFontBody);
        health_ = addLine(networkCard, theme::kFontBody);

        auto* filesCard = addCard(left, tr("pipensx/files/card"));
        filesSummary_ = addLine(filesCard, theme::kFontSmall);
        filesSummary_->setTextColor(theme::textSecondary());
        deployPhase_ = addLine(filesCard, theme::kFontBody);
        deployPhase_->setTextColor(theme::textSecondary());
        deployProgress_ = new ProgressBar();
        deployProgress_->setHeight(14);
        deployProgress_->setMarginBottom(4);
        filesCard->addView(deployProgress_);
        deployStatus_ = addLine(filesCard, theme::kFontSmall);
        deployStatus_->setTextColor(theme::textSecondary());
        deployStatus_->setSingleLine(false);

        error_ = addLine(left, theme::kFontSmall);
        error_->setTextColor(theme::error());

        auto* scroll = new brls::ScrollingFrame();
        scroll->setGrow(1);
        scroll->setContentView(left);
        root->addView(scroll);

        auto* sidebar = new brls::Box(brls::Axis::COLUMN);
        sidebar->setWidth(kSidebarWidth);
        sidebar->setBackgroundColor(theme::surface());
        sidebar->setCornerRadius(theme::kRadiusMedium);
        sidebar->setPadding(16, 16, 16, 16);
        sidebar->setAlignItems(brls::AlignItems::STRETCH);

        pauseButton_ = addActionButton(sidebar, tr("pipensx/common/pause"),
                                       &brls::BUTTONSTYLE_PRIMARY);
        verifyButton_ = addActionButton(sidebar, tr("pipensx/common/verify"),
                                        &brls::BUTTONSTYLE_DEFAULT);
        removeButton_ = addActionButton(sidebar, tr("pipensx/common/remove"),
                                        &brls::BUTTONSTYLE_DEFAULT);

        auto* gap = new brls::Box();
        gap->setHeight(12);
        sidebar->addView(gap);

        filesButton_ = addActionButton(sidebar, tr("pipensx/files/open"),
                                       &brls::BUTTONSTYLE_DEFAULT);
        copyButton_ = addActionButton(sidebar, tr("pipensx/deploy/copy"),
                                      &brls::BUTTONSTYLE_PRIMARY);

        pauseButton_->registerClickAction([this](brls::View*) {
            onPauseResume();
            return true;
        });
        verifyButton_->registerClickAction([this](brls::View*) {
            manager_->verify(taskId_);
            refresh();
            return true;
        });
        removeButton_->registerClickAction([this](brls::View*) {
            openRemoveDialog();
            return true;
        });
        filesButton_->registerClickAction([this](brls::View*) {
            if (deploy_)
                brls::Application::pushActivity(
                    new TaskFilesActivity(taskId_, deploy_));
            return true;
        });
        copyButton_->registerClickAction([this](brls::View*) {
            onCopyToSwitch();
            return true;
        });

        root->addView(sidebar);
        frame_ = new brls::AppletFrame(root);
    }

    brls::View* createContentView() override {
        return frame_;
    }

    void onContentAvailable() override {
        refresh();
        loadDeployAvailability();
        timer_.setCallback([this] { refresh(); });
        timer_.start(500);
        brls::Application::giveFocus(pauseButton_);
    }

    ~DetailsActivity() override {
        alive_->store(false);
        timer_.stop();
    }

private:
    static constexpr float kSidebarWidth = 260.0f;

    static brls::Label* addLine(brls::Box* box, float size) {
        auto* label = new brls::Label();
        label->setWidth(brls::View::AUTO);
        label->setFontSize(size);
        label->setMarginBottom(4);
        box->addView(label);
        return label;
    }

    static brls::Box* addCard(brls::Box* parent, const std::string& title) {
        auto* card = new brls::Box(brls::Axis::COLUMN);
        card->setBackgroundColor(theme::surface());
        card->setCornerRadius(theme::kRadiusMedium);
        card->setPadding(12, 16, 12, 16);
        card->setMarginBottom(10);
        auto* heading = new brls::Label();
        heading->setFontSize(theme::kFontCaption);
        heading->setTextColor(theme::textSecondary());
        heading->setMarginBottom(8);
        heading->setText(title);
        card->addView(heading);
        parent->addView(card);
        return card;
    }

    static brls::Label* addSpeedLegend(brls::Box* row, NVGcolor color,
                                       brls::Box** itemOut) {
        auto* item = new brls::Box(brls::Axis::ROW);
        item->setAlignItems(brls::AlignItems::CENTER);
        item->setMarginRight(28);

        auto* dot = new brls::Box();
        dot->setWidth(10);
        dot->setHeight(10);
        dot->setCornerRadius(5);
        dot->setBackgroundColor(color);
        dot->setMarginRight(8);
        item->addView(dot);

        auto* label = new brls::Label();
        label->setFontSize(theme::kFontBody);
        label->setSingleLine(true);
        label->setAutoAnimate(false);
        item->addView(label);
        row->addView(item);
        if (itemOut)
            *itemOut = item;
        return label;
    }

    static brls::Button* addActionButton(brls::Box* column, const std::string& text,
                                         const brls::ButtonStyle* style) {
        auto* button = new brls::Button();
        button->setStyle(style);
        button->setFontSize(theme::kFontCaption);
        button->setHeight(52);
        button->setMarginBottom(10);
        button->setText(text);
        column->addView(button);
        return button;
    }

    static std::string healthText(TorrentHealth health) {
        switch (health) {
        case TorrentHealth::Excellent:
            return tr("pipensx/downloads/health_line",
                      tr("pipensx/downloads/health_excellent"));
        case TorrentHealth::Slow:
            return tr("pipensx/downloads/health_line",
                      tr("pipensx/downloads/health_slow"));
        case TorrentHealth::Poor:
            return tr("pipensx/downloads/health_line",
                      tr("pipensx/downloads/health_poor"));
        case TorrentHealth::NotActive:
            return {};
        }
        return {};
    }

    static NVGcolor healthColor(TorrentHealth health) {
        switch (health) {
        case TorrentHealth::Excellent: return theme::success();
        case TorrentHealth::Slow: return theme::warning();
        case TorrentHealth::Poor: return theme::error();
        case TorrentHealth::NotActive: return theme::textSecondary();
        }
        return theme::textSecondary();
    }

    const DownloadTask* currentTask() {
        auto task = manager_->snapshot(taskId_);
        if (!task)
            return nullptr;
        cache_.clear();
        cache_.push_back(std::move(*task));
        return &cache_.front();
    }

    void onPauseResume() {
        const DownloadTask* task = currentTask();
        if (!task)
            return;
        if (task->status == DownloadStatus::Paused ||
            task->status == DownloadStatus::Error)
            manager_->resume(taskId_);
        else
            manager_->pause(taskId_);
        refresh();
    }

    void loadDeployAvailability() {
        if (!deploy_ || availabilityLoaded_ || availabilityLoading_)
            return;
        const auto task = manager_->snapshot(taskId_);
        if (!task || !taskReadyForSwitchDeploy(*task))
            return;
        availabilityLoading_ = true;
        filesSummary_->setText(tr("pipensx/files/loading"));
        auto alive = alive_;
        const std::string taskId = taskId_;
        SwitchDeployService* deploy = deploy_;
        brls::async([this, alive, taskId, deploy] {
            SwitchDeployInspection inspection = deploy->inspect(taskId);
            brls::sync([this, alive,
                        inspection = std::move(inspection)]() mutable {
                if (!alive->load())
                    return;
                availabilityLoading_ = false;
                availabilityLoaded_ = true;
                filesSummary_->setText(tr(
                    "pipensx/files/summary", inspection.inventory.files.size(),
                    formatBytes(inspection.inventory.presentBytes)));
                copyAvailable_ = inspection.problem == SwitchDeployProblem::None ||
                    inspection.problem == SwitchDeployProblem::Conflict ||
                    inspection.problem == SwitchDeployProblem::NoSpace ||
                    inspection.problem == SwitchDeployProblem::NoRam;
                if (!copyAvailable_ &&
                    inspection.problem != SwitchDeployProblem::NotReady) {
                    setTextIfChanged(deployPhase_,
                                     deployProblemText(inspection.problem,
                                                       inspection.detail));
                    deployPhase_->setTextColor(theme::error());
                }
                refresh();
            });
        });
    }

    void loadReceiptState() {
        if (!deploy_ || receiptChecked_ || receiptLoading_)
            return;
        receiptLoading_ = true;
        const uint64_t generation = deploy_->snapshot().generation;
        auto alive = alive_;
        const std::string taskId = taskId_;
        SwitchDeployService* deploy = deploy_;
        brls::async([this, alive, taskId, deploy, generation] {
            const SwitchDeployReceiptState state = deploy->receiptState(taskId);
            brls::sync([this, alive, deploy, generation, state] {
                if (!alive->load())
                    return;
                receiptLoading_ = false;
                if (deploy->snapshot().generation != generation)
                    return;
                receiptState_ = state;
                receiptChecked_ = true;
                refresh();
            });
        });
    }

    void showReceiptState() {
        if (!receiptChecked_) {
            loadReceiptState();
            setTextIfChanged(deployPhase_, tr("pipensx/deploy/preparing"));
            setTextIfChanged(deployStatus_, "");
        } else if (receiptState_ == SwitchDeployReceiptState::Valid) {
            deployProgress_->setProgress(1.0f);
            setTextIfChanged(deployPhase_, tr("pipensx/deploy/receipt_valid"));
            setTextIfChanged(deployStatus_, "");
        } else if (receiptState_ == SwitchDeployReceiptState::Modified) {
            deployProgress_->setProgress(0.0f);
            setTextIfChanged(deployPhase_,
                             tr("pipensx/deploy/receipt_modified"));
            setTextIfChanged(deployStatus_, "");
        }
    }

    void onCopyToSwitch() {
        if (!deploy_)
            return;
        const SwitchDeploySnapshot state = deploy_->snapshot();
        if (state.active()) {
            if (state.taskId == taskId_) {
                deploy_->cancel();
                brls::Application::notify(
                    tr("pipensx/deploy/cancel_requested"));
            } else {
                brls::Application::notify(tr("pipensx/deploy/problem_busy"));
            }
            return;
        }
        auto alive = alive_;
        const std::string taskId = taskId_;
        SwitchDeployService* deploy = deploy_;
        copyButton_->setState(brls::ButtonState::DISABLED);
        setTextIfChanged(deployPhase_, tr("pipensx/deploy/preparing"));
        setTextIfChanged(deployStatus_, "");
        brls::async([this, alive, taskId, deploy] {
            SwitchDeployInspection inspection = deploy->inspect(taskId);
            brls::sync([this, alive,
                        inspection = std::move(inspection)]() mutable {
                if (!alive->load())
                    return;
                copyButton_->setState(brls::ButtonState::ENABLED);
                if (inspection.problem != SwitchDeployProblem::None &&
                    inspection.problem != SwitchDeployProblem::Conflict &&
                    inspection.problem != SwitchDeployProblem::NoSpace &&
                    inspection.problem != SwitchDeployProblem::NoRam) {
                    setTextIfChanged(deployPhase_,
                                     deployProblemText(inspection.problem,
                                                       inspection.detail));
                    deployPhase_->setTextColor(theme::error());
                    return;
                }
                brls::Application::pushActivity(
                    new SwitchDeployPreviewActivity(std::move(inspection),
                                                    deploy_));
            });
        });
    }

    void refreshDeploy(const DownloadTask& task) {
        if (!deploy_) {
            filesSummary_->setText(tr("pipensx/files/unavailable"));
            return;
        }
        const SwitchDeploySnapshot state = deploy_->snapshot();
        if (state.taskId == taskId_ && state.active()) {
            receiptChecked_ = false;
            const float fraction = state.totalBytes
                ? static_cast<float>(state.bytesCopied) /
                      static_cast<float>(state.totalBytes)
                : 0.0f;
            deployProgress_->setProgress(fraction);
            copyButton_->setText(tr("pipensx/deploy/cancel"));
            const char* phaseKey =
                state.phase == SwitchDeployPhase::Preparing
                    ? "pipensx/deploy/phase_preparing"
                    : state.phase == SwitchDeployPhase::Extracting
                          ? "pipensx/deploy/phase_extracting"
                          : "pipensx/deploy/phase_copying";
            setTextIfChanged(deployPhase_, tr(phaseKey));
            deployPhase_->setTextColor(theme::accent());
            setTextIfChanged(
                deployStatus_,
                tr("pipensx/deploy/progress",
                   percentOf(fraction), state.filesCopied, state.totalFiles,
                   formatBytes(state.bytesCopied),
                   formatBytes(state.totalBytes), state.currentPath));
            deployStatus_->setTextColor(theme::accent());
            return;
        }
        copyButton_->setText(tr("pipensx/deploy/copy"));
        deployPhase_->setTextColor(theme::textSecondary());
        deployStatus_->setTextColor(theme::textSecondary());
        if (state.taskId == taskId_) {
            if (state.phase == SwitchDeployPhase::Completed) {
                if (state.detail.empty())
                    showReceiptState();
                else {
                    setTextIfChanged(
                        deployPhase_,
                        tr("pipensx/deploy/completed_warning", state.detail));
                    setTextIfChanged(deployStatus_, "");
                }
            } else if (state.phase == SwitchDeployPhase::Failed) {
                setTextIfChanged(deployPhase_,
                                 deployProblemText(state.problem,
                                                   state.detail));
                deployPhase_->setTextColor(theme::error());
                setTextIfChanged(deployStatus_, "");
            } else if (state.phase == SwitchDeployPhase::Cancelled) {
                setTextIfChanged(deployPhase_,
                                 tr("pipensx/deploy/cancelled"));
                setTextIfChanged(deployStatus_, "");
            }
        } else {
            showReceiptState();
        }
        if (taskReadyForSwitchDeploy(task) && !availabilityLoaded_ &&
            !availabilityLoading_)
            loadDeployAvailability();
    }

    void refresh() {
        const DownloadTask* task = currentTask();
        if (!task) {
            brls::Application::popActivity();
            return;
        }
        if (frameTitle_ != task->name) {
            frameTitle_ = task->name;
            frame_->setTitle(frameTitle_);
        }
        setTextIfChanged(status_, tr("pipensx/downloads/status_line",
                                     downloadStatusLabel(task->status)));
        status_->setTextColor(statusColor(task->status));

        bool installing = task->status == DownloadStatus::Installing ||
                          task->status == DownloadStatus::Committing;
        float progress = installing ? installProgressOf(*task)
                                    : progressOf(*task);
        progressBar_->setProgress(progress);
        // Installing phases: the bar and the byte line track the same
        // per-package install numbers. Downloading/other: the byte line
        // follows the wanted (selection-aware) range like the bar does.
        const auto wanted = downloadProgressBytes(*task);
        const uint64_t doneBytes =
            installing ? task->installedBytes : wanted.first;
        const uint64_t totalBytes =
            installing ? task->installTotalBytes : wanted.second;
        setTextIfChanged(progress_, tr("pipensx/downloads/progress_line",
                                       percentOf(progress),
                                       formatBytes(doneBytes),
                                       formatBytes(totalBytes)));

        const uint64_t now = now_ms();
        std::string eta;
        if (auto seconds = taskEtaSeconds(*task, now))
            eta = formatEtaSeconds(*seconds);
        setTextIfChanged(eta_, eta.empty()
                                   ? std::string()
                                   : tr("pipensx/downloads/eta_line", eta));

        if (task->mode == TransferMode::StreamInstall && task->packageCount) {
            const bool hasCurrent = !task->currentPackage.empty() &&
                                    task->packagesInstalled < task->packageCount;
            if (hasCurrent) {
                setTextIfChanged(
                    package_,
                    tr("pipensx/downloads/package_of",
                       task->packagesInstalled + 1, task->packageCount));
            } else {
                setTextIfChanged(
                    package_,
                    tr("pipensx/downloads/packages_installed",
                       task->packagesInstalled, task->packageCount));
            }
            setTextIfChanged(currentPackage_, task->currentPackage);
        } else {
            setTextIfChanged(package_, "");
            setTextIfChanged(currentPackage_, "");
        }

        recordSpeedSample(*task, now);
        setTextIfChanged(downloadSpeed_,
                          tr("pipensx/downloads/speed_download",
                             formatSpeed(task->speedBytesPerSecond)));
        const uint64_t installSpeed = currentInstallSpeed(*task, now);
        if (task->mode == TransferMode::StreamInstall) {
            installSpeedItem_->setVisibility(brls::Visibility::VISIBLE);
            setTextIfChanged(installSpeed_,
                             tr("pipensx/downloads/speed_install",
                                formatSpeed(installSpeed)));
        } else {
            installSpeedItem_->setVisibility(brls::Visibility::GONE);
        }
        setTextIfChanged(peers_, tr("pipensx/downloads/peers_line", task->peers,
                                    task->dhtGood, task->dhtDubious));
        setTextIfChanged(pieces_,
                         tr("pipensx/downloads/pieces_line", task->piecesDone,
                            task->piecesTotal, task->piecesVerified));
        const TorrentHealth health = torrentHealth(*task, now);
        setTextIfChanged(health_, healthText(health));
        health_->setTextColor(healthColor(health));
        setTextIfChanged(error_,
                         task->error.empty()
                             ? std::string()
                              : tr("pipensx/downloads/error_line", task->error));

        refreshDeploy(*task);

        updateButtons(*task);
    }

    void updateButtons(const DownloadTask& task) {
        const SwitchDeploySnapshot deploy = deploy_ ? deploy_->snapshot()
                                                     : SwitchDeploySnapshot{};
        const bool leased = deploy.active() && deploy.taskId == taskId_;
        bool paused = task.status == DownloadStatus::Paused ||
                            task.status == DownloadStatus::Error;
        bool active = task.status == DownloadStatus::Queued ||
                      task.status == DownloadStatus::Checking ||
                      task.status == DownloadStatus::Fetching ||
                      task.status == DownloadStatus::Downloading ||
                      task.status == DownloadStatus::Installing ||
                      task.status == DownloadStatus::Committing ||
                      task.status == DownloadStatus::Verifying;
        setTextIfChanged(pauseButton_, paused ? tr("pipensx/common/resume")
                                              : tr("pipensx/common/pause"));
        setButtonAvailable(pauseButton_, !leased && (paused || active));

        bool canVerify = task.status == DownloadStatus::Paused ||
                         task.status == DownloadStatus::Error ||
                         task.status == DownloadStatus::Completed ||
                         task.status == DownloadStatus::Installed;
        setButtonAvailable(verifyButton_, !leased && canVerify);
        setButtonAvailable(removeButton_,
                           !leased && task.status != DownloadStatus::Removing);
        setButtonAvailable(filesButton_, deploy_ != nullptr);
        const bool busyElsewhere = deploy.active() && deploy.taskId != taskId_;
        const bool packageBusy =
            !leased &&
            (task.status == DownloadStatus::Installing ||
             task.status == DownloadStatus::Committing);
        const bool copyEnabled = deploy_ != nullptr &&
            ((deploy.active() && deploy.taskId == taskId_) ||
             (copyAvailable_ && !busyElsewhere && !packageBusy));
        setButtonAvailable(copyButton_, copyEnabled);
        if (deploy_ && !leased && !copyAvailable_ && availabilityLoaded_ &&
            !availabilityLoading_) {
            // Keep the label informative when Copy cannot start yet.
            copyButton_->setText(tr("pipensx/deploy/copy"));
        } else if (busyElsewhere) {
            copyButton_->setText(tr("pipensx/deploy/problem_busy"));
        } else if (packageBusy) {
            copyButton_->setText(tr("pipensx/deploy/problem_busy"));
        } else if (leased) {
            copyButton_->setText(tr("pipensx/deploy/cancel"));
        } else {
            copyButton_->setText(tr("pipensx/deploy/copy"));
        }
    }

    static void setButtonAvailable(brls::Button* button, bool available) {
        button->setState(available ? brls::ButtonState::ENABLED
                                   : brls::ButtonState::DISABLED);
        button->setAlpha(available ? 1.0f : 0.32f);
    }

    static void appendSpeedSample(std::vector<uint64_t>& samples,
                                  uint64_t value) {
        constexpr size_t kMaxSpeedSamples = 60;
        if (samples.size() == kMaxSpeedSamples)
            samples.erase(samples.begin());
        samples.push_back(value);
    }

    void recordSpeedSample(const DownloadTask& task, uint64_t now) {
        appendSpeedSample(downloadSpeedSamples_, task.speedBytesPerSecond);

        if (task.mode == TransferMode::StreamInstall) {
            appendSpeedSample(installSpeedSamples_,
                              currentInstallSpeed(task, now));
        } else {
            installSpeedSamples_.clear();
        }

        speedGraph_->setSamples(downloadSpeedSamples_, installSpeedSamples_);
    }

    void openRemoveDialog() {
        auto* dialog = new brls::Dialog(
            tr("pipensx/downloads/remove_question"));
        dialog->addButton(tr("pipensx/downloads/remove_keep"), [this] {
            std::string error;
            if (!manager_->remove(taskId_, false, error))
                brls::Application::notify(error);
            else
                brls::Application::popActivity();
        });
        dialog->addButton(tr("pipensx/downloads/remove_delete"), [this] {
            std::string error;
            if (!manager_->remove(taskId_, true, error))
                brls::Application::notify(error);
            else
                brls::Application::popActivity();
        });
        dialog->addButton(tr("pipensx/common/cancel"), [] {});
        dialog->open();
    }

    std::string taskId_;
    DownloadManager* manager_;
    SwitchDeployService* deploy_;
    std::shared_ptr<std::atomic<bool>> alive_;
    std::string frameTitle_;
    brls::AppletFrame* frame_;
    brls::Label* status_;
    brls::Button* pauseButton_;
    brls::Button* verifyButton_;
    brls::Button* removeButton_;
    ProgressBar* progressBar_;
    brls::Label* progress_;
    brls::Label* package_;
    brls::Label* currentPackage_;
    brls::Label* eta_;
    brls::Label* downloadSpeed_;
    brls::Label* installSpeed_;
    brls::Box* installSpeedItem_;
    SpeedGraphView* speedGraph_;
    brls::Label* peers_;
    brls::Label* pieces_;
    brls::Label* health_;
    brls::Label* filesSummary_;
    brls::Label* deployPhase_;
    brls::Label* deployStatus_;
    ProgressBar* deployProgress_;
    brls::Button* filesButton_;
    brls::Button* copyButton_;
    brls::Label* error_;
    brls::RepeatingTimer timer_;
    bool availabilityLoaded_ = false;
    bool availabilityLoading_ = false;
    bool copyAvailable_ = false;
    bool receiptChecked_ = false;
    bool receiptLoading_ = false;
    SwitchDeployReceiptState receiptState_ = SwitchDeployReceiptState::None;
    std::vector<DownloadTask> cache_;
    std::vector<uint64_t> downloadSpeedSamples_;
    std::vector<uint64_t> installSpeedSamples_;
};

}  // namespace pipensx::ui
