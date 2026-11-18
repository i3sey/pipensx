/* Format regression: build without console services, then read the sections as
 * Horizon/hbloader do. Crypto stubs leave headers readable; SHA-256 is real.
 * The libnx type shim is adapted from AutoRun's tests (see vendor attribution).
 */
#include <assert.h>
#include <openssl/sha.h>
#include "autorun_switch/switch.h"
#include "../vendor/autorun-forwarder/builder.c"

static Result crypto_result;
void sha256CalculateHash(void *out, const void *data, size_t size) { SHA256(data, size, out); }
void aes128XtsContextCreate(Aes128XtsContext *ctx, const void *a, const void *b, int enc)
{ (void)a; (void)b; (void)enc; memset(ctx, 0, sizeof(*ctx)); }
void aes128XtsContextResetSector(Aes128XtsContext *ctx, u64 sector, int nintendo)
{ (void)nintendo; ctx->sector = sector; }
size_t aes128XtsEncrypt(Aes128XtsContext *ctx, void *out, const void *in, size_t size)
{ (void)ctx; memmove(out, in, size); return size; }
Result splInitialize(void) { return 0; }
void splExit(void) {}
Result splGetConfig(SplConfigItem item, u64 *out)
{ assert(item == 65000); *out = UINT64_C(1) << 56 | UINT64_C(8) << 48; return 0; }
Result splCryptoInitialize(void) { return crypto_result; }
void splCryptoExit(void) {}
Result splCryptoGenerateAesKek(const void *in, u32 gen, u32 opt, void *out)
{ (void)in; (void)gen; (void)opt; memset(out, 1, 16); return 0; }
Result splCryptoGenerateAesKey(const void *kek, const void *in, void *out)
{ (void)kek; memcpy(out, in, 16); return 0; }

static const u8 *pfs_file(const u8 *pfs, const char *name, size_t *size)
{
    const struct pfs0_header *h = (const void *)pfs;
    assert(h->magic == PFS0_MAGIC);
    const struct pfs0_entry *entries = (const void *)(h + 1);
    const char *strings = (const void *)(entries + h->total_files);
    const u8 *data = (const void *)(strings + h->string_table_size);
    for (u32 i = 0; i < h->total_files; ++i)
        if (!strcmp(strings + entries[i].name_offset, name)) {
            *size = entries[i].data_size;
            return data + entries[i].data_offset;
        }
    assert(!"missing PFS0 member");
    return NULL;
}

static const u8 *section_pfs(const u8 *nca, int section)
{
    const struct nca_header *h = (const void *)nca;
    assert(h->fs_header[section].fs_type == NCA_FS_PFS0);
    return nca + h->fs_table[section].media_start_offset * 0x200ull +
        h->fs_header[section].hash_data.hierarchical_sha256_data.pfs0_layer.offset;
}

static const u8 *romfs_file_data(const u8 *nca, int section, const char *filename, size_t *size)
{
    const struct nca_header *h = (const void *)nca;
    assert(h->fs_header[section].fs_type == NCA_FS_ROMFS);
    const u8 *base = nca + h->fs_table[section].media_start_offset * 0x200ull +
        h->fs_header[section].hash_data.integrity_meta_info.info_level_hash.levels[5].logical_offset;
    const romfs_header *romfs = (const void *)base;
    const romfs_dir *root = (const void *)(base + romfs->dirTableOff);
    const size_t want_len = strlen(filename);
    for (u32 off = root->childFile; off != UINT32_MAX;) {
        const u8 *at = base + romfs->fileTableOff + off;
        romfs_file file;
        memcpy(&file, at, sizeof(file));
        const char *name = (const char *)(at + sizeof(file));
        if (file.nameLen == want_len && !memcmp(name, filename, want_len)) {
            *size = (size_t)file.dataSize;
            return base + romfs->fileDataOff + file.dataOff;
        }
        off = file.sibling;
    }
    assert(!"missing RomFS member");
    return NULL;
}

static void check_launch(const u8 *program, const char *nro, const char *args)
{
    size_t size = 0;
    const u8 *got_nro = romfs_file_data(program, 1, "nextNroPath", &size);
    assert(size == strlen(nro) && !memcmp(got_nro, nro, size));
    const u8 *got_args = romfs_file_data(program, 1, "nextArgv", &size);
    assert(size == strlen(args) && !memcmp(got_args, args, size));
}

int main(void)
{
    /* Minimal trusted NPDM fixture: one kernel and one debug capability in
     * each access control block. The builder must patch both title IDs. */
    u8 npdm[0x400] = {0};
    struct npdm_meta *m = (void *)npdm;
    m->aci0_offset = 0x80; m->acid_offset = 0x100;
    struct npdm_aci0 *aci = (void *)(npdm + 0x80);
    struct npdm_acid *acid = (void *)(npdm + 0x100);
    aci->kac_offset = 0x40; aci->kac_size = 8;
    acid->kac_offset = 0x240; acid->kac_size = 8;
    u32 caps[2] = { 7, 0xffff };
    memcpy(npdm + 0xc0, caps, sizeof(caps));
    memcpy(npdm + 0x340, caps, sizeof(caps));
    const char main[] = "NSO0 loader fixture", icon[] = "JPEG fixture";
    const char *nro = "sdmc:/switch/wine/wine-nx-runtime.nro";
    const char *args = "sdmc:/switch/wine/wine-nx-runtime.nro \"sdmc:/switch/wine/drive_c/Games/FTL/FTLGame.exe\"";
    const u64 tid = UINT64_C(0x0100BEEFAA6E0000);
    void *package = NULL;
    size_t size = 0;
    assert(!pipensxBuildForwarder(tid, "FTL", nro, args, main, sizeof(main),
        npdm, sizeof(npdm), icon, sizeof(icon), &package, &size));
    const struct pfs0_header *pfs = package;
    assert(pfs->total_files == 3);
    const struct pfs0_entry *entries = (const void *)(pfs + 1);
    const char *names = (const void *)(entries + 3);
    const u8 *data = (const void *)(names + pfs->string_table_size);
    u8 hashes[2][32];
    for (int i = 0; i < 3; ++i) {
        assert(entries[i].data_offset + entries[i].data_size <= size - (data - (u8 *)package));
        const u8 *nca = data + entries[i].data_offset;
        const struct nca_header *h = (const void *)nca;
        /* Independent offsets catch the old generator's bogus section table,
         * content types, unaligned size, and FsHeader written at 0x240. */
        assert(!memcmp(nca + 0x200, "NCA3", 4));
        assert(nca[0x205] == (i == 0 ? 0 : i == 1 ? 2 : 1));
        assert(h->program_id == tid && h->size == entries[i].data_size);
        assert(!(h->size % 0x200));
        for (int j = 0; j < (i == 0 ? 2 : 1); ++j) {
            u8 digest[32];
            SHA256(nca + 0x400 + j * 0x200, 0x200, digest);
            assert(!memcmp(digest, nca + 0x280 + j * 32, 32));
        }
        if (i < 2) SHA256(nca, h->size, hashes[i]);
        if (i == 0) {
            size_t member_size;
            const u8 *exefs = section_pfs(nca, 0);
            const u8 *loader = pfs_file(exefs, "main", &member_size);
            assert(member_size == sizeof(main) && !memcmp(loader, main, sizeof(main)));
            const u8 *patched = pfs_file(exefs, "main.npdm", &member_size);
            assert(member_size == sizeof(npdm));
            assert(((patched[0xc] >> 1) & 7) == 3); /* 39-bit address space */
            u64 id;
            memcpy(&id, patched + 0x80 + 0x10, 8); assert(id == tid);
            check_launch(nca, nro, args);
        } else if (i == 2) {
            size_t member_size;
            const u8 *cnmt = pfs_file(section_pfs(nca, 0), "Application_0100BEEFAA6E0000.cnmt", &member_size);
            assert(cnmt[12] == 0x80 && cnmt[16] == 2 && cnmt[18] == 0);
            assert(member_size == 0x20 + 0x10 + 2 * 56 + 32);
            assert(!memcmp(cnmt + 0x30, hashes[0], 32));
            assert(!memcmp(cnmt + 0x30 + 56, hashes[1], 32));
        }
    }
    free(package);
    crypto_result = 0;
    const char *arch_nro = "sdmc:/switch/pipensx/pipensx.nro";
    const char *arch_args = "--restore-archive 0100BEEFAA6E0000";
    assert(!pipensxBuildArchiveForwarder(tid, "FTL", "Subset Games", "1.6.14",
        arch_nro, arch_args, main, sizeof(main), npdm, sizeof(npdm), icon, sizeof(icon),
        &package, &size));
    const struct pfs0_header *apfs = package;
    assert(apfs->total_files == 3);
    const struct pfs0_entry *aentries = (const void *)(apfs + 1);
    const char *anames = (const void *)(aentries + 3);
    const u8 *adata = (const void *)(anames + apfs->string_table_size);
    check_launch(adata + aentries[0].data_offset, arch_nro, arch_args);
    size_t nacp_size = 0;
    const u8 *nacp_bytes = romfs_file_data(adata + aentries[1].data_offset, 0, "control.nacp", &nacp_size);
    assert(nacp_size == sizeof(NacpStruct));
    NacpStruct arch_nacp;
    memcpy(&arch_nacp, nacp_bytes, sizeof(arch_nacp));
    const NacpLanguageEntry *arch_titles = (const NacpLanguageEntry *)&arch_nacp;
    assert(strcmp(arch_titles[0].name, "FTL") == 0);
    assert(strcmp(arch_titles[0].author, "Subset Games") == 0);
    assert(strcmp(arch_nacp.display_version, "1.6.14") == 0);
    assert(!memcmp(&arch_nacp.application_error_code_category, "pnxarch", 7));
    assert(arch_nacp.user_account_save_data_size == 0);
    assert(arch_nacp.device_save_data_size == 0);
    size_t icon_len = 0;
    const u8 *got_icon = romfs_file_data(adata + aentries[1].data_offset, 0, "icon_AmericanEnglish.dat", &icon_len);
    assert(icon_len == sizeof(icon) && !memcmp(got_icon, icon, sizeof(icon)));
    free(package);
    assert(pipensxBuildArchiveForwarder(tid, "FTL", "Subset Games", "1.6.14",
        arch_nro, arch_args, main, sizeof(main), npdm, sizeof(npdm), NULL, 0,
        &package, &size) != 0);
    assert(!package && !size);

    crypto_result = 0x1234;
    assert(pipensxBuildForwarder(tid, "FTL", nro, args, main, sizeof(main),
        npdm, sizeof(npdm), icon, sizeof(icon), &package, &size) == 0x1234);
    assert(!package && !size);
    puts("test_autorun_package: all assertions passed");
}
