#pragma once

#include <string>

#include <borealis.hpp>

#include "app/download_manager.hpp"
#include "ui/common/progress_bar.hpp"
#include "ui/common/speed_graph.hpp"
#include "ui/common/ui_helpers.hpp"
#include "ui/i18n.hpp"
#include "ui/theme.hpp"

namespace pipensx::ui {

// UI_PLAN O8 — download details as eShop-style cards (Progress / Speed /
// Network) with on-screen Pause/Verify/Remove buttons instead of blind X/Y
// hotkeys. Big progress bar + ETA (S1), speed graph kept, network stats
// grouped. All colors/fonts on O1 tokens.
class DetailsActivity : public brls::Activity {
public:
    DetailsActivity(std::string taskId, DownloadManager* manager)
        : taskId_(std::move(taskId)), manager_(manager) {
        auto* content = new brls::Box(brls::Axis::COLUMN);
        content->setPadding(24, 40, 24, 40);
        content->setAlignItems(brls::AlignItems::STRETCH);

        status_ = new brls::Label();
        status_->setFontSize(theme::kFontHeading);
        status_->setMarginBottom(16);
        content->addView(status_);

        // Action buttons replace the old X/Y hotkeys.
        auto* actions = new brls::Box(brls::Axis::ROW);
        actions->setMarginBottom(20);
        pauseButton_ = addActionButton(actions, tr("pipensx/common/pause"),
                                       &brls::BUTTONSTYLE_PRIMARY);
        verifyButton_ = addActionButton(actions, tr("pipensx/common/verify"),
                                        &brls::BUTTONSTYLE_DEFAULT);
        removeButton_ = addActionButton(actions, tr("pipensx/common/remove"),
                                        &brls::BUTTONSTYLE_DEFAULT);
        content->addView(actions);
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

        auto* progressCard = addCard(content, tr("pipensx/downloads/card_progress"));
        progressBar_ = new ProgressBar();
        progressBar_->setHeight(14);
        progressBar_->setMarginBottom(12);
        progressCard->addView(progressBar_);
        progress_ = addLine(progressCard, theme::kFontBody);
        package_ = addLine(progressCard, theme::kFontSmall);
        package_->setTextColor(theme::textSecondary());
        currentPackage_ = addLine(progressCard, theme::kFontSmall);
        currentPackage_->setSingleLine(true);
        currentPackage_->setAutoAnimate(false);
        eta_ = addLine(progressCard, theme::kFontSmall);
        eta_->setTextColor(theme::textSecondary());

        auto* speedCard = addCard(content, tr("pipensx/downloads/card_speed"));
        auto* speedLegend = new brls::Box(brls::Axis::ROW);
        speedLegend->setAlignItems(brls::AlignItems::CENTER);
        speedLegend->setMarginBottom(8);
        downloadSpeed_ = addSpeedLegend(speedLegend, theme::accent(), nullptr);
        installSpeed_ = addSpeedLegend(speedLegend, theme::success(),
                                       &installSpeedItem_);
        speedCard->addView(speedLegend);
        speedGraph_ = new SpeedGraphView();
        speedCard->addView(speedGraph_);

        auto* networkCard = addCard(content, tr("pipensx/downloads/card_network"));
        peers_ = addLine(networkCard, theme::kFontBody);
        pieces_ = addLine(networkCard, theme::kFontBody);

        error_ = addLine(content, theme::kFontSmall);
        error_->setTextColor(theme::error());

        auto* scroll = new brls::ScrollingFrame();
        scroll->setGrow(1);
        scroll->setContentView(content);
        frame_ = new brls::AppletFrame(scroll);
    }

    brls::View* createContentView() override {
        return frame_;
    }

    void onContentAvailable() override {
        refresh();
        timer_.setCallback([this] { refresh(); });
        timer_.start(500);
        brls::Application::giveFocus(pauseButton_);
    }

    ~DetailsActivity() override {
        timer_.stop();
    }

private:
    static brls::Label* addLine(brls::Box* box, float size) {
        auto* label = new brls::Label();
        label->setWidth(brls::View::AUTO);
        label->setFontSize(size);
        label->setMarginBottom(6);
        box->addView(label);
        return label;
    }

    static brls::Box* addCard(brls::Box* parent, const std::string& title) {
        auto* card = new brls::Box(brls::Axis::COLUMN);
        card->setBackgroundColor(theme::surface());
        card->setCornerRadius(theme::kRadiusMedium);
        card->setPadding(16, 20, 16, 20);
        card->setMarginBottom(16);
        auto* heading = new brls::Label();
        heading->setFontSize(theme::kFontCaption);
        heading->setTextColor(theme::textSecondary());
        heading->setMarginBottom(10);
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

    static brls::Button* addActionButton(brls::Box* row, const std::string& text,
                                         const brls::ButtonStyle* style) {
        auto* button = new brls::Button();
        button->setStyle(style);
        button->setFontSize(theme::kFontSmall);
        button->setHeight(52);
        button->setGrow(1);
        button->setMarginRight(12);
        button->setText(text);
        row->addView(button);
        return button;
    }

    const DownloadTask* currentTask() {
        cache_ = manager_->snapshot();
        for (const auto& task : cache_)
            if (task.id == taskId_)
                return &task;
        return nullptr;
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
        setTextIfChanged(progress_, tr("pipensx/downloads/progress_line",
                                       percentOf(progress),
                                       formatBytes(task->completedBytes),
                                       formatBytes(task->totalBytes)));

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
        setTextIfChanged(error_,
                         task->error.empty()
                             ? std::string()
                             : tr("pipensx/downloads/error_line", task->error));

        updateButtons(*task);
    }

    void updateButtons(const DownloadTask& task) {
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
        setButtonAvailable(pauseButton_, paused || active);

        bool canVerify = task.status == DownloadStatus::Paused ||
                         task.status == DownloadStatus::Error ||
                         task.status == DownloadStatus::Completed ||
                         task.status == DownloadStatus::Installed;
        setButtonAvailable(verifyButton_, canVerify);
        setButtonAvailable(removeButton_,
                           task.status != DownloadStatus::Removing);
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
    brls::Label* error_;
    brls::RepeatingTimer timer_;
    std::vector<DownloadTask> cache_;
    std::vector<uint64_t> downloadSpeedSamples_;
    std::vector<uint64_t> installSpeedSamples_;
};

}  // namespace pipensx::ui
