#include "ui/catalog/batch_install.hpp"

namespace pipensx::ui {

void BatchInstallDataSource::didSelectRowAt(brls::RecyclerFrame*,
                                            brls::IndexPath index) {
    if (!prepared_ || index.row < 0)
        return;
    const size_t row = static_cast<size_t>(index.row);
    if (row < prepared_->items().size())
        owner_->togglePrepared(row);
}

}  // namespace pipensx::ui
