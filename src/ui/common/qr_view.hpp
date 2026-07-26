#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <exception>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <borealis.hpp>

#include "qrcodegen/qrcodegen.hpp"
#include "ui/theme.hpp"

namespace pipensx::ui {

// Renders `value` as a QR code on a rounded light card. Fixed default size;
// override with setWidth/setHeight. Used by the About cards and the web
// companion address dialog.
//
// The code goes to the GPU once as a texture of one texel per module and is
// drawn as a single nearest-filtered quad. Emitting a rect per module instead
// costs a nanovg subpath per dark module every frame — deko3d issues a draw
// command per subpath, and a large code is thousands of them, the same
// command-memory hazard ReportQrView documents.
class QrCodeView : public brls::View {
public:
    static constexpr float kDefaultSize = 176.0f;

    explicit QrCodeView(std::string value) : value_(std::move(value)) {
        setWidth(kDefaultSize);
        setHeight(kDefaultSize);
        try {
            qr_.emplace(qrcodegen::QrCode::encodeText(
                value_.c_str(), qrcodegen::QrCode::Ecc::MEDIUM));
        } catch (const std::exception&) {
            qr_.reset();
        }
        if (qr_) {
            cells_ = qr_->getSize() + kQuietZone * 2;
            // Upload here rather than lazily in draw(): deko3d uploads a
            // texture by submitting to the render queue and waiting on it,
            // which must not happen while a frame is being recorded. Views
            // are only constructed outside a frame.
            texture_ = createTexture(brls::Application::getNVGContext());
        }
    }

    ~QrCodeView() override {
        if (texture_ > 0)
            nvgDeleteImage(brls::Application::getNVGContext(), texture_);
    }

    void draw(NVGcontext* vg, float x, float y, float width, float height,
              brls::Style, brls::FrameContext*) override {
        const NVGcolor paper =
            brls::Application::getTheme().getColor(
                "brls/button/default_enabled_background");
        nvgBeginPath(vg);
        nvgRoundedRect(vg, x, y, width, height, theme::kRadiusMedium);
        nvgFillColor(vg, paper);
        nvgFill(vg);

        if (texture_ <= 0)
            return;

        const float available = std::max(
            0.0f, std::min(width, height) - theme::kSpacingUnit * 2.0f);
        const float cellSize =
            std::floor(available / static_cast<float>(cells_));
        if (cellSize < 1.0f)
            return;

        const float drawnSize = cellSize * static_cast<float>(cells_);
        const float originX =
            std::floor(x + (width - drawnSize) * 0.5f);
        const float originY =
            std::floor(y + (height - drawnSize) * 0.5f);

        const NVGpaint paint = nvgImagePattern(
            vg, originX, originY, drawnSize, drawnSize, 0.0f, texture_, 1.0f);
        nvgBeginPath(vg);
        nvgRect(vg, originX, originY, drawnSize, drawnSize);
        nvgFillPaint(vg, paint);
        nvgFill(vg);
    }

private:
    static constexpr int kQuietZone = 4;

    // One texel per module, quiet zone included; light modules stay fully
    // transparent so the card fill shows through, exactly like the old
    // rect-per-module rendering.
    int createTexture(NVGcontext* vg) {
        const NVGcolor ink = theme::textPrimary();
        std::vector<std::uint8_t> pixels(
            static_cast<std::size_t>(cells_) * cells_ * 4, 0);
        for (int row = 0; row < cells_ - kQuietZone * 2; row++) {
            for (int col = 0; col < cells_ - kQuietZone * 2; col++) {
                if (!qr_->getModule(col, row))
                    continue;
                std::uint8_t* texel = &pixels[
                    (static_cast<std::size_t>(row + kQuietZone) * cells_ +
                     (col + kQuietZone)) * 4];
                for (int channel = 0; channel < 4; channel++)
                    texel[channel] = static_cast<std::uint8_t>(
                        std::clamp(ink.rgba[channel], 0.0f, 1.0f) * 255.0f);
            }
        }
        return nvgCreateImageRGBA(vg, cells_, cells_, NVG_IMAGE_NEAREST,
                                  pixels.data());
    }

    std::string value_;
    std::optional<qrcodegen::QrCode> qr_;
    int cells_ = 0;
    int texture_ = 0;
};

}  // namespace pipensx::ui
