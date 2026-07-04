#pragma once

#include <curl/curl.h>

#include <borealis.hpp>

namespace pipensx::ui {

class AboutView : public brls::Box {
public:
    AboutView() : brls::Box(brls::Axis::COLUMN) {
        auto* content = new brls::Box(brls::Axis::COLUMN);
        content->setPadding(30, 50, 30, 50);
        addLine(content, "pipensx", 32, nvgRGB(245, 245, 250));
        addLine(content, std::string("Version ") + PIPENSX_VERSION +
            "   Built " + __DATE__ + " " + __TIME__, 18,
            nvgRGB(180, 180, 190));
        addLine(content,
            "A Nintendo Switch storefront and BitTorrent client for "
            "downloading or streaming NSP/NSZ packages to SD.",
            19, nvgRGB(220, 220, 225));
        addLine(content,
            "Questions or feedback? Message @i3sey on Telegram.",
            19, nvgRGB(0, 195, 227));
        addLine(content,
            "Catalog: cached on SD with a bundled offline fallback.\n"
            "Log: sdmc:/switch/pipensx/pipensx.log\n"
            "Settings: sdmc:/switch/pipensx/settings.json",
            17, nvgRGB(175, 175, 185));
        addLine(content,
            "Built with libnx, Borealis, libcurl, zstd, mbedTLS and "
            "miniupnpc. See THIRD_PARTY_NOTICES.md for licenses.",
            17, nvgRGB(175, 175, 185));
        addLine(content,
            "pipensx is an independent open-source project and is not "
            "affiliated with Nintendo.",
            16, nvgRGB(150, 150, 160));
        auto* scroll = new brls::ScrollingFrame();
        scroll->setGrow(1);
        scroll->setContentView(content);
        addView(scroll);
    }

private:
    static void addLine(brls::Box* content, const std::string& text,
                        float size, NVGcolor color) {
        auto* label = new brls::Label();
        label->setText(text);
        label->setFontSize(size);
        label->setTextColor(color);
        label->setMarginBottom(22);
        content->addView(label);
    }
};

}  // namespace pipensx::ui
