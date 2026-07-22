#include "ui/downloads/downloads_view.hpp"
#include "ui/i18n.hpp"

namespace pipensx::ui {

void DownloadDataSource::setTasks(std::vector<DownloadTask> tasks) {
    sections_.clear();
    // std::string, not const char*: the titles are now resolved per-locale at
    // call time rather than being string literals with static storage.
    const struct {
        std::string title;
        bool (*matches)(DownloadStatus);
    } groups[] = {
        {tr("pipensx/downloads/section_active"), [](DownloadStatus s) {
            return s == DownloadStatus::Checking ||
                   s == DownloadStatus::Downloading ||
                   s == DownloadStatus::Installing ||
                   s == DownloadStatus::Committing ||
                   s == DownloadStatus::Verifying;
        }},
        {tr("pipensx/downloads/section_queue"), [](DownloadStatus s) {
            return s == DownloadStatus::Queued;
        }},
        {tr("pipensx/downloads/section_paused"), [](DownloadStatus s) {
            return s == DownloadStatus::Paused;
        }},
        {tr("pipensx/downloads/section_completed"), [](DownloadStatus s) {
            return s == DownloadStatus::Completed ||
                   s == DownloadStatus::Installed;
        }},
        {tr("pipensx/downloads/section_errors"), [](DownloadStatus s) {
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
    cell->setTask(section.tasks[index.row], owner_->metadataService());
    return cell;
}

void DownloadDataSource::didSelectRowAt(brls::RecyclerFrame*,
                                         brls::IndexPath index) {
    Section& section = sections_[index.section];
    if (!section.tasks.empty())
        owner_->openRowMenu(section.tasks[index.row].id);
    else
        owner_->openFilePicker();
}

}  // namespace pipensx::ui
