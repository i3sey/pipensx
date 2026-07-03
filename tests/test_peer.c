#include "../src/core/peer.h"
#include "../src/core/net.h"
#include "../src/core/util.h"

#include <assert.h>
#include <errno.h>
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

/* Build one MSG_PIECE frame (index, offset 0, 8192-byte block of `index`)
   into a caller buffer of 4 + 1 + 8 + 8192 bytes. */
static void fill_piece(uint8_t *message, uint32_t index) {
    uint32_t payloadSize = 1 + 8 + 8192;
    message[0] = (uint8_t)(payloadSize >> 24);
    message[1] = (uint8_t)(payloadSize >> 16);
    message[2] = (uint8_t)(payloadSize >> 8);
    message[3] = (uint8_t)payloadSize;
    message[4] = MSG_PIECE;
    message[5] = (uint8_t)(index >> 24);
    message[6] = (uint8_t)(index >> 16);
    message[7] = (uint8_t)(index >> 8);
    message[8] = (uint8_t)index;
    message[9] = message[10] = message[11] = message[12] = 0; /* offset 0 */
    memset(message + 13, (int)index, 8192);
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

static void test_partial_send_queues_tail_and_flush_completes_frame(void) {
    int sockets[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    assert(net_set_nonblock(sockets[0]));
    int sndbuf = 1;
    assert(setsockopt(sockets[0], SOL_SOCKET, SO_SNDBUF, &sndbuf,
                      sizeof(sndbuf)) == 0);

    peer_t peer;
    memset(&peer, 0, sizeof(peer));
    peer.fd = sockets[0];

    enum { BF_BYTES = 16000, FRAME = 4 + 1 + BF_BYTES };
    uint8_t bitfield[BF_BYTES];
    for (int i = 0; i < BF_BYTES; i++)
        bitfield[i] = (uint8_t)i;

    assert(peer_send_bitfield(&peer, bitfield, BF_BYTES));
    assert(peer.sbuf_len > 0); /* kernel buffer too small for whole frame */

    uint8_t frame[FRAME];
    size_t got = 0;
    while (got < sizeof(frame)) {
        ssize_t n = recv(sockets[1], frame + got, sizeof(frame) - got,
                         MSG_DONTWAIT);
        if (n > 0) {
            got += (size_t)n;
            continue;
        }
        assert(n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK));
        assert(peer.sbuf_len > 0); /* otherwise the frame was truncated */
        assert(peer_flush(&peer));
    }
    assert(peer.sbuf_len == 0);
    assert(frame[0] == 0 && frame[1] == 0 &&
           frame[2] == ((1 + BF_BYTES) >> 8) &&
           frame[3] == ((1 + BF_BYTES) & 0xFF));
    assert(frame[4] == MSG_BITFIELD);
    assert(memcmp(frame + 5, bitfield, BF_BYTES) == 0);

    close(sockets[0]);
    close(sockets[1]);
}

static void test_tcp_connect_defers_large_receive_buffer(void) {
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
    assert(receiveBufferSize < NET_TCP_RECEIVE_BUFFER_SIZE);
    assert(net_set_tcp_receive_buffer(client));
    assert(getsockopt(client, SOL_SOCKET, SO_RCVBUF, &receiveBufferSize,
                      &optionSize) == 0);
    assert(receiveBufferSize >= NET_TCP_RECEIVE_BUFFER_SIZE);

    net_close(client);
    close(listener);
}

static void test_handshake_applies_large_receive_buffer(void) {
    int sockets[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    assert(net_set_nonblock(sockets[0]));
    int smallBuffer = 4096;
    assert(setsockopt(sockets[0], SOL_SOCKET, SO_RCVBUF, &smallBuffer,
                      sizeof(smallBuffer)) == 0);

    uint8_t infoHash[20] = {0};
    uint8_t peerId[20] = {0};
    uint8_t bitfield = 0;
    peer_ctx_t context = {
        .info_hash = infoHash,
        .peer_id = peerId,
        .num_pieces = 1,
        .bf_bytes = 1,
        .our_bf = &bitfield,
        .listen_port = 0,
    };
    peer_t peer;
    memset(&peer, 0, sizeof(peer));
    peer.fd = sockets[0];
    peer.state = PS_HANDSHAKE;
    peer.rbuf[0] = 19;
    memcpy(peer.rbuf + 1, "BitTorrent protocol", 19);
    memcpy(peer.rbuf + 28, infoHash, sizeof(infoHash));
    peer.rbuf_len = BT_HANDSHAKE_LEN;

    assert(peer_recv(&peer, &context, NULL, NULL, NULL, NULL) == 0);
    assert(peer.state == PS_ACTIVE);
    int receiveBufferSize = 0;
    socklen_t optionSize = sizeof(receiveBufferSize);
    assert(getsockopt(sockets[0], SOL_SOCKET, SO_RCVBUF, &receiveBufferSize,
                      &optionSize) == 0);
    assert(receiveBufferSize >= NET_TCP_RECEIVE_BUFFER_SIZE);

    close(sockets[0]);
    close(sockets[1]);
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

/* A message torn across two reads must reassemble, and the second message
   must be parsed from a nonzero rbuf_head — i.e. the cursor advances instead
   of memmoving the tail to the front on every consume. */
static void test_recv_reassembles_message_split_across_reads(void) {
    int sockets[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    assert(net_set_nonblock(sockets[0]));

    enum { BLOCK_SIZE = 8192, MESSAGE_SIZE = 4 + 1 + 8 + BLOCK_SIZE };
    uint8_t first[MESSAGE_SIZE], second[MESSAGE_SIZE];
    fill_piece(first, 0);
    fill_piece(second, 1);

    peer_t peer;
    memset(&peer, 0, sizeof(peer));
    peer.fd = sockets[0];
    peer.state = PS_ACTIVE;
    peer_ctx_t context;
    memset(&context, 0, sizeof(context));
    context.num_pieces = 8;

    block_capture_t capture = {0};

    /* Whole first message plus a torn 3-byte prefix of the second: leaves a
       partial frame parked at a nonzero rbuf_head. */
    send_all(sockets[1], first, sizeof(first));
    send_all(sockets[1], second, 3);
    assert(peer_recv(&peer, &context, capture_block, NULL, NULL, &capture) == 0);
    assert(capture.count == 1);
    assert(peer.rbuf_head == sizeof(first)); /* consumed via cursor, no memmove */
    assert(peer.rbuf_len == sizeof(first) + 3);

    /* Rest of the second message: it must parse starting at rbuf_head. */
    send_all(sockets[1], second + 3, sizeof(second) - 3);
    assert(peer_recv(&peer, &context, capture_block, NULL, NULL, &capture) == 0);
    assert(capture.count == 2);
    assert(capture.bytes == 2 * BLOCK_SIZE);
    assert(peer.rbuf_len == 0);              /* fully drained → cursor reset */

    close(sockets[0]);
    close(sockets[1]);
}

/* A received block must fold its request->piece round trip into the peer's
   latency EMA (PERF_PLAN 5.1); unsolicited blocks must not contribute. */
static void test_piece_receipt_samples_block_latency(void) {
    int sockets[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    assert(net_set_nonblock(sockets[0]));

    enum { MESSAGE_SIZE = 4 + 1 + 8 + 8192 };
    uint8_t message[MESSAGE_SIZE];

    peer_t peer;
    memset(&peer, 0, sizeof(peer));
    peer.fd = sockets[0];
    peer.state = PS_ACTIVE;
    peer_ctx_t context;
    memset(&context, 0, sizeof(context));
    context.num_pieces = 8;

    /* Unsolicited block (empty pipeline): no latency sample. */
    fill_piece(message, 0);
    send_all(sockets[1], message, sizeof(message));
    block_capture_t capture = {0};
    assert(peer_recv(&peer, &context, capture_block, NULL, NULL, &capture) == 0);
    assert(capture.count == 1);
    assert(peer.block_lat_ema_ms == 0);

    /* Requested ~400 ms ago: first sample seeds the EMA directly. */
    peer.pipeline_len = 1;
    peer.pipeline[0] = (block_req_t){1, 0, 8192, now_ms() - 400};
    fill_piece(message, 1);
    send_all(sockets[1], message, sizeof(message));
    assert(peer_recv(&peer, &context, capture_block, NULL, NULL, &capture) == 0);
    assert(capture.count == 2);
    assert(peer.pipeline_len == 0);
    assert(peer.block_lat_ema_ms >= 400 && peer.block_lat_ema_ms < 2000);

    /* Second sample moves the EMA toward the new latency, not to it. */
    uint32_t first_ema = peer.block_lat_ema_ms;
    peer.pipeline_len = 1;
    peer.pipeline[0] = (block_req_t){2, 0, 8192, now_ms() - 4000};
    fill_piece(message, 2);
    send_all(sockets[1], message, sizeof(message));
    assert(peer_recv(&peer, &context, capture_block, NULL, NULL, &capture) == 0);
    assert(capture.count == 3);
    assert(peer.block_lat_ema_ms > first_ema);
    assert(peer.block_lat_ema_ms < 4000);

    close(sockets[0]);
    close(sockets[1]);
}

int main(void) {
    test_expiry_compacts_pipeline();
    test_cancel_sends_message_and_removes_request();
    test_partial_send_queues_tail_and_flush_completes_frame();
    test_tcp_connect_defers_large_receive_buffer();
    test_handshake_applies_large_receive_buffer();
    test_recv_drains_socket_until_would_block();
    test_receive_buffer_holds_256_kib();
    test_recv_reassembles_message_split_across_reads();
    test_piece_receipt_samples_block_latency();
    puts("peer tests passed");
    return 0;
}
