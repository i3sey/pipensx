#include "app/direct_hint.hpp"

#include <cassert>
#include <cstdio>

int main() {
    using pipensx::classifyDirectHint;
    using pipensx::DirectHint;

    assert(classifyDirectHint("Europe/Moscow", "") == DirectHint::Recommended);
    assert(classifyDirectHint("Europe/Kyiv", "") == DirectHint::Recommended);
    assert(classifyDirectHint("Europe/Kiev", "de") == DirectHint::Recommended);
    assert(classifyDirectHint("Asia/Almaty", "en-US") == DirectHint::Recommended);

    assert(classifyDirectHint("Europe/Berlin", "") == DirectHint::Discouraged);
    assert(classifyDirectHint("Europe/Vienna", "") == DirectHint::Discouraged);
    assert(classifyDirectHint("Europe/Berlin", "ru") == DirectHint::Discouraged);

    assert(classifyDirectHint("Europe/Paris", "") == DirectHint::None);
    assert(classifyDirectHint("America/New_York", "") == DirectHint::None);
    assert(classifyDirectHint("America/Sao_Paulo", "") == DirectHint::None);
    assert(classifyDirectHint("America/Sao_Paulo", "en-US") == DirectHint::None);
    assert(classifyDirectHint("", "en-US") == DirectHint::None);
    assert(classifyDirectHint("", "") == DirectHint::None);
    assert(classifyDirectHint("", "fr") == DirectHint::None);

    assert(classifyDirectHint("", "ru") == DirectHint::Recommended);
    assert(classifyDirectHint("", "de") == DirectHint::Discouraged);

    assert(pipensx::consoleTimeZone().empty());
    assert(pipensx::consoleLanguage().empty());

    std::printf("test_direct_hint: ok\n");
    return 0;
}
