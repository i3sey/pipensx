#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <utility>

#include <borealis.hpp>

#include "ui/i18n.hpp"
#include "ui/theme.hpp"

namespace pipensx::ui {

struct PortInstallDialogHost {
    brls::Dialog* dialog = nullptr;
    brls::Label* status = nullptr;
    brls::Button* continueButton = nullptr;
    std::shared_ptr<std::atomic<bool>> live;
    std::shared_ptr<std::atomic<bool>> proceeded;
    std::shared_ptr<std::atomic<bool>> continueReady;
};

// Unofficial-port warning plus a status line that later reports layout.
// Continue is a no-op until setPortInstallReady(). B / Cancel abort.
inline PortInstallDialogHost openPortInstallDialog(
    std::function<void()> onContinue,
    std::function<void()> onCancel,
    const std::string& statusText = {},
    bool continueEnabled = false) {
    PortInstallDialogHost host;
    host.live = std::make_shared<std::atomic<bool>>(true);
    host.proceeded = std::make_shared<std::atomic<bool>>(false);
    host.continueReady = std::make_shared<std::atomic<bool>>(continueEnabled);

    class Body : public brls::Box {
    public:
        std::function<void()> onDismiss;
        ~Body() override {
            if (onDismiss)
                onDismiss();
        }
    };

    auto* box = new Body();
    box->setAxis(brls::Axis::COLUMN);

    auto* warning = new brls::Label();
    warning->setFontSize(brls::Application::getStyle()["brls/dialog/fontSize"]);
    warning->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    warning->setSingleLine(false);
    warning->setText(tr("pipensx/port_install/unofficial"));
    box->addView(warning);

    auto* status = new brls::Label();
    status->setFontSize(theme::kFontSmall);
    status->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    status->setSingleLine(false);
    status->setTextColor(theme::textSecondary());
    status->setMarginTop(16);
    status->setText(statusText.empty()
                        ? tr("pipensx/port_install/indexing")
                        : statusText);
    box->addView(status);
    host.status = status;

    auto onContinueShared =
        std::make_shared<std::function<void()>>(std::move(onContinue));
    auto onCancelShared =
        std::make_shared<std::function<void()>>(std::move(onCancel));

    box->onDismiss = [live = host.live, proceeded = host.proceeded,
                      onCancelShared] {
        if (!live->exchange(false))
            return;
        if (proceeded->load())
            return;
        if (*onCancelShared)
            (*onCancelShared)();
    };

    auto* dialog = new brls::Dialog(box);
    host.dialog = dialog;
    dialog->addButton(tr("pipensx/common/continue"), [] {});
    dialog->addButton(tr("pipensx/common/cancel"), [] {});

    brls::Button* first = nullptr;
    brls::Button* second = nullptr;
    std::function<void(brls::View*)> findButtons = [&](brls::View* node) {
        if (auto* button = dynamic_cast<brls::Button*>(node)) {
            if (button->getVisibility() == brls::Visibility::VISIBLE) {
                if (!first)
                    first = button;
                else if (!second)
                    second = button;
            }
        }
        if (auto* parent = dynamic_cast<brls::Box*>(node))
            for (brls::View* child : parent->getChildren())
                findButtons(child);
    };
    findButtons(dialog);
    host.continueButton = first;

    if (first) {
        first->setState(continueEnabled ? brls::ButtonState::ENABLED
                                        : brls::ButtonState::DISABLED);
        first->registerClickAction(
            [dialog, live = host.live, proceeded = host.proceeded,
             ready = host.continueReady, onContinueShared](brls::View*) {
                if (!ready->load() || !live->load())
                    return true;
                proceeded->store(true);
                live->store(false);
                dialog->close([onContinueShared] {
                    if (*onContinueShared)
                        (*onContinueShared)();
                });
                return true;
            });
    }
    if (second) {
        second->registerClickAction(
            [dialog, live = host.live, proceeded = host.proceeded,
             onCancelShared](brls::View*) {
                if (!live->load())
                    return true;
                proceeded->store(true);
                live->store(false);
                dialog->close([onCancelShared] {
                    if (*onCancelShared)
                        (*onCancelShared)();
                });
                return true;
            });
    }
    dialog->open();
    return host;
}

inline void setPortInstallReady(PortInstallDialogHost& host,
                                const std::string& statusText) {
    if (!host.live || !host.live->load())
        return;
    if (host.status)
        host.status->setText(statusText);
    if (host.continueReady)
        host.continueReady->store(true);
    if (host.continueButton)
        host.continueButton->setState(brls::ButtonState::ENABLED);
}

}  // namespace pipensx::ui
