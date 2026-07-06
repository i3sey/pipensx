#include "core/tracker.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

/* W1: classification of a completed RuTracker reachability probe. Exercises
   tracker_probe_classify offline; the network path around it (tracker_probe)
   is a thin curl wrapper verified live. */
static void test_classify(void) {
    /* Transport failure (timeout / reset / connection refused): body, if any,
       is meaningless and must never be classified as reachable. */
    assert(tracker_probe_classify(0, 0, NULL, 0) == TRACKER_PROBE_TIMEOUT);
    assert(tracker_probe_classify(0, 200, "d5:peers0:e", 11) ==
           TRACKER_PROBE_TIMEOUT);

    /* DPI stub page — takes priority even over a 200 status. */
    const char *blocked =
        "<title>Доступ к запрашиваемому ресурсу ограничен</title>";
    assert(tracker_probe_classify(1, 200, blocked, strlen(blocked)) ==
           TRACKER_PROBE_BLOCKED);

    /* Genuine tracker replies are bencode dicts — a "not registered"
       failure (our stub hash) or a real peers list both count as reachable. */
    const char *not_registered = "d14:failure reason22:torrent not registerede";
    assert(tracker_probe_classify(1, 200, not_registered,
                                  strlen(not_registered)) ==
           TRACKER_PROBE_REACHABLE);
    const char *peers = "d8:intervali1800e5:peers6:ABCDEFe";
    assert(tracker_probe_classify(1, 200, peers, strlen(peers)) ==
           TRACKER_PROBE_REACHABLE);

    /* Reached something that is not a tracker (proxy/captive HTML). */
    const char *html = "<html><body>hello</body></html>";
    assert(tracker_probe_classify(1, 200, html, strlen(html)) ==
           TRACKER_PROBE_BLOCKED);

    /* A tracker answers 2xx; a non-2xx completion is not a live tracker. */
    assert(tracker_probe_classify(1, 403, not_registered,
                                  strlen(not_registered)) ==
           TRACKER_PROBE_BLOCKED);
    /* Empty 2xx body is not a valid bencode reply. */
    assert(tracker_probe_classify(1, 204, NULL, 0) == TRACKER_PROBE_BLOCKED);
}

int main(void) {
    test_classify();
    puts("tracker probe tests passed");
    return 0;
}
