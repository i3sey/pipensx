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
    entries_[static_cast<size_t>(index.row)].selected =
        !entries_[static_cast<size_t>(index.row)].selected;
    recycler->reloadData();
    if (owner_)
        owner_->refreshSummary();
}

}  // namespace pipensx::ui
