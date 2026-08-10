#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <borealis.hpp>

#include "app/switch_deploy.hpp"
#include "ui/common/ui_helpers.hpp"
#include "ui/i18n.hpp"
#include "ui/theme.hpp"

namespace pipensx::ui {

inline std::string taskFileStateText(TaskFileState state) {
    switch (state) {
        case TaskFileState::Pending: return tr("pipensx/files/state_pending");
        case TaskFileState::Present: return tr("pipensx/files/state_present");
        case TaskFileState::Installed: return tr("pipensx/files/state_installed");
        case TaskFileState::Skipped: return tr("pipensx/files/state_skipped");
        case TaskFileState::Missing: return tr("pipensx/files/state_missing");
        case TaskFileState::Unsafe: return tr("pipensx/files/state_unsafe");
    }
    return {};
}

inline std::string deployProblemText(SwitchDeployProblem problem,
                                     const std::string& detail) {
    const char* key = nullptr;
    switch (problem) {
        case SwitchDeployProblem::None: return {};
        case SwitchDeployProblem::TaskNotFound:
            key = "pipensx/deploy/problem_task"; break;
        case SwitchDeployProblem::NotReady:
            key = "pipensx/deploy/problem_not_ready"; break;
        case SwitchDeployProblem::LayoutNotFound:
            key = "pipensx/deploy/problem_layout"; break;
        case SwitchDeployProblem::AmbiguousLayout:
            key = "pipensx/deploy/problem_ambiguous"; break;
        case SwitchDeployProblem::UnsafePath:
            key = "pipensx/deploy/problem_unsafe"; break;
        case SwitchDeployProblem::MissingSource:
            key = "pipensx/deploy/problem_missing"; break;
        case SwitchDeployProblem::Conflict:
            key = "pipensx/deploy/problem_conflict"; break;
        case SwitchDeployProblem::NoSpace:
            key = "pipensx/deploy/problem_space"; break;
        case SwitchDeployProblem::Busy:
            key = "pipensx/deploy/problem_busy"; break;
        case SwitchDeployProblem::Io:
            key = "pipensx/deploy/problem_io"; break;
    }
    std::string text = tr(key);
    if (!detail.empty())
        text += "\n" + detail;
    return text;
}

class TaskFileCell : public brls::RecyclerCell {
public:
    TaskFileCell() {
        setFocusable(true);
        setHeight(82);
        setPadding(12, 20, 12, 20);
        setAxis(brls::Axis::COLUMN);
        path_ = new brls::Label();
        path_->setSingleLine(true);
        path_->setFontSize(18);
        addView(path_);
        meta_ = new brls::Label();
        meta_->setSingleLine(true);
        meta_->setFontSize(14);
        meta_->setMarginTop(4);
        meta_->setTextColor(theme::textTertiary());
        addView(meta_);
    }

    void setFile(const TaskFileInfo& file) {
        path_->setText(file.logicalPath);
        meta_->setText(taskFileStateText(file.state) + "   " +
                       formatBytes(file.size));
        path_->setTextColor(file.state == TaskFileState::Missing ||
                                    file.state == TaskFileState::Unsafe
                                ? theme::error() : theme::textPrimary());
    }

private:
    brls::Label* path_;
    brls::Label* meta_;
};

class TaskFilesMessageCell : public brls::RecyclerCell {
public:
    TaskFilesMessageCell() {
        setHeight(72);
        setPadding(16, 20, 16, 20);
        label_ = new brls::Label();
        label_->setFontSize(theme::kFontBody);
        label_->setTextColor(theme::textSecondary());
        addView(label_);
    }
    void setText(const std::string& text) { label_->setText(text); }
private:
    brls::Label* label_;
};

class TaskFilesActivity;

class TaskFilesDataSource : public brls::RecyclerDataSource {
public:
    explicit TaskFilesDataSource(TaskFilesActivity* owner) : owner_(owner) {}
    void setFiles(std::vector<TaskFileInfo> files) { files_ = std::move(files); }
    int numberOfRows(brls::RecyclerFrame*, int) override {
        return files_.empty() ? 1 : static_cast<int>(files_.size());
    }
    brls::RecyclerCell* cellForRow(brls::RecyclerFrame* recycler,
                                    brls::IndexPath index) override;
    void didSelectRowAt(brls::RecyclerFrame*, brls::IndexPath index) override;
    const TaskFileInfo* at(size_t index) const {
        return index < files_.size() ? &files_[index] : nullptr;
    }

private:
    TaskFilesActivity* owner_;
    std::vector<TaskFileInfo> files_;
};

class TaskFilesActivity : public brls::Activity {
public:
    enum class Filter { All, Present, Installed, Skipped, Missing };

    TaskFilesActivity(std::string taskId, SwitchDeployService* deploy)
        : taskId_(std::move(taskId)), deploy_(deploy),
          alive_(std::make_shared<std::atomic<bool>>(true)) {
        auto* content = new brls::Box(brls::Axis::COLUMN);
        content->setPadding(12, 32, 12, 32);
        root_ = new brls::Label();
        root_->setFontSize(theme::kFontSmall);
        root_->setTextColor(theme::textSecondary());
        root_->setMarginBottom(10);
        content->addView(root_);
        summary_ = new brls::Label();
        summary_->setFontSize(theme::kFontBody);
        summary_->setMarginBottom(10);
        summary_->setText(tr("pipensx/files/loading"));
        content->addView(summary_);

        auto* filters = new brls::Box(brls::Axis::ROW);
        addFilter(filters, tr("pipensx/files/filter_all"), Filter::All);
        addFilter(filters, tr("pipensx/files/filter_sd"), Filter::Present);
        addFilter(filters, tr("pipensx/files/filter_installed"),
                  Filter::Installed);
        addFilter(filters, tr("pipensx/files/filter_skipped"), Filter::Skipped);
        addFilter(filters, tr("pipensx/files/filter_missing"), Filter::Missing);
        filters->setMarginBottom(8);
        content->addView(filters);

        recycler_ = new brls::RecyclerFrame();
        recycler_->setGrow(1);
        recycler_->estimatedRowHeight = 82;
        recycler_->registerCell("File", [] { return new TaskFileCell(); });
        recycler_->registerCell("Message", [] {
            return new TaskFilesMessageCell();
        });
        dataSource_ = new TaskFilesDataSource(this);
        recycler_->setDataSource(dataSource_);
        content->addView(recycler_);
        frame_ = new brls::AppletFrame(content);
        frame_->setTitle(tr("pipensx/files/title"));
    }

    explicit TaskFilesActivity(TaskFileInventory inventory)
        : TaskFilesActivity(inventory.taskId, nullptr) {
        inventory_ = std::move(inventory);
        root_->setText(tr("pipensx/files/root", inventory_.rootPath));
        summary_->setText(tr("pipensx/files/summary",
                             inventory_.files.size(),
                             formatBytes(inventory_.presentBytes)));
        applyFilter();
    }

    ~TaskFilesActivity() override { alive_->store(false); }
    brls::View* createContentView() override { return frame_; }

    void onContentAvailable() override {
        if (!deploy_)
            return;
        auto alive = alive_;
        const std::string taskId = taskId_;
        SwitchDeployService* deploy = deploy_;
        brls::async([this, alive, taskId, deploy] {
            TaskFileInventory inventory;
            std::string error;
            const bool ok = deploy->inventory(taskId, inventory, error);
            brls::sync([this, alive, ok, inventory = std::move(inventory),
                        error = std::move(error)]() mutable {
                if (!alive->load())
                    return;
                if (!ok) {
                    summary_->setText(error);
                    return;
                }
                inventory_ = std::move(inventory);
                root_->setText(tr("pipensx/files/root", inventory_.rootPath));
                summary_->setText(tr("pipensx/files/summary",
                                     inventory_.files.size(),
                                     formatBytes(inventory_.presentBytes)));
                applyFilter();
            });
        });
    }

    void showFile(size_t index) {
        const TaskFileInfo* file = dataSource_->at(index);
        if (!file)
            return;
        std::string text = file->logicalPath + "\n\n" +
            taskFileStateText(file->state) + " · " + formatBytes(file->size);
        if (!file->absolutePath.empty())
            text += "\n\n" + file->absolutePath;
        auto* dialog = new brls::Dialog(text);
        dialog->addButton(tr("pipensx/common/close"), [] {});
        dialog->open();
    }

private:
    void addFilter(brls::Box* row, const std::string& text, Filter filter) {
        auto* button = new brls::Button();
        button->setHeight(44);
        button->setGrow(1);
        button->setMarginRight(8);
        button->setFontSize(theme::kFontCaption);
        button->setText(text);
        button->registerClickAction([this, filter](brls::View*) {
            filter_ = filter;
            applyFilter();
            return true;
        });
        filterButtons_.push_back({button, filter});
        row->addView(button);
    }

    void applyFilter() {
        for (const auto& item : filterButtons_)
            item.first->setStyle(item.second == filter_
                ? &brls::BUTTONSTYLE_PRIMARY : &brls::BUTTONSTYLE_DEFAULT);
        std::vector<TaskFileInfo> files;
        for (const TaskFileInfo& file : inventory_.files) {
            bool include = filter_ == Filter::All ||
                (filter_ == Filter::Present && file.state == TaskFileState::Present) ||
                (filter_ == Filter::Installed && file.state == TaskFileState::Installed) ||
                (filter_ == Filter::Skipped && file.state == TaskFileState::Skipped) ||
                (filter_ == Filter::Missing &&
                    (file.state == TaskFileState::Missing ||
                     file.state == TaskFileState::Unsafe));
            if (include)
                files.push_back(file);
        }
        dataSource_->setFiles(std::move(files));
        recycler_->reloadData();
    }

    std::string taskId_;
    SwitchDeployService* deploy_;
    TaskFileInventory inventory_;
    Filter filter_ = Filter::All;
    std::shared_ptr<std::atomic<bool>> alive_;
    brls::AppletFrame* frame_;
    brls::Label* root_;
    brls::Label* summary_;
    brls::RecyclerFrame* recycler_;
    TaskFilesDataSource* dataSource_;
    std::vector<std::pair<brls::Button*, Filter>> filterButtons_;
};

inline brls::RecyclerCell* TaskFilesDataSource::cellForRow(
    brls::RecyclerFrame* recycler, brls::IndexPath index) {
    if (files_.empty()) {
        auto* cell = static_cast<TaskFilesMessageCell*>(
            recycler->dequeueReusableCell("Message"));
        cell->setText(tr("pipensx/files/empty"));
        return cell;
    }
    auto* cell = static_cast<TaskFileCell*>(
        recycler->dequeueReusableCell("File"));
    cell->setFile(files_[index.row]);
    return cell;
}

inline void TaskFilesDataSource::didSelectRowAt(
    brls::RecyclerFrame*, brls::IndexPath index) {
    if (!files_.empty())
        owner_->showFile(static_cast<size_t>(index.row));
}

class DeployPreviewCell : public brls::RecyclerCell {
public:
    DeployPreviewCell() {
        setFocusable(true);
        setHeight(94);
        setPadding(10, 20, 10, 20);
        setAxis(brls::Axis::COLUMN);
        source_ = new brls::Label();
        source_->setSingleLine(true);
        source_->setFontSize(16);
        source_->setTextColor(theme::textSecondary());
        addView(source_);
        destination_ = new brls::Label();
        destination_->setSingleLine(true);
        destination_->setFontSize(18);
        destination_->setMarginTop(3);
        addView(destination_);
        state_ = new brls::Label();
        state_->setSingleLine(true);
        state_->setFontSize(13);
        state_->setMarginTop(3);
        addView(state_);
    }

    void setEntry(const SwitchDeployEntry& entry) {
        source_->setText(entry.sourceRelativePath);
        destination_->setText("/switch/" + entry.destinationRelativePath);
        const char* key = entry.state == SwitchDeployEntryState::Missing
            ? "pipensx/deploy/state_copy"
            : entry.state == SwitchDeployEntryState::ExistingIdentical
                ? "pipensx/deploy/state_identical"
                : "pipensx/deploy/state_conflict";
        state_->setText(tr(key) + " · " + formatBytes(entry.size));
        state_->setTextColor(entry.state ==
                                     SwitchDeployEntryState::ExistingConflict
                                 ? theme::error() : theme::textTertiary());
    }

private:
    brls::Label* source_;
    brls::Label* destination_;
    brls::Label* state_;
};

class DeployPreviewDataSource : public brls::RecyclerDataSource {
public:
    explicit DeployPreviewDataSource(const SwitchDeployPlan* plan)
        : plan_(plan) {}
    int numberOfRows(brls::RecyclerFrame*, int) override {
        return static_cast<int>(plan_->files.size());
    }
    brls::RecyclerCell* cellForRow(brls::RecyclerFrame* recycler,
                                    brls::IndexPath index) override {
        auto* cell = static_cast<DeployPreviewCell*>(
            recycler->dequeueReusableCell("Mapping"));
        cell->setEntry(plan_->files[index.row]);
        return cell;
    }
private:
    const SwitchDeployPlan* plan_;
};

class SwitchDeployPreviewActivity : public brls::Activity {
public:
    SwitchDeployPreviewActivity(SwitchDeployInspection inspection,
                                SwitchDeployService* deploy)
        : inspection_(std::move(inspection)), deploy_(deploy) {
        auto* content = new brls::Box(brls::Axis::COLUMN);
        content->setPadding(12, 32, 12, 32);
        auto* summary = new brls::Label();
        summary->setFontSize(theme::kFontBody);
        summary->setMarginBottom(8);
        summary->setText(tr("pipensx/deploy/summary",
                            inspection_.plan.files.size(),
                            formatBytes(inspection_.plan.bytesToCopy),
                            inspection_.plan.identicalFiles,
                            inspection_.plan.ignoredFiles));
        content->addView(summary);
        auto* warning = new brls::Label();
        warning->setFontSize(theme::kFontSmall);
        warning->setTextColor(inspection_.problem == SwitchDeployProblem::None
                                  ? theme::textSecondary() : theme::error());
        warning->setText(inspection_.problem == SwitchDeployProblem::None
            ? tr("pipensx/deploy/warning")
            : deployProblemText(inspection_.problem, inspection_.detail));
        warning->setMarginBottom(8);
        content->addView(warning);

        auto* recycler = new brls::RecyclerFrame();
        recycler->setGrow(1);
        recycler->estimatedRowHeight = 94;
        recycler->registerCell("Mapping", [] {
            return new DeployPreviewCell();
        });
        recycler->setDataSource(new DeployPreviewDataSource(&inspection_.plan));
        content->addView(recycler);

        auto* copy = new brls::Button();
        copy->setHeight(52);
        copy->setStyle(&brls::BUTTONSTYLE_PRIMARY);
        copy->setText(tr("pipensx/deploy/copy"));
        copy->setState(inspection_.canStart() && deploy_
                           ? brls::ButtonState::ENABLED
                           : brls::ButtonState::DISABLED);
        copy->registerClickAction([this](brls::View*) {
            std::string error;
            if (!deploy_ || !deploy_->start(inspection_.plan.taskId, error)) {
                const bool busy = error ==
                        "Another /switch copy is already active." ||
                    error == "A package installation is active.";
                brls::Application::notify(deployProblemText(
                    busy ? SwitchDeployProblem::Busy
                         : SwitchDeployProblem::Io,
                    {}));
                return true;
            }
            brls::Application::popActivity();
            return true;
        });
        content->addView(copy);
        frame_ = new brls::AppletFrame(content);
        frame_->setTitle(tr("pipensx/deploy/preview_title"));
    }

    brls::View* createContentView() override { return frame_; }

private:
    SwitchDeployInspection inspection_;
    SwitchDeployService* deploy_;
    brls::AppletFrame* frame_;
};

} // namespace pipensx::ui
