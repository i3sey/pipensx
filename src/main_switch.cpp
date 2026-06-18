#include "app/download_manager.hpp"
#include "app/catalog_service.hpp"
#include "app/magnet_resolver.hpp"
#include "app/game_metadata_service.hpp"
#include "core/antizapret.h"
#include "platform/switch_crashlog.h"

extern "C" {
#include "core/util.h"
}

#include <borealis.hpp>
#include <curl/curl.h>
#include <dirent.h>
#include <switch.h>
#include <switch-ipcext.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <exception>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

using pipensx::DownloadManager;
using pipensx::DownloadStatus;
using pipensx::DownloadTask;
using pipensx::CatalogEntry;
using pipensx::CatalogService;
using pipensx::GameMetadata;
using pipensx::GameMetadataService;
using pipensx::MagnetResolver;
using pipensx::TransferMode;

namespace {

FILE* gBorealisLog = nullptr;
std::atomic<uint32_t> gCatalogTempSerial{0};

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
        if (task.status == DownloadStatus::Installing ||
            task.status == DownloadStatus::Committing) {
            meta = "Package " + std::to_string(task.packagesInstalled + 1) +
                   " / " + std::to_string(task.packageCount);
            if (!task.currentPackage.empty())
                meta += "   " + task.currentPackage;
        } else if (task.status == DownloadStatus::Installed) {
            meta = std::to_string(task.packagesInstalled) +
                   " package(s) installed to SD";
        } else if (task.status == DownloadStatus::Downloading)
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
        install_ = addLine(content, 20);
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
                         task->status == DownloadStatus::Installing ||
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
    brls::Label* install_;
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
        if (preview.packageCount)
            text += "\n" + std::to_string(preview.packageCount) +
                    " installable NSP/NSZ package(s)";
        auto* dialog = new brls::Dialog(text);
        auto add = [this, path = entry.path](TransferMode mode) {
            std::string id;
            std::string error;
            if (manager_->importTorrent(path, mode, id, error)) {
                brls::Application::notify("Torrent added to the queue.");
                brls::Application::popActivity();
            } else {
                brls::Application::notify(error);
            }
        };
        if (preview.packageCount) {
            dialog->addButton("Install to SD while downloading",
                [add] { add(TransferMode::StreamInstall); });
            dialog->addButton("Download only",
                [add] { add(TransferMode::DownloadOnly); });
        } else {
            dialog->addButton("Add to queue",
                [add] { add(TransferMode::DownloadOnly); });
        }
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
        registerAction("Add torrent", brls::BUTTON_X, [this](brls::View*) {
            openFilePicker();
            return true;
        });
        startRefreshing();
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
                if (next[i].packagesInstalled !=
                        tasks_[i].packagesInstalled ||
                    next[i].installedBytes != tasks_[i].installedBytes ||
                    next[i].currentPackage != tasks_[i].currentPackage) {
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
                   s == DownloadStatus::Installing ||
                   s == DownloadStatus::Committing ||
                   s == DownloadStatus::Verifying;
        }},
        {"Queue", [](DownloadStatus s) {
            return s == DownloadStatus::Queued;
        }},
        {"Paused", [](DownloadStatus s) {
            return s == DownloadStatus::Paused;
        }},
        {"Completed", [](DownloadStatus s) {
            return s == DownloadStatus::Completed ||
                   s == DownloadStatus::Installed;
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

// ---------------------------------------------------------------------------
// RuTracker catalog tab
// ---------------------------------------------------------------------------

std::string formatCatalogDate(int64_t timestamp) {
    if (timestamp <= 0)
        return "Unknown date";
    std::time_t value = static_cast<std::time_t>(timestamp);
    std::tm result{};
    localtime_r(&value, &result);
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d", &result);
    return buffer;
}

class CatalogCell : public brls::RecyclerCell {
public:
    CatalogCell() {
        setFocusable(true);
        setAxis(brls::Axis::COLUMN);
        setPadding(12, 20, 12, 20);
        setHeight(82);

        auto* top = new brls::Box(brls::Axis::ROW);
        top->setAlignItems(brls::AlignItems::CENTER);
        title_ = new brls::Label();
        title_->setSingleLine(true);
        title_->setFontSize(21);
        title_->setGrow(1);
        badge_ = new brls::Label();
        badge_->setSingleLine(true);
        badge_->setFontSize(16);
        badge_->setMarginLeft(12);
        badge_->setTextColor(nvgRGB(0, 195, 227));
        top->addView(title_);
        top->addView(badge_);

        sub_ = new brls::Label();
        sub_->setSingleLine(true);
        sub_->setFontSize(16);
        sub_->setMarginTop(6);
        sub_->setTextColor(nvgRGB(160, 160, 170));
        addView(top);
        addView(sub_);
    }

    void setEntry(const CatalogEntry& entry, const std::string& badge) {
        title_->setText(entry.title);
        badge_->setText(badge);
        std::string sub = entry.size ? formatBytes(entry.size) : "Unknown size";
        sub += "   " + formatCatalogDate(entry.publishedAt);
        sub_->setText(sub);
    }

private:
    brls::Label* title_;
    brls::Label* badge_;
    brls::Label* sub_;
};

class TextMessageCell : public brls::RecyclerCell {
public:
    TextMessageCell() {
        setFocusable(true);
        setHeight(100);
        setPadding(24);
        label_ = new brls::Label();
        label_->setFontSize(20);
        addView(label_);
    }
    void setMessage(const std::string& text) { label_->setText(text); }

private:
    brls::Label* label_;
};

class CatalogView;

class CatalogDataSource : public brls::RecyclerDataSource {
public:
    explicit CatalogDataSource(CatalogView* owner) : owner_(owner) {}

    void setEntries(std::vector<CatalogEntry> entries,
                    std::vector<std::string> badges,
                    std::vector<std::string> gameNames) {
        entries_ = std::move(entries);
        badges_ = std::move(badges);
        gameNames_ = std::move(gameNames);
    }
    void setMessage(const std::string& message) { message_ = message; }
    const CatalogEntry* entryAt(int row) const {
        if (row < 0 || static_cast<size_t>(row) >= entries_.size())
            return nullptr;
        return &entries_[static_cast<size_t>(row)];
    }

    int numberOfRows(brls::RecyclerFrame*, int) override {
        return entries_.empty() ? 1 : static_cast<int>(entries_.size());
    }
    brls::RecyclerCell* cellForRow(brls::RecyclerFrame* recycler,
                                    brls::IndexPath index) override {
        if (entries_.empty()) {
            auto* cell = static_cast<TextMessageCell*>(
                recycler->dequeueReusableCell("Message"));
            cell->setMessage(message_);
            return cell;
        }
        auto* cell = static_cast<CatalogCell*>(
            recycler->dequeueReusableCell("Catalog"));
        size_t row = static_cast<size_t>(index.row);
        CatalogEntry display = entries_[row];
        if (row < gameNames_.size() && !gameNames_[row].empty())
            display.title = gameNames_[row];
        cell->setEntry(display,
                       row < badges_.size() ? badges_[row] : std::string());
        return cell;
    }
    void didSelectRowAt(brls::RecyclerFrame*, brls::IndexPath index) override;

private:
    CatalogView* owner_;
    std::vector<CatalogEntry> entries_;
    std::vector<std::string> badges_;
    std::vector<std::string> gameNames_;
    std::string message_;
};

class CatalogView : public brls::Box {
public:
    CatalogView(DownloadManager* manager, CatalogService* catalog,
                GameMetadataService* metadata)
        : brls::Box(brls::Axis::COLUMN), manager_(manager), catalog_(catalog),
          metadata_(metadata),
          alive_(std::make_shared<std::atomic<bool>>(true)),
          cancelled_(std::make_shared<std::atomic<bool>>(false)) {
        recycler_ = new brls::RecyclerFrame();
        recycler_->setGrow(1);
        recycler_->setPadding(6, 32, 6, 32);
        recycler_->estimatedRowHeight = 82;
        recycler_->registerCell("Catalog", [] { return new CatalogCell(); });
        recycler_->registerCell("Message",
            [] { return new TextMessageCell(); });
        dataSource_ = new CatalogDataSource(this);
        recycler_->setDataSource(dataSource_);

        status_ = new brls::Label();
        status_->setFontSize(15);
        status_->setMarginTop(10);
        status_->setMarginLeft(34);
        status_->setMarginBottom(2);
        status_->setTextColor(nvgRGB(140, 140, 150));
        addView(status_);
        addView(recycler_);
        rebuildEntries();

        registerAction("Search", brls::BUTTON_X, [this](brls::View*) {
            openSearchKeyboard();
            return true;
        });
        registerAction("Sort / Cancel", brls::BUTTON_Y, [this](brls::View*) {
            if (busy_)
                cancelled_->store(true);
            else
                cycleSort();
            return true;
        });
        registerAction("Refresh", brls::BUTTON_RB, [this](brls::View*) {
            refreshCatalog();
            return true;
        });
    }

    ~CatalogView() override {
        alive_->store(false);
        cancelled_->store(true);
    }

    void openSearchKeyboard() {
        if (busy_)
            return;
        brls::Application::getImeManager()->openForText(
            [this](std::string text) {
                query_ = std::move(text);
                rebuildEntries();
            },
            "Search catalog", "", 256, query_,
            brls::KEYBOARD_DISABLE_NONE);
    }

    void onEntrySelected(int row) {
        const CatalogEntry* picked = dataSource_->entryAt(row);
        if (!picked || busy_)
            return;
        showDetails(*picked);
    }

private:
    enum class SortMode { Latest, Alphabetical, Largest };

    static std::string classifyResolveFailure(const std::string& error) {
        std::string lower = lowerAscii(error);
        if (lower.find("not registered") != std::string::npos ||
            lower.find("stale") != std::string::npos)
            return "Stale";
        if (lower.find("metadata") != std::string::npos)
            return "No metadata";
        if (lower.find("no usable peers") != std::string::npos ||
            lower.find("no peers") != std::string::npos)
            return "No peers";
        return "Resolve failed";
    }

    static std::string badgeForCatalogHealth(const CatalogEntry& entry) {
        switch (entry.health) {
            case pipensx::CatalogHealth::Ok:
                return entry.metadataOk ? "Fresh" : "Checked";
            case pipensx::CatalogHealth::NoPeers:
                return "No peers";
            case pipensx::CatalogHealth::MetadataTimeout:
                return "No metadata";
            case pipensx::CatalogHealth::TrackerNotRegistered:
            case pipensx::CatalogHealth::Dead:
                return "Dead";
            case pipensx::CatalogHealth::Replaced:
                return "Replaced";
            case pipensx::CatalogHealth::Unknown:
                break;
        }
        return entry.catalogGeneratedAt || entry.sourceUpdatedAt
             ? "Unchecked" : std::string();
    }

    void resolveEntry(const CatalogEntry& entry) {
        if (busy_)
            return;
        busy_ = true;
        cancelled_->store(false);
        status_->setText("Finding peers...   (Y to cancel)");
        brls::Application::notify("Resolving torrent...");
        auto alive = alive_;
        auto cancelled = cancelled_;
        uint32_t serial = gCatalogTempSerial.fetch_add(1);
        std::string tmp = manager_->rootPath() + "/_catalog_tmp_" +
                          lowerAscii(entry.infoHash) + "_" +
                          std::to_string(serial) + ".torrent";
        brls::async([this, alive, cancelled, entry, tmp] {
            std::string err;
            MagnetResolver resolver;
            // Live stage feedback, marshalled onto the UI thread. The mutable
            // capture keeps the last text so identical updates don't flood.
            auto progress = [this, alive, last = std::string()](
                                const pipensx::MagnetProgress& p) mutable {
                std::string text;
                switch (p.stage) {
                    case pipensx::MagnetProgress::Stage::FindingPeers:
                        text = "Finding peers...";
                        break;
                    case pipensx::MagnetProgress::Stage::Connecting:
                        text = "Contacting peer " +
                               std::to_string(p.peerIndex) + "/" +
                               std::to_string(p.peerCount) + "...";
                        break;
                    case pipensx::MagnetProgress::Stage::FetchingMetadata:
                        text = "Fetching metadata " +
                               std::to_string(p.completedPieces) + "/" +
                               std::to_string(p.totalPieces) + "...";
                        break;
                    case pipensx::MagnetProgress::Stage::Validating:
                        text = "Validating...";
                        break;
                }
                if (text == last)
                    return;
                last = text;
                brls::sync([this, alive, text] {
                    if (!alive->load())
                        return;
                    if (busy_)
                        status_->setText(text + "   (Y to cancel)");
                });
            };
            bool ok = resolver.resolveToFile(
                entry.magnetUri, tmp, *cancelled, progress, err);
            brls::sync([this, alive, ok, err, tmp,
                        title = entry.title,
                        infoHash = entry.infoHash] {
                if (!alive->load())
                    return;
                busy_ = false;
                status_->setText(countText_);
                if (!ok) {
                    catalogFailures_[lowerAscii(infoHash)] =
                        classifyResolveFailure(err);
                    rebuildEntries();
                    brls::Application::notify(err);
                    return;
                }
                catalogFailures_.erase(lowerAscii(infoHash));
                rebuildEntries();
                presentImport(tmp, title);
            });
        });
    }

    static std::string lowerAscii(std::string value) {
        std::transform(value.begin(), value.end(), value.begin(),
                       [](unsigned char c) {
                           return static_cast<char>(std::tolower(c));
                       });
        return value;
    }

    void rebuildEntries() {
        // Info-hash (lower-case hex) -> status for anything already managed,
        // so rows can be badged. Task ids are lower-case hex; catalog info
        // hashes are upper-case, hence the case fold on both sides.
        std::unordered_map<std::string, DownloadStatus> added;
        for (const DownloadTask& task : manager_->snapshot())
            added[lowerAscii(task.id)] = task.status;

        std::vector<CatalogEntry> visible;
        std::string needle = lowerAscii(query_);
        for (const CatalogEntry& entry : catalog_->entries()) {
            if (entry.isHiddenByDefault())
                continue;
            if (!needle.empty() &&
                lowerAscii(entry.title).find(needle) == std::string::npos)
                continue;
            visible.push_back(entry);
        }
        if (sort_ == SortMode::Alphabetical) {
            std::stable_sort(visible.begin(), visible.end(),
                [](const CatalogEntry& left, const CatalogEntry& right) {
                    return lowerAscii(left.title) < lowerAscii(right.title);
                });
        } else if (sort_ == SortMode::Largest) {
            std::stable_sort(visible.begin(), visible.end(),
                [](const CatalogEntry& left, const CatalogEntry& right) {
                    return left.size > right.size;
                });
        } else {
            std::stable_sort(visible.begin(), visible.end(),
                [](const CatalogEntry& left, const CatalogEntry& right) {
                    return left.publishedAt > right.publishedAt;
                });
        }

        std::vector<std::string> badges;
        std::vector<std::string> gameNames;
        badges.reserve(visible.size());
        gameNames.reserve(visible.size());
        for (const CatalogEntry& entry : visible) {
            std::string hash = lowerAscii(entry.infoHash);
            auto it = added.find(hash);
            if (it != added.end()) {
                badges.push_back(badgeForStatus(it->second));
            } else {
                auto fail = catalogFailures_.find(hash);
                badges.push_back(fail == catalogFailures_.end()
                    ? badgeForCatalogHealth(entry) : fail->second);
            }
            const GameMetadata* meta = metadata_->findByInfoHash(entry.infoHash);
            gameNames.push_back(meta ? meta->name : std::string());
        }

        size_t count = visible.size();
        dataSource_->setEntries(std::move(visible), std::move(badges),
                                std::move(gameNames));
        dataSource_->setMessage(query_.empty()
            ? "Catalog is empty. Press R to refresh."
            : "Nothing found. Press X to change the search.");
        recycler_->reloadData();

        countText_ = query_.empty()
            ? withThousands(count) + (count == 1 ? " release" : " releases")
            : withThousands(count) + (count == 1 ? " match" : " matches");
        if (!busy_)
            status_->setText(countText_);
    }

    static std::string withThousands(size_t value) {
        std::string digits = std::to_string(value);
        int insertAt = static_cast<int>(digits.size()) - 3;
        while (insertAt > 0) {
            digits.insert(static_cast<size_t>(insertAt), ",");
            insertAt -= 3;
        }
        return digits;
    }

    static std::string badgeForStatus(DownloadStatus status) {
        switch (status) {
            case DownloadStatus::Queued:      return "In queue";
            case DownloadStatus::Checking:
            case DownloadStatus::Downloading:
            case DownloadStatus::Verifying:   return "Downloading";
            case DownloadStatus::Paused:      return "Paused";
            case DownloadStatus::Installing:
            case DownloadStatus::Committing:  return "Installing";
            case DownloadStatus::Completed:   return "Downloaded";
            case DownloadStatus::Installed:   return "Installed";
            case DownloadStatus::Error:       return "Error";
            case DownloadStatus::Removing:    return "Removing";
        }
        return "";
    }

    void cycleSort() {
        if (sort_ == SortMode::Latest)
            sort_ = SortMode::Alphabetical;
        else if (sort_ == SortMode::Alphabetical)
            sort_ = SortMode::Largest;
        else
            sort_ = SortMode::Latest;
        rebuildEntries();
        brls::Application::notify(
            sort_ == SortMode::Latest ? "Sorted by latest."
          : sort_ == SortMode::Alphabetical ? "Sorted alphabetically."
                                             : "Sorted by size.");
    }

    void refreshCatalog() {
        if (busy_)
            return;
        busy_ = true;
        brls::Application::notify("Updating catalog from GitHub...");
        auto alive = alive_;
        CatalogService* catalog = catalog_;
        brls::async([this, alive, catalog] {
            std::string err;
            bool ok = catalog->refresh(err);
            brls::sync([this, alive, ok, err] {
                if (!alive->load())
                    return;
                busy_ = false;
                if (!ok) {
                    brls::Application::notify(err);
                    return;
                }
                rebuildEntries();
                brls::Application::notify(
                    "Catalog updated: " +
                    std::to_string(catalog_->entries().size()) + " entries.");
            });
        });
    }

    static std::string joinStrings(const std::vector<std::string>& values,
                                   const char* separator) {
        std::string out;
        for (const std::string& value : values) {
            if (value.empty())
                continue;
            if (!out.empty())
                out += separator;
            out += value;
        }
        return out;
    }

    static std::string shortDescription(const std::string& value) {
        if (value.size() <= 900)
            return value;
        return value.substr(0, 900) + "...";
    }

    void addAsyncImage(brls::Box* parent, const std::string& url,
                       float height) {
        if (url.empty())
            return;
        auto* image = new brls::Image();
        image->setHeight(height);
        image->setMarginBottom(12);
        image->setScalingType(brls::ImageScalingType::FIT);
        GameMetadataService* service = metadata_;
        image->setImageAsync([service, url](
            std::function<void(const std::string&, size_t)> done) {
            brls::async([service, url, done] {
                std::vector<uint8_t> bytes;
                std::string error;
                if (!service->loadImage(url, bytes, error)) {
                    done("", 0);
                    return;
                }
                done(std::string(reinterpret_cast<const char*>(bytes.data()),
                                 bytes.size()), bytes.size());
            });
        });
        parent->addView(image);
    }

    brls::Box* makeDetailsContent(const CatalogEntry& entry,
                                  const GameMetadata* found) {
        auto* content = new brls::Box(brls::Axis::COLUMN);
        content->setPadding(20);

        GameMetadata meta;
        if (found)
            meta = *found;

        if (found)
            addAsyncImage(content, !meta.bannerUrl.empty()
                                     ? meta.bannerUrl : meta.iconUrl,
                          190);

        auto* title = new brls::Label();
        title->setFontSize(24);
        title->setText(found ? meta.name : entry.title);
        content->addView(title);

        auto* release = new brls::Label();
        release->setFontSize(15);
        release->setMarginTop(8);
        release->setTextColor(nvgRGB(160, 160, 170));
        release->setText("RuTracker release: " + entry.title);
        content->addView(release);

        if (found) {
            std::string facts;
            if (!meta.publisher.empty())
                facts += meta.publisher;
            if (!meta.releaseDate.empty()) {
                if (!facts.empty())
                    facts += "   ";
                facts += meta.releaseDate;
            }
            std::string categories = joinStrings(meta.categories, ", ");
            if (!categories.empty()) {
                if (!facts.empty())
                    facts += "   ";
                facts += categories;
            }
            if (!facts.empty()) {
                auto* factLabel = new brls::Label();
                factLabel->setFontSize(16);
                factLabel->setMarginTop(12);
                factLabel->setTextColor(nvgRGB(190, 190, 200));
                factLabel->setText(facts);
                content->addView(factLabel);
            }

            std::string text = !meta.description.empty()
                             ? meta.description : meta.intro;
            if (!text.empty()) {
                auto* desc = new brls::Label();
                desc->setFontSize(17);
                desc->setMarginTop(16);
                desc->setText(shortDescription(text));
                content->addView(desc);
            }

            if (!meta.screenshots.empty()) {
                auto* shots = new brls::Label();
                shots->setFontSize(16);
                shots->setMarginTop(18);
                shots->setMarginBottom(8);
                shots->setTextColor(nvgRGB(190, 190, 200));
                shots->setText("Screenshot");
                content->addView(shots);
                addAsyncImage(content, meta.screenshots.front(), 170);
            }
        } else {
            auto* missing = new brls::Label();
            missing->setFontSize(17);
            missing->setMarginTop(16);
            missing->setText("No game metadata match yet. Torrent resolving "
                             "still works from the RuTracker release.");
            content->addView(missing);
        }

        auto* torrent = new brls::Label();
        torrent->setFontSize(15);
        torrent->setMarginTop(16);
        torrent->setTextColor(nvgRGB(160, 160, 170));
        torrent->setText("Torrent size: " +
                         (entry.size ? formatBytes(entry.size)
                                     : std::string("Unknown")));
        content->addView(torrent);

        auto failure = catalogFailures_.find(lowerAscii(entry.infoHash));
        if (failure != catalogFailures_.end()) {
            auto* warning = new brls::Label();
            warning->setFontSize(15);
            warning->setMarginTop(10);
            warning->setTextColor(nvgRGB(230, 150, 80));
            warning->setText("Last resolve status: " + failure->second);
            content->addView(warning);
        } else {
            std::string health = badgeForCatalogHealth(entry);
            if (!health.empty() && health != "Fresh") {
                auto* warning = new brls::Label();
                warning->setFontSize(15);
                warning->setMarginTop(10);
                warning->setTextColor(nvgRGB(230, 150, 80));
                std::string text = "Catalog health: " + health;
                if (!entry.healthReason.empty())
                    text += " (" + entry.healthReason + ")";
                content->addView(warning);
                warning->setText(text);
            }
        }

        return content;
    }

    void showDetails(const CatalogEntry& entry) {
        const GameMetadata* found = metadata_->findByInfoHash(entry.infoHash);
        auto* scroll = new brls::ScrollingFrame();
        scroll->setHeight(520);
        scroll->setContentView(makeDetailsContent(entry, found));
        auto* dialog = new brls::Dialog(scroll);
        dialog->addButton("Check and resolve torrent", [this, entry] {
            resolveEntry(entry);
        });
        dialog->addButton("Close", [] {});
        dialog->open();
    }

    void presentImport(const std::string& path, const std::string& fallback) {
        pipensx::TorrentPreview preview;
        std::string error;
        if (!DownloadManager::previewTorrent(path, preview, error)) {
            brls::Application::notify(error);
            ::unlink(path.c_str());
            return;
        }
        std::string name = preview.name.empty() ? fallback : preview.name;
        std::string text = name + "\n" + formatBytes(preview.totalBytes) +
                           "   " + std::to_string(preview.fileCount) +
                           " files   " + std::to_string(preview.trackerCount) +
                           " trackers";
        if (preview.packageCount)
            text += "\n" + std::to_string(preview.packageCount) +
                    " installable NSP/NSZ package(s)";
        auto* dialog = new brls::Dialog(text);
        auto add = [this, path](TransferMode mode) {
            std::string id;
            std::string err;
            if (manager_->importTorrent(path, mode, id, err)) {
                log_msg("[catalog] imported torrent %s\n", id.c_str());
                brls::Application::notify("Added to downloads.");
                rebuildEntries();  // reflect the new badge immediately
            } else {
                log_msg("[catalog] import failed from '%s': %s\n",
                        path.c_str(), err.c_str());
                if (lowerAscii(err).find("already in the download manager") !=
                    std::string::npos) {
                    brls::Application::notify("Already in downloads.");
                    rebuildEntries();
                } else {
                    brls::Application::notify(err);
                }
            }
            ::unlink(path.c_str());
        };
        if (preview.packageCount) {
            dialog->addButton("Install to SD while downloading",
                [add] { add(TransferMode::StreamInstall); });
            dialog->addButton("Download only",
                [add] { add(TransferMode::DownloadOnly); });
        } else {
            dialog->addButton("Add to queue",
                [add] { add(TransferMode::DownloadOnly); });
        }
        dialog->addButton("Cancel", [path] { ::unlink(path.c_str()); });
        dialog->open();
    }

    DownloadManager* manager_;
    CatalogService* catalog_;
    GameMetadataService* metadata_;
    brls::RecyclerFrame* recycler_;
    CatalogDataSource* dataSource_;
    brls::Label* status_;
    std::shared_ptr<std::atomic<bool>> alive_;
    std::shared_ptr<std::atomic<bool>> cancelled_;
    std::unordered_map<std::string, std::string> catalogFailures_;
    std::string query_;
    std::string countText_;
    SortMode sort_ = SortMode::Latest;
    bool busy_ = false;
};

void CatalogDataSource::didSelectRowAt(brls::RecyclerFrame*,
                                       brls::IndexPath index) {
    if (!entryAt(index.row))
        owner_->openSearchKeyboard();
    else
        owner_->onEntrySelected(index.row);
}

class MainActivity : public brls::Activity {
public:
    MainActivity(DownloadManager* manager, CatalogService* catalog,
                 GameMetadataService* metadata)
        : manager_(manager), catalog_(catalog), metadata_(metadata) {
        auto* tabs = new brls::TabFrame();
        tabs->addTab("Downloads", [manager] { return new MainView(manager); });
        tabs->addTab("Catalog", [manager, catalog, metadata] {
            return new CatalogView(manager, catalog, metadata);
        });
        frame_ = new brls::AppletFrame(tabs);
        frame_->setTitle("pipensx");
    }

    brls::View* createContentView() override {
        return frame_;
    }

    void onContentAvailable() override {
        registerAction("Exit", brls::BUTTON_START,
            [this](brls::View*) {
                startupStage("quit requested by Plus");
                brls::Application::quit();
                return true;
            });
    }

private:
    DownloadManager* manager_;
    CatalogService* catalog_;
    GameMetadataService* metadata_;
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
    bool ncmReady = false;
    bool nsReady = false;
    bool esReady = false;
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

        startupStage("installer services");
        Result rc = ncmInitialize();
        if (R_FAILED(rc))
            throw std::runtime_error("ncmInitialize failed");
        ncmReady = true;
        rc = nsInitialize();
        if (R_FAILED(rc))
            throw std::runtime_error("nsInitialize failed");
        nsReady = true;
        rc = esInitialize();
        if (R_FAILED(rc))
            throw std::runtime_error("esInitialize failed");
        esReady = true;

        startupStage("Borealis Application::init");
        brls::Platform::APP_LOCALE_DEFAULT = brls::LOCALE_EN_US;
        if (!brls::Application::init())
            throw std::runtime_error("Borealis Application::init failed");

        startupStage("Borealis createWindow");
        brls::Application::createWindow("pipensx");
        brls::Application::setGlobalQuit(false);

        startupStage("CatalogService construction");
        antizapret_init("sdmc:/switch/pipensx");
        antizapret_set_enabled(1);
        unlink("sdmc:/switch/pipensx/rutracker.cfg");
        unlink("sdmc:/switch/pipensx/rutracker_cookies.txt");
        CatalogService catalog("sdmc:/switch/pipensx");
        std::string catalogError;
        if (!catalog.load(catalogError))
            log_msg("[catalog] initial load failed: %s\n",
                    catalogError.c_str());
        startupStage("GameMetadataService construction");
        GameMetadataService metadata("sdmc:/switch/pipensx");
        std::string metadataError;
        if (!metadata.load(metadataError))
            log_msg("[metadata] initial load failed: %s\n",
                    metadataError.c_str());

        startupStage("DownloadManager construction");
        DownloadManager manager("sdmc:/switch/pipensx");

        startupStage("MainActivity construction");
        auto* activity = new MainActivity(&manager, &catalog, &metadata);

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
    if (esReady)
        esExit();
    if (nsReady)
        nsExit();
    if (ncmReady)
        ncmExit();
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
