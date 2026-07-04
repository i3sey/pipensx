#include "ui/catalog/catalog_view.hpp"

namespace pipensx::ui {

void CatalogDataSource::didSelectRowAt(brls::RecyclerFrame*,
                                       brls::IndexPath index) {
    if (!entryAt(index.row))
        owner_->openSearchKeyboard();
    else
        owner_->onEntrySelected(index.row);
}

}  // namespace pipensx::ui
