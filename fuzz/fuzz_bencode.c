/* libFuzzer entry for the bencode parser — hostile peer / .torrent bytes. */
#include "../src/core/bencode.h"

#include <stddef.h>
#include <stdint.h>

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (!data || size == 0)
        return 0;
    const char* p = (const char*)data;
    const char* end = p + size;
    be_node_t node;
    (void)be_decode(&p, end, &node);
    if (node.type == BE_DICT) {
        be_node_t val;
        (void)be_dict_get(node.buf, node.buf + node.raw_len, "info", 4, &val);
        const char* dp = node.buf + 1;
        const char* de = node.buf + node.raw_len - 1;
        const char* key = NULL;
        size_t klen = 0;
        be_node_t item;
        while (be_dict_next(&dp, de, &key, &klen, &item)) {
        }
    } else if (node.type == BE_LIST) {
        const char* lp = node.buf + 1;
        const char* le = node.buf + node.raw_len - 1;
        be_node_t item;
        while (be_list_next(&lp, le, &item)) {
        }
    }
    return 0;
}
