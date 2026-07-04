#include "ui/catalog/catalog_view.hpp"

namespace pipensx::ui {

brls::RecyclerCell* CatalogDataSource::cellForRow(
    brls::RecyclerFrame* recycler, brls::IndexPath index) {
    if (entries_.empty()) {
        auto* cell = static_cast<TextMessageCell*>(
            recycler->dequeueReusableCell("Message"));
        cell->setMessage(message_);
        return cell;
    }

    // Cards route activation straight to the view: entry indices are stable
    // for the lifetime of one setEntries() generation, recycler rows are not.
    CatalogView* owner = owner_;
    auto activate = [owner](int entryIndex) {
        owner->onEntrySelected(entryIndex);
    };

    if (index.row < shelfRowCount()) {
        const bool isNew = !shelfNew_.empty() && index.row == 0;
        const std::vector<int>& picks = isNew ? shelfNew_ : shelfPopular_;
        std::vector<GridCardInfo> infos;
        infos.reserve(picks.size());
        for (int pick : picks)
            infos.push_back(makeInfo(pick));
        auto* cell =
            static_cast<ShelfCell*>(recycler->dequeueReusableCell("Shelf"));
        cell->setShelf(isNew ? "New" : "Popular", infos, metadata_,
                       std::move(activate), index.row);
        return cell;
    }

    const int start = (index.row - shelfRowCount()) * grid::kColumns;
    const int end = std::min(start + grid::kColumns,
                             static_cast<int>(entries_.size()));
    std::vector<GridCardInfo> infos;
    infos.reserve(static_cast<size_t>(grid::kColumns));
    for (int i = start; i < end; ++i)
        infos.push_back(makeInfo(i));
    auto* cell =
        static_cast<GridRowCell*>(recycler->dequeueReusableCell("GridRow"));
    cell->setRow(infos, metadata_, std::move(activate));
    return cell;
}

void CatalogDataSource::didSelectRowAt(brls::RecyclerFrame*,
                                       brls::IndexPath) {
    // Cards handle their own activation (click action + tap recognizer);
    // a row-level select only ever fires for the empty-state message cell.
    if (entries_.empty())
        owner_->openSearchKeyboard();
}

}  // namespace pipensx::ui
