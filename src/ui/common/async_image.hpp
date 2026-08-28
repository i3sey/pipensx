#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <borealis.hpp>

#include "app/game_metadata_service.hpp"
#include "app/stream_install_flag.hpp"

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

// All full texture uploads originate on the UI thread. Use a process-wide
// 16 ms bucket so a burst of cache hits cannot monopolize one rendered frame.
inline bool claimFullImageUploadBudget() {
    struct Budget {
        uint64_t bucket = static_cast<uint64_t>(-1);
        unsigned uploads = 0;
    };
    static Budget budget;
    const uint64_t bucket = now_us() / 16000;
    if (budget.bucket != bucket) {
        budget.bucket = bucket;
        budget.uploads = 0;
    }
    if (budget.uploads >= 2)
        return false;
    ++budget.uploads;
    return true;
}

class AsyncRgbaImage : public brls::Image {
public:
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
    void setRgbaNow(const uint8_t* pixels, int width, int height) {
        if (!pixels || width <= 0 || height <= 0)
            return;
        if (streamInstallActive()) {
            const size_t bytes =
                static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
            applyRgba(std::make_shared<std::vector<uint8_t>>(
                          pixels, pixels + bytes),
                      width, height);
            return;
        }
        if (!claimFullImageUploadBudget()) {
            const size_t bytes =
                static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
            deferred_ = DeferredRgba{
                std::make_shared<std::vector<uint8_t>>(pixels, pixels + bytes),
                width, height};
            clear();
            return;
        }
        deferred_.reset();
        uploadRgba(pixels, width, height, false);
    }

    void setRgbaAsync(std::function<void(std::function<void(
        std::shared_ptr<const std::vector<uint8_t>>, int, int)>)> provider) {
        std::weak_ptr<AsyncImageLifetime> weakLifetime = lifetime_;
        provider([weakLifetime](
            std::shared_ptr<const std::vector<uint8_t>> pixels,
            int width, int height) {
            brls::sync([weakLifetime, pixels = std::move(pixels),
                        width, height] {
                auto lifetime = weakLifetime.lock();
                if (!lifetime)
                    return;
                std::lock_guard<std::mutex> lock(lifetime->mutex);
                if (!lifetime->image)
                    return;
                lifetime->image->applyRgba(std::move(pixels), width, height);
            });
        });
    }

private:
    struct DeferredRgba {
        std::shared_ptr<const std::vector<uint8_t>> pixels;
        int width = 0;
        int height = 0;
    };

    void uploadRgba(const uint8_t* pixels, int width, int height,
                    bool deferred) {
        const uint64_t startedUs = telemetry_enabled() ? now_us() : 0;
        NVGcontext* vg = brls::Application::getNVGContext();
        innerSetImage(nvgCreateImageRGBA(vg, width, height, 0, pixels));
        if (startedUs)
            telemetry_log(
                "ui", "image",
                "event=upload duration_us=%llu bytes=%llu deferred=%d",
                (unsigned long long)(now_us() - startedUs),
                (unsigned long long)(
                    static_cast<size_t>(width) *
                    static_cast<size_t>(height) * 4),
                deferred ? 1 : 0);
    }

    void applyRgba(std::shared_ptr<const std::vector<uint8_t>> pixels,
                   int width, int height) {
        if (!pixels || pixels->empty() || width <= 0 || height <= 0)
            return;
        // Stream-install already owns the process mapping slack (ENOBUFS
        // on sockets, ~4 MB kernel headroom). Full-res covers killed the
        // Zelda session; a 160px preview does not. Keep the decode for a
        // full upload on the first draw after the worker exits.
        if (streamInstallActive()) {
            // Always replace the texture with the new preview: a recycled
            // tile can still hold the previous game's texture, and keeping
            // it makes covers appear swapped while the install runs.
            deferred_ = DeferredRgba{pixels, width, height};
            std::vector<uint8_t> preview;
            int pw = 0;
            int ph = 0;
            nearestDownscaleRgba(pixels->data(), width, height, preview, pw,
                                 ph, kStreamInstallPreviewDim);
            uploadRgba(preview.data(), pw, ph, true);
            return;
        }
        if (!claimFullImageUploadBudget()) {
            deferred_ = DeferredRgba{std::move(pixels), width, height};
            clear();
            return;
        }
        deferred_.reset();
        uploadRgba(pixels->data(), width, height, false);
    }

    void flushDeferred() {
        if (!deferred_ || streamInstallActive())
            return;
        if (!claimFullImageUploadBudget())
            return;
        DeferredRgba held = std::move(*deferred_);
        deferred_.reset();
        uploadRgba(held.pixels->data(), held.width, held.height, true);
    }

    std::shared_ptr<AsyncImageLifetime> lifetime_;
    std::optional<DeferredRgba> deferred_;
};

inline void loadImageInto(AsyncRgbaImage* image, GameMetadataService* service,
                   const std::string& url,
                   const std::shared_ptr<ImageRequestState>& state,
                   uint64_t generation,
                   int maxDim = GameMetadataService::kImageDimCard) {
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
        image->setRgbaNow(cached->pixels.data(), cached->width,
                          cached->height);
        return;
    }
    image->resetArtwork();
    state->pending = true;
    image->setRgbaAsync([service, url, state, generation, maxDim](
        std::function<void(std::shared_ptr<const std::vector<uint8_t>>,
                           int, int)> done) {
        service->requestImage(url, [done, state, generation](
            GameMetadataService::ImageData bytes) {
            if (state->generation.load() != generation) {
                // A superseded request must not leave the recycled card marked
                // pending, or its current same-URL binding can be skipped.
                state->pending = false;
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
        }, maxDim);
    });
}

inline void loadImageInto(AsyncRgbaImage* image, GameMetadataService* service,
                   const std::string& url,
                   int maxDim = GameMetadataService::kImageDimCard) {
    auto state = std::make_shared<ImageRequestState>();
    uint64_t generation = ++state->generation;
    loadImageInto(image, service, url, state, generation, maxDim);
}

inline void setArtworkUrl(AsyncRgbaImage* image, GameMetadataService* service,
                   const std::string& url, std::string& currentUrl,
                   const std::shared_ptr<ImageRequestState>& state,
                   int maxDim = GameMetadataService::kImageDimCard) {
    if (currentUrl == url &&
        (image->hasArtwork() || state->pending.load()))
        return;
    currentUrl = url;
    uint64_t generation = ++state->generation;
    loadImageInto(image, service, url, state, generation, maxDim);
}

}  // namespace pipensx::ui
