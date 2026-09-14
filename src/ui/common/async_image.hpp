#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <borealis.hpp>

#include "app/game_metadata_service.hpp"
#include "app/stream_install_flag.hpp"
#include "ui/common/image_upload_budget.hpp"

extern "C" {
#include "core/util.h"
}

namespace pipensx::ui {

// Full-res covers (~360px RGBA) are what blew Horizon's ~4 MB mapping
// slack during stream-install. A 160px nearest preview is ~102 KB; ~20
// tiles still fit the headroom and stay readable until the worker exits.
constexpr int kStreamInstallPreviewDim = 160;

inline void nearestDownscaleRgba(const uint8_t* src, int sw, int sh,
                                 std::vector<uint8_t>& dst, int& dw, int& dh,
                                 int maxDim) {
    const int longEdge = std::max(sw, sh);
    if (longEdge <= maxDim) {
        dw = sw;
        dh = sh;
        dst.assign(src, src + static_cast<size_t>(sw) * sh * 4);
        return;
    }
    dw = std::max(1, sw * maxDim / longEdge);
    dh = std::max(1, sh * maxDim / longEdge);
    dst.resize(static_cast<size_t>(dw) * dh * 4);
    for (int y = 0; y < dh; ++y) {
        const int sy = y * sh / dh;
        for (int x = 0; x < dw; ++x) {
            const int sx = x * sw / dw;
            std::memcpy(dst.data() + (static_cast<size_t>(y) * dw + x) * 4,
                        src + (static_cast<size_t>(sy) * sw + sx) * 4, 4);
        }
    }
}

struct ImageRequestState {
    std::atomic<uint64_t> generation {0};
    std::atomic<bool> pending {false};
};

class AsyncRgbaImage;

struct AsyncImageLifetime {
    std::mutex mutex;
    AsyncRgbaImage* image = nullptr;
};

inline uint64_t& imageUploadFrameNumber() {
    static uint64_t frameNumber = 0;
    return frameNumber;
}

inline ImageUploadBudget& imageUploadBudget() {
    static ImageUploadBudget budget;
    return budget;
}

// Called exactly once immediately before Borealis renders a real frame.
inline void beginImageUploadFrame() {
    ++imageUploadFrameNumber();
}

class AsyncRgbaImage : public brls::Image {
public:
    using RequestCurrent = std::function<bool()>;

    AsyncRgbaImage() : lifetime_(std::make_shared<AsyncImageLifetime>()) {
        lifetime_->image = this;
    }

    ~AsyncRgbaImage() override {
        std::lock_guard<std::mutex> lock(lifetime_->mutex);
        lifetime_->image = nullptr;
    }

    void draw(NVGcontext* vg, float x, float y, float width, float height,
              brls::Style style, brls::FrameContext* ctx) override {
        flushDeferred();
        brls::Image::draw(vg, x, y, width, height, style, ctx);
    }

    void resetArtwork() {
        deferred_.reset();
        clear();
    }

    bool hasArtwork() {
        return getTexture() != 0 || deferred_.has_value();
    }

    // UI_PLAN F6: synchronous upload for memory-cache hits. UI thread only
    // (needs the live NVG context) — the cover paints in the same frame,
    // so catalog re-entry shows no placeholder flash.
    void setRgbaNow(std::shared_ptr<const std::vector<uint8_t>> pixels,
                    int width, int height, RequestCurrent current = {}) {
        if (!pixels || pixels->empty() || width <= 0 || height <= 0)
            return;
        deferred_ = DeferredRgba{std::move(pixels), width, height,
                                 std::move(current)};
        // A recycled cell must not paint the previous item's texture while it
        // waits for its turn in the frame budget.
        clear();
    }

    void setRgbaNow(const uint8_t* pixels, int width, int height,
                    RequestCurrent current = {}) {
        if (!pixels || width <= 0 || height <= 0)
            return;
        const size_t bytes =
            static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
        setRgbaNow(std::make_shared<std::vector<uint8_t>>(
                       pixels, pixels + bytes),
                   width, height, std::move(current));
    }

    void setRgbaAsync(std::function<void(std::function<void(
        std::shared_ptr<const std::vector<uint8_t>>, int, int)>)> provider,
        RequestCurrent current = {}) {
        std::weak_ptr<AsyncImageLifetime> weakLifetime = lifetime_;
        provider([weakLifetime, current = std::move(current)](
            std::shared_ptr<const std::vector<uint8_t>> pixels,
            int width, int height) {
            brls::sync([weakLifetime, pixels = std::move(pixels),
                        width, height, current] {
                auto lifetime = weakLifetime.lock();
                if (!lifetime)
                    return;
                std::lock_guard<std::mutex> lock(lifetime->mutex);
                if (!lifetime->image || (current && !current()))
                    return;
                lifetime->image->applyRgba(std::move(pixels), width, height,
                                           current);
            });
        });
    }

private:
    struct DeferredRgba {
        DeferredRgba(std::shared_ptr<const std::vector<uint8_t>> value,
                     int imageWidth, int imageHeight,
                     RequestCurrent requestCurrent)
            : pixels(std::move(value)), width(imageWidth),
              height(imageHeight), current(std::move(requestCurrent)) {}

        std::shared_ptr<const std::vector<uint8_t>> pixels;
        int width = 0;
        int height = 0;
        RequestCurrent current;
        std::vector<uint8_t> preview;
        int previewWidth = 0;
        int previewHeight = 0;
        bool previewUploaded = false;
    };

    bool uploadRgba(const uint8_t* pixels, int width, int height,
                    bool deferred, const RequestCurrent& current) {
        const size_t bytes =
            static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
        const uint64_t frameNumber = imageUploadFrameNumber();
        ImageUploadBudget& budget = imageUploadBudget();
        if (!budget.canUpload(frameNumber, bytes))
            return false;
        // Generation may have changed while this upload waited for a visible
        // draw and for earlier textures to consume their frame budget.
        if (current && !current())
            return false;
        const uint64_t startedUs = now_us();
        NVGcontext* vg = brls::Application::getNVGContext();
        innerSetImage(nvgCreateImageRGBA(vg, width, height, 0, pixels));
        const uint64_t durationUs = now_us() - startedUs;
        budget.recordUpload(frameNumber, bytes, durationUs);
        if (telemetry_enabled())
            telemetry_log(
                "ui", "image",
                "event=upload duration_us=%llu bytes=%llu deferred=%d",
                (unsigned long long)durationUs,
                (unsigned long long)bytes,
                deferred ? 1 : 0);
        return true;
    }

    void applyRgba(std::shared_ptr<const std::vector<uint8_t>> pixels,
                   int width, int height, RequestCurrent current) {
        if (!pixels || pixels->empty() || width <= 0 || height <= 0)
            return;
        deferred_ = DeferredRgba{std::move(pixels), width, height,
                                 std::move(current)};
    }

    void flushDeferred() {
        if (!deferred_)
            return;
        if (deferred_->current && !deferred_->current()) {
            deferred_.reset();
            return;
        }

        if (streamInstallActive()) {
            if (deferred_->previewUploaded)
                return;
            // The preview uses this same frame budget. Retain the full decode
            // so a later visible frame can upgrade it after installation.
            if (deferred_->preview.empty())
                nearestDownscaleRgba(
                    deferred_->pixels->data(), deferred_->width,
                    deferred_->height, deferred_->preview,
                    deferred_->previewWidth, deferred_->previewHeight,
                    kStreamInstallPreviewDim);
            if (uploadRgba(deferred_->preview.data(),
                           deferred_->previewWidth,
                           deferred_->previewHeight, true,
                           deferred_->current))
                deferred_->previewUploaded = true;
            return;
        }

        if (uploadRgba(deferred_->pixels->data(), deferred_->width,
                       deferred_->height, true, deferred_->current))
            deferred_.reset();
    }

    std::shared_ptr<AsyncImageLifetime> lifetime_;
    std::optional<DeferredRgba> deferred_;
};

inline void loadImageInto(AsyncRgbaImage* image, GameMetadataService* service,
                   const std::string& url,
                   const std::shared_ptr<ImageRequestState>& state,
                   uint64_t generation,
                   int maxDim = GameMetadataService::kImageDimCard,
                   GameMetadataService::ImagePriority priority =
                       GameMetadataService::ImagePriority::Visible) {
    if (!image)
        return;
    if (!service || url.empty()) {
        image->resetArtwork();
        state->pending = false;
        return;
    }
    // UI_PLAN F6: memory-cache hit → texture in the first frame, skipping
    // the worker queue (disk read + decode) and the placeholder flash.
    if (GameMetadataService::ImageData cached =
            service->cachedImage(url, maxDim)) {
        state->pending = false;
        std::shared_ptr<const std::vector<uint8_t>> pixels(
            cached, &cached->pixels);
        image->setRgbaNow(std::move(pixels), cached->width,
                          cached->height, [state, generation] {
                              return state->generation.load() == generation;
                          });
        return;
    }
    image->resetArtwork();
    state->pending = true;
    image->setRgbaAsync([service, url, state, generation, maxDim, priority](
        std::function<void(std::shared_ptr<const std::vector<uint8_t>>,
                           int, int)> done) {
        service->requestImage(url, [done, state, generation](
            GameMetadataService::ImageData bytes) {
            if (state->generation.load() != generation) {
                done(nullptr, 0, 0);
                return;
            }
            state->pending = false;
            if (!bytes || bytes->pixels.empty()) {
                done(nullptr, 0, 0);
                return;
            }
            std::shared_ptr<const std::vector<uint8_t>> pixels(
                bytes, &bytes->pixels);
            done(std::move(pixels), bytes->width, bytes->height);
        }, maxDim, priority, [state, generation] {
            return state->generation.load() == generation;
        });
    }, [state, generation] {
        return state->generation.load() == generation;
    });
}

inline void loadImageInto(AsyncRgbaImage* image, GameMetadataService* service,
                   const std::string& url,
                   int maxDim = GameMetadataService::kImageDimCard,
                   GameMetadataService::ImagePriority priority =
                       GameMetadataService::ImagePriority::Visible) {
    auto state = std::make_shared<ImageRequestState>();
    uint64_t generation = ++state->generation;
    loadImageInto(image, service, url, state, generation, maxDim, priority);
}

inline void setArtworkUrl(AsyncRgbaImage* image, GameMetadataService* service,
                   const std::string& url, std::string& currentUrl,
                   const std::shared_ptr<ImageRequestState>& state,
                   int maxDim = GameMetadataService::kImageDimCard,
                   GameMetadataService::ImagePriority priority =
                       GameMetadataService::ImagePriority::Visible) {
    if (currentUrl == url &&
        (image->hasArtwork() || state->pending.load()))
        return;
    currentUrl = url;
    uint64_t generation = ++state->generation;
    loadImageInto(image, service, url, state, generation, maxDim, priority);
}

}  // namespace pipensx::ui
