#pragma once

#include <string>

#include <borealis.hpp>

namespace pipensx::ui {

class MessageCell : public brls::RecyclerCell {
public:
    MessageCell() {
        setFocusable(true);
        setHeight(100);
        setPadding(24);
        label_ = new brls::Label();
        label_->setFontSize(21);
        label_->setText("No downloads yet. Press X to add a .torrent file.");
        addView(label_);
    }

private:
    brls::Label* label_;
};

class TextMessageCell : public brls::RecyclerCell {
public:
    TextMessageCell() {
        setFocusable(true);
        setHeight(100);
        setPadding(24);
        label_ = new brls::Label();
        label_->setFontSize(20);
        addView(label_);
    }
    void setMessage(const std::string& text) { label_->setText(text); }

private:
    brls::Label* label_;
};

}  // namespace pipensx::ui
