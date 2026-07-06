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
                                   int *request_ok,
                                   tracker_announce_result_t *result,
                                   const tracker_cancel_t *cancel) {
    *request_ok = 0;
    if (tracker_cancelled(cancel))
        return 0;
    char ih_enc[64], pid_enc[64];
    url_encode_hash(ih_enc, sizeof(ih_enc), info_hash, 20);
    url_encode_hash(pid_enc, sizeof(pid_enc), peer_id, 20);

    char full_url[1024];
    snprintf(full_url, sizeof(full_url),
             "%s%cinfo_hash=%s&peer_id=%s&port=%u"
             "&uploaded=0&downloaded=%lld&left=%lld"
             "&compact=1&event=started&numwant=200",
             url, strchr(url, '?') ? '&' : '?', ih_enc, pid_enc,
             (unsigned)listen_port,
             (long long)downloaded, (long long)left);

    CURL *curl = curl_easy_init();
    if (!curl) return 0;

    curl_buf_t buf = {0};
    curl_easy_setopt(curl, CURLOPT_URL, full_url);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "pipensx/0.4");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
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
                              tracker_announce_result_t *result,
                              const tracker_cancel_t *cancel) {
    int request_ok = 0;
    return http_announce_once(url, info_hash, peer_id, listen_port,
                              downloaded, left, compact_out, max_peers,
                              &request_ok, result, cancel);
}

/* ---- UDP tracker (BEP15) ---- */
#define UDP_MAGIC  0x41727101980ULL
#define UDP_CONNECT  0
#define UDP_ANNOUNCE 1

static uint32_t udp_announce(const char *host, uint16_t tport,
                             const uint8_t *info_hash,
                             const uint8_t *peer_id,
                             uint16_t listen_port,
                             int64_t downloaded __attribute__((unused)),
                             int64_t left __attribute__((unused)),
                             uint8_t *compact_out, uint32_t max_peers) {
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
    /* transaction_id */
    uint32_t txid = (uint32_t)now_ms();
    con_req[12]=(txid>>24)&0xFF; con_req[13]=(txid>>16)&0xFF;
    con_req[14]=(txid>> 8)&0xFF; con_req[15]=(txid    )&0xFF;

    sendto(fd, con_req, 16, 0, (struct sockaddr*)&addr, sizeof(addr));

    /* Wait for connect response */
    struct pollfd pfd = { fd, POLLIN, 0 };
    if (poll(&pfd, 1, 5000) <= 0) { net_close(fd); return 0; }

    uint8_t con_resp[16];
    socklen_t alen = sizeof(addr);
    if (recvfrom(fd, con_resp, 16, 0, (struct sockaddr*)&addr, &alen) < 16) {
        net_close(fd); return 0;
    }
    uint64_t conn_id = ((uint64_t)con_resp[8]<<56)|((uint64_t)con_resp[9]<<48)|
                       ((uint64_t)con_resp[10]<<40)|((uint64_t)con_resp[11]<<32)|
                       ((uint64_t)con_resp[12]<<24)|((uint64_t)con_resp[13]<<16)|
                       ((uint64_t)con_resp[14]<< 8)|((uint64_t)con_resp[15]);

    /* Announce phase */
    uint8_t ann[98];
    memset(ann, 0, sizeof(ann));
    ann[0]=(conn_id>>56)&0xFF; ann[1]=(conn_id>>48)&0xFF;
    ann[2]=(conn_id>>40)&0xFF; ann[3]=(conn_id>>32)&0xFF;
    ann[4]=(conn_id>>24)&0xFF; ann[5]=(conn_id>>16)&0xFF;
    ann[6]=(conn_id>> 8)&0xFF; ann[7]=(conn_id    )&0xFF;
    ann[8]=ann[9]=ann[10]=0; ann[11]=UDP_ANNOUNCE;
    txid++;
    ann[12]=(txid>>24)&0xFF; ann[13]=(txid>>16)&0xFF;
    ann[14]=(txid>> 8)&0xFF; ann[15]=(txid    )&0xFF;
    memcpy(ann+16, info_hash, 20);
    memcpy(ann+36, peer_id,   20);
    /* downloaded / left / uploaded: 8 bytes each */
    /* event=2 (started) at offset 80 */
    ann[83] = 2; /* event started */
    /* num_want: -1 = 200 */
    ann[92]=0xFF; ann[93]=0xFF; ann[94]=0xFF; ann[95]=0xFF;
    ann[96]=(listen_port>>8)&0xFF; ann[97]=listen_port&0xFF;

    sendto(fd, ann, 98, 0, (struct sockaddr*)&addr, sizeof(addr));

    /* Wait for announce response */
    if (poll(&pfd, 1, 5000) <= 0) { net_close(fd); return 0; }

    uint8_t resp[1500];
    ssize_t rlen = recvfrom(fd, resp, sizeof(resp), 0, NULL, NULL);
    net_close(fd);

    if (rlen < 20) return 0;
    uint32_t count = (uint32_t)((rlen - 20) / 6);
    if (count > max_peers) count = max_peers;
    memcpy(compact_out, resp + 20, count * 6);
    log_msg("[tracker] UDP %s:%u: %u peers\n", host, tport, count);
    return count;
}

/* ---- public API ---- */
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
                              result, &cancel);
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
                             downloaded, left, compact_out, max_peers);
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
                          uint8_t *compact_out, uint32_t max_peers) {
    uint32_t total = 0;
    uint8_t tmp[200*6];

    for (uint32_t t = 0; t < mi->num_trackers && total < max_peers; t++) {
        const char *url = mi->trackers[t];
        uint32_t n = 0;

        n = tracker_announce_url(url, mi->info_hash, peer_id, listen_port,
                                 downloaded, left, tmp, 200);

        uint32_t can = (total + n <= max_peers) ? n : max_peers - total;
        memcpy(compact_out + total*6, tmp, can*6);
        total += can;
    }
    return total;
}

