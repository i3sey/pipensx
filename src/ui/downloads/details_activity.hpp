#pragma once

#include <string>

#include <borealis.hpp>

#include "app/download_manager.hpp"
#include "ui/common/progress_bar.hpp"
#include "ui/common/speed_graph.hpp"
#include "ui/common/ui_helpers.hpp"
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
        pauseButton_ = addActionButton(actions, "Pause",
                                       &brls::BUTTONSTYLE_PRIMARY);
        verifyButton_ = addActionButton(actions, "Verify",
                                        &brls::BUTTONSTYLE_DEFAULT);
        removeButton_ = addActionButton(actions, "Remove",
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

        auto* progressCard = addCard(content, "Progress");
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

        auto* speedCard = addCard(content, "Speed");
        auto* speedLegend = new brls::Box(brls::Axis::ROW);
        speedLegend->setAlignItems(brls::AlignItems::CENTER);
        speedLegend->setMarginBottom(8);
        downloadSpeed_ = addSpeedLegend(speedLegend, theme::accent(), nullptr);
        installSpeed_ = addSpeedLegend(speedLegend, theme::success(),
                                       &installSpeedItem_);
        speedCard->addView(speedLegend);
        speedGraph_ = new SpeedGraphView();
        speedCard->addView(speedGraph_);

        auto* networkCard = addCard(content, "Network");
        peers_ = addLine(networkCard, theme::kFontBody);
        pieces_ = addLine(networkCard, theme::kFontBody);

        path_ = addLine(content, theme::kFontCaption);
        path_->setTextColor(theme::textTertiary());
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
        frame_->setTitle(task->name);
        status_->setText(std::string("Status: ") +
                         pipensx::statusName(task->status));
        status_->setTextColor(statusColor(task->status));

        bool installing = task->status == DownloadStatus::Installing ||
                          task->status == DownloadStatus::Committing;
        float progress = installing ? installProgressOf(*task)
                                    : progressOf(*task);
        progressBar_->setProgress(progress);
        progress_->setText(std::to_string(percentOf(progress)) + "%  ·  " +
                           formatBytes(task->completedBytes) + " / " +
                           formatBytes(task->totalBytes));

        std::string eta;
        if (task->status == DownloadStatus::Downloading &&
            task->totalBytes > task->completedBytes)
            eta = formatEta(task->totalBytes - task->completedBytes,
                            task->speedBytesPerSecond);
        eta_->setText(eta.empty() ? "" : "ETA " + eta);

        if (task->mode == TransferMode::StreamInstall && task->packageCount) {
            const bool hasCurrent = !task->currentPackage.empty() &&
                                    task->packagesInstalled < task->packageCount;
            if (hasCurrent) {
                package_->setText(
                    "Package " + std::to_string(task->packagesInstalled + 1) +
                    " of " + std::to_string(task->packageCount));
            } else {
                package_->setText(
                    "Packages installed: " +
                    std::to_string(task->packagesInstalled) + " of " +
                    std::to_string(task->packageCount));
            }
            currentPackage_->setText(task->currentPackage);
        } else {
            package_->setText("");
            currentPackage_->setText("");
        }

        recordSpeedSample(*task);
        downloadSpeed_->setText(
            "Download: " + formatSpeed(task->speedBytesPerSecond));
        if (task->mode == TransferMode::StreamInstall) {
            installSpeedItem_->setVisibility(brls::Visibility::VISIBLE);
            installSpeed_->setText(
                "Install: " + formatSpeed(installSpeedSmoothed_));
        } else {
            installSpeedItem_->setVisibility(brls::Visibility::GONE);
        }
        peers_->setText("Peers: " + std::to_string(task->peers) +
                        "   DHT: " + std::to_string(task->dhtGood) + " good / " +
                        std::to_string(task->dhtDubious) + " dubious");
        pieces_->setText("Pieces: " + std::to_string(task->piecesDone) + " / " +
                         std::to_string(task->piecesTotal) +
                         "   Checked: " +
                         std::to_string(task->piecesVerified));
        path_->setText("Output: " + task->dataPath);
        error_->setText(task->error.empty() ? "" : "Error: " + task->error);

        updateButtons(*task);
    }

    void updateButtons(const DownloadTask& task) {
        bool paused = task.status == DownloadStatus::Paused ||
                      task.status == DownloadStatus::Error;
        bool active = task.status == DownloadStatus::Queued ||
                      task.status == DownloadStatus::Checking ||
                      task.status == DownloadStatus::Downloading ||
                      task.status == DownloadStatus::Installing ||
                      task.status == DownloadStatus::Committing ||
                      task.status == DownloadStatus::Verifying;
        pauseButton_->setText(paused ? "Resume" : "Pause");
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

    void recordSpeedSample(const DownloadTask& task) {
        uint64_t now = now_ms();
        appendSpeedSample(downloadSpeedSamples_, task.speedBytesPerSecond);

        if (task.mode == TransferMode::StreamInstall) {
            uint64_t installSpeed = 0;
            if (hasInstallSample_ && now > lastInstallSampleMs_ &&
                task.installedBytes >= lastInstallBytes_) {
                uint64_t elapsed = now - lastInstallSampleMs_;
                installSpeed =
                    (task.installedBytes - lastInstallBytes_) * 1000 / elapsed;
            }
            lastInstallBytes_ = task.installedBytes;
            lastInstallSampleMs_ = now;
            hasInstallSample_ = true;
            installSpeedSmoothed_ = emaUpdate(installSpeedSmoothed_,
                                              installSpeed);
            appendSpeedSample(installSpeedSamples_, installSpeedSmoothed_);
        } else {
            hasInstallSample_ = false;
            lastInstallBytes_ = 0;
            lastInstallSampleMs_ = now;
            installSpeedSmoothed_ = 0;
            installSpeedSamples_.clear();
        }

        speedGraph_->setSamples(downloadSpeedSamples_, installSpeedSamples_);
    }

    void openRemoveDialog() {
        auto* dialog = new brls::Dialog(
            "Remove this download from pipensx?");
        dialog->addButton("Keep downloaded data", [this] {
            std::string error;
            if (!manager_->remove(taskId_, false, error))
                brls::Application::notify(error);
            else
                brls::Application::popActivity();
        });
        dialog->addButton("Delete downloaded data", [this] {
            std::string error;
            if (!manager_->remove(taskId_, true, error))
                brls::Application::notify(error);
            else
                brls::Application::popActivity();
        });
        dialog->addButton("Cancel", [] {});
        dialog->open();
    }

    std::string taskId_;
    DownloadManager* manager_;
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
    brls::Label* path_;
    brls::Label* error_;
    brls::RepeatingTimer timer_;
    std::vector<DownloadTask> cache_;
    std::vector<uint64_t> downloadSpeedSamples_;
    std::vector<uint64_t> installSpeedSamples_;
    uint64_t installSpeedSmoothed_ = 0;
    uint64_t lastInstallBytes_ = 0;
    uint64_t lastInstallSampleMs_ = 0;
    bool hasInstallSample_ = false;
};

}  // namespace pipensx::ui
