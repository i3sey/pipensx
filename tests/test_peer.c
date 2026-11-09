#include "../src/core/peer.h"
#include "../src/core/client_identity.h"
#include "../src/core/bencode.h"
#include "../src/core/net.h"
#include "../src/core/util.h"
#include "utp.h"

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static socket_t receive_buffer_failure_fd = INVALID_SOCK;
static int receive_buffer_primary_attempts;
static int receive_buffer_fallback_attempts;

int __real_setsockopt(int fd, int level, int option_name,
                      const void *option_value, socklen_t option_len);

int __wrap_setsockopt(int fd, int level, int option_name,
                      const void *option_value, socklen_t option_len) {
    if (fd == receive_buffer_failure_fd &&
        level == SOL_SOCKET && option_name == SO_RCVBUF &&
        option_len == sizeof(int)) {
        int requested = 0;
        memcpy(&requested, option_value, sizeof(requested));
        if (requested == NET_TCP_RECEIVE_BUFFER_SIZE) {
            receive_buffer_primary_attempts++;
            errno = ENOBUFS;
            return -1;
        }
        if (requested == NET_TCP_RECEIVE_BUFFER_FALLBACK_SIZE)
            receive_buffer_fallback_attempts++;
    }
    return __real_setsockopt(fd, level, option_name, option_value, option_len);
}

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

/* Stack-allocated peers in these tests never go through peer_destroy, but
   peer_recv heap-allocates rbuf on first use — free it explicitly. */
static void peer_fini(peer_t *p) {
    free(p->rbuf);
    p->rbuf = NULL;
    free(p->bitfield);
    p->bitfield = NULL;
    free(p->pex_buf);
    p->pex_buf = NULL;
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

static void test_receive_buffer_falls_back_after_enobufs(void) {
    int sockets[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    int smallBuffer = 4096;
    assert(setsockopt(sockets[0], SOL_SOCKET, SO_RCVBUF, &smallBuffer,
                      sizeof(smallBuffer)) == 0);

    receive_buffer_primary_attempts = 0;
    receive_buffer_fallback_attempts = 0;
    receive_buffer_failure_fd = sockets[0];
    assert(net_set_tcp_receive_buffer(sockets[0]));
    receive_buffer_failure_fd = INVALID_SOCK;

    assert(receive_buffer_primary_attempts == 1);
    assert(receive_buffer_fallback_attempts == 1);
    int receiveBufferSize = 0;
    socklen_t optionSize = sizeof(receiveBufferSize);
    assert(getsockopt(sockets[0], SOL_SOCKET, SO_RCVBUF, &receiveBufferSize,
                      &optionSize) == 0);
    assert(receiveBufferSize >= NET_TCP_RECEIVE_BUFFER_FALLBACK_SIZE);

    close(sockets[0]);
    close(sockets[1]);
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
    uint8_t handshake[BT_HANDSHAKE_LEN];
    memset(handshake, 0, sizeof(handshake));
    handshake[0] = 19;
    memcpy(handshake + 1, "BitTorrent protocol", 19);
    memcpy(handshake + 28, infoHash, sizeof(infoHash));
    assert(peer_rbuf_append(&peer, handshake, BT_HANDSHAKE_LEN) == 0);

    assert(peer_recv(&peer, &context, NULL, NULL, NULL, NULL) == 0);
    assert(peer.state == PS_ACTIVE);
    int receiveBufferSize = 0;
    socklen_t optionSize = sizeof(receiveBufferSize);
    assert(getsockopt(sockets[0], SOL_SOCKET, SO_RCVBUF, &receiveBufferSize,
                      &optionSize) == 0);
    assert(receiveBufferSize >= NET_TCP_RECEIVE_BUFFER_SIZE);

    peer_fini(&peer);
    close(sockets[0]);
    close(sockets[1]);
}

static void test_keepalive_is_empty_length_prefix(void) {
    int sockets[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    peer_t peer;
    memset(&peer, 0, sizeof(peer));
    peer.fd = sockets[0];
    peer.state = PS_ACTIVE;
    assert(peer_send_keepalive(&peer));
    peer_flush(&peer);
    uint8_t frame[4];
    assert(recv(sockets[1], frame, sizeof(frame), 0) == 4);
    assert(frame[0] == 0 && frame[1] == 0 && frame[2] == 0 && frame[3] == 0);
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
    for (uint32_t index = 0; index < 8; ++index)
        peer.pipeline[peer.pipeline_len++] =
            (block_req_t){(int)index, 0, BLOCK_SIZE, 1};

    block_capture_t capture = {0};
    assert(peer_recv(&peer, &context, capture_block, NULL, NULL, &capture) == 0);
    assert(capture.count == 8);
    assert(capture.bytes == 8 * BLOCK_SIZE);
    assert(peer.rbuf_len == 0);
    assert(peer.pipeline_len == 0);

    peer_fini(&peer);
    close(sockets[0]);
    close(sockets[1]);
}

static void test_receive_buffer_holds_256_kib(void) {
    assert(PEER_RECV_BUFFER_SIZE >= 256 * 1024);
    /* The buffer is heap-allocated on demand; an untouched peer reports the
       full capacity as free without owning any of it yet. */
    peer_t peer;
    memset(&peer, 0, sizeof(peer));
    assert(peer.rbuf == NULL);
    assert(peer_rbuf_space(&peer) == PEER_RECV_BUFFER_SIZE);
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
    peer.pipeline[0] = (block_req_t){0, 0, BLOCK_SIZE, 1};
    peer.pipeline[1] = (block_req_t){1, 0, BLOCK_SIZE, 1};
    peer.pipeline_len = 2;

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
    assert(peer.pipeline_len == 0);

    peer_fini(&peer);
    close(sockets[0]);
    close(sockets[1]);
}

/* A received block must fold its request->piece round trip into the peer's
   latency EMA (PERF_PLAN 5.1); unsolicited / wrong-length blocks are dropped. */
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

    /* Unsolicited block (empty pipeline): dropped, no callback, no latency. */
    fill_piece(message, 0);
    send_all(sockets[1], message, sizeof(message));
    block_capture_t capture = {0};
    assert(peer_recv(&peer, &context, capture_block, NULL, NULL, &capture) == 0);
    assert(capture.count == 0);
    assert(peer.block_lat_ema_ms == 0);
    assert(peer.unsolicited_piece_strikes == 1);
    assert(peer.downloaded == 0);

    /* Matching index/offset but wrong length: still rejected. */
    peer.pipeline_len = 1;
    peer.pipeline[0] = (block_req_t){1, 0, 16384, now_ms() - 400};
    fill_piece(message, 1); /* 8192-byte payload */
    send_all(sockets[1], message, sizeof(message));
    assert(peer_recv(&peer, &context, capture_block, NULL, NULL, &capture) == 0);
    assert(capture.count == 0);
    assert(peer.pipeline_len == 1);
    assert(peer.unsolicited_piece_strikes == 2);
    assert(peer.block_lat_ema_ms == 0);

    /* Requested ~400 ms ago: first sample seeds the EMA directly. */
    peer.pipeline[0] = (block_req_t){1, 0, 8192, now_ms() - 400};
    fill_piece(message, 1);
    send_all(sockets[1], message, sizeof(message));
    assert(peer_recv(&peer, &context, capture_block, NULL, NULL, &capture) == 0);
    assert(capture.count == 1);
    assert(peer.pipeline_len == 0);
    assert(peer.block_lat_ema_ms >= 400 && peer.block_lat_ema_ms < 2000);

    /* Second sample moves the EMA toward the new latency, not to it. */
    uint32_t first_ema = peer.block_lat_ema_ms;
    peer.pipeline_len = 1;
    peer.pipeline[0] = (block_req_t){2, 0, 8192, now_ms() - 4000};
    fill_piece(message, 2);
    send_all(sockets[1], message, sizeof(message));
    assert(peer_recv(&peer, &context, capture_block, NULL, NULL, &capture) == 0);
    assert(capture.count == 2);
    assert(peer.block_lat_ema_ms > first_ema);
    assert(peer.block_lat_ema_ms < 4000);

    peer_fini(&peer);
    close(sockets[0]);
    close(sockets[1]);
}

/* Requests we expired or CANCEL'd ourselves must not become unsolicited
   strikes when the peer's late PIECE arrives. */
static void test_late_piece_after_expire_is_not_a_strike(void) {
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

    peer.pipeline_len = 1;
    peer.pipeline[0] = (block_req_t){3, 0, 8192, 1};
    expired_capture_t expired = {0};
    assert(peer_expire_requests(&peer, 10000, 1000, capture_expired,
                                &expired) == 1);
    assert(peer.pipeline_len == 0);
    assert(expired.count == 1);

    fill_piece(message, 3);
    send_all(sockets[1], message, sizeof(message));
    block_capture_t capture = {0};
    assert(peer_recv(&peer, &context, capture_block, NULL, NULL, &capture) == 0);
    assert(capture.count == 0);
    assert(peer.unsolicited_piece_strikes == 0);

    peer_fini(&peer);
    close(sockets[0]);
    close(sockets[1]);
}

/* A 16-slot ring forgot the first of 17 expired requests; the late PIECE
   was scored unsolicited and disconnected the only peer. */
static void test_late_piece_after_mass_expire_is_not_a_strike(void) {
    int sockets[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    assert(net_set_nonblock(sockets[0]));

    enum { MESSAGE_SIZE = 4 + 1 + 8 + 8192 };
    enum { EXPIRED = 17 };
    uint8_t message[MESSAGE_SIZE];

    peer_t peer;
    memset(&peer, 0, sizeof(peer));
    peer.fd = sockets[0];
    peer.state = PS_ACTIVE;
    peer_ctx_t context;
    memset(&context, 0, sizeof(context));
    context.num_pieces = 32;

    peer.pipeline_len = EXPIRED;
    for (int i = 0; i < EXPIRED; i++)
        peer.pipeline[i] = (block_req_t){i, 0, 8192, 1};
    assert(peer_expire_requests(&peer, 10000, 1000, NULL, NULL) == EXPIRED);
    assert(peer.pipeline_len == 0);

    fill_piece(message, 0);
    send_all(sockets[1], message, sizeof(message));
    block_capture_t capture = {0};
    assert(peer_recv(&peer, &context, capture_block, NULL, NULL, &capture) == 0);
    assert(capture.count == 0);
    assert(peer.unsolicited_piece_strikes == 0);
    assert(peer.state != PS_DEAD);

    peer_fini(&peer);
    close(sockets[0]);
    close(sockets[1]);
}

static void test_unsolicited_during_expiry_grace_is_not_a_strike(void) {
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

    fill_piece(message, 0);
    peer.last_expiry_ms = now_ms();
    send_all(sockets[1], message, sizeof(message));
    block_capture_t capture = {0};
    assert(peer_recv(&peer, &context, capture_block, NULL, NULL, &capture) == 0);
    assert(capture.count == 0);
    assert(peer.unsolicited_piece_strikes == 0);

    peer.last_expiry_ms = now_ms() - (LATE_PIECE_GRACE_MS + 1);
    send_all(sockets[1], message, sizeof(message));
    assert(peer_recv(&peer, &context, capture_block, NULL, NULL, &capture) == 0);
    assert(capture.count == 0);
    assert(peer.unsolicited_piece_strikes == 1);

    peer_fini(&peer);
    close(sockets[0]);
    close(sockets[1]);
}

static void test_late_piece_after_cancel_is_not_a_strike(void) {
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

    peer.pipeline_len = 1;
    peer.pipeline[0] = (block_req_t){4, 0, 8192, now_ms()};
    assert(peer_cancel_block(&peer, 4, 0, 8192) == 1);
    assert(peer.pipeline_len == 0);

    /* Drain the CANCEL frame we just queued so the socket stays clean. */
    peer_flush(&peer);

    fill_piece(message, 4);
    block_capture_t capture = {0};
    assert(peer_recv(&peer, &context, capture_block, NULL, NULL, &capture) == 0);
    assert(capture.count == 0);
    assert(peer.unsolicited_piece_strikes == 0);

    peer_fini(&peer);
    close(sockets[0]);
    close(sockets[1]);
}

/* ---- μTP loopback ----
 * Two libutp contexts over two real UDP sockets, driven with the same callback
 * shape torrent.c uses. Proves the vendored engine connects, transfers an
 * ordered byte stream, and that our SENDTO/ON_READ/timeout wiring is correct on
 * this platform — the abstraction μTP peers ride on. */
typedef struct {
    socket_t     fd;
    utp_socket  *sock;          /* connecting (A) or accepted (B) socket */
    int          connected;
    uint8_t      rx[4096];
    size_t       rx_len;
} utp_end_t;

static uint64 tu_sendto(utp_callback_arguments *a) {
    utp_end_t *e = (utp_end_t*)utp_context_get_userdata(a->context);
    sendto(e->fd, a->buf, a->len, 0, a->address, a->address_len);
    return 0;
}
static uint64 tu_on_read(utp_callback_arguments *a) {
    utp_end_t *e = (utp_end_t*)utp_context_get_userdata(a->context);
    if (e->rx_len + a->len <= sizeof(e->rx)) {
        memcpy(e->rx + e->rx_len, a->buf, a->len);
        e->rx_len += a->len;
    }
    utp_read_drained(a->socket);
    return 0;
}
static uint64 tu_get_read_buffer_size(utp_callback_arguments *a) {
    utp_end_t *e = (utp_end_t*)utp_context_get_userdata(a->context);
    return sizeof(e->rx) - e->rx_len;
}
static uint64 tu_on_state_change(utp_callback_arguments *a) {
    utp_end_t *e = (utp_end_t*)utp_context_get_userdata(a->context);
    if (a->state == UTP_STATE_CONNECT || a->state == UTP_STATE_WRITABLE)
        e->connected = 1;
    return 0;
}
static uint64 tu_on_firewall(utp_callback_arguments *a) { (void)a; return 0; }
static uint64 tu_on_accept(utp_callback_arguments *a) {
    utp_end_t *e = (utp_end_t*)utp_context_get_userdata(a->context);
    e->sock = a->socket;            /* B's inbound socket */
    return 0;
}
static uint64 tu_get_udp_mtu(utp_callback_arguments *a) { (void)a; return 1500; }
static uint64 tu_get_ms(utp_callback_arguments *a) { (void)a; return now_ms(); }
static uint64 tu_get_us(utp_callback_arguments *a) {
    (void)a;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64)ts.tv_sec * 1000000ULL + (uint64)ts.tv_nsec / 1000ULL;
}
static uint64 tu_get_random(utp_callback_arguments *a) { (void)a; return (uint64)rand(); }

static void utp_register(utp_context *c) {
    utp_set_callback(c, UTP_SENDTO,               tu_sendto);
    utp_set_callback(c, UTP_ON_READ,              tu_on_read);
    utp_set_callback(c, UTP_GET_READ_BUFFER_SIZE, tu_get_read_buffer_size);
    utp_set_callback(c, UTP_ON_STATE_CHANGE,      tu_on_state_change);
    utp_set_callback(c, UTP_ON_FIREWALL,          tu_on_firewall);
    utp_set_callback(c, UTP_ON_ACCEPT,            tu_on_accept);
    utp_set_callback(c, UTP_GET_UDP_MTU,          tu_get_udp_mtu);
    utp_set_callback(c, UTP_GET_MILLISECONDS,     tu_get_ms);
    utp_set_callback(c, UTP_GET_MICROSECONDS,     tu_get_us);
    utp_set_callback(c, UTP_GET_RANDOM,           tu_get_random);
}

static void utp_pump_fd(utp_context *ctx, socket_t fd) {
    uint8_t buf[2048];
    struct sockaddr_in from;
    for (;;) {
        socklen_t flen = sizeof(from);
        ssize_t n = recvfrom(fd, buf, sizeof(buf), 0,
                             (struct sockaddr*)&from, &flen);
        if (n < 0) break;
        utp_process_udp(ctx, buf, (size_t)n, (struct sockaddr*)&from, flen);
    }
    utp_issue_deferred_acks(ctx);
}

static void test_utp_loopback_transfers_ordered_stream(void) {
    utp_end_t ea, eb;
    memset(&ea, 0, sizeof(ea));
    memset(&eb, 0, sizeof(eb));
    ea.fd = net_udp_socket(0);
    eb.fd = net_udp_socket(0);
    assert(ea.fd != INVALID_SOCK && eb.fd != INVALID_SOCK);

    /* net_udp_socket(0) leaves the socket unbound (production μTP is
       outgoing-only, so the OS assigns a port on first send). For a loopback
       both ends must be addressable, so bind explicitly to 127.0.0.1:0 and read
       back the assigned ports. */
    struct sockaddr_in la = {0}, lb = {0};
    la.sin_family = lb.sin_family = AF_INET;
    la.sin_addr.s_addr = lb.sin_addr.s_addr = htonl(0x7F000001); /* 127.0.0.1 */
    assert(bind(ea.fd, (struct sockaddr*)&la, sizeof(la)) == 0);
    assert(bind(eb.fd, (struct sockaddr*)&lb, sizeof(lb)) == 0);

    struct sockaddr_in baddr;
    socklen_t blen = sizeof(baddr);
    assert(getsockname(eb.fd, (struct sockaddr*)&baddr, &blen) == 0);
    baddr.sin_addr.s_addr = htonl(0x7F000001); /* 127.0.0.1 */

    utp_context *ca = utp_init(2);
    utp_context *cb = utp_init(2);
    assert(ca && cb);
    utp_context_set_userdata(ca, &ea);
    utp_context_set_userdata(cb, &eb);
    utp_register(ca);
    utp_register(cb);

    ea.sock = utp_create_socket(ca);
    assert(ea.sock);
    assert(utp_connect(ea.sock, (struct sockaddr*)&baddr, sizeof(baddr)) == 0);

    /* Build a payload larger than one MTU so it must span several packets and
       reassemble in order. */
    uint8_t msg[3000];
    for (size_t i = 0; i < sizeof(msg); i++)
        msg[i] = (uint8_t)(i * 31 + 7);

    size_t off = 0;
    uint64_t last_timeout = now_ms();
    for (int iter = 0; iter < 200000 && eb.rx_len < sizeof(msg); iter++) {
        utp_pump_fd(ca, ea.fd);
        utp_pump_fd(cb, eb.fd);
        if (ea.connected && off < sizeof(msg)) {
            ssize_t w = utp_write(ea.sock, msg + off, sizeof(msg) - off);
            if (w > 0) off += (size_t)w; /* libutp accepted part of the payload */
        }
        uint64_t now = now_ms();
        if (now - last_timeout >= 20) {
            utp_check_timeouts(ca);
            utp_check_timeouts(cb);
            last_timeout = now;
        }
        usleep(200);
    }

    assert(off == sizeof(msg));       /* whole payload handed to libutp */
    assert(eb.rx_len == sizeof(msg)); /* and fully reassembled at the peer */
    assert(memcmp(eb.rx, msg, sizeof(msg)) == 0);

    utp_destroy(ca);
    utp_destroy(cb);
    close(ea.fd);
    close(eb.fd);
    puts("utp loopback ok");
}

static int read_upload_pattern(void *user, uint32_t index, uint32_t offset,
                                uint8_t *data, uint32_t length) {
    assert(index == 0 && offset == 0);
    memset(data, *(uint8_t *)user, length);
    return 1;
}

static int inject_request(peer_t *peer, peer_ctx_t *ctx, uint8_t type,
                            uint32_t index, uint32_t offset, uint32_t length) {
    uint8_t frame[17] = {0, 0, 0, 13};
    frame[4] = type;
    uint32_t words[] = {index, offset, length};
    for (unsigned i = 0; i < 3; ++i)
        for (unsigned j = 0; j < 4; ++j)
            frame[5 + 4 * i + j] = (uint8_t)(words[i] >> (24 - 8 * j));
    assert(peer_rbuf_append(peer, frame, sizeof(frame)) == 0);
    return peer_process(peer, ctx, NULL, NULL, NULL, NULL);
}

static void test_upload_cancel_eviction_and_backpressure(int encrypted) {
    int fds[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    assert(net_set_nonblock(fds[0]) && net_set_nonblock(fds[1]));
    int small = 1024;
    assert(setsockopt(fds[0], SOL_SOCKET, SO_SNDBUF, &small, sizeof(small)) == 0);
    uint8_t bf = 0x80, pattern = 67;
    peer_ctx_t ctx = {0};
    ctx.num_pieces = 1; ctx.bf_bytes = 1; ctx.our_bf = &bf;
    ctx.piece_length = 16384; ctx.total_length = 16384;
    peer_t peer = {0};
    peer.fd = fds[0]; peer.state = PS_ACTIVE; peer.supports_fast = 1;
    uint64_t total = 0;
    peer.uploaded_total = &total;
    assert(inject_request(&peer, &ctx, MSG_REQUEST, 0, 0, 16384) == 0);
    assert(peer.upload_count == 1);
    uint32_t budget = 8192;
    assert(peer_upload(&peer, &ctx, read_upload_pattern, &pattern, &budget));
    assert(peer.upload_count == 1 && !peer.uploaded);
    assert(inject_request(&peer, &ctx, MSG_CANCEL, 0, 0, 16384) == 0);
    assert(!peer.upload_count);
    uint8_t reject[17];
    assert(recv(fds[1], reject, sizeof(reject), 0) == sizeof(reject));
    assert(reject[4] == MSG_REJECT_REQUEST);

    assert(inject_request(&peer, &ctx, MSG_REQUEST, 0, 0, 16384) == 0);
    bf = 0;
    budget = 16384;
    assert(peer_upload(&peer, &ctx, read_upload_pattern, &pattern, &budget));
    assert(recv(fds[1], reject, sizeof(reject), 0) == sizeof(reject));
    assert(reject[4] == MSG_REJECT_REQUEST && !peer.uploaded);
    bf = 0x80;
    const uint8_t key[] = "upload-backpressure";
    rc4_t receiver;
    if (encrypted) {
        peer.mse_active = 1;
        rc4_init(&peer.mse.send_rc4, key, sizeof(key));
        rc4_init(&receiver, key, sizeof(key));
    }
    assert(inject_request(&peer, &ctx, MSG_REQUEST, 0, 0, 16384) == 0);
    assert(peer_upload(&peer, &ctx, read_upload_pattern, &pattern, &budget));
    assert(peer.uploaded < 16384 && peer.sbuf_len);
    pattern = 99;
    assert(peer_send_have(&peer, 0));
    uint8_t wire[16384 + 13 + 9];
    size_t used = 0;
    for (unsigned i = 0; i < 1000 && used < sizeof(wire); ++i) {
        ssize_t n = recv(fds[1], wire + used, sizeof(wire) - used, 0);
        if (n > 0) used += (size_t)n;
        assert(peer_flush(&peer));
    }
    assert(used == sizeof(wire));
    if (encrypted) rc4_crypt(&receiver, wire, wire, used);
    assert(wire[4] == MSG_PIECE);
    for (unsigned i = 13; i < 13 + 16384; ++i) assert(wire[i] == 67);
    assert(wire[13 + 16384 + 4] == MSG_HAVE);
    assert(peer.uploaded == 16384 && total == 16384 && !peer.sbuf_len);
    assert(peer.upload_requests == 3 && peer.upload_rejected == 2);
    assert(inject_request(&peer, &ctx, MSG_REQUEST, 0, 16380, 8) == -1);
    peer_fini(&peer);
    close(fds[0]); close(fds[1]);
}

typedef struct {
    uint32_t count;
    uint8_t endpoint[6];
} peer_exchange_capture_t;

static void capture_peer_exchange(void *user, const uint8_t *compact,
                                   uint32_t count) {
    peer_exchange_capture_t *capture = user;
    capture->count += count;
    if (count) memcpy(capture->endpoint, compact, 6);
}

static void capture_pex_source(void *user, peer_t *source, const uint8_t *compact,
                               const uint8_t *flags, uint32_t count) {
    assert(source->peer_ext_pex == 7);
    assert(count == 1 && flags && flags[0] == 0x0c);
    capture_peer_exchange(user, compact, count);
}

typedef struct {
    int count;
    uint8_t type;
    uint32_t ip, error;
    uint16_t port;
} holepunch_capture_t;

static void capture_holepunch(void *user, peer_t *peer, uint8_t type,
                              uint32_t ip, uint16_t port, uint32_t error) {
    (void)peer;
    holepunch_capture_t *capture = user;
    ++capture->count;
    capture->type = type;
    capture->ip = ip;
    capture->port = port;
    capture->error = error;
}

static void test_holepunch_extension_wire(void) {
    int fds[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    assert(net_set_nonblock(fds[0]));
    peer_t peer = {0};
    peer.fd = fds[0];
    peer.state = PS_ACTIVE;
    peer.holepunch_enabled = 1;
    peer.peer_ext_holepunch = 7;
    uint32_t ip = htonl(0x08080404u);
    uint16_t port = htons(6881);
    assert(peer_send_holepunch(&peer, 0, ip, port, 0));
    uint8_t frame[14];
    assert(recv(fds[1], frame, sizeof(frame), MSG_WAITALL) == sizeof(frame));
    assert(frame[3] == 10 && frame[4] == MSG_EXTENDED && frame[5] == 7);
    assert(frame[6] == 0 && frame[7] == 0);
    assert(!memcmp(frame + 8, &ip, 4) && !memcmp(frame + 12, &port, 2));
    peer_ctx_t ctx = {0};
    holepunch_capture_t capture = {0};
    ctx.on_holepunch = capture_holepunch;
    ctx.holepunch_user = &capture;
    frame[5] = EXT_HOLEPUNCH_ID;
    send_all(fds[1], frame, sizeof(frame));
    assert(peer_recv(&peer, &ctx, NULL, NULL, NULL, NULL) == 0);
    assert(capture.count == 1 && capture.type == 0 && capture.ip == ip &&
           capture.port == port && !capture.error);
    uint8_t padded[18] = {0};
    memcpy(padded, frame, sizeof(frame));
    padded[3] = 14;
    send_all(fds[1], padded, sizeof(padded));
    assert(peer_recv(&peer, &ctx, NULL, NULL, NULL, NULL) == 0);
    assert(capture.count == 2);
    frame[5] = peer.peer_ext_holepunch;
    send_all(fds[1], frame, sizeof(frame));
    assert(peer_recv(&peer, &ctx, NULL, NULL, NULL, NULL) == 0);
    assert(capture.count == 2);
    frame[5] = EXT_HOLEPUNCH_ID;
    frame[7] = 1;
    send_all(fds[1], frame, sizeof(frame));
    assert(peer_recv(&peer, &ctx, NULL, NULL, NULL, NULL) == 0);
    assert(capture.count == 2);
    frame[7] = 0;
    peer.private_torrent = 1;
    send_all(fds[1], frame, sizeof(frame));
    assert(peer_recv(&peer, &ctx, NULL, NULL, NULL, NULL) == 0);
    assert(capture.count == 2);
    assert(!peer_send_holepunch(&peer, 0, ip, port, 0));
    peer_fini(&peer);
    close(fds[0]); close(fds[1]);
}

static void test_peer_exchange_uses_receiver_extension_id(void) {
    int fds[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    assert(net_set_nonblock(fds[0]));
    peer_t peer = {0};
    peer.fd = fds[0];
    peer.state = PS_ACTIVE;
    peer.peer_ext_pex = 7;
    peer_ctx_t ctx = {0};
    peer_exchange_capture_t capture = {0};
    uint8_t endpoint[] = {8, 8, 4, 4, 0x1a, 0xe1};
    uint8_t frame[23] = {0};
    frame[3] = 19;
    frame[4] = MSG_EXTENDED;
    frame[5] = EXT_PEX_ID;
    memcpy(frame + 6, "d5:added6:", 10);
    memcpy(frame + 16, endpoint, sizeof(endpoint));
    frame[22] = 'e';
    send_all(fds[1], frame, sizeof(frame));
    assert(peer_recv(&peer, &ctx, NULL, NULL, capture_peer_exchange, &capture) == 0);
    assert(capture.count == 1 && !memcmp(capture.endpoint, endpoint, 6));
    frame[5] = peer.peer_ext_pex;
    send_all(fds[1], frame, sizeof(frame));
    assert(peer_recv(&peer, &ctx, NULL, NULL, capture_peer_exchange, &capture) == 0);
    assert(capture.count == 1);
    uint8_t flagged[36];
    memcpy(flagged, frame, sizeof(frame));
    flagged[3] = 31;
    flagged[5] = EXT_PEX_ID;
    memcpy(flagged + 22, "7:added.f1:", 11);
    flagged[33] = 0x0c;
    flagged[34] = 'e';
    ctx.on_pex = capture_pex_source;
    ctx.pex_user = &capture;
    send_all(fds[1], flagged, 35);
    assert(peer_recv(&peer, &ctx, NULL, NULL, NULL, NULL) == 0);
    assert(capture.count == 2);
    peer.private_torrent = 1;
    frame[5] = EXT_PEX_ID;
    send_all(fds[1], frame, sizeof(frame));
    assert(peer_recv(&peer, &ctx, NULL, NULL, capture_peer_exchange, &capture) == 0);
    assert(capture.count == 2);
    peer_fini(&peer);
    close(fds[0]); close(fds[1]);
}

static void test_remote_upload_only_updates(void) {
    int fds[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    assert(net_set_nonblock(fds[0]));
    peer_t peer = {0};
    peer.fd = fds[0];
    peer.state = PS_ACTIVE;
    peer_ctx_t ctx = {0};
    const char *updates[] = {
        "d11:upload_onlyi1ee", "d11:upload_only1:xe", "de",
        "d11:upload_onlyi0ee"
    };
    const int expected[] = {1, 1, 1, 0};
    for (unsigned i = 0; i < 4; ++i) {
        uint8_t frame[64] = {0};
        size_t length = strlen(updates[i]);
        frame[3] = (uint8_t)(length + 2);
        frame[4] = MSG_EXTENDED;
        frame[5] = EXT_HANDSHAKE_ID;
        memcpy(frame + 6, updates[i], length);
        send_all(fds[1], frame, length + 6);
        assert(peer_recv(&peer, &ctx, NULL, NULL, NULL, NULL) == 0);
        assert(peer.remote_upload_only == expected[i]);
    }
    peer_fini(&peer);
    close(fds[0]); close(fds[1]);
}

int main(void) {
    test_holepunch_extension_wire();
    test_peer_exchange_uses_receiver_extension_id();
    test_remote_upload_only_updates();
    for (int seed = 0; seed < 2; ++seed) {
        int fds[2];
        assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
        peer_t peer = {0};
        peer.fd = fds[0]; peer.upload_only = seed; peer.private_torrent = seed;
        assert(peer_send_ext_handshake(&peer, 51413));
        uint8_t frame[256];
        ssize_t n = recv(fds[1], frame, sizeof(frame), 0);
        assert(n > 6 && frame[4] == MSG_EXTENDED && frame[5] == 0);
        be_node_t v, upload;
        assert(be_dict_get((const char*)frame + 6, (const char*)frame + n, "v", 1, &v));
        assert(v.type == BE_STR && v.slen == strlen(PIPENSX_CLIENT_NAME));
        assert(!memcmp(v.sval, PIPENSX_CLIENT_NAME, v.slen));
        assert(be_dict_get((const char*)frame + 6, (const char*)frame + n, "upload_only", 11, &upload));
        assert(upload.ival == seed);
        close(fds[0]); close(fds[1]);
    }
    test_upload_cancel_eviction_and_backpressure(0);
    test_upload_cancel_eviction_and_backpressure(1);
    test_expiry_compacts_pipeline();
    test_cancel_sends_message_and_removes_request();
    test_partial_send_queues_tail_and_flush_completes_frame();
    test_tcp_connect_defers_large_receive_buffer();
    test_receive_buffer_falls_back_after_enobufs();
    test_handshake_applies_large_receive_buffer();
    test_keepalive_is_empty_length_prefix();
    test_recv_drains_socket_until_would_block();
    test_receive_buffer_holds_256_kib();
    test_recv_reassembles_message_split_across_reads();
    test_piece_receipt_samples_block_latency();
    test_late_piece_after_expire_is_not_a_strike();
    test_late_piece_after_mass_expire_is_not_a_strike();
    test_unsolicited_during_expiry_grace_is_not_a_strike();
    test_late_piece_after_cancel_is_not_a_strike();
    test_utp_loopback_transfers_ordered_stream();
    puts("peer tests passed");
    return 0;
}
