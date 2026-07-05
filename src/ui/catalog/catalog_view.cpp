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

    if (index.row < headerRowCount()) {
        const bool hasHero = heroIndex_ >= 0;
        if (hasHero && index.row == 0) {
            auto* cell =
                static_cast<HeroCell*>(recycler->dequeueReusableCell("Hero"));
            cell->setHero(makeInfo(heroIndex_), heroImage_, metadata_,
                          std::move(activate));
            return cell;
        }
        const CatalogShelf& shelf =
            shelves_[static_cast<size_t>(index.row - (hasHero ? 1 : 0))];
        std::vector<GridCardInfo> infos;
        infos.reserve(shelf.items.size());
        for (int pick : shelf.items)
            infos.push_back(makeInfo(pick));
        auto* cell =
            static_cast<ShelfCell*>(recycler->dequeueReusableCell("Shelf"));
        cell->setShelf(shelf.title, infos, metadata_, std::move(activate),
                       index.row, shelf.seeAll);
        return cell;
    }

    const int start = (index.row - headerRowCount()) * grid::kColumns;
    const int end = std::min(start + grid::kColumns,
                             static_cast<int>(entries_.size()));
    std::vector<GridCardInfo> infos;
    infos.reserve(static_cast<size_t>(grid::kColumns));
    for (int i = start; i < end; ++i)
        infos.push_back(makeInfo(i));
    auto* cell =
        static_cast<GridRowCell*>(recycler->dequeueReusableCell("GridRow"));
    cell->setRow(infos, metadata_, std::move(activate));
    // UI_PLAN F6: pre-decode the neighbouring rows into the memory cache so
    // scrolling hits it instead of the disk-read + decode path.
    prefetchGridRow(index.row - 1);
    prefetchGridRow(index.row + 1);
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
