#include "antizapret.h"
#include "util.h"

#include <ctype.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PAC_MAX_BYTES (2 * 1024 * 1024)

static const antizapret_route_t DEFAULT_ROUTES[] = {
    {ANTIZAPRET_ROUTE_HTTP,
     "proxy-nossl.antizapret.prostovpn.org:29976"},
    {ANTIZAPRET_ROUTE_HTTPS,
     "proxy-ssl.antizapret.prostovpn.org:3143"},
    {ANTIZAPRET_ROUTE_DIRECT, ""},
};

typedef struct {
    char *data;
    size_t len;
} pac_buffer_t;

static atomic_flag state_lock = ATOMIC_FLAG_INIT;
static atomic_int enabled = 1;
static atomic_int proxy_preferred = 0;
static char cache_path[512];
static antizapret_route_t cached_routes[ANTIZAPRET_MAX_ROUTES];
static size_t cached_route_count;

static void lock_state(void) {
    while (atomic_flag_test_and_set_explicit(&state_lock,
                                              memory_order_acquire)) {
    }
}

static void unlock_state(void) {
    atomic_flag_clear_explicit(&state_lock, memory_order_release);
}

static int ascii_equal_nocase(const char *a, const char *b, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i]))
            return 0;
    }
    return 1;
}

static int host_is_target(const char *host, size_t len) {
    static const char *const domains[] = {
        "rutracker.org",
        "t-ru.org",
        "img-eshop.cdn.nintendo.net",
    };
    while (len && host[len - 1] == '.')
        --len;
    for (size_t i = 0; i < sizeof(domains) / sizeof(domains[0]); ++i) {
        size_t domain_len = strlen(domains[i]);
        if (len == domain_len &&
            ascii_equal_nocase(host, domains[i], domain_len))
            return 1;
        if (len > domain_len && host[len - domain_len - 1] == '.' &&
            ascii_equal_nocase(host + len - domain_len, domains[i],
                               domain_len))
            return 1;
    }
    return 0;
}

static size_t pac_write_cb(char *ptr, size_t size, size_t nmemb, void *userdata) {
    pac_buffer_t *buffer = (pac_buffer_t *)userdata;
    size_t bytes = size * nmemb;
    if (bytes > PAC_MAX_BYTES || buffer->len > PAC_MAX_BYTES - bytes)
        return 0;
    char *next = (char *)realloc(buffer->data, buffer->len + bytes + 1);
    if (!next)
        return 0;
    buffer->data = next;
    memcpy(buffer->data + buffer->len, ptr, bytes);
    buffer->len += bytes;
    buffer->data[buffer->len] = '\0';
    return bytes;
}

static int route_exists(const antizapret_route_t *routes, size_t count,
                        antizapret_route_type_t type, const char *address) {
    for (size_t i = 0; i < count; ++i) {
        if (routes[i].type == type &&
            strcmp(routes[i].address, address) == 0)
            return 1;
    }
    return 0;
}

static size_t parse_chain(const char *text, size_t len,
                          antizapret_route_t *routes, size_t max_routes) {
    size_t count = 0;
    size_t pos = 0;
    while (pos < len && count < max_routes) {
        while (pos < len &&
               (isspace((unsigned char)text[pos]) || text[pos] == ';'))
            ++pos;
        size_t type_start = pos;
        while (pos < len && isalnum((unsigned char)text[pos]))
            ++pos;
        size_t type_len = pos - type_start;
        while (pos < len && isspace((unsigned char)text[pos]))
            ++pos;

        antizapret_route_type_t type;
        if (type_len == 6 &&
            ascii_equal_nocase(text + type_start, "DIRECT", 6)) {
            type = ANTIZAPRET_ROUTE_DIRECT;
        } else if (type_len == 5 &&
                   ascii_equal_nocase(text + type_start, "PROXY", 5)) {
            type = ANTIZAPRET_ROUTE_HTTP;
        } else if (type_len == 5 &&
                   ascii_equal_nocase(text + type_start, "HTTPS", 5)) {
            type = ANTIZAPRET_ROUTE_HTTPS;
        } else if ((type_len == 6 &&
                    ascii_equal_nocase(text + type_start, "SOCKS5", 6)) ||
                   (type_len == 5 &&
                    ascii_equal_nocase(text + type_start, "SOCKS", 5))) {
            type = ANTIZAPRET_ROUTE_SOCKS5;
        } else if (type_len == 6 &&
                   ascii_equal_nocase(text + type_start, "SOCKS4", 6)) {
            type = ANTIZAPRET_ROUTE_SOCKS4;
        } else {
            while (pos < len && text[pos] != ';')
                ++pos;
            continue;
        }

        char address[256] = "";
        if (type != ANTIZAPRET_ROUTE_DIRECT) {
            size_t address_start = pos;
            while (pos < len && text[pos] != ';' &&
                   !isspace((unsigned char)text[pos]))
                ++pos;
            size_t address_len = pos - address_start;
            if (!address_len || address_len >= sizeof(address) ||
                !memchr(text + address_start, ':', address_len)) {
                while (pos < len && text[pos] != ';')
                    ++pos;
                continue;
            }
            memcpy(address, text + address_start, address_len);
            address[address_len] = '\0';
        }

        if (!route_exists(routes, count, type, address)) {
            routes[count].type = type;
            snprintf(routes[count].address, sizeof(routes[count].address),
                     "%s", address);
            ++count;
        }
        while (pos < len && text[pos] != ';')
            ++pos;
    }
    return count;
}

size_t antizapret_parse_pac(const char *pac, size_t pac_len,
                            antizapret_route_t *routes, size_t max_routes) {
    if (!pac || !routes || !max_routes)
        return 0;

    size_t best_count = 0;
    for (size_t i = 0; i < pac_len; ++i) {
        if (pac[i] != '\'' && pac[i] != '"')
            continue;
        char quote = pac[i++];
        size_t start = i;
        while (i < pac_len && pac[i] != quote) {
            if (pac[i] == '\\' && i + 1 < pac_len)
                ++i;
            ++i;
        }
        if (i >= pac_len)
            break;

        antizapret_route_t candidate[ANTIZAPRET_MAX_ROUTES];
        size_t count = parse_chain(pac + start, i - start, candidate,
                                   ANTIZAPRET_MAX_ROUTES);
        int has_proxy = 0;
        for (size_t r = 0; r < count; ++r)
            has_proxy |= candidate[r].type != ANTIZAPRET_ROUTE_DIRECT;
        if (has_proxy && count > best_count) {
            best_count = count < max_routes ? count : max_routes;
            memcpy(routes, candidate, best_count * sizeof(routes[0]));
        }
    }
    return best_count;
}

static int read_file(const char *path, pac_buffer_t *buffer) {
    FILE *file = fopen(path, "rb");
    if (!file)
        return 0;
    char chunk[4096];
    size_t bytes;
    while ((bytes = fread(chunk, 1, sizeof(chunk), file)) != 0) {
        if (pac_write_cb(chunk, 1, bytes, buffer) != bytes) {
            fclose(file);
            return 0;
        }
    }
    int ok = !ferror(file);
    fclose(file);
    return ok;
}

void antizapret_init(const char *root_path) {
    lock_state();
    cache_path[0] = '\0';
    cached_route_count = 0;
    if (root_path && root_path[0])
        snprintf(cache_path, sizeof(cache_path), "%s/antizapret.pac", root_path);
    unlock_state();
}

void antizapret_set_enabled(int value) {
    atomic_store_explicit(&enabled, value != 0, memory_order_release);
    if (!value)
        atomic_store_explicit(&proxy_preferred, 0, memory_order_release);
}

int antizapret_is_enabled(void) {
    return atomic_load_explicit(&enabled, memory_order_acquire);
}

int antizapret_is_target_url(const char *url) {
    if (!url)
        return 0;
    const char *scheme = strstr(url, "://");
    if (!scheme)
        return 0;
    const char *host = scheme + 3;
    const char *end = host;
    if (*host == '[')
        return 0;
    while (*end && *end != ':' && *end != '/' && *end != '?' && *end != '#')
        ++end;
    return host_is_target(host, (size_t)(end - host));
}

int antizapret_proxy_preferred(void) {
    return antizapret_is_enabled() &&
           atomic_load_explicit(&proxy_preferred, memory_order_acquire);
}

void antizapret_note_proxy_success(void) {
    if (antizapret_is_enabled())
        atomic_store_explicit(&proxy_preferred, 1, memory_order_release);
}

size_t antizapret_get_routes(antizapret_route_t *routes, size_t max_routes) {
    if (!antizapret_is_enabled() || !routes || !max_routes)
        return 0;

    lock_state();
    if (cached_route_count) {
        size_t count = cached_route_count < max_routes ? cached_route_count
                                                       : max_routes;
        memcpy(routes, cached_routes, count * sizeof(routes[0]));
        unlock_state();
        return count;
    }
    char local_cache_path[sizeof(cache_path)];
    snprintf(local_cache_path, sizeof(local_cache_path), "%s", cache_path);
    unlock_state();

    pac_buffer_t buffer = {0};
    antizapret_route_t loaded_routes[ANTIZAPRET_MAX_ROUTES];
    size_t count = 0;
    if (local_cache_path[0] && read_file(local_cache_path, &buffer))
        count = antizapret_parse_pac(buffer.data, buffer.len, loaded_routes,
                                    ANTIZAPRET_MAX_ROUTES);
    free(buffer.data);
    if (!count) {
        count = sizeof(DEFAULT_ROUTES) / sizeof(DEFAULT_ROUTES[0]);
        memcpy(loaded_routes, DEFAULT_ROUTES,
               count * sizeof(loaded_routes[0]));
        log_msg("[antizapret] PAC cache unavailable, using built-in routes\n");
    } else {
        log_msg("[antizapret] loaded %zu routes from PAC cache\n", count);
    }

    lock_state();
    if (!cached_route_count && count) {
        memcpy(cached_routes, loaded_routes, count * sizeof(cached_routes[0]));
        cached_route_count = count;
    }
    count = cached_route_count < max_routes ? cached_route_count : max_routes;
    memcpy(routes, cached_routes, count * sizeof(routes[0]));
    unlock_state();
    return count;
}

void antizapret_apply_route(CURL *curl, const antizapret_route_t *route) {
    if (!curl)
        return;
    if (!route || route->type == ANTIZAPRET_ROUTE_DIRECT) {
        curl_easy_setopt(curl, CURLOPT_PROXY, "");
        return;
    }
    curl_easy_setopt(curl, CURLOPT_PROXY, route->address);
    curl_easy_setopt(curl, CURLOPT_PROXY_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_PROXY_SSL_VERIFYHOST, 0L);
    switch (route->type) {
        case ANTIZAPRET_ROUTE_HTTPS:
            curl_easy_setopt(curl, CURLOPT_PROXYTYPE, CURLPROXY_HTTPS);
            break;
        case ANTIZAPRET_ROUTE_SOCKS4:
            curl_easy_setopt(curl, CURLOPT_PROXYTYPE, CURLPROXY_SOCKS4A);
            break;
        case ANTIZAPRET_ROUTE_SOCKS5:
            curl_easy_setopt(curl, CURLOPT_PROXYTYPE, CURLPROXY_SOCKS5_HOSTNAME);
            break;
        case ANTIZAPRET_ROUTE_HTTP:
        default:
            curl_easy_setopt(curl, CURLOPT_PROXYTYPE, CURLPROXY_HTTP);
            break;
    }
}

const char *antizapret_route_name(const antizapret_route_t *route) {
    if (!route || route->type == ANTIZAPRET_ROUTE_DIRECT)
        return "direct";
    switch (route->type) {
        case ANTIZAPRET_ROUTE_HTTP: return "HTTP proxy";
        case ANTIZAPRET_ROUTE_HTTPS: return "HTTPS proxy";
        case ANTIZAPRET_ROUTE_SOCKS4: return "SOCKS4 proxy";
        case ANTIZAPRET_ROUTE_SOCKS5: return "SOCKS5 proxy";
        default: return "unknown proxy";
    }
}

int antizapret_route_supported(const antizapret_route_t *route) {
    if (!route)
        return 1;
#ifdef __SWITCH__
    /* devkitPro's current switch-curl lacks HTTPS-proxy support. */
    return route->type != ANTIZAPRET_ROUTE_HTTPS;
#else
    return 1;
#endif
}

static int contains_nocase(const char *body, size_t len, const char *needle) {
    size_t needle_len = strlen(needle);
    if (!needle_len || needle_len > len)
        return 0;
    for (size_t i = 0; i + needle_len <= len; ++i) {
        if (ascii_equal_nocase(body + i, needle, needle_len))
            return 1;
    }
    return 0;
}

int antizapret_response_looks_blocked(const char *body, size_t body_len) {
    if (!body || !body_len)
        return 0;
    return contains_nocase(body, body_len, "access to the requested resource") ||
           contains_nocase(body, body_len, "запрашиваемому ресурсу") ||
           contains_nocase(body, body_len, "роскомнадзор") ||
           contains_nocase(body, body_len, "resource is blocked");
}
