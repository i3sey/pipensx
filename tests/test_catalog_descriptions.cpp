#include "app/catalog_descriptions.hpp"
extern "C" {
#include "core/sha1.h"
}
#include <cassert>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <unistd.h>
#include <zstd.h>

void u32(std::string& bytes, uint32_t value) {
    for (unsigned n = 0; n < 4; ++n) bytes.push_back(static_cast<char>(value >> (n * 8)));
}
std::string pack(const std::string& source, const std::string& text) {
    std::string compressed(ZSTD_compressBound(text.size()), '\0');
    const size_t size = ZSTD_compress(compressed.data(), compressed.size(), text.data(), text.size(), 3);
    assert(!ZSTD_isError(size)); compressed.resize(size);
    std::string bytes("PNXDESC1", 8);
    u32(bytes, 1); u32(bytes, 1);
    uint8_t hash[20]; sha1(source.data(), source.size(), hash);
    bytes.append(reinterpret_cast<char*>(hash), 20);
    u32(bytes, 56); u32(bytes, size); u32(bytes, text.size());
    u32(bytes, 0); u32(bytes, text.size());
    return bytes + compressed;
}
void write(const std::string& path, const std::string& data) {
    std::ofstream out(path, std::ios::binary); out.write(data.data(), data.size()); assert(out.good());
}
int main() {
    char root[] = "/tmp/pipensx-descriptions-XXXXXX"; assert(mkdtemp(root));
    const std::string source = "Описание игры. Полный текст.";
    const std::pair<const char*, const char*> languages[] = {
        {"en-US", "Game description."}, {"pt-BR", "Descrição do jogo."},
        {"fr", "Description du jeu."}, {"es", "Descripción del juego."},
        {"it", "Descrizione del gioco."}, {"zh-CN", "游戏描述。"},
        {"ko", "게임 설명."}, {"ru", "Описание игры."}};
    for (const auto& item : languages) write(std::string(root) + "/" + item.first + ".pdx", pack(source, item.second));
    pipensx::OfflineCatalogDescriptions reader(root);
    std::string text;
    for (int pass = 0; pass < 2; ++pass)
        for (const auto& item : languages) {
            assert(reader.lookup(source, item.first, text));
            assert(text == item.second);
            assert(reader.contains(source, item.first));
            assert(!reader.contains(source + " Changed.", item.first));
            assert(!reader.lookup(source + " Changed.", item.first, text));
            assert(text.empty()); // Changed catalog text cannot use a stale translation.
        }
    assert(reader.lookup(source, "pt-PT", text) && text == "Descrição do jogo.");
    assert(!reader.lookup(source, "../../pt-BR", text));
    assert(!reader.lookup(source, "ja", text));
    std::string corrupt = pack(source, "Corrupt");
    corrupt[48] = static_cast<char>(0xff); // Text offset lies outside decoded block.
    write(std::string(root) + "/pt-BR.pdx", corrupt);
    pipensx::OfflineCatalogDescriptions broken(root);
    assert(!broken.lookup(source, "pt-BR", text));
    corrupt = pack(source, "Corrupt"); corrupt.resize(58);
    write(std::string(root) + "/pt-BR.pdx", corrupt);
    pipensx::OfflineCatalogDescriptions truncated(root);
    assert(!truncated.lookup(source, "pt-BR", text));
    for (const auto& item : languages) unlink((std::string(root) + "/" + item.first + ".pdx").c_str());
    rmdir(root);
    std::puts("offline catalog description tests passed");
}
