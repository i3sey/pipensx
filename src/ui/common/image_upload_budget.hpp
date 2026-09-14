#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>

namespace pipensx::ui {

// Keep texture creation from taking an unbounded slice of one rendered frame.
// A single fullscreen image may exceed the byte ceiling, so the first upload
// is always admitted; its measured cost then closes the frame to more work.
class ImageUploadBudget {
public:
    // One MiB is roughly two worst-case 360px card textures. The time ceiling
    // limits measured driver work to about one eighth of a 60 Hz frame.
    static constexpr size_t kMaxBytesPerFrame = 1024 * 1024;
    static constexpr uint64_t kMaxUploadTimeUsPerFrame = 2000;

    bool canUpload(uint64_t frameNumber, size_t bytes) {
        selectFrame(frameNumber);
        if (bytes == 0 || uploadTimeUs_ >= kMaxUploadTimeUsPerFrame)
            return false;
        if (uploadedBytes_ == 0)
            return true;
        if (uploadedBytes_ >= kMaxBytesPerFrame)
            return false;
        return bytes <= kMaxBytesPerFrame - uploadedBytes_;
    }

    void recordUpload(uint64_t frameNumber, size_t bytes,
                      uint64_t durationUs) {
        selectFrame(frameNumber);
        uploadedBytes_ = saturatingAdd(uploadedBytes_, bytes);
        uploadTimeUs_ = saturatingAdd(uploadTimeUs_, durationUs);
    }

    size_t uploadedBytes() const { return uploadedBytes_; }
    uint64_t uploadTimeUs() const { return uploadTimeUs_; }

private:
    template <typename T>
    static T saturatingAdd(T left, T right) {
        const T limit = std::numeric_limits<T>::max();
        return right > limit - left ? limit : left + right;
    }

    void selectFrame(uint64_t frameNumber) {
        if (frameNumber_ == frameNumber)
            return;
        frameNumber_ = frameNumber;
        uploadedBytes_ = 0;
        uploadTimeUs_ = 0;
    }

    uint64_t frameNumber_ = std::numeric_limits<uint64_t>::max();
    size_t uploadedBytes_ = 0;
    uint64_t uploadTimeUs_ = 0;
};

}  // namespace pipensx::ui
