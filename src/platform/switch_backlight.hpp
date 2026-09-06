#pragma once

// Screen-off without sleep for long downloads (B5).
//
// The console's own auto-sleep suspends sockets, so a download that is alive
// when the screen dims comes back as an error. The idle loop in
// main_switch.cpp instead covers the UI with BurnInSaverActivity and switches
// the panel off through lbl — the torrent engine keeps running the whole
// time. Any controller button or touch dismisses the saver and switches the
// panel back on; the wake press never touches the download queue.
//
// lbl is initialized and torn down by borealis' switch_wrapper
// (userAppInit/userAppExit), so this unit only issues the on/off commands.
// Every failure degrades to "saver without backlight-off" and is logged with
// its Result — never asserted, per the house idiom for homebrew.

namespace pipensx {

bool switchBacklightOff();
bool switchBacklightOn();

// Restores the backlight on destruction when it was turned off successfully,
// so a crash or an early return cannot leave the console on a black panel
// that reads as a hung system.
class SwitchBacklightGuard {
public:
    SwitchBacklightGuard() = default;
    ~SwitchBacklightGuard();

    SwitchBacklightGuard(const SwitchBacklightGuard&) = delete;
    SwitchBacklightGuard& operator=(const SwitchBacklightGuard&) = delete;

    bool turnOff();
    void turnOn();
    bool isOff() const { return off_; }

private:
    bool off_ = false;
};

} // namespace pipensx
