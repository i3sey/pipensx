#include "../src/core/peer.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

typedef struct {
    block_req_t requests[4];
    int count;
} expired_capture_t;

static void capture_expired(void *user, const block_req_t *request) {
    expired_capture_t *capture = (expired_capture_t*)user;
    assert(capture->count < 4);
    capture->requests[capture->count++] = *request;
}

static void test_expiry_compacts_pipeline(void) {
    peer_t peer;
    memset(&peer, 0, sizeof(peer));
    peer.pipeline_len = 3;
    peer.pipeline[0] = (block_req_t){1, 0, 16384, 100};
    peer.pipeline[1] = (block_req_t){1, 16384, 16384, 500};
    peer.pipeline[2] = (block_req_t){2, 0, 16384, 900};

    expired_capture_t capture = {0};
    int expired = peer_expire_requests(&peer, 1000, 500,
                                       capture_expired, &capture);
    assert(expired == 2);
    assert(capture.count == 2);
    assert(peer.pipeline_len == 1);
    assert(peer.pipeline[0].index == 2);
    assert(peer.pipeline[0].requested_ms == 900);
}

static void test_cancel_sends_message_and_removes_request(void) {
    int sockets[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);

    peer_t peer;
    memset(&peer, 0, sizeof(peer));
    peer.fd = sockets[0];
    peer.pipeline_len = 2;
    peer.pipeline[0] = (block_req_t){7, 32768, 16384, 100};
    peer.pipeline[1] = (block_req_t){8, 0, 16384, 100};

    assert(peer_cancel_block(&peer, 7, 32768, 16384));
    assert(peer.pipeline_len == 1);
    assert(peer.pipeline[0].index == 8);

    uint8_t message[17] = {0};
    assert(recv(sockets[1], message, sizeof(message), 0) ==
           (ssize_t)sizeof(message));
    assert(message[0] == 0 && message[1] == 0 &&
           message[2] == 0 && message[3] == 13);
    assert(message[4] == MSG_CANCEL);
    assert(message[8] == 7);
    assert(message[11] == 0x80);

    assert(!peer_cancel_block(&peer, 7, 32768, 16384));
    close(sockets[0]);
    close(sockets[1]);
}

int main(void) {
    test_expiry_compacts_pipeline();
    test_cancel_sends_message_and_removes_request();
    puts("peer tests passed");
    return 0;
}
