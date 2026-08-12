#include "tracker.h"
#include "bencode.h"
#include "util.h"
#include "net.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* ---- helpers ---- */
static void url_encode_hash(char *out, size_t outsz, const uint8_t *hash, size_t len) {
    size_t off = 0;
    for (size_t i = 0; i < len && off + 4 < outsz; i++) {
        if ((hash[i] >= '0' && hash[i] <= '9') ||
            (hash[i] >= 'A' && hash[i] <= 'Z') ||
            (hash[i] >= 'a' && hash[i] <= 'z') ||
            hash[i] == '-' || hash[i] == '_' || hash[i] == '.' || hash[i] == '~') {
            out[off++] = (char)hash[i];
        } else {
            off += snprintf(out+off, outsz-off, "%%%02X", hash[i]);
        }
    }
    out[off] = 0;
}

static void tracker_result_init(tracker_announce_result_t *result) {
    if (!result)
        return;
    memset(result, 0, sizeof(*result));
}

static void tracker_result_failure(tracker_announce_result_t *result,
                                   const char *reason) {
    if (!result || !reason)
        return;
    result->request_ok = 1;
    result->tracker_failure = 1;
    strncpy(result->failure_reason, reason, sizeof(result->failure_reason) - 1);
    result->failure_reason[sizeof(result->failure_reason) - 1] = 0;
}

/* ---- HTTP tracker via libcurl ---- */
#include <curl/curl.h>

typedef struct {
    uint8_t *data;
    size_t   len;
    size_t   cap;
} curl_buf_t;

typedef struct {
    tracker_cancel_cb callback;
    void *user;
} tracker_cancel_t;

static int tracker_cancelled(const tracker_cancel_t *cancel) {
    return cancel && cancel->callback && cancel->callback(cancel->user);
}

static int curl_progress_cb(void *user, curl_off_t download_total,
                            curl_off_t download_now, curl_off_t upload_total,
                            curl_off_t upload_now) {
    (void)download_total;
    (void)download_now;
    (void)upload_total;
    (void)upload_now;
    return tracker_cancelled((const tracker_cancel_t *)user);
}

static int http_build_announce_url(char *out, size_t outsz, const char *url,
                                   const uint8_t *info_hash,
                                   const uint8_t *peer_id,
                                   uint16_t listen_port,
                                   int64_t downloaded, int64_t left,
                                   int started_event) {
    char ih_enc[64], pid_enc[64];
    url_encode_hash(ih_enc, sizeof(ih_enc), info_hash, 20);
    url_encode_hash(pid_enc, sizeof(pid_enc), peer_id, 20);

    int n = snprintf(out, outsz,
                     "%s%cinfo_hash=%s&peer_id=%s&port=%u"
                     "&uploaded=0&downloaded=%lld&left=%lld"
                     "&compact=1%s&numwant=200",
                     url, strchr(url, '?') ? '&' : '?', ih_enc, pid_enc,
                     (unsigned)listen_port,
                     (long long)downloaded, (long long)left,
                     started_event ? "&event=started" : "");
    return n >= 0 && (size_t)n < outsz;
}

static size_t curl_write_cb(void *ptr, size_t sz, size_t nmemb, void *ud) {
    curl_buf_t *b = (curl_buf_t*)ud;
    size_t total = sz * nmemb;
    uint8_t *newbuf = (uint8_t*)realloc(b->data, b->len + total + 1);
    if (!newbuf) return 0;
    b->data = newbuf;
    memcpy(b->data + b->len, ptr, total);
    b->len += total;
    return total;
}

static uint32_t http_announce_once(const char *url,
                                   const uint8_t *info_hash,
                                   const uint8_t *peer_id,
                                   uint16_t listen_port,
                                   int64_t downloaded, int64_t left,
                                   uint8_t *compact_out, uint32_t max_peers,
                                   int started_event,
                                   int *request_ok,
                                   tracker_announce_result_t *result,
                                   const tracker_cancel_t *cancel) {
    *request_ok = 0;
    if (tracker_cancelled(cancel))
        return 0;
    char full_url[1024];
    if (!http_build_announce_url(full_url, sizeof(full_url), url, info_hash,
                                 peer_id, listen_port, downloaded, left,
                                 started_event))
        return 0;

    CURL *curl = curl_easy_init();
    if (!curl) return 0;

    curl_buf_t buf = {0};
    curl_easy_setopt(curl, CURLOPT_URL, full_url);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "pipensx/0.4");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
    /* Every handle here runs on a background thread; without this
       libcurl installs signal handlers to time out DNS, which is not
       thread-safe (libcurl-thread(3)). */
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    long tls12_only = (long)CURL_SSLVERSION_TLSv1_2 |
                      (long)CURL_SSLVERSION_MAX_TLSv1_2;
    curl_easy_setopt(curl, CURLOPT_SSLVERSION, tls12_only);
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
    if (cancel && cancel->callback) {
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, curl_progress_cb);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, (void *)cancel);
    }
    CURLcode rc = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK || status < 200 || status >= 300) {
        if (rc == CURLE_ABORTED_BY_CALLBACK && tracker_cancelled(cancel))
            log_msg("[tracker] HTTP %s: cancelled\n", url);
        else if (rc != CURLE_OK)
            log_msg("[tracker] HTTP %s: %s\n", url, curl_easy_strerror(rc));
        else
            log_msg("[tracker] HTTP %s: status %ld\n", url, status);
        free(buf.data);
        return 0;
    }

    if (!buf.data) return 0;

    /* Parse bencode response */
    const char *p = (const char*)buf.data;
    const char *end = p + buf.len;
    be_node_t root;
    if (!be_decode(&p, end, &root) || root.type != BE_DICT) {
        free(buf.data);
        return 0;
    }
    *request_ok = 1;

    /* Check for failure */
    be_node_t fail;
    if (be_dict_get(root.buf, root.buf+root.raw_len, "failure reason", 14, &fail)
        && fail.type == BE_STR) {
        char tmp[128];
        size_t n = fail.slen < 127 ? fail.slen : 127;
        memcpy(tmp, fail.sval, n); tmp[n] = 0;
        tracker_result_failure(result, tmp);
        log_msg("[tracker] HTTP failure: %s\n", tmp);
        free(buf.data);
        return 0;
    }

    /* Extract compact peers */
    be_node_t peers;
    uint32_t count = 0;
    if (be_dict_get(root.buf, root.buf+root.raw_len, "peers", 5, &peers) && peers.type == BE_STR) {
        count = (uint32_t)(peers.slen / 6);
        if (count > max_peers) count = max_peers;
        memcpy(compact_out, peers.sval, count * 6);
        log_msg("[tracker] HTTP %s: %u peers\n", url, count);
    }
    if (result) {
        result->request_ok = 1;
        result->peers = count;
    }

    free(buf.data);
    return count;
}

static uint32_t http_announce(const char *url,
                              const uint8_t *info_hash,
                              const uint8_t *peer_id,
                              uint16_t listen_port,
                              int64_t downloaded, int64_t left,
                              uint8_t *compact_out, uint32_t max_peers,
                              int started_event,
                              tracker_announce_result_t *result,
                              const tracker_cancel_t *cancel) {
    int request_ok = 0;
    return http_announce_once(url, info_hash, peer_id, listen_port,
                              downloaded, left, compact_out, max_peers,
                              started_event, &request_ok, result, cancel);
}

/* ---- UDP tracker (BEP15) ---- */
#define UDP_MAGIC  0x41727101980ULL
#define UDP_CONNECT  0
#define UDP_ANNOUNCE 1
#ifndef UDP_RETRIES
#define UDP_RETRIES 3
#endif
#ifndef UDP_RETRY_TIMEOUT_MS
#define UDP_RETRY_TIMEOUT_MS 2500
#endif

static void wr64be(uint8_t *p, uint64_t v) {
    p[0]=(v>>56)&0xFF; p[1]=(v>>48)&0xFF;
    p[2]=(v>>40)&0xFF; p[3]=(v>>32)&0xFF;
    p[4]=(v>>24)&0xFF; p[5]=(v>>16)&0xFF;
    p[6]=(v>> 8)&0xFF; p[7]=(v    )&0xFF;
}

static void wr32be(uint8_t *p, uint32_t v) {
    p[0]=(v>>24)&0xFF; p[1]=(v>>16)&0xFF;
    p[2]=(v>> 8)&0xFF; p[3]=(v    )&0xFF;
}

static void udp_build_announce_packet(uint8_t ann[98], uint64_t conn_id,
                                      uint32_t txid,
                                      const uint8_t *info_hash,
                                      const uint8_t *peer_id,
                                      uint16_t listen_port,
                                      int64_t downloaded, int64_t left,
                                      int started_event) {
    memset(ann, 0, 98);
    wr64be(ann, conn_id);
    wr32be(ann + 8, UDP_ANNOUNCE);
    wr32be(ann + 12, txid);
    memcpy(ann + 16, info_hash, 20);
    memcpy(ann + 36, peer_id, 20);
    wr64be(ann + 56, downloaded < 0 ? 0 : (uint64_t)downloaded);
    wr64be(ann + 64, left < 0 ? 0 : (uint64_t)left);
    wr32be(ann + 80, started_event ? 2u : 0u);
    /* num_want: -1 lets the tracker choose its default. */
    ann[92]=0xFF; ann[93]=0xFF; ann[94]=0xFF; ann[95]=0xFF;
    ann[96]=(listen_port>>8)&0xFF; ann[97]=listen_port&0xFF;
}

static int udp_poll_read(socket_t fd, int timeout_ms,
                         const tracker_cancel_t *cancel) {
    int waited = 0;
    while (waited < timeout_ms) {
        if (tracker_cancelled(cancel))
            return -1;
        int slice = timeout_ms - waited;
        if (slice > 100)
            slice = 100;
        struct pollfd pfd = { fd, POLLIN, 0 };
        int rc = poll(&pfd, 1, slice);
        if (rc != 0)
            return rc;
        waited += slice;
    }
    return 0;
}

/* BEP-15: every reply repeats the action and transaction_id of the request.
   Checking both, plus the source address, is what keeps an unsolicited
   datagram on our ephemeral port from steering the exchange. */
static int udp_reply_matches(const uint8_t *resp, uint32_t action,
                             uint32_t txid,
                             const struct sockaddr_in *expect,
                             const struct sockaddr_in *from) {
    if (from->sin_addr.s_addr != expect->sin_addr.s_addr ||
        from->sin_port != expect->sin_port)
        return 0;
    uint32_t got_action = ((uint32_t)resp[0]<<24)|((uint32_t)resp[1]<<16)|
                          ((uint32_t)resp[2]<< 8)|((uint32_t)resp[3]);
    uint32_t got_txid   = ((uint32_t)resp[4]<<24)|((uint32_t)resp[5]<<16)|
                          ((uint32_t)resp[6]<< 8)|((uint32_t)resp[7]);
    return got_action == action && got_txid == txid;
}

static uint32_t udp_announce(const char *host, uint16_t tport,
                             const uint8_t *info_hash,
                             const uint8_t *peer_id,
                             uint16_t listen_port,
                             int64_t downloaded, int64_t left,
                             uint8_t *compact_out, uint32_t max_peers,
                             int started_event,
                             const tracker_cancel_t *cancel) {
    if (tracker_cancelled(cancel))
        return 0;
    struct sockaddr_in addr;
    if (!net_resolve(host, tport, &addr)) return 0;

    socket_t fd = net_udp_socket(0);
    if (fd == INVALID_SOCK) return 0;

    /* Connect phase */
    uint8_t con_req[16];
    uint64_t magic = UDP_MAGIC;
    con_req[0]=(magic>>56)&0xFF; con_req[1]=(magic>>48)&0xFF;
    con_req[2]=(magic>>40)&0xFF; con_req[3]=(magic>>32)&0xFF;
    con_req[4]=(magic>>24)&0xFF; con_req[5]=(magic>>16)&0xFF;
    con_req[6]=(magic>> 8)&0xFF; con_req[7]=(magic    )&0xFF;
    /* action=connect */
    con_req[8]=con_req[9]=con_req[10]=0; con_req[11]=UDP_CONNECT;
    /* transaction_id. BEP-15 uses it to match responses to requests, so a
       predictable one (it was the clock) is a spoofing aid. */
    uint32_t txid = 0;
    rand_bytes((uint8_t*)&txid, sizeof(txid));
    con_req[12]=(txid>>24)&0xFF; con_req[13]=(txid>>16)&0xFF;
    con_req[14]=(txid>> 8)&0xFF; con_req[15]=(txid    )&0xFF;

    uint8_t con_resp[16];
    struct sockaddr_in from;
    socklen_t alen = sizeof(from);

    int connected = 0;
    for (int attempt = 1; attempt <= UDP_RETRIES; ++attempt) {
        if (tracker_cancelled(cancel)) {
            log_msg("[tracker] UDP %s:%u: cancelled during connect\n",
                    host, tport);
            net_close(fd); return 0;
        }
        sendto(fd, con_req, 16, 0, (struct sockaddr*)&addr, sizeof(addr));
        int poll_rc = udp_poll_read(fd, UDP_RETRY_TIMEOUT_MS, cancel);
        if (poll_rc <= 0) {
            if (poll_rc < 0) {
                log_msg("[tracker] UDP %s:%u: cancelled during connect\n",
                        host, tport);
                net_close(fd); return 0;
            }
            log_msg("[tracker] UDP %s:%u: connect timeout (%d/%d)\n",
                    host, tport, attempt, UDP_RETRIES);
            continue;
        }

        /* Read into a scratch address: passing &addr here let whoever answered
           first replace the tracker we resolved, and the announce below then
           went to them. */
        alen = sizeof(from);
        if (recvfrom(fd, con_resp, 16, 0, (struct sockaddr*)&from, &alen) < 16) {
            log_msg("[tracker] UDP %s:%u: short connect reply (%d/%d)\n",
                    host, tport, attempt, UDP_RETRIES);
            continue;
        }
        if (!udp_reply_matches(con_resp, UDP_CONNECT, txid, &addr, &from)) {
            log_msg("[tracker] UDP %s:%u: bogus connect reply\n", host, tport);
            net_close(fd); return 0;
        }
        connected = 1;
        break;
    }
    if (!connected) {
        net_close(fd); return 0;
    }
    uint64_t conn_id =((uint64_t)con_resp[8]<<56)|((uint64_t)con_resp[9]<<48)|
                       ((uint64_t)con_resp[10]<<40)|((uint64_t)con_resp[11]<<32)|
                       ((uint64_t)con_resp[12]<<24)|((uint64_t)con_resp[13]<<16)|
                       ((uint64_t)con_resp[14]<< 8)|((uint64_t)con_resp[15]);

    /* Announce phase */
    uint8_t ann[98];
    txid++;
    udp_build_announce_packet(ann, conn_id, txid, info_hash, peer_id,
                              listen_port, downloaded, left, started_event);

    uint8_t resp[1500];
    ssize_t rlen = 0;
    int announced = 0;
    for (int attempt = 1; attempt <= UDP_RETRIES; ++attempt) {
        if (tracker_cancelled(cancel)) {
            log_msg("[tracker] UDP %s:%u: cancelled during announce\n",
                    host, tport);
            net_close(fd); return 0;
        }
        sendto(fd, ann, 98, 0, (struct sockaddr*)&addr, sizeof(addr));
        int poll_rc = udp_poll_read(fd, UDP_RETRY_TIMEOUT_MS, cancel);
        if (poll_rc <= 0) {
            if (poll_rc < 0) {
                log_msg("[tracker] UDP %s:%u: cancelled during announce\n",
                        host, tport);
                net_close(fd); return 0;
            }
            log_msg("[tracker] UDP %s:%u: announce timeout (%d/%d)\n",
                    host, tport, attempt, UDP_RETRIES);
            continue;
        }

        alen = sizeof(from);
        rlen = recvfrom(fd, resp, sizeof(resp), 0,
                        (struct sockaddr*)&from, &alen);
        if (rlen < 20) {
            log_msg("[tracker] UDP %s:%u: short announce reply (%d/%d)\n",
                    host, tport, attempt, UDP_RETRIES);
            continue;
        }
        if (!udp_reply_matches(resp, UDP_ANNOUNCE, txid, &addr, &from)) {
            log_msg("[tracker] UDP %s:%u: bogus announce reply\n", host, tport);
            net_close(fd); return 0;
        }
        announced = 1;
        break;
    }
    net_close(fd);

    if (!announced)
        return 0;
    uint32_t count = (uint32_t)((rlen - 20) / 6);
    if (count > max_peers) count = max_peers;
    memcpy(compact_out, resp + 20, count * 6);
    log_msg("[tracker] UDP %s:%u: %u peers\n", host, tport, count);
    return count;
}

/* ---- public API ---- */
static uint32_t tracker_announce_url_ex_cancel_event(
    const char *url, const uint8_t *info_hash, const uint8_t *peer_id,
    uint16_t listen_port, int64_t downloaded, int64_t left,
    uint8_t *compact_out, uint32_t max_peers,
    tracker_announce_result_t *result, int started_event,
    tracker_cancel_cb cancel_callback, void *cancel_user);

uint32_t tracker_announce_url_ex(const char *url,
                                 const uint8_t *info_hash,
                                 const uint8_t *peer_id,
                                 uint16_t listen_port,
                                 int64_t downloaded, int64_t left,
                                 uint8_t *compact_out, uint32_t max_peers,
                                 tracker_announce_result_t *result) {
    return tracker_announce_url_ex_cancel(
        url, info_hash, peer_id, listen_port, downloaded, left, compact_out,
        max_peers, result, NULL, NULL);
}

uint32_t tracker_announce_url_ex_cancel(
                                 const char *url,
                                 const uint8_t *info_hash,
                                 const uint8_t *peer_id,
                                 uint16_t listen_port,
                                 int64_t downloaded, int64_t left,
                                 uint8_t *compact_out, uint32_t max_peers,
                                 tracker_announce_result_t *result,
                                 tracker_cancel_cb cancel_callback,
                                 void *cancel_user) {
    return tracker_announce_url_ex_cancel_event(
        url, info_hash, peer_id, listen_port, downloaded, left, compact_out,
        max_peers, result, 1, cancel_callback, cancel_user);
}

static uint32_t tracker_announce_url_ex_cancel_event(
                                 const char *url,
                                 const uint8_t *info_hash,
                                 const uint8_t *peer_id,
                                 uint16_t listen_port,
                                 int64_t downloaded, int64_t left,
                                 uint8_t *compact_out, uint32_t max_peers,
                                 tracker_announce_result_t *result,
                                 int started_event,
                                 tracker_cancel_cb cancel_callback,
                                 void *cancel_user) {
    tracker_result_init(result);
    if (!url || !info_hash || !peer_id || !compact_out || !max_peers)
        return 0;
    uint32_t count = 0;
    tracker_cancel_t cancel = { cancel_callback, cancel_user };
    if (tracker_cancelled(&cancel))
        return 0;
    if (strncmp(url, "http", 4) == 0) {
        count = http_announce(url, info_hash, peer_id, listen_port,
                              downloaded, left, compact_out, max_peers,
                              started_event, result, &cancel);
        if (result)
            result->peers = count;
        return count;
    }
    if (strncmp(url, "udp://", 6) == 0) {
        char host[128] = "";
        uint16_t port = 80;
        if (sscanf(url + 6, "%127[^:/]:%hu", host, &port) < 1)
            return 0;
        count = udp_announce(host, port, info_hash, peer_id, listen_port,
                             downloaded, left, compact_out, max_peers,
                             started_event, &cancel);
        if (result)
            result->peers = count;
        return count;
    }
    return 0;
}

uint32_t tracker_announce_url(const char *url,
                              const uint8_t *info_hash,
                              const uint8_t *peer_id,
                              uint16_t listen_port,
                              int64_t downloaded, int64_t left,
                              uint8_t *compact_out, uint32_t max_peers) {
    return tracker_announce_url_ex(url, info_hash, peer_id, listen_port,
                                   downloaded, left, compact_out, max_peers,
                                   NULL);
}

uint32_t tracker_announce(const metainfo_t *mi,
                          const uint8_t *peer_id,
                          uint16_t listen_port,
                          int64_t downloaded, int64_t left,
                          uint8_t *compact_out, uint32_t max_peers,
                          tracker_cancel_cb cancel, void *cancel_user) {
    return tracker_announce_with_event(mi, peer_id, listen_port, downloaded,
                                       left, compact_out, max_peers, 1,
                                       cancel, cancel_user);
}

uint32_t tracker_announce_with_event(const metainfo_t *mi,
                          const uint8_t *peer_id,
                          uint16_t listen_port,
                          int64_t downloaded, int64_t left,
                          uint8_t *compact_out, uint32_t max_peers,
                          int started_event,
                          tracker_cancel_cb cancel, void *cancel_user) {
    uint32_t total = 0;
    uint8_t tmp[200*6];

    for (uint32_t t = 0; t < mi->num_trackers && total < max_peers; t++) {
        /* Each tracker blocks for up to CURLOPT_TIMEOUT, and there can be
           MAX_TRACKERS of them — a teardown waiting on this thread must not
           have to sit through the whole list. */
        if (cancel && cancel(cancel_user)) {
            log_msg("[tracker] announce cancelled after %u tracker(s)\n", t);
            break;
        }
        const char *url = mi->trackers[t];
        uint32_t n = 0;

        n = tracker_announce_url_ex_cancel_event(
            url, mi->info_hash, peer_id, listen_port, downloaded, left, tmp,
            200, NULL, started_event, cancel, cancel_user);

        uint32_t can = (total + n <= max_peers) ? n : max_peers - total;
        memcpy(compact_out + total*6, tmp, can*6);
        total += can;
    }
    return total;
}
