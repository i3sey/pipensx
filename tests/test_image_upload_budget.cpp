#include "ui/common/image_upload_budget.hpp"

#include <cassert>

using pipensx::ui::ImageUploadBudget;

int main() {
    ImageUploadBudget budget;
    const size_t half = ImageUploadBudget::kMaxBytesPerFrame / 2;

    assert(budget.canUpload(10, half));
    budget.recordUpload(10, half, 400);
    assert(budget.canUpload(10, half));
    budget.recordUpload(10, half, 500);
    assert(!budget.canUpload(10, 1));

    // The budget follows the render-loop frame number, not elapsed wall time.
    assert(budget.canUpload(11, half));
    budget.recordUpload(11, half, 400);

    // One large texture must make progress, then close this frame by bytes.
    const size_t fullScreen = ImageUploadBudget::kMaxBytesPerFrame * 5;
    assert(budget.canUpload(12, fullScreen));
    budget.recordUpload(12, fullScreen, 500);
    assert(!budget.canUpload(12, 1));

    // Actual measured upload time also closes the frame below the byte cap.
    assert(budget.canUpload(13, 64));
    budget.recordUpload(13, 64,
                        ImageUploadBudget::kMaxUploadTimeUsPerFrame);
    assert(!budget.canUpload(13, 64));
    assert(budget.canUpload(14, 64));

    assert(!budget.canUpload(15, 0));
    return 0;
}
