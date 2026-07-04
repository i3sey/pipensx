#pragma once

#include <dirent.h>
#include <sys/stat.h>

#include <cstring>
#include <string>
#include <vector>

#include <borealis.hpp>

#include "app/download_manager.hpp"
#include "ui/common/ui_helpers.hpp"

namespace pipensx::ui {

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

}  // namespace pipensx::ui
