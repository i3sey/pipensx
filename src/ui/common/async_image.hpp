#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

#include <borealis.hpp>

#include "app/game_metadata_service.hpp"

namespace pipensx::ui {

struct ImageRequestState {
    std::atomic<uint64_t> generation {0};
    std::atomic<bool> pending {false};
};

class AsyncRgbaImage;

struct AsyncImageLifetime {
    std::mutex mutex;
    AsyncRgbaImage* image = nullptr;
};

class AsyncRgbaImage : public brls::Image {
public:
    AsyncRgbaImage() : lifetime_(std::make_shared<AsyncImageLifetime>()) {
        lifetime_->image = this;
    }

    ~AsyncRgbaImage() override {
        std::lock_guard<std::mutex> lock(lifetime_->mutex);
        lifetime_->image = nullptr;
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
                if (!lifetime->image || !pixels || pixels->empty() ||
                    width <= 0 || height <= 0)
                    return;
                NVGcontext* vg = brls::Application::getNVGContext();
                lifetime->image->innerSetImage(nvgCreateImageRGBA(
                    vg, width, height, 0, pixels->data()));
            });
        });
    }

private:
    std::shared_ptr<AsyncImageLifetime> lifetime_;
};

inline void loadImageInto(AsyncRgbaImage* image, GameMetadataService* service,
                   const std::string& url,
                   const std::shared_ptr<ImageRequestState>& state,
                   uint64_t generation) {
    if (!image)
        return;
    image->clear();
    if (!service || url.empty()) {
        state->pending = false;
        return;
    }
    state->pending = true;
    image->setRgbaAsync([service, url, state, generation](
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
        });
    });
}

inline void loadImageInto(AsyncRgbaImage* image, GameMetadataService* service,
                   const std::string& url) {
    auto state = std::make_shared<ImageRequestState>();
    uint64_t generation = ++state->generation;
    loadImageInto(image, service, url, state, generation);
}

inline void setArtworkUrl(AsyncRgbaImage* image, GameMetadataService* service,
                   const std::string& url, std::string& currentUrl,
                   const std::shared_ptr<ImageRequestState>& state) {
    if (currentUrl == url &&
        (image->getTexture() != 0 || state->pending.load()))
        return;
    currentUrl = url;
    uint64_t generation = ++state->generation;
    loadImageInto(image, service, url, state, generation);
}

}  // namespace pipensx::ui
