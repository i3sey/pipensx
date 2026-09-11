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

    CatalogView* owner = owner_;
    auto activate = [owner](int entryIndex) {
        owner->onEntrySelected(entryIndex);
    };

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
    if (indices_.empty())
        owner_->openSearchKeyboard();
}

}  // namespace pipensx::ui
