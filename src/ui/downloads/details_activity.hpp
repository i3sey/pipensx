#pragma once

#include <string>

#include <borealis.hpp>

#include "app/download_manager.hpp"
#include "ui/common/progress_bar.hpp"
#include "ui/common/speed_graph.hpp"
#include "ui/common/ui_helpers.hpp"

namespace pipensx::ui {

class DetailsActivity : public brls::Activity {
public:
    DetailsActivity(std::string taskId, DownloadManager* manager)
        : taskId_(std::move(taskId)), manager_(manager) {
        auto* content = new brls::Box(brls::Axis::COLUMN);
        content->setFocusable(true);
        content->setGrow(1);
        content->setPadding(30, 55, 30, 55);
        content->setJustifyContent(brls::JustifyContent::CENTER);
        content->setAlignItems(brls::AlignItems::STRETCH);
        content->setBackgroundColor(nvgRGBA(45, 45, 50, 180));
        content->setCornerRadius(12);

        status_ = addLine(content, 27);
        progress_ = addLine(content, 22);
        install_ = addLine(content, 20);
        transfer_ = addLine(content, 22);
        speedGraph_ = new SpeedGraphView();
        content->addView(speedGraph_);
        peers_ = addLine(content, 22);
        pieces_ = addLine(content, 22);
        path_ = addLine(content, 18);
        error_ = addLine(content, 18);
        error_->setTextColor(nvgRGB(255, 80, 80));

        auto* frame = new brls::AppletFrame(content);
        frame_ = frame;
    }

    brls::View* createContentView() override {
        return frame_;
    }

    void onContentAvailable() override {
        refresh();
        registerAction("Pause / Resume / Verify", brls::BUTTON_Y,
            [this](brls::View*) {
                auto task = currentTask();
                if (!task)
                    return false;
                if (task->status == DownloadStatus::Paused ||
                    task->status == DownloadStatus::Error)
                    manager_->resume(taskId_);
                else if (task->status == DownloadStatus::Completed)
                    manager_->verify(taskId_);
                else if (task->status == DownloadStatus::Queued ||
                         task->status == DownloadStatus::Checking ||
                         task->status == DownloadStatus::Downloading ||
                         task->status == DownloadStatus::Installing ||
                         task->status == DownloadStatus::Committing ||
                         task->status == DownloadStatus::Verifying)
                    manager_->pause(taskId_);
                refresh();
                return true;
            });
        registerAction("Remove", brls::BUTTON_X,
            [this](brls::View*) {
                openRemoveDialog();
                return true;
            });

        timer_.setCallback([this] { refresh(); });
        timer_.start(500);
    }

    ~DetailsActivity() override {
        timer_.stop();
    }

private:
    static brls::Label* addLine(brls::Box* box, float size) {
        auto* label = new brls::Label();
        label->setWidth(brls::View::AUTO);
        label->setFontSize(size);
        label->setMarginBottom(13);
        box->addView(label);
        return label;
    }

    const DownloadTask* currentTask() {
        cache_ = manager_->snapshot();
        for (const auto& task : cache_)
            if (task.id == taskId_)
                return &task;
        return nullptr;
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
        int percent = task->totalBytes
            ? static_cast<int>(task->completedBytes * 100 / task->totalBytes)
            : 0;
        progress_->setText("Progress: " + std::to_string(percent) + "%  (" +
                           formatBytes(task->completedBytes) + " / " +
                           formatBytes(task->totalBytes) + ")");
        if (task->mode == TransferMode::StreamInstall) {
            int installPercent = task->installTotalBytes
                ? static_cast<int>(task->installedBytes * 100 /
                                   task->installTotalBytes)
                : 0;
            install_->setText(
                "Install: " + std::to_string(task->packagesInstalled) +
                " / " + std::to_string(task->packageCount) + " packages" +
                (task->currentPackage.empty()
                    ? ""
                    : "   " + task->currentPackage + "   " +
                      std::to_string(installPercent) + "%"));
        } else {
            install_->setText("");
        }
        recordSpeedSample(*task);
        if (task->mode == TransferMode::StreamInstall) {
            transfer_->setText(
                "Download: " + formatSpeed(task->speedBytesPerSecond) +
                "   Install: " + formatSpeed(installSpeedSmoothed_));
        } else {
            transfer_->setText(
                "Download: " + formatSpeed(task->speedBytesPerSecond));
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
    brls::Label* progress_;
    brls::Label* install_;
    brls::Label* transfer_;
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
