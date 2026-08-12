#include "ui/downloads/file_picker.hpp"

namespace pipensx::ui {

int FileDataSource::numberOfRows(brls::RecyclerFrame*, int) {
    return static_cast<int>(owner_->entries().size());
}

brls::RecyclerCell* FileDataSource::cellForRow(
    brls::RecyclerFrame* recycler, brls::IndexPath index) {
    auto* cell = static_cast<FileCell*>(
        recycler->dequeueReusableCell("File"));
    cell->setEntry(owner_->entries()[index.row]);
    return cell;
}

void FileDataSource::didSelectRowAt(brls::RecyclerFrame*,
                                    brls::IndexPath index) {
    owner_->select(static_cast<size_t>(index.row));
}

}  // namespace pipensx::ui
