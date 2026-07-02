#include "../src/core/peer.h"
#include "../src/core/net.h"

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

typedef struct {
    int count;
    uint64_t bytes;
} block_capture_t;

static void capture_expired(void *user, const block_req_t *request) {
    expired_capture_t *capture = (expired_capture_t*)user;
    assert(capture->count < 4);
    capture->requests[capture->count++] = *request;
}

static void capture_block(void *user, uint32_t index, uint32_t offset,
                          const uint8_t *data, uint32_t length) {
    block_capture_t *capture = (block_capture_t*)user;
    assert(index == (uint32_t)capture->count);
    assert(offset == 0);
    assert(length == 8192);
    assert(data[0] == (uint8_t)index);
    capture->count++;
    capture->bytes += length;
}

static void send_all(int fd, const uint8_t *data, size_t length) {
    size_t sent = 0;
    while (sent < length) {
        ssize_t count = send(fd, data + sent, length - sent, 0);
        assert(count > 0);
        sent += (size_t)count;
    }
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

static void test_tcp_connect_uses_large_receive_buffer(void) {
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    assert(listener >= 0);

    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(bind(listener, (struct sockaddr*)&address, sizeof(address)) == 0);
    assert(listen(listener, 1) == 0);

    socklen_t addressSize = sizeof(address);
    assert(getsockname(listener, (struct sockaddr*)&address, &addressSize) == 0);

    socket_t client = net_tcp_connect(&address);
    assert(client != INVALID_SOCK);
    int receiveBufferSize = 0;
    socklen_t optionSize = sizeof(receiveBufferSize);
    assert(getsockopt(client, SOL_SOCKET, SO_RCVBUF, &receiveBufferSize,
                      &optionSize) == 0);
    assert(receiveBufferSize >= NET_TCP_RECEIVE_BUFFER_SIZE);

    net_close(client);
    close(listener);
}

static void test_recv_drains_socket_until_would_block(void) {
    int sockets[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    assert(net_set_nonblock(sockets[0]));

    enum { BLOCK_SIZE = 8192, MESSAGE_SIZE = 4 + 1 + 8 + BLOCK_SIZE };
    uint8_t message[MESSAGE_SIZE];
    memset(message, 0, sizeof(message));
    uint32_t payloadSize = 1 + 8 + BLOCK_SIZE;
    message[0] = (uint8_t)(payloadSize >> 24);
    message[1] = (uint8_t)(payloadSize >> 16);
    message[2] = (uint8_t)(payloadSize >> 8);
    message[3] = (uint8_t)payloadSize;
    message[4] = MSG_PIECE;
    for (uint32_t index = 0; index < 8; ++index) {
        message[5] = (uint8_t)(index >> 24);
        message[6] = (uint8_t)(index >> 16);
        message[7] = (uint8_t)(index >> 8);
        message[8] = (uint8_t)index;
        memset(message + 13, (int)index, BLOCK_SIZE);
        send_all(sockets[1], message, sizeof(message));
    }

    peer_t peer;
    memset(&peer, 0, sizeof(peer));
    peer.fd = sockets[0];
    peer.state = PS_ACTIVE;
    peer_ctx_t context;
    memset(&context, 0, sizeof(context));
    context.num_pieces = 8;

    block_capture_t capture = {0};
    assert(peer_recv(&peer, &context, capture_block, NULL, NULL, &capture) == 0);
    assert(capture.count == 8);
    assert(capture.bytes == 8 * BLOCK_SIZE);
    assert(peer.rbuf_len == 0);

    close(sockets[0]);
    close(sockets[1]);
}

static void test_receive_buffer_holds_256_kib(void) {
    assert(sizeof(((peer_t*)0)->rbuf) >= 256 * 1024);
}

int main(void) {
    test_expiry_compacts_pipeline();
    test_cancel_sends_message_and_removes_request();
    test_tcp_connect_uses_large_receive_buffer();
    test_recv_drains_socket_until_would_block();
    test_receive_buffer_holds_256_kib();
    puts("peer tests passed");
    return 0;
}
