#pragma once

#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <borealis.hpp>

#include "app/app_settings.hpp"
#include "app/download_manager.hpp"
#include "ui/common/ui_helpers.hpp"
#include "ui/debrid_ui.hpp"
#include "ui/i18n.hpp"

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
        label_->setAutoAnimate(false);
        label_->setFontSize(21);
        addView(label_);
    }
    void setEntry(const FileEntry& entry) {
        label_->setText(entry.directory
                            ? tr("pipensx/picker/folder", entry.name)
                                        : entry.name);
    }
    void onFocusGained() override {
        brls::RecyclerCell::onFocusGained();
        label_->setAnimated(true);
    }

    void onFocusLost() override {
        brls::RecyclerCell::onFocusLost();
        label_->setAnimated(false);
    }
private:
    brls::Label* label_;
};

class FilePickerActivity : public brls::Activity {
public:
    FilePickerActivity(DownloadManager* manager, AppSettings* settings)
        : manager_(manager), settings_(settings), currentPath_("sdmc:/"),
          alive_(std::make_shared<std::atomic<bool>>(true)) {
        recycler_ = new brls::RecyclerFrame();
        recycler_->setPadding(8, 32, 8, 32);
        recycler_->estimatedRowHeight = 64;
        recycler_->registerCell("File", [] { return new FileCell(); });
        recycler_->setDataSource(new FileDataSource(this));
        // AppletFrame::setContentView inserts the content at index 1, below the
        // header, so handing it the recycler directly would give the recycler a
        // non-zero localY — see recyclerHost().
        frame_ = new brls::AppletFrame(recyclerHost(recycler_));
        loadDirectory(currentPath_);
    }

    ~FilePickerActivity() override {
        alive_->store(false);
        ++generation_;
    }

    brls::View* createContentView() override {
        return frame_;
    }

    const std::vector<FileEntry>& entries() const {
        return loading_ ? loadingEntries_ : entries_;
    }

    void select(size_t index) {
        if (loading_ || index >= entries_.size())
            return;
        const FileEntry entry = entries_[index];
        if (entry.directory) {
            loadDirectory(entry.path);
            return;
        }

        pipensx::TorrentPreview preview;
        std::string error;
        if (!DownloadManager::previewTorrent(entry.path, preview, error)) {
            brls::Application::notify(error);
            return;
        }
        std::string text = tr("pipensx/picker/preview", preview.name,
                              formatBytes(preview.totalBytes),
                              preview.fileCount, preview.trackerCount);
        if (preview.packageCount)
            text += tr("pipensx/picker/preview_packages",
                       preview.packageCount);
        auto* dialog = new brls::Dialog(text);
        auto add = [this, path = entry.path, preview](TransferMode mode) {
            std::string id;
            std::string error;
            bool imported = false;
            if (debridModeActive(settings_)) {
                if (!ensureDebridLinked(settings_, manager_))
                    return;
                DebridImport import;
                import.infoHash = preview.infoHash;
                import.name = preview.name;
                import.totalBytes = preview.totalBytes;
                import.provider = settings_->get().debridProvider;
                import.torrentPath = path;
                import.mode = mode;
                import.packageCount = mode == TransferMode::StreamInstall
                    ? preview.packageCount : 0;
                imported = manager_->importDebrid(import, id, error);
            } else {
                imported = manager_->importTorrent(path, mode, id, error);
            }
            if (imported) {
                brls::Application::notify(tr("pipensx/picker/added"));
                brls::Application::popActivity();
            } else {
                brls::Application::notify(error);
            }
        };
        if (preview.packageCount) {
            dialog->addButton(tr("pipensx/picker/stream_install"),
                [add] { add(TransferMode::StreamInstall); });
            dialog->addButton(tr("pipensx/picker/download_only"),
                [add] { add(TransferMode::DownloadOnly); });
        } else {
            dialog->addButton(tr("pipensx/picker/add_to_queue"),
                [add] { add(TransferMode::DownloadOnly); });
        }
        dialog->addButton(tr("pipensx/common/cancel"), [] {});
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
        const std::string previousFocus = focusedPath();
        const std::string restoreFocus =
            path == currentPath_ ? previousFocus : std::string();
        const uint64_t generation = ++generation_;
        loading_ = true;
        loadingEntries_ = {
            {tr("pipensx/files/loading"), std::string(), false}};
        reloadRecycler(std::string());

        auto alive = alive_;
        brls::async([this, alive, generation, path, previousFocus,
                     restoreFocus] {
            const uint64_t startedUs = telemetry_enabled() ? now_us() : 0;
            std::vector<FileEntry> directories;
            std::vector<FileEntry> files;
            DIR* dir = opendir(path.c_str());
            const bool opened = dir != nullptr;
            if (dir) {
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
            }
            auto byName = [](const FileEntry& a, const FileEntry& b) {
                return a.name < b.name;
            };
            std::sort(directories.begin(), directories.end(), byName);
            std::sort(files.begin(), files.end(), byName);
            std::vector<FileEntry> entries;
            if (path != "sdmc:/" && path != "/")
                entries.push_back({"..", parentPath(path), true});
            entries.insert(entries.end(), directories.begin(),
                           directories.end());
            entries.insert(entries.end(), files.begin(), files.end());
            if (startedUs)
                telemetry_log(
                    "ui", "file_picker",
                    "event=list duration_us=%llu entries=%zu opened=%d",
                    static_cast<unsigned long long>(now_us() - startedUs),
                    entries.size(), opened ? 1 : 0);
            brls::sync([this, alive, generation, path, opened,
                        previousFocus, restoreFocus,
                        entries = std::move(entries)]() mutable {
                if (!alive->load() || generation != generation_)
                    return;
                loading_ = false;
                if (!opened) {
                    brls::Application::notify(
                        tr("pipensx/picker/unable_to_open"));
                    reloadRecycler(previousFocus);
                    return;
                }
                entries_ = std::move(entries);
                currentPath_ = path;
                frame_->setTitle(
                    tr("pipensx/picker/frame_title", currentPath_));
                reloadRecycler(restoreFocus);
            });
        });
    }

    std::string focusedPath() const {
        auto* cell = dynamic_cast<brls::RecyclerCell*>(
            brls::Application::getCurrentFocus());
        if (!cell || cell->getParentActivity() != recycler_->getParentActivity())
            return {};
        const int row = cell->getIndexPath().row;
        return row >= 0 && static_cast<size_t>(row) < entries_.size()
            ? entries_[static_cast<size_t>(row)].path
            : std::string();
    }

    void reloadRecycler(const std::string& focusPath) {
        brls::View* focused = brls::Application::getCurrentFocus();
        bool ownsFocus = focused && recycler_->getParentActivity() &&
                         focused->getParentActivity() ==
                             recycler_->getParentActivity();
        int focusRow = 0;
        if (!loading_ && !focusPath.empty())
            for (size_t i = 0; i < entries_.size(); ++i)
                if (entries_[i].path == focusPath) {
                    focusRow = static_cast<int>(i);
                    break;
                }
        if (ownsFocus) {
            recycler_->setFocusable(true);
            brls::Application::giveFocus(recycler_);
        }
        recycler_->setDefaultCellFocus(brls::IndexPath(0, focusRow));
        recycler_->reloadData();
        if (ownsFocus) {
            recycler_->setFocusable(false);
            brls::Application::giveFocus(recycler_);
        }
    }

    DownloadManager* manager_;
    AppSettings* settings_;
    std::string currentPath_;
    std::vector<FileEntry> entries_;
    std::vector<FileEntry> loadingEntries_;
    std::shared_ptr<std::atomic<bool>> alive_;
    brls::RecyclerFrame* recycler_;
    brls::AppletFrame* frame_;
    uint64_t generation_ = 0;
    bool loading_ = false;

    friend class FileDataSource;
};

}  // namespace pipensx::ui
