#pragma once
#include "net.h"
#include <stdint.h>
#include <stddef.h>

#define MAX_PIPELINE   24      /* requests in flight per peer */
#define MAX_PEERS     96
#define BT_HANDSHAKE_LEN 68
#define PEER_BUF_SIZE (4 + 1 + (1<<14) + 9)  /* enough for one piece msg */

/* BT message IDs */
#define MSG_CHOKE        0
#define MSG_UNCHOKE      1
#define MSG_INTERESTED   2
#define MSG_NOT_INTEREST 3
#define MSG_HAVE         4
#define MSG_BITFIELD     5
#define MSG_REQUEST      6
#define MSG_PIECE        7
#define MSG_CANCEL       8
#define MSG_PORT         9
#define MSG_EXTENDED    20

/* BEP10 extension IDs (local mapping) */
#define EXT_HANDSHAKE_ID  0
#define EXT_PEX_ID        1   /* we assign this to ut_pex */

typedef enum {
    PS_CONNECTING = 0,
    PS_HANDSHAKE,
    PS_EXTENSION,
    PS_ACTIVE,
    PS_DEAD
} peer_state_t;

typedef struct {
    int          index;   /* piece */
    int          offset;  /* byte offset within piece */
    int          length;  /* block size */
} block_req_t;

typedef struct peer {
    socket_t     fd;
    peer_state_t state;

    struct sockaddr_in addr;
    char               addr_str[32];

    /* Send/recv buffers */
    uint8_t  rbuf[PEER_BUF_SIZE * 2];
    uint32_t rbuf_len;
    uint8_t  sbuf[PEER_BUF_SIZE];
    uint32_t sbuf_len;

    /* BT state */
    int      am_choked;
    int      am_interested;
    int      peer_choked;
    int      peer_interested;

    uint8_t *bitfield;
    uint32_t bf_bytes;

    /* Pending requests */
    block_req_t pipeline[MAX_PIPELINE];
    int         pipeline_len;

    /* BEP10 extensions */
    int      ext_handshake_sent;
    uint8_t  peer_ext_pex;    /* peer's ut_pex extension ID */
    int      supports_ext;    /* peer set extension bit */

    uint64_t connect_time_ms;
    uint64_t last_recv_ms;
    uint64_t downloaded;

    /* PEX data received (raw bencode, owned) */
    uint8_t *pex_buf;
    uint32_t pex_len;
} peer_t;

typedef struct {
    const uint8_t *info_hash;
    const uint8_t *peer_id;
    uint32_t       num_pieces;
    uint32_t       bf_bytes;   /* (num_pieces+7)/8 */
    const uint8_t *our_bf;    /* our have-bitfield */
    uint16_t       listen_port;
} peer_ctx_t;

/* Allocate/free a peer slot */
peer_t *peer_create(socket_t fd, struct sockaddr_in addr,
                    const peer_ctx_t *ctx);
void    peer_destroy(peer_t *p);

/*
 * Called when the socket is readable; dispatches messages.
 * Returns:
 *   0 = ok
 *  -1 = peer dead (close & destroy)
 */
int peer_recv(peer_t *p, const peer_ctx_t *ctx,
              /* callbacks */
              void (*on_block)(void *ud, uint32_t idx, uint32_t off,
                               const uint8_t *data, uint32_t len),
              void (*on_have)(void *ud, uint32_t idx),
              void (*on_peers)(void *ud, const uint8_t *compact, uint32_t cnt),
              void *ud);

/*
 * Send handshake immediately after connect.
 * Returns 0 on failure.
 */
int peer_send_handshake(peer_t *p, const peer_ctx_t *ctx);

/*
 * Queue a block request.  Returns 0 if pipeline full.
 */
int peer_request_block(peer_t *p, uint32_t piece, uint32_t offset, uint32_t len);

/* Flush any queued sends */
int peer_flush(peer_t *p);

/* Send bitfield */
int peer_send_bitfield(peer_t *p, const uint8_t *bf, uint32_t bf_bytes);

/* Send interested */
int peer_send_interested(peer_t *p);

/* Send BEP10 extension handshake */
int peer_send_ext_handshake(peer_t *p, uint16_t listen_port);

/* Send ut_pex (BEP11) with added peers list */
int peer_send_pex(peer_t *p, const uint8_t *compact6, uint32_t cnt);
