#pragma once
#include <stdint.h>
#include <string.h>

#ifndef PIPENSX_VERSION
#error PIPENSX_VERSION must be supplied by the build
#endif

#define PIPENSX_USER_AGENT "pipensx/" PIPENSX_VERSION
#define PIPENSX_CLIENT_NAME PIPENSX_USER_AGENT

static inline void pipensx_peer_prefix_for_version(uint8_t out[8], const char *version) {
    static const char digits[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    uint8_t prefix[8] = {'-', 'P', 'N', '0', '0', '0', '0', '-'};
    const char *cursor = version;
    for (unsigned component = 0; component < 3; ++component) {
        unsigned value = 0;
        if (*cursor < '0' || *cursor > '9') goto unknown;
        do {
            value = value * 10 + (unsigned)(*cursor++ - '0');
            if (value >= sizeof(digits) - 1) goto unknown;
        } while (*cursor >= '0' && *cursor <= '9');
        prefix[3 + component] = (uint8_t)digits[value];
        if (component < 2 && *cursor++ != '.') goto unknown;
    }
    if (*cursor && *cursor != '-' && *cursor != '+') goto unknown;
    memcpy(out, prefix, 8);
    return;
unknown:
    /* The full BEP-10 v string remains authoritative for versions > 35. */
    memcpy(out, "-PN0000-", 8);
}

static inline void pipensx_peer_prefix(uint8_t out[8]) {
    pipensx_peer_prefix_for_version(out, PIPENSX_VERSION);
}
