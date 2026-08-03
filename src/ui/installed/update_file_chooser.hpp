#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include <borealis.hpp>

#include "app/download_manager.hpp"
#include "ui/common/ui_helpers.hpp"
#include "ui/detail/torrent_selection.hpp"
#include "ui/i18n.hpp"
#include "ui/theme.hpp"

namespace pipensx::ui {

// Single-tap chooser for "which of these packages is the update?" A release
// bundle can carry several packages with the update's [vN] tag — a mod
// reusing the version of the release it patches is the classic lookalike —
// and only the user can tell which one to install.
//
// The previous implementation paged candidates through a brls::Dialog two at
// a time. The dialog's third slot is a full-width button on top, so the
// auxiliary "more/later" button outranked both candidates, and the two
// half-width bottom slots (~330 px each) cut long file names — exactly when
// the deep directory prefixes that tell the candidates apart matter. A
// compact list replaces it: TorrentSelectionCell draws the directory dimmed,
// the name readable and the byte size right-aligned, so deep paths and
// same-name files (e.g. "update.nsp" vs "mods/update.nsp", usually different
// sizes) resolve visually. One A press on a row is the whole choice — no
// paging, no inversion.
class UpdateFileChooserActivity : public brls::Activity {
public:
    // `matches` are indices into preview.files that carry the update version
    // (at least two, or this activity would never be shown). `initialPeers`
    // are the bootstrap peers from the magnet resolve — on a network where
    // the tracker is unreachable they are the only way an import can start —
    // so onPick hands them back untouched for the import.
    //
    // onPick fires with the chosen match index and those peers; onCancel
    // fires when the user backs out. The caller owns the tmp torrent and
    // unlinks it in both callbacks.
    UpdateFileChooserActivity(
        pipensx::TorrentPreview preview, std::vector<size_t> matches,
        std::vector<uint8_t> initialPeers,
        std::function<void(size_t, std::vector<uint8_t>)> onPick,
        std::function<void()> onCancel)
        : preview_(std::move(preview)), matches_(std::move(matches)),
          initialPeers_(std::move(initialPeers)), onPick_(std::move(onPick)),
          onCancel_(std::move(onCancel)) {
        auto* content = new brls::Box(brls::Axis::COLUMN);
        content->setGrow(1);
        content->setPadding(18, 38, 18, 34);
        content->setBackgroundColor(theme::overlay());
        content->setCornerRadius(12);

        title_ = new brls::Label();
        title_->setFontSize(26);
        title_->setText(tr("pipensx/installed/update_choose_file"));
        content->addView(title_);

        recycler_ = new brls::RecyclerFrame();
        recycler_->setGrow(1);
        recycler_->setPadding(6, 0, 6, 0);
        recycler_->estimatedRowHeight = 82;
        recycler_->registerCell("FileSelect",
                                [] { return new TorrentSelectionCell(); });
        dataSource_ = new ChooserDataSource(this);
        dataSource_->setMatches(preview_, matches_);
        recycler_->setDataSource(dataSource_);
        content->addView(recyclerHost(recycler_));

        auto* cancel = new brls::Button();
        cancel->setStyle(&brls::BUTTONSTYLE_DEFAULT);
        cancel->setFontSize(18);
        cancel->setHeight(46);
        cancel->setMarginTop(12);
        cancel->setText(tr("pipensx/common/cancel"));
        cancel->registerClickAction([this](brls::View*) {
            backOut();
            return true;
        });
        content->addView(cancel);

        frame_ = new brls::AppletFrame(content);
        frame_->setTitle(preview_.name.empty()
                             ? tr("pipensx/torrent/frame_title")
                             : preview_.name);
    }

    ~UpdateFileChooserActivity() override {
        // Backing out with B pops the activity without touching backOut(),
        // so the cancel contract is enforced here, like TorrentSelectionActivity.
        if (!finished_ && onCancel_)
            onCancel_();
    }

    brls::View* createContentView() override { return frame_; }

    void onContentAvailable() override {
        registerAction(tr("pipensx/common/cancel"), brls::BUTTON_B,
                       [this](brls::View*) {
            backOut();
            return true;
        });
    }

private:
    void pick(size_t row) {
        if (finished_)
            return;
        finished_ = true;
        if (row < matches_.size())
            onPick_(matches_[row], std::move(initialPeers_));
        brls::Application::popActivity();
    }

    void backOut() {
        if (finished_)
            return;
        finished_ = true;
        onCancel_();
        brls::Application::popActivity();
    }

    // Rows are read-only: every entry is an install candidate and A on a row
    // is the choice itself, so didSelectRowAt picks instead of toggling.
    class ChooserDataSource : public brls::RecyclerDataSource {
    public:
        explicit ChooserDataSource(UpdateFileChooserActivity* owner)
            : owner_(owner) {}

        void setMatches(const pipensx::TorrentPreview& preview,
                        const std::vector<size_t>& matches) {
            entries_.clear();
            entries_.reserve(matches.size());
            for (const size_t index : matches) {
                if (index >= preview.files.size())
                    continue;
                const auto& file = preview.files[index];
                TorrentSelectionEntry entry;
                entry.path = file.path;
                entry.length = file.length;
                entry.package = file.package;
                entry.compressed = file.compressed;
                entry.cartridge = file.cartridge;
                entry.action = FileAction::Install;
                entries_.push_back(std::move(entry));
            }
        }

        int numberOfSections(brls::RecyclerFrame*) override { return 1; }

        int numberOfRows(brls::RecyclerFrame*, int) override {
            return static_cast<int>(entries_.size());
        }

        brls::RecyclerCell* cellForRow(brls::RecyclerFrame* recycler,
                                       brls::IndexPath index) override {
            auto* cell = static_cast<TorrentSelectionCell*>(
                recycler->dequeueReusableCell("FileSelect"));
            if (index.row >= 0 &&
                static_cast<size_t>(index.row) < entries_.size())
                cell->setEntry(entries_[static_cast<size_t>(index.row)]);
            return cell;
        }

        void didSelectRowAt(brls::RecyclerFrame*, brls::IndexPath index)
            override {
            if (index.row >= 0 &&
                static_cast<size_t>(index.row) < entries_.size())
                owner_->pick(static_cast<size_t>(index.row));
        }

    private:
        UpdateFileChooserActivity* owner_;
        std::vector<TorrentSelectionEntry> entries_;
    };

    pipensx::TorrentPreview preview_;
    std::vector<size_t> matches_;
    std::vector<uint8_t> initialPeers_;
    std::function<void(size_t, std::vector<uint8_t>)> onPick_;
    std::function<void()> onCancel_;
    brls::AppletFrame* frame_ = nullptr;
    brls::Label* title_ = nullptr;
    brls::RecyclerFrame* recycler_ = nullptr;
    ChooserDataSource* dataSource_ = nullptr;
    bool finished_ = false;
};

}  // namespace pipensx::ui
