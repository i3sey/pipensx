#include "ui/detail/torrent_selection.hpp"

#include <map>

namespace pipensx::ui {

namespace {

std::vector<std::string> pathSegments(const std::string& path) {
    std::vector<std::string> parts;
    size_t start = 0;
    while (start < path.size()) {
        const size_t slash = path.find('/', start);
        if (slash == std::string::npos) {
            if (start < path.size())
                parts.push_back(path.substr(start));
            break;
        }
        if (slash > start)
            parts.push_back(path.substr(start, slash - start));
        start = slash + 1;
    }
    return parts;
}

}  // namespace

void TorrentSelectionDataSource::rebuildGroups() {
    groups_.clear();
    rootOrder_.clear();

    std::map<std::string, size_t> byPrefix;

    auto ensureFolder = [&](const std::string& prefix, const std::string& name,
                            int depth, size_t parent) -> size_t {
        auto it = byPrefix.find(prefix);
        if (it != byPrefix.end())
            return it->second;
        FolderGroup group;
        group.name = name;
        group.prefix = prefix;
        group.depth = depth;
        group.expanded = false;
        const size_t index = groups_.size();
        groups_.push_back(std::move(group));
        byPrefix.emplace(prefix, index);
        if (parent != static_cast<size_t>(-1))
            groups_[parent].children.push_back(index);
        return index;
    };

    for (size_t i = 0; i < entries_.size(); ++i) {
        const std::vector<std::string> parts = pathSegments(entries_[i].path);
        if (parts.size() <= 1) {
            rootOrder_.push_back({false, i});
            continue;
        }

        size_t parent = static_cast<size_t>(-1);
        std::string prefix;
        for (size_t d = 0; d + 1 < parts.size(); ++d) {
            prefix += parts[d];
            prefix += '/';
            const bool created = byPrefix.find(prefix) == byPrefix.end();
            const size_t folder =
                ensureFolder(prefix, parts[d], static_cast<int>(d), parent);
            if (d == 0 && created)
                rootOrder_.push_back({true, folder});
            parent = folder;
        }
        groups_[parent].files.push_back(i);
    }

    // Fill descendant file lists from the leaves up.
    for (size_t i = 0; i < entries_.size(); ++i) {
        const std::vector<std::string> parts = pathSegments(entries_[i].path);
        if (parts.size() <= 1)
            continue;
        std::string prefix;
        for (size_t d = 0; d + 1 < parts.size(); ++d) {
            prefix += parts[d];
            prefix += '/';
            auto it = byPrefix.find(prefix);
            if (it != byPrefix.end())
                groups_[it->second].indices.push_back(i);
        }
    }

    rebuildVisible();
}

void TorrentSelectionDataSource::emitVisible(size_t groupIndex) {
    if (groupIndex >= groups_.size())
        return;
    const FolderGroup& group = groups_[groupIndex];
    visible_.push_back(
        {VisibleKind::Folder, groupIndex, 0, group.depth});
    if (!group.expanded)
        return;

    struct Child {
        bool folder = false;
        size_t index = 0;
        size_t sortKey = 0;
    };
    std::vector<Child> merged;
    merged.reserve(group.children.size() + group.files.size());
    for (size_t child : group.children) {
        const size_t key = groups_[child].indices.empty()
            ? static_cast<size_t>(-1)
            : groups_[child].indices.front();
        merged.push_back({true, child, key});
    }
    for (size_t file : group.files)
        merged.push_back({false, file, file});
    std::sort(merged.begin(), merged.end(),
              [](const Child& a, const Child& b) {
                  return a.sortKey < b.sortKey;
              });
    for (const Child& child : merged) {
        if (child.folder)
            emitVisible(child.index);
        else
            visible_.push_back({VisibleKind::File, groupIndex, child.index,
                                group.depth + 1});
    }
}

void TorrentSelectionDataSource::rebuildVisible() {
    visible_.clear();
    for (const RootItem& item : rootOrder_) {
        if (item.folder)
            emitVisible(item.index);
        else
            visible_.push_back({VisibleKind::File, 0, item.index, 0});
    }
}

int TorrentSelectionDataSource::visibleRowCount() const {
    return entries_.empty() ? 1 : static_cast<int>(visible_.size());
}

bool TorrentSelectionDataSource::anyFolderCollapsed() const {
    for (const auto& group : groups_) {
        if (isCollapsibleFolder(group) && !group.expanded)
            return true;
    }
    return false;
}

bool TorrentSelectionDataSource::toggleFolderAtVisibleRow(int row) {
    if (row < 0 || static_cast<size_t>(row) >= visible_.size())
        return false;
    const VisibleRow& vr = visible_[static_cast<size_t>(row)];
    if (vr.kind != VisibleKind::Folder) {
        const bool expand = anyFolderCollapsed();
        for (auto& g : groups_) {
            if (isCollapsibleFolder(g))
                g.expanded = expand;
        }
    } else {
        groups_[vr.groupIndex].expanded = !groups_[vr.groupIndex].expanded;
    }
    rebuildVisible();
    return true;
}

void TorrentSelectionDataSource::setAllFoldersExpanded(bool expanded) {
    for (auto& group : groups_) {
        if (isCollapsibleFolder(group))
            group.expanded = expanded;
    }
    rebuildVisible();
}

void TorrentSelectionDataSource::cycleFolder(size_t groupIndex) {
    if (groupIndex >= groups_.size())
        return;
    const FolderGroup& group = groups_[groupIndex];
    if (group.indices.empty())
        return;

    bool allPackages = true;
    bool allInstall = true;
    bool allDownload = true;
    for (size_t index : group.indices) {
        const TorrentSelectionEntry& entry = entries_[index];
        if (!entry.package)
            allPackages = false;
        if (entry.action != FileAction::Install)
            allInstall = false;
        if (entry.action != FileAction::Download)
            allDownload = false;
    }

    FileAction next;
    if (allPackages) {
        if (allInstall)
            next = FileAction::Download;
        else if (allDownload)
            next = FileAction::Skip;
        else
            next = FileAction::Install;
    } else {
        if (allDownload)
            next = FileAction::Skip;
        else
            next = FileAction::Download;
    }

    for (size_t index : group.indices)
        entries_[index].action = next;
}

const TorrentSelectionDataSource::VisibleRow*
TorrentSelectionDataSource::visibleAt(int row) const {
    if (row < 0 || static_cast<size_t>(row) >= visible_.size())
        return nullptr;
    return &visible_[static_cast<size_t>(row)];
}

const TorrentSelectionDataSource::FolderGroup*
TorrentSelectionDataSource::groupAt(size_t groupIndex) const {
    if (groupIndex >= groups_.size())
        return nullptr;
    return &groups_[groupIndex];
}

brls::RecyclerCell* TorrentSelectionDataSource::cellForRow(
    brls::RecyclerFrame* recycler, brls::IndexPath index) {
    if (entries_.empty()) {
        auto* cell = static_cast<TorrentSelectionCell*>(
            recycler->dequeueReusableCell("FileSelect"));
        cell->setEmpty();
        return cell;
    }
    const VisibleRow* vr = visibleAt(index.row);
    if (!vr)
        return nullptr;
    if (vr->kind == VisibleKind::Folder) {
        auto* cell = static_cast<TorrentFolderCell*>(
            recycler->dequeueReusableCell("FolderSelect"));
        cell->setGroup(*groupAt(vr->groupIndex), entries_);
        return cell;
    }
    auto* cell = static_cast<TorrentSelectionCell*>(
        recycler->dequeueReusableCell("FileSelect"));
    cell->setEntry(entries_[vr->entryIndex], vr->depth);
    return cell;
}

void TorrentSelectionDataSource::didSelectRowAt(brls::RecyclerFrame*,
                                                brls::IndexPath index) {
    if (entries_.empty())
        return;
    const VisibleRow* vr = visibleAt(index.row);
    if (!vr)
        return;
    if (vr->kind == VisibleKind::Folder) {
        if (owner_)
            owner_->cycleFolderAtRow(index.row);
        return;
    }
    cycleEntry(static_cast<int>(vr->entryIndex));
    if (owner_) {
        owner_->repaintVisibleRow(index.row);
        owner_->refreshSummary();
    }
}

void TorrentSelectionDataSource::cycleEntry(int entryIndex) {
    if (entryIndex < 0 ||
        static_cast<size_t>(entryIndex) >= entries_.size())
        return;
    TorrentSelectionEntry& entry =
        entries_[static_cast<size_t>(entryIndex)];
    if (entry.package) {
        entry.action = entry.action == FileAction::Install
            ? FileAction::Download
            : entry.action == FileAction::Download ? FileAction::Skip
                                                   : FileAction::Install;
    } else {
        entry.action = entry.action == FileAction::Download
            ? FileAction::Skip
            : FileAction::Download;
    }
}

void TorrentSelectionDataSource::cycleRow(int row) {
    const VisibleRow* vr = visibleAt(row);
    if (!vr || vr->kind != VisibleKind::File)
        return;
    cycleEntry(static_cast<int>(vr->entryIndex));
}

}  // namespace pipensx::ui
