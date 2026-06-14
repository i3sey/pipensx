#include "app/download_manager.hpp"
#include "platform/switch_crashlog.h"

extern "C" {
#include "core/util.h"
}

#include <borealis.hpp>
#include <curl/curl.h>
#include <dirent.h>
#include <switch.h>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <exception>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <vector>

using pipensx::DownloadManager;
using pipensx::DownloadStatus;
using pipensx::DownloadTask;

namespace {

FILE* gBorealisLog = nullptr;

void startupStage(const char* stage) {
    switch_crashlog_stage(stage);
    log_msg("[startup] %s\n", stage);
}

bool isApplicationMode() {
    AppletType type = appletGetAppletType();
    return type == AppletType_Application ||
           type == AppletType_SystemApplication;
}

void showApplicationModeRequired() {
    consoleInit(nullptr);
    std::printf("\npipensx requires application mode.\n\n");
    std::printf("Close hbmenu, then hold R while launching a game.\n");
    std::printf("Keep holding R until hbmenu opens, then start pipensx.\n\n");
    std::printf("Album applet mode does not provide enough memory and\n");
    std::printf("network sessions for the GUI torrent client.\n\n");
    std::printf("Press + to exit.\n");
    consoleUpdate(nullptr);

    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);
    while (appletMainLoop()) {
        padUpdate(&pad);
        if (padGetButtonsDown(&pad) & HidNpadButton_Plus)
            break;
    }
    consoleExit(nullptr);
}

std::string formatBytes(uint64_t bytes) {
    char buffer[32];
    fmt_bytes(buffer, sizeof(buffer), bytes);
    return buffer;
}

std::string formatSpeed(uint64_t bytes) {
    char buffer[32];
    fmt_speed(buffer, sizeof(buffer), bytes);
    return buffer;
}

float progressOf(const DownloadTask& task) {
    if (!task.totalBytes)
        return 0.0f;
    return std::min(1.0f, static_cast<float>(task.completedBytes) /
                              static_cast<float>(task.totalBytes));
}

class ProgressBar : public brls::View {
public:
    ProgressBar() {
        setHeight(7);
        setCornerRadius(3.5f);
    }

    void setProgress(float progress) {
        progress_ = std::max(0.0f, std::min(1.0f, progress));
    }

    void draw(NVGcontext* vg, float x, float y, float width, float height,
              brls::Style, brls::FrameContext*) override {
        nvgBeginPath(vg);
        nvgRoundedRect(vg, x, y, width, height, height / 2);
        nvgFillColor(vg, nvgRGBA(128, 128, 128, 70));
        nvgFill(vg);
        if (progress_ > 0) {
            nvgBeginPath(vg);
            nvgRoundedRect(vg, x, y, width * progress_, height, height / 2);
            nvgFillColor(vg, nvgRGB(0, 195, 227));
            nvgFill(vg);
        }
    }

private:
    float progress_ = 0;
};

class DownloadCell : public brls::RecyclerCell {
public:
    DownloadCell() {
        setFocusable(true);
        setAxis(brls::Axis::COLUMN);
        setPadding(14, 20, 12, 20);
        setHeight(104);

        auto* top = new brls::Box(brls::Axis::ROW);
        top->setAlignItems(brls::AlignItems::CENTER);
        title_ = new brls::Label();
        title_->setSingleLine(true);
        title_->setFontSize(22);
        title_->setGrow(1);
        status_ = new brls::Label();
        status_->setSingleLine(true);
        status_->setFontSize(17);
        status_->setTextColor(nvgRGB(0, 195, 227));
        top->addView(title_);
        top->addView(status_);

        meta_ = new brls::Label();
        meta_->setSingleLine(true);
        meta_->setFontSize(16);
        meta_->setMarginTop(6);
        meta_->setMarginBottom(9);

        progress_ = new ProgressBar();
        addView(top);
        addView(meta_);
        addView(progress_);
    }

    void setTask(const DownloadTask& task) {
        title_->setText(task.name);
        status_->setText(pipensx::statusName(task.status));
        progress_->setProgress(progressOf(task));

        std::string meta = formatBytes(task.completedBytes) + " / " +
                           formatBytes(task.totalBytes);
        if (task.status == DownloadStatus::Downloading)
            meta += "   " + formatSpeed(task.speedBytesPerSecond) +
                    "   " + std::to_string(task.peers) + " peers";
        else if (task.status == DownloadStatus::Queued)
            meta += "   Waiting for the active download";
        else if (task.status == DownloadStatus::Error && !task.error.empty())
            meta += "   " + task.error;
        meta_->setText(meta);
    }

private:
    brls::Label* title_;
    brls::Label* status_;
    brls::Label* meta_;
    ProgressBar* progress_;
};

class MessageCell : public brls::RecyclerCell {
public:
    MessageCell() {
        setFocusable(true);
        setHeight(100);
        setPadding(24);
        label_ = new brls::Label();
        label_->setFontSize(21);
        label_->setText("No downloads yet. Press X to add a .torrent file.");
        addView(label_);
    }

private:
    brls::Label* label_;
};

class MainView;

class DownloadDataSource : public brls::RecyclerDataSource {
public:
    explicit DownloadDataSource(MainView* owner) : owner_(owner) {}

    void setTasks(std::vector<DownloadTask> tasks);
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
        transfer_ = addLine(content, 22);
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
        transfer_->setText("Download speed: " +
                           formatSpeed(task->speedBytesPerSecond));
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
    brls::Label* transfer_;
    brls::Label* peers_;
    brls::Label* pieces_;
    brls::Label* path_;
    brls::Label* error_;
    brls::RepeatingTimer timer_;
    std::vector<DownloadTask> cache_;
};

struct FileEntry {
    std::string name;
    std::string path;
    bool directory = false;
};

class FilePickerActivity;

class FileDataSource : public brls::RecyclerDataSource {
public:
    explicit FileDataSource(FilePickerActivity* owner) : owner_(owner) {}
    int numberOfRows(brls::RecyclerFrame*, int) override;
    brls::RecyclerCell* cellForRow(brls::RecyclerFrame*,
                                    brls::IndexPath) override;
    void didSelectRowAt(brls::RecyclerFrame*, brls::IndexPath) override;

private:
    FilePickerActivity* owner_;
};

class FileCell : public brls::RecyclerCell {
public:
    FileCell() {
        setFocusable(true);
        setHeight(64);
        setPadding(12, 24, 12, 24);
        label_ = new brls::Label();
        label_->setSingleLine(true);
        label_->setFontSize(21);
        addView(label_);
    }
    void setEntry(const FileEntry& entry) {
        label_->setText(entry.directory ? "[Folder]  " + entry.name
                                        : entry.name);
    }
private:
    brls::Label* label_;
};

class FilePickerActivity : public brls::Activity {
public:
    explicit FilePickerActivity(DownloadManager* manager)
        : manager_(manager), currentPath_("sdmc:/") {
        recycler_ = new brls::RecyclerFrame();
        recycler_->setPadding(8, 32, 8, 32);
        recycler_->estimatedRowHeight = 64;
        recycler_->registerCell("File", [] { return new FileCell(); });
        recycler_->setDataSource(new FileDataSource(this));
        frame_ = new brls::AppletFrame(recycler_);
        loadDirectory(currentPath_);
    }

    brls::View* createContentView() override {
        return frame_;
    }

    const std::vector<FileEntry>& entries() const { return entries_; }

    void select(size_t index) {
        if (index >= entries_.size())
            return;
        const FileEntry entry = entries_[index];
        if (entry.directory) {
            brls::sync([this, path = entry.path] {
                loadDirectory(path);
            });
            return;
        }

        pipensx::TorrentPreview preview;
        std::string error;
        if (!DownloadManager::previewTorrent(entry.path, preview, error)) {
            brls::Application::notify(error);
            return;
        }
        std::string text = preview.name + "\n" +
                           formatBytes(preview.totalBytes) + "   " +
                           std::to_string(preview.fileCount) + " files   " +
                           std::to_string(preview.trackerCount) + " trackers";
        auto* dialog = new brls::Dialog(text);
        dialog->addButton("Add to queue", [this, path = entry.path] {
            std::string id;
            std::string error;
            if (manager_->importTorrent(path, id, error)) {
                brls::Application::notify("Torrent added to the queue.");
                brls::Application::popActivity();
            } else {
                brls::Application::notify(error);
            }
        });
        dialog->addButton("Cancel", [] {});
        dialog->open();
    }

private:
    static bool hasTorrentExtension(const std::string& name) {
        if (name.size() < 8)
            return false;
        std::string extension = name.substr(name.size() - 8);
        std::transform(extension.begin(), extension.end(), extension.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        return extension == ".torrent";
    }

    static std::string parentPath(const std::string& path) {
        if (path == "sdmc:/" || path == "/")
            return path;
        std::string trimmed = path;
        while (trimmed.size() > 1 && trimmed.back() == '/')
            trimmed.pop_back();
        size_t slash = trimmed.find_last_of('/');
        if (slash == std::string::npos)
            return "sdmc:/";
        return trimmed.substr(0, slash + 1);
    }

    void loadDirectory(const std::string& path) {
        DIR* dir = opendir(path.c_str());
        if (!dir) {
            brls::Application::notify("Unable to open this directory.");
            return;
        }
        std::vector<FileEntry> directories;
        std::vector<FileEntry> files;
        while (dirent* item = readdir(dir)) {
            if (std::strcmp(item->d_name, ".") == 0 ||
                std::strcmp(item->d_name, "..") == 0)
                continue;
            std::string child = path;
            if (!child.empty() && child.back() != '/')
                child += '/';
            child += item->d_name;
            struct stat st {};
            if (stat(child.c_str(), &st) != 0)
                continue;
            if (S_ISDIR(st.st_mode))
                directories.push_back({item->d_name, child, true});
            else if (hasTorrentExtension(item->d_name))
                files.push_back({item->d_name, child, false});
        }
        closedir(dir);
        auto byName = [](const FileEntry& a, const FileEntry& b) {
            return a.name < b.name;
        };
        std::sort(directories.begin(), directories.end(), byName);
        std::sort(files.begin(), files.end(), byName);
        entries_.clear();
        if (path != "sdmc:/" && path != "/")
            entries_.push_back({"..", parentPath(path), true});
        entries_.insert(entries_.end(), directories.begin(), directories.end());
        entries_.insert(entries_.end(), files.begin(), files.end());
        currentPath_ = path;
        frame_->setTitle("Select .torrent   " + currentPath_);
        reloadRecycler();
    }

    void reloadRecycler() {
        brls::View* focused = brls::Application::getCurrentFocus();
        bool ownsFocus = focused && recycler_->getParentActivity() &&
                         focused->getParentActivity() ==
                             recycler_->getParentActivity();
        if (ownsFocus) {
            recycler_->setFocusable(true);
            brls::Application::giveFocus(recycler_);
        }
        recycler_->setDefaultCellFocus(brls::IndexPath(0, 0));
        recycler_->reloadData();
        if (ownsFocus) {
            recycler_->setFocusable(false);
            brls::Application::giveFocus(recycler_);
        }
    }

    DownloadManager* manager_;
    std::string currentPath_;
    std::vector<FileEntry> entries_;
    brls::RecyclerFrame* recycler_;
    brls::AppletFrame* frame_;

    friend class FileDataSource;
};

int FileDataSource::numberOfRows(brls::RecyclerFrame*, int) {
    return static_cast<int>(owner_->entries().size());
}

brls::RecyclerCell* FileDataSource::cellForRow(
    brls::RecyclerFrame* recycler, brls::IndexPath index) {
    auto* cell = static_cast<FileCell*>(
        recycler->dequeueReusableCell("File"));
    cell->setEntry(owner_->entries()[index.row]);
    return cell;
}

void FileDataSource::didSelectRowAt(brls::RecyclerFrame*,
                                    brls::IndexPath index) {
    owner_->select(static_cast<size_t>(index.row));
}

class MainView : public brls::Box {
public:
    explicit MainView(DownloadManager* manager)
        : brls::Box(brls::Axis::COLUMN), manager_(manager) {
        recycler_ = new brls::RecyclerFrame();
        recycler_->setGrow(1);
        recycler_->setPadding(6, 32, 6, 32);
        recycler_->estimatedRowHeight = 104;
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
    }

    ~MainView() override {
        timer_.stop();
    }

    void openDetails(const std::string& taskId) {
        brls::Application::pushActivity(
            new DetailsActivity(taskId, manager_));
    }

    void openFilePicker() {
        brls::Application::pushActivity(new FilePickerActivity(manager_));
    }

    void startRefreshing(bool fast = false) {
        timer_.stop();
        fastRefresh_ = fast;
        timer_.start(fast ? 100 : 750);
    }

    void stopRefreshing() {
        timer_.stop();
    }

private:
    void refresh() {
        auto next = manager_->snapshot();
        bool structureChanged = !initialized_ || next.size() != tasks_.size();
        bool changed = structureChanged;
        if (!structureChanged) {
            for (size_t i = 0; i < next.size(); ++i) {
                if (next[i].id != tasks_[i].id ||
                    next[i].status != tasks_[i].status) {
                    structureChanged = true;
                    changed = true;
                    break;
                }
                if (next[i].completedBytes != tasks_[i].completedBytes ||
                    next[i].speedBytesPerSecond !=
                        tasks_[i].speedBytesPerSecond ||
                    next[i].peers != tasks_[i].peers) {
                    changed = true;
                    break;
                }
            }
        }
        if (!changed)
            return;
        float offset = recycler_->getContentOffsetY();
        brls::View* focused = brls::Application::getCurrentFocus();
        bool ownsFocus = focused && recycler_->getParentActivity() &&
                         focused->getParentActivity() ==
                             recycler_->getParentActivity();
        auto* focusedCell = ownsFocus
            ? dynamic_cast<brls::RecyclerCell*>(focused)
            : nullptr;
        std::string focusedTaskId;
        if (focusedCell)
            focusedTaskId =
                dataSource_->taskIdAt(focusedCell->getIndexPath());
        tasks_ = std::move(next);
        initialized_ = true;
        dataSource_->setTasks(tasks_);
        recycler_->setDefaultCellFocus(
            dataSource_->indexForTask(focusedTaskId));
        if (ownsFocus) {
            recycler_->setFocusable(true);
            brls::Application::giveFocus(recycler_);
        }
        recycler_->reloadData();
        if (ownsFocus) {
            recycler_->setFocusable(false);
            brls::Application::giveFocus(recycler_);
        }
        if (!structureChanged)
            recycler_->setContentOffsetY(offset, false);
    }

    DownloadManager* manager_;
    brls::RecyclerFrame* recycler_;
    DownloadDataSource* dataSource_;
    brls::RepeatingTimer timer_;
    std::vector<DownloadTask> tasks_;
    bool initialized_ = false;
    bool fastRefresh_ = false;
};

void DownloadDataSource::setTasks(std::vector<DownloadTask> tasks) {
    sections_.clear();
    const struct {
        const char* title;
        bool (*matches)(DownloadStatus);
    } groups[] = {
        {"Active", [](DownloadStatus s) {
            return s == DownloadStatus::Checking ||
                   s == DownloadStatus::Downloading ||
                   s == DownloadStatus::Verifying;
        }},
        {"Queue", [](DownloadStatus s) {
            return s == DownloadStatus::Queued;
        }},
        {"Paused", [](DownloadStatus s) {
            return s == DownloadStatus::Paused;
        }},
        {"Completed", [](DownloadStatus s) {
            return s == DownloadStatus::Completed;
        }},
        {"Errors", [](DownloadStatus s) {
            return s == DownloadStatus::Error;
        }},
    };
    for (const auto& group : groups) {
        Section section;
        section.title = group.title;
        for (const auto& task : tasks)
            if (group.matches(task.status))
                section.tasks.push_back(task);
        if (!section.tasks.empty())
            sections_.push_back(std::move(section));
    }
    if (sections_.empty())
        sections_.push_back({"", {}});
}

std::string DownloadDataSource::taskIdAt(brls::IndexPath index) const {
    if (index.section >= sections_.size())
        return {};
    const Section& section = sections_[index.section];
    if (index.row < 0 ||
        static_cast<size_t>(index.row) >= section.tasks.size())
        return {};
    return section.tasks[static_cast<size_t>(index.row)].id;
}

brls::IndexPath DownloadDataSource::indexForTask(
    const std::string& taskId) const {
    if (!taskId.empty()) {
        for (size_t section = 0; section < sections_.size(); ++section) {
            for (size_t row = 0; row < sections_[section].tasks.size(); ++row) {
                if (sections_[section].tasks[row].id == taskId)
                    return brls::IndexPath(section, row);
            }
        }
    }
    return brls::IndexPath(0, 0);
}

int DownloadDataSource::numberOfSections(brls::RecyclerFrame*) {
    return static_cast<int>(sections_.size());
}

int DownloadDataSource::numberOfRows(brls::RecyclerFrame*, int section) {
    return sections_[section].tasks.empty()
        ? 1
        : static_cast<int>(sections_[section].tasks.size());
}

std::string DownloadDataSource::titleForHeader(brls::RecyclerFrame*,
                                                int section) {
    return sections_[section].title;
}

brls::RecyclerCell* DownloadDataSource::cellForRow(
    brls::RecyclerFrame* recycler, brls::IndexPath index) {
    Section& section = sections_[index.section];
    if (section.tasks.empty())
        return recycler->dequeueReusableCell("Message");
    auto* cell = static_cast<DownloadCell*>(
        recycler->dequeueReusableCell("Download"));
    cell->setTask(section.tasks[index.row]);
    return cell;
}

void DownloadDataSource::didSelectRowAt(brls::RecyclerFrame*,
                                         brls::IndexPath index) {
    Section& section = sections_[index.section];
    if (!section.tasks.empty())
        owner_->openDetails(section.tasks[index.row].id);
    else
        owner_->openFilePicker();
}

class MainActivity : public brls::Activity {
public:
    explicit MainActivity(DownloadManager* manager) : manager_(manager) {
        mainView_ = new MainView(manager);
        frame_ = new brls::AppletFrame(mainView_);
        frame_->setTitle("pipensx");
    }

    brls::View* createContentView() override {
        return frame_;
    }

    void onContentAvailable() override {
        mainView_->startRefreshing();
        registerAction("Add torrent", brls::BUTTON_X,
            [this](brls::View*) {
                mainView_->openFilePicker();
                return true;
            });
        registerAction("Exit", brls::BUTTON_START,
            [this](brls::View*) {
                startupStage("quit requested by Plus");
                mainView_->stopRefreshing();
                brls::Application::quit();
                return true;
            });
    }

    void onPause() override {
        mainView_->stopRefreshing();
    }

    void onResume() override {
        mainView_->startRefreshing(true);
    }

private:
    DownloadManager* manager_;
    MainView* mainView_;
    brls::AppletFrame* frame_;
};

} // namespace

int main(int, char**) {
    switch_crashlog_install();
    switch_crashlog_stage("creating application directories");
    mkdir("sdmc:/switch", 0755);
    mkdir("sdmc:/switch/pipensx", 0755);
    log_init("sdmc:/switch/pipensx/pipensx.log");
    startupStage("entered main");

    gBorealisLog = std::fopen(
        "sdmc:/switch/pipensx/pipensx.log", "a");
    if (gBorealisLog) {
        brls::Logger::setLogOutput(gBorealisLog);
        brls::Logger::setLogLevel(brls::LogLevel::LOG_DEBUG);
    }

    std::set_terminate([] {
        switch_crashlog_stage("uncaught C++ exception");
        log_msg("[crash] std::terminate called\n");
        if (gBorealisLog)
            std::fflush(gBorealisLog);
        std::_Exit(134);
    });

    bool curlReady = false;
    try {
        log_msg("[startup] applet_type=%d operation_mode=%d\n",
                (int)appletGetAppletType(), (int)appletGetOperationMode());

        if (!isApplicationMode()) {
            startupStage("unsupported applet mode");
            showApplicationModeRequired();
            startupStage("applet mode exit");
            if (gBorealisLog) {
                std::fflush(gBorealisLog);
                std::fclose(gBorealisLog);
                gBorealisLog = nullptr;
            }
            log_close();
            return 2;
        }

        startupStage("curl_global_init");
        CURLcode curlResult = curl_global_init(CURL_GLOBAL_DEFAULT);
        if (curlResult != CURLE_OK) {
            log_msg("[startup] curl_global_init failed: %d\n",
                    (int)curlResult);
            throw std::runtime_error("curl_global_init failed");
        }
        curlReady = true;

        startupStage("Borealis Application::init");
        brls::Platform::APP_LOCALE_DEFAULT = brls::LOCALE_EN_US;
        if (!brls::Application::init())
            throw std::runtime_error("Borealis Application::init failed");

        startupStage("Borealis createWindow");
        brls::Application::createWindow("pipensx");
        brls::Application::setGlobalQuit(false);

        startupStage("DownloadManager construction");
        DownloadManager manager("sdmc:/switch/pipensx");

        startupStage("MainActivity construction");
        auto* activity = new MainActivity(&manager);

        startupStage("push MainActivity");
        brls::Application::pushActivity(activity);

        startupStage("first main loop");
        bool firstFrame = true;
        while (brls::Application::mainLoop()) {
            if (firstFrame) {
                startupStage("main loop running");
                firstFrame = false;
            }
        }

        startupStage("manager shutdown");
        manager.shutdown();
    } catch (const std::exception& error) {
        log_msg("[crash] exception at stage '%s': %s\n",
                "see previous startup marker", error.what());
    } catch (...) {
        log_msg("[crash] unknown exception\n");
    }

    startupStage("cleanup");
    if (curlReady)
        curl_global_cleanup();
    if (gBorealisLog) {
        std::fflush(gBorealisLog);
        std::fclose(gBorealisLog);
        gBorealisLog = nullptr;
    }
    log_close();
    return 0;
}
