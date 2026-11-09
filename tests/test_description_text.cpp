#include "ui/common/description_text.hpp"

#include <cassert>
#include <cstdio>
#include <string>

int main() {
    using namespace pipensx::ui;
    assert(compactDescription(" \t\r\n ").empty());
    assert(compactDescription(" First\r\nwrapped\r\n\r\n\r\nsecond \t ") ==
           "First wrapped\nsecond");
    assert(shortDescription("").empty());
    assert(shortDescription("Short description") == "Short description");
    assert(shortDescription(std::string(280, 'a')) == std::string(280, 'a'));
    assert(shortDescription(std::string(281, 'a')) == std::string(280, 'a') + "…");
    assert(shortDescription(std::string(270, 'a') + " boundaryword tail") ==
           std::string(270, 'a') + "…");
    assert(shortDescription("one\ntwo\nthree\nfour\nfive") == "one\ntwo\nthree\nfour…");
    std::string unicode;
    for (int i = 0; i < 70; ++i)
        unicode += "Яé猫🎮";
    assert(shortDescription(unicode) == unicode);
    assert(shortDescription(unicode + "猫") == unicode + "…");
    assert(shortDescription(std::string(279, 'a') + "🎮猫") ==
           std::string(279, 'a') + "🎮…");
    assert(compactDescription("Title\n\nDescription\n\nVersion notes") ==
           "Title\nDescription\nVersion notes");
    std::puts("description text tests passed");
}
