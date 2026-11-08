#include "core/client_identity.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
    uint8_t id[20];
    memset(id, 0xa5, sizeof(id));
    pipensx_peer_prefix_for_version(id, "1.6.6");
    assert(!memcmp(id, "-PN1660-", 8));
    for (unsigned i = 8; i < sizeof(id); ++i) assert(id[i] == 0xa5);
    pipensx_peer_prefix_for_version(id, "12.3.4-dev.1+gabcdef.dirty");
    assert(!memcmp(id, "-PNC340-", 8));
    pipensx_peer_prefix_for_version(id, "35.35.35");
    assert(!memcmp(id, "-PNZZZ0-", 8));
    const char *invalid[] = {"36.0.0", "1.2", "x.2.3", "1.2.3.4", "99999999999999.0.0"};
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        pipensx_peer_prefix_for_version(id, invalid[i]);
        assert(!memcmp(id, "-PN0000-", 8));
    }
    assert(!strcmp(PIPENSX_USER_AGENT, "pipensx/" PIPENSX_VERSION));
    assert(!strcmp(PIPENSX_CLIENT_NAME, PIPENSX_USER_AGENT));
    printf("client identity tests passed: %s\n", PIPENSX_CLIENT_NAME);
}
