#pragma once
#include "net.h"
#include "mse.h"
#include <stdint.h>

#define PEER_HANDOFF_MAX 8

/*
 * Live TCP socket from magnet resolve, already past the BT handshake.
 * `pending` is optional already-decrypted BT frames (length-prefixed) that
 * magnet consumed while fetching metadata (bitfield / unchoke / HAVE).
 * torrent_stash_peer copies pending and takes ownership of fd.
 */
typedef struct torrent_peer_handoff {
    socket_t fd;
    struct sockaddr_in addr;
    int mse_active;
    rc4_t send_rc4;
    rc4_t recv_rc4;
    const uint8_t *pending;
    uint32_t pending_len;
} torrent_peer_handoff_t;

void torrent_stash_peer(const uint8_t info_hash[20],
                        const torrent_peer_handoff_t *h);
void torrent_drop_stashed_peers(const uint8_t info_hash[20]);

/* Transfer matching sockets out of the stash. Caller owns fd and must
   free pending. Returns the number written to out. */
int torrent_take_stashed_peers(const uint8_t info_hash[20],
                               torrent_peer_handoff_t *out, int max);
