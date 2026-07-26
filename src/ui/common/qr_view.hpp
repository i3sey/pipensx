#pragma once

#include <algorithm>
#include <cmath>
#include <exception>
#include <optional>
#include <string>
#include <utility>

#include <borealis.hpp>

#include "qrcodegen/qrcodegen.hpp"
#include "ui/theme.hpp"

namespace pipensx::ui {

// Renders `value` as a QR code on a rounded light card. Fixed default size;
// override with setWidth/setHeight. Used by the About cards and the web
// companion address dialog.
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

        if (!qr_)
            return;

        constexpr int kQuietZone = 4;
        const int modules = qr_->getSize();
        const int cells = modules + kQuietZone * 2;
        const float available = std::max(
            0.0f, std::min(width, height) - theme::kSpacingUnit * 2.0f);
        const float cellSize = std::floor(available / static_cast<float>(cells));
        if (cellSize < 1.0f)
            return;

        const float drawnSize = cellSize * static_cast<float>(cells);
        const float originX =
            std::floor(x + (width - drawnSize) * 0.5f);
        const float originY =
            std::floor(y + (height - drawnSize) * 0.5f);

        nvgBeginPath(vg);
        for (int row = 0; row < modules; row++) {
            for (int col = 0; col < modules; col++) {
                if (!qr_->getModule(col, row))
                    continue;
                const float px =
                    originX + static_cast<float>(col + kQuietZone) * cellSize;
                const float py =
                    originY + static_cast<float>(row + kQuietZone) * cellSize;
                nvgRect(vg, px, py, cellSize, cellSize);
            }
        }
        nvgFillColor(vg, theme::textPrimary());
        nvgFill(vg);
    }

private:
    std::string value_;
    std::optional<qrcodegen::QrCode> qr_;
};

}  // namespace pipensx::ui
