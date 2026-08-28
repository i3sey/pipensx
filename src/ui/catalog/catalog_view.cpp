#include "ui/catalog/catalog_view.hpp"

namespace pipensx::ui {

brls::RecyclerCell* CatalogDataSource::cellForRow(
    brls::RecyclerFrame* recycler, brls::IndexPath index) {
    if (index.row == 0)
        return recycler->dequeueReusableCell("TopInset");

    if (indices_.empty()) {
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
        if (hasHero && index.row == 1) {
            auto* cell =
                static_cast<HeroCell*>(recycler->dequeueReusableCell("Hero"));
            cell->setHero(makeInfo(heroIndex_), heroImage_, metadata_,
                          std::move(activate));
            return cell;
        }
        const CatalogShelf& shelf =
            shelves_[static_cast<size_t>(
                index.row - 1 - (hasHero ? 1 : 0))];
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
                             static_cast<int>(indices_.size()));
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

void CatalogDataSource::repaintCell(brls::RecyclerCell* cell) {
    if (!cell)
        return;
    const brls::IndexPath index = cell->getIndexPath();
    if (index.row == 0)
        return;
    if (indices_.empty()) {
        if (auto* message = dynamic_cast<TextMessageCell*>(cell))
            message->setMessage(message_);
        return;
    }

    CatalogView* owner = owner_;
    auto activate = [owner](int entryIndex) {
        owner->onEntrySelected(entryIndex);
    };
    if (index.row < headerRowCount()) {
        const bool hasHero = heroIndex_ >= 0;
        if (hasHero && index.row == 1) {
            if (auto* hero = dynamic_cast<HeroCell*>(cell))
                hero->setHero(makeInfo(heroIndex_), heroImage_, metadata_,
                              std::move(activate));
            return;
        }
        const size_t shelfIndex = static_cast<size_t>(
            index.row - 1 - (hasHero ? 1 : 0));
        if (shelfIndex >= shelves_.size())
            return;
        const CatalogShelf& shelf = shelves_[shelfIndex];
        std::vector<GridCardInfo> infos;
        infos.reserve(shelf.items.size());
        for (int pick : shelf.items)
            infos.push_back(makeInfo(pick));
        if (auto* shelfCell = dynamic_cast<ShelfCell*>(cell))
            shelfCell->setShelf(shelf.title, infos, metadata_,
                                std::move(activate), index.row, shelf.seeAll);
        return;
    }

    const int start = (index.row - headerRowCount()) * grid::kColumns;
    const int end =
        std::min(start + grid::kColumns, static_cast<int>(indices_.size()));
    std::vector<GridCardInfo> infos;
    infos.reserve(static_cast<size_t>(grid::kColumns));
    for (int row = start; row < end; ++row)
        infos.push_back(makeInfo(row));
    if (auto* gridRow = dynamic_cast<GridRowCell*>(cell))
        gridRow->setRow(infos, metadata_, std::move(activate));
}

void CatalogDataSource::didSelectRowAt(brls::RecyclerFrame*,
                                       brls::IndexPath) {
    // Cards handle their own activation (click action + tap recognizer);
    // a row-level select only ever fires for the empty-state message cell.
    if (indices_.empty())
        owner_->openSearchKeyboard();
}

}  // namespace pipensx::ui
