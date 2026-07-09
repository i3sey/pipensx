#include "ui/detail/torrent_selection.hpp"

namespace pipensx::ui {

brls::RecyclerCell* TorrentSelectionDataSource::cellForRow(
    brls::RecyclerFrame* recycler, brls::IndexPath index) {
    if (entries_.empty()) {
        return recycler->dequeueReusableCell("FileSelect");
    }
    auto* cell = static_cast<TorrentSelectionCell*>(
        recycler->dequeueReusableCell("FileSelect"));
    cell->setEntry(entries_[static_cast<size_t>(index.row)]);
    return cell;
}

void TorrentSelectionDataSource::didSelectRowAt(brls::RecyclerFrame* recycler,
                                                brls::IndexPath index) {
    if (entries_.empty() ||
        index.row < 0 || static_cast<size_t>(index.row) >= entries_.size())
        return;
    cycleRow(index.row);
    recycler->reloadData();
    if (owner_)
        owner_->refreshSummary();
}

void TorrentSelectionDataSource::cycleRow(int row) {
    if (row < 0 || static_cast<size_t>(row) >= entries_.size())
        return;
    TorrentSelectionEntry& entry = entries_[static_cast<size_t>(row)];
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

}  // namespace pipensx::ui
