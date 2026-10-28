/*
 * Building a forwarder and installing it, ported from sphaira's src/owo.cpp
 * (ISC, TotalJustice), which in turn takes its NCA building from hacbrewpack
 * and its installing from yati.
 *
 * Three NCAs make an application: the program, which carries the homebrew
 * loader's exefs and, in its romfs, the NRO to start and the arguments to start
 * it with; the control, whose romfs holds the NACP and the icon the home menu
 * shows; and the meta, which says the other two belong together. The console
 * writes them through ncm and is told about them through ns.
 *
 * Nothing here is signed. The NCA header is encrypted with the console's own
 * header key, which spl derives on the spot, and Atmosphere does not ask an
 * installed application for a signature.
 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include <switch.h>

#include "builder.h"

/* The key the NCA header is encrypted with is derived from these, the same two
 * constants every tool that builds an NCA uses. */
static const u8 HEADER_KEK_SRC[0x10] = {
    0x1F, 0x12, 0x91, 0x3A, 0x4A, 0xCB, 0xF0, 0x0D, 0x4C, 0xDE, 0x3A, 0xF6, 0xD5, 0x23, 0x88, 0x2A
};
static const u8 HEADER_KEY_SRC[0x20] = {
    0x5A, 0x3E, 0xD8, 0x4F, 0xDE, 0xC0, 0xD8, 0x26, 0x31, 0xF7, 0xE2, 0x5D, 0x19, 0x7B, 0xF5, 0xD0,
    0x1C, 0x9B, 0x7B, 0xFA, 0xF6, 0x28, 0x18, 0x3D, 0x71, 0xF6, 0x4D, 0x73, 0xF1, 0x50, 0xB9, 0xD2
};

#define NCA3_MAGIC               0x3341434E
#define PFS0_MAGIC               0x30534650
#define IVFC_MAGIC               0x43465649
#define IVFC_LEVELS              6
#define IVFC_HASH_BLOCK_SIZE     0x4000
#define PFS0_EXEFS_HASH_BLOCK    0x10000
#define PFS0_META_HASH_BLOCK     0x1000
#define PFS0_PADDING_SIZE        0x200
#define ROMFS_ENTRY_EMPTY        0xFFFFFFFFu
#define ROMFS_FILEPARTITION_OFS  0x200

enum nca_content_type { NCA_CONTENT_PROGRAM = 0, NCA_CONTENT_META = 1, NCA_CONTENT_CONTROL = 2 };
enum nca_fs_type { NCA_FS_ROMFS = 0, NCA_FS_PFS0 = 1 };
enum nca_hash_type { NCA_HASH_SHA256 = 2, NCA_HASH_INTEGRITY = 3 };
enum nca_encryption { NCA_ENCRYPTION_NONE = 1 };

struct layer_region { u64 offset, size; };

struct sha256_data
{
    u8 master_hash[0x20];
    u32 block_size;
    u32 layer_count;
    struct layer_region hash_layer;
    struct layer_region pfs0_layer;
    struct layer_region unused_layers[3];
    u8 _0x78[0x80];
};

struct NX_PACKED integrity_level
{
    u64 logical_offset;
    u64 hash_data_size;
    u32 block_size;   /* log2 */
    u32 _0x14;
};

struct info_level_hash
{
    u32 max_layers;
    struct integrity_level levels[IVFC_LEVELS];
    u8 signature_salt[0x20];
};

struct integrity_meta_info
{
    u32 magic;
    u32 version;
    u32 master_hash_size;
    struct info_level_hash info_level_hash;
    u8 master_hash[0x20];
    u8 _0xE0[0x18];
};

struct bucket_tree_header { u32 magic, version, count; u8 _0xC[4]; };

struct patch_info
{
    u64 indirect_offset, indirect_size;
    struct bucket_tree_header indirect_header;
    u64 aes_ctr_offset, aes_ctr_size;
    struct bucket_tree_header aes_ctr_header;
};

struct compression_info
{
    u64 table_offset, table_size;
    struct bucket_tree_header table_header;
    u8 _0x20[8];
};

struct nca_fs_header
{
    u16 version;
    u8 fs_type;
    u8 hash_type;
    u8 encryption_type;
    u8 metadata_hash_type;
    u8 _0x6[2];
    union
    {
        struct sha256_data hierarchical_sha256_data;
        struct integrity_meta_info integrity_meta_info;
    } hash_data;
    struct patch_info patch_info;
    u64 section_ctr;
    u8 spares_info[0x30];
    struct compression_info compression_info;
    u8 meta_data_hash_data_info[0x30];
    u8 reserved[0x30];
};

struct nca_section { u32 media_start_offset, media_end_offset; u8 _0x8[4], _0xC[4]; };

struct nca_header
{
    u8 rsa_fixed_key[0x100];
    u8 rsa_npdm[0x100];
    u32 magic;
    u8 distribution_type;
    u8 content_type;
    u8 old_key_gen;
    u8 kaek_index;
    u64 size;
    u64 program_id;
    u32 context_id;
    u32 sdk_version;
    u8 key_gen;
    u8 sig_key_gen;
    u8 _0x222[0xE];
    FsRightsId rights_id;
    struct nca_section fs_table[4];
    u8 fs_header_hash[4][0x20];
    u8 key_area[4][0x10];
    u8 _0x340[0xC0];
    struct nca_fs_header fs_header[4];
};

struct pfs0_header { u32 magic, total_files, string_table_size, padding; };
struct pfs0_entry { u64 data_offset, data_size; u32 name_offset, padding; };

struct npdm_meta
{
    u32 magic;
    u32 signature_key_generation;
    u32 _0x8;
    u8 flags;
    u8 _0xD;
    u8 main_thread_priority;
    u8 main_thread_core_num;
    u32 _0x10;
    u32 sys_resource_size;
    u32 version;
    u32 main_thread_stack_size;
    char title_name[0x10];
    char product_code[0x10];
    u8 _0x40[0x30];
    u32 aci0_offset, aci0_size, acid_offset, acid_size;
};

struct npdm_acid
{
    u8 rsa_sig[0x100];
    u8 rsa_pub[0x100];
    u32 magic;
    u32 size;
    u8 version;
    u8 _0x209[1];
    u8 _0x20A[2];
    u32 flags;
    u64 program_id_min, program_id_max;
    u32 fac_offset, fac_size, sac_offset, sac_size, kac_offset, kac_size;
    u8 _0x238[8];
};

struct npdm_aci0
{
    u32 magic;
    u8 _0x4[0xC];
    u64 program_id;
    u8 _0x18[8];
    u32 fac_offset, fac_size, sac_offset, sac_size, kac_offset, kac_size;
    u8 _0x38[8];
};

struct cnmt_header
{
    u64 title_id;
    u32 title_version;
    u8 meta_type;
    u8 _0xD;
    NcmContentMetaHeader meta_header;
    u8 install_type;
    u8 _0x17;
    u32 required_sys_version;
    u8 _0x1C[4];
};

/* The console reads these by offset, so a field out of place is a forwarder that
 * does not start. */
_Static_assert( sizeof(struct integrity_level) == 0x18, "integrity level" );
/* What packing it is for: the u32 in front of the array of them must not be
 * padded out to the alignment a u64 would ask for. */
_Static_assert( __alignof__(struct integrity_level) == 1, "integrity level alignment" );
_Static_assert( sizeof(struct info_level_hash) == 0xB4, "info level hash" );
_Static_assert( sizeof(struct sha256_data) == 0xF8, "hierarchical sha256 data" );
_Static_assert( sizeof(struct integrity_meta_info) == 0xF8, "integrity meta info" );
_Static_assert( sizeof(struct patch_info) == 0x40, "patch info" );
_Static_assert( sizeof(struct compression_info) == 0x28, "compression info" );
_Static_assert( sizeof(struct nca_fs_header) == 0x200, "nca fs header" );
_Static_assert( sizeof(struct nca_header) == 0xC00, "nca header" );
_Static_assert( sizeof(struct cnmt_header) == 0x20, "cnmt header" );
_Static_assert( sizeof(struct npdm_meta) == 0x80, "npdm meta" );
_Static_assert( sizeof(struct npdm_acid) == 0x240, "npdm acid" );
_Static_assert( sizeof(struct npdm_aci0) == 0x40, "npdm aci0" );
_Static_assert( sizeof(struct pfs0_header) == 0x10, "pfs0 header" );
_Static_assert( sizeof(struct pfs0_entry) == 0x18, "pfs0 entry" );

/* One file going into a PFS0 or a romfs. romfs names begin with '/'. */
struct file_entry { const char *name; const void *data; size_t size; };

/***********************************************************************
 * A buffer that grows, seeks and remembers how far it was written
 */

struct buf { u8 *data; size_t size, offset, capacity; };

static int buf_reserve( struct buf *b, size_t need )
{
    size_t capacity = b->capacity ? b->capacity : 0x10000;
    u8 *data;

    if (need <= b->capacity) return 1;
    while (capacity < need) capacity *= 2;
    if (!(data = realloc( b->data, capacity ))) return 0;
    memset( data + b->capacity, 0, capacity - b->capacity );
    b->data = data;
    b->capacity = capacity;
    return 1;
}

static int buf_write( struct buf *b, const void *data, size_t size )
{
    if (!size) return 1;
    if (!buf_reserve( b, b->offset + size )) return 0;
    memcpy( b->data + b->offset, data, size );
    b->offset += size;
    if (b->offset > b->size) b->size = b->offset;
    return 1;
}

/* Zeroes, which the buffer is already full of: only the extent moves. */
static int buf_zero( struct buf *b, size_t size )
{
    if (!size) return 1;
    if (!buf_reserve( b, b->offset + size )) return 0;
    memset( b->data + b->offset, 0, size );
    b->offset += size;
    if (b->offset > b->size) b->size = b->offset;
    return 1;
}

static void buf_free( struct buf *b ) { free( b->data ); memset( b, 0, sizeof(*b) ); }

/* Up to the next multiple of block -- and a whole block when it is already
 * there, as the tools this follows do. */
static size_t buf_pad( struct buf *b, size_t at, size_t block )
{
    size_t size = block - (at % block);

    if (!buf_zero( b, size )) return 0;
    return size;
}

static u64 align64( u64 value, u64 alignment )
{
    return (value + alignment - 1) & ~(alignment - 1);
}

static u32 align32( u32 value, u32 alignment )
{
    return (value + alignment - 1) & ~(alignment - 1);
}

/***********************************************************************
 * romfs, flat: every file sits in the root
 */

static u32 romfs_path_hash( u32 parent, const u8 *path, u32 start, u32 length )
{
    u32 hash = parent ^ 123456789, i;

    for (i = 0; i < length; i++)
    {
        hash = (hash >> 5) | (hash << 27);
        hash ^= path[start + i];
    }
    return hash;
}

static u32 romfs_hash_count( u32 entries )
{
    u32 count;

    if (entries < 3) return 3;
    if (entries < 19) return entries | 1;
    count = entries;
    while (!(count % 2) || !(count % 3) || !(count % 5) || !(count % 7) ||
           !(count % 11) || !(count % 13) || !(count % 17))
        count++;
    return count;
}

/* Builds the image and returns its size before the padding that follows it. */
static int romfs_build( const struct file_entry *entries, int count, struct buf *out, u64 *unpadded )
{
    u32 dir_hash_count = romfs_hash_count( 1 ), file_hash_count = romfs_hash_count( count );
    u32 *dir_hash = NULL, *file_hash = NULL;
    u8 *dir_table = NULL, *file_table = NULL;
    u64 *offsets = NULL;
    u32 *entry_offsets = NULL;
    size_t dir_table_size = 0x18, file_table_size = 0;
    u64 partition = 0;
    romfs_header header;
    romfs_dir *root;
    int i, ok = 0;

    for (i = 0; i < count; i++)
        file_table_size += sizeof(romfs_file) + align32( (u32)strlen( entries[i].name ) - 1, 4 );

    dir_hash = malloc( dir_hash_count * 4 );
    file_hash = malloc( file_hash_count * 4 );
    dir_table = calloc( 1, dir_table_size );
    file_table = calloc( 1, file_table_size );
    offsets = calloc( count, sizeof(*offsets) );
    entry_offsets = calloc( count, sizeof(*entry_offsets) );
    if (!dir_hash || !file_hash || !dir_table || !file_table || !offsets || !entry_offsets) goto done;
    for (i = 0; i < (int)dir_hash_count; i++) dir_hash[i] = ROMFS_ENTRY_EMPTY;
    for (i = 0; i < (int)file_hash_count; i++) file_hash[i] = ROMFS_ENTRY_EMPTY;

    /* Where each file's data and its table entry go. */
    {
        u32 at = 0;

        for (i = 0; i < count; i++)
        {
            partition = align64( partition, 0x10 );
            offsets[i] = partition;
            partition += entries[i].size;
            entry_offsets[i] = at;
            at += sizeof(romfs_file) + align32( (u32)strlen( entries[i].name ) - 1, 4 );
        }
    }

    /* An entry is four-byte aligned, not eight, so it is assembled and copied
     * rather than written through a pointer into the table. */
    for (i = 0; i < count; i++)
    {
        u32 name_size = (u32)strlen( entries[i].name ) - 1;
        u32 hash = romfs_path_hash( 0, (const u8 *)entries[i].name, 1, name_size );
        romfs_file entry;

        memset( &entry, 0, sizeof(entry) );
        entry.parent = 0;
        entry.sibling = i + 1 < count ? entry_offsets[i + 1] : ROMFS_ENTRY_EMPTY;
        entry.dataOff = offsets[i];
        entry.dataSize = entries[i].size;
        entry.nextHash = file_hash[hash % file_hash_count];
        file_hash[hash % file_hash_count] = entry_offsets[i];
        entry.nameLen = name_size;
        memcpy( file_table + entry_offsets[i], &entry, sizeof(entry) );
        memcpy( file_table + entry_offsets[i] + sizeof(entry), entries[i].name + 1, name_size );
    }

    root = (romfs_dir *)dir_table;
    root->parent = 0;
    root->sibling = ROMFS_ENTRY_EMPTY;
    root->childDir = ROMFS_ENTRY_EMPTY;
    root->childFile = count ? entry_offsets[0] : ROMFS_ENTRY_EMPTY;
    root->nextHash = dir_hash[romfs_path_hash( 0, NULL, 0, 0 ) % dir_hash_count];
    dir_hash[romfs_path_hash( 0, NULL, 0, 0 ) % dir_hash_count] = 0;
    root->nameLen = 0;

    memset( &header, 0, sizeof(header) );
    header.headerSize = sizeof(header);
    header.fileHashTableSize = file_hash_count * 4;
    header.fileTableSize = file_table_size;
    header.dirHashTableSize = dir_hash_count * 4;
    header.dirTableSize = dir_table_size;
    header.fileDataOff = ROMFS_FILEPARTITION_OFS;
    header.dirHashTableOff = align64( partition + ROMFS_FILEPARTITION_OFS, 4 );
    header.dirTableOff = header.dirHashTableOff + header.dirHashTableSize;
    header.fileHashTableOff = header.dirTableOff + header.dirTableSize;
    header.fileTableOff = header.fileHashTableOff + header.fileHashTableSize;

    if (!buf_write( out, &header, sizeof(header) )) goto done;
    for (i = 0; i < count; i++)
    {
        out->offset = offsets[i] + ROMFS_FILEPARTITION_OFS;
        if (!buf_write( out, entries[i].data, entries[i].size )) goto done;
    }
    out->offset = header.dirHashTableOff;
    if (!buf_write( out, dir_hash, header.dirHashTableSize ) ||
        !buf_write( out, dir_table, dir_table_size ) ||
        !buf_write( out, file_hash, header.fileHashTableSize ) ||
        !buf_write( out, file_table, file_table_size )) goto done;

    out->offset = out->size;
    *unpadded = out->offset;
    if (!buf_pad( out, out->offset, IVFC_HASH_BLOCK_SIZE )) goto done;
    ok = 1;

done:
    free( dir_hash );
    free( file_hash );
    free( dir_table );
    free( file_table );
    free( offsets );
    free( entry_offsets );
    return ok;
}

/***********************************************************************
 * PFS0 and the hash layers over both kinds of section
 */

static int pfs0_build( const struct file_entry *entries, int count, struct buf *out )
{
    struct pfs0_header header;
    struct pfs0_entry *table = calloc( count, sizeof(*table) );
    char *strings = NULL;
    size_t string_size = 0, aligned;
    u64 data_offset = 0;
    int i, ok = 0;

    if (!table) return 0;
    for (i = 0; i < count; i++) string_size += strlen( entries[i].name ) + 1;
    aligned = (string_size + 0x1F) & ~(size_t)0x1F;
    if (!(strings = calloc( 1, aligned ))) goto done;
    string_size = 0;
    for (i = 0; i < count; i++)
    {
        table[i].data_offset = data_offset;
        table[i].data_size = entries[i].size;
        table[i].name_offset = string_size;
        memcpy( strings + string_size, entries[i].name, strlen( entries[i].name ) + 1 );
        data_offset += entries[i].size;
        string_size += strlen( entries[i].name ) + 1;
    }

    memset( &header, 0, sizeof(header) );
    header.magic = PFS0_MAGIC;
    header.total_files = count;
    header.string_table_size = aligned;
    if (!buf_write( out, &header, sizeof(header) ) ||
        !buf_write( out, table, sizeof(*table) * count ) ||
        !buf_write( out, strings, aligned )) goto done;
    for (i = 0; i < count; i++)
        if (!buf_write( out, entries[i].data, entries[i].size )) goto done;
    ok = 1;

done:
    free( table );
    free( strings );
    return ok;
}

/* A hash of every block of src, in order. */
static int hash_layer( const u8 *src, size_t size, size_t block, struct buf *out )
{
    size_t read = block, i;
    u8 hash[SHA256_HASH_SIZE];

    for (i = 0; i < size; i += read)
    {
        if (i + read >= size) read = size - i;
        sha256CalculateHash( hash, src + i, read );
        if (!buf_write( out, hash, sizeof(hash) )) return 0;
    }
    return 1;
}

static int ivfc_level( const u8 *src, size_t size, struct buf *out )
{
    if (!hash_layer( src, size, IVFC_HASH_BLOCK_SIZE, out )) return 0;
    return buf_pad( out, out->offset, IVFC_HASH_BLOCK_SIZE ) != 0;
}

/***********************************************************************
 * The NCA around them
 */

static void nca_write_section( struct nca_header *header, int index, u64 start, u64 end )
{
    header->fs_table[index].media_start_offset = start / 0x200;
    header->fs_table[index].media_end_offset = end / 0x200;
    header->fs_table[index]._0x8[0] = 1;
}

static u64 nca_section_start( const struct nca_header *header, int index )
{
    return index == 0 ? sizeof(*header) : (u64)header->fs_table[index - 1].media_end_offset * 0x200;
}

static int nca_write_pfs0( struct nca_header *header, int index, const struct file_entry *entries, int count,
                           u32 block_size, struct buf *out )
{
    struct nca_fs_header *fs = &header->fs_header[index];
    struct buf pfs0 = {0}, table = {0};
    u64 start = nca_section_start( header, index );
    size_t padding;
    int ok = 0;

    if (!pfs0_build( entries, count, &pfs0 )) goto done;
    if (!hash_layer( pfs0.data, pfs0.size, block_size, &table )) goto done;
    if (!buf_write( out, table.data, table.size )) goto done;
    if (!(padding = buf_pad( out, table.size, PFS0_PADDING_SIZE ))) goto done;

    fs->hash_data.hierarchical_sha256_data.pfs0_layer.offset = table.size + padding;
    fs->hash_data.hierarchical_sha256_data.pfs0_layer.size = pfs0.size;
    if (!buf_write( out, pfs0.data, pfs0.size )) goto done;
    if (!buf_pad( out, out->offset, 0x200 )) goto done;
    nca_write_section( header, index, start, out->offset );

    fs->hash_type = NCA_HASH_SHA256;
    fs->fs_type = NCA_FS_PFS0;
    fs->version = 2;
    fs->encryption_type = NCA_ENCRYPTION_NONE;
    fs->hash_data.hierarchical_sha256_data.layer_count = 2;
    fs->hash_data.hierarchical_sha256_data.block_size = block_size;
    fs->hash_data.hierarchical_sha256_data.hash_layer.size = table.size;
    sha256CalculateHash( fs->hash_data.hierarchical_sha256_data.master_hash, table.data, table.size );
    sha256CalculateHash( header->fs_header_hash[index], fs, sizeof(*fs) );
    ok = 1;

done:
    buf_free( &pfs0 );
    buf_free( &table );
    return ok;
}

static int nca_write_romfs( struct nca_header *header, int index, const struct file_entry *entries, int count,
                            struct buf *out )
{
    struct nca_fs_header *fs = &header->fs_header[index];
    struct integrity_meta_info *meta = &fs->hash_data.integrity_meta_info;
    struct info_level_hash *levels = &meta->info_level_hash;
    struct buf ivfc[IVFC_LEVELS] = {{0}};
    u64 start = nca_section_start( header, index ), romfs_size = 0;
    int i, ok = 0;

    if (!romfs_build( entries, count, &ivfc[5], &romfs_size )) goto done;
    levels->levels[5].hash_data_size = romfs_size;
    for (i = 4; i >= 0; i--)
    {
        if (!ivfc_level( ivfc[i + 1].data, ivfc[i + 1].size, &ivfc[i] )) goto done;
        levels->levels[i].hash_data_size = ivfc[i].size;
        levels->levels[i].block_size = 0x0E;   /* 0x4000 */
    }
    levels->levels[0].logical_offset = 0;
    for (i = 1; i <= 5; i++)
        levels->levels[i].logical_offset = levels->levels[i - 1].logical_offset + levels->levels[i - 1].hash_data_size;
    for (i = 0; i < IVFC_LEVELS; i++)
        if (!buf_write( out, ivfc[i].data, ivfc[i].size )) goto done;
    if (!buf_pad( out, out->offset, 0x200 )) goto done;
    sha256CalculateHash( meta->master_hash, ivfc[0].data, ivfc[0].size );
    nca_write_section( header, index, start, out->offset );

    fs->hash_type = NCA_HASH_INTEGRITY;
    fs->fs_type = NCA_FS_ROMFS;
    fs->version = 2;
    fs->encryption_type = NCA_ENCRYPTION_NONE;
    meta->magic = IVFC_MAGIC;
    meta->version = 0x20000;
    meta->master_hash_size = SHA256_HASH_SIZE;
    levels->max_layers = 7;
    levels->levels[5].block_size = 0x0E;
    sha256CalculateHash( header->fs_header_hash[index], fs, sizeof(*fs) );
    ok = 1;

done:
    for (i = 0; i < IVFC_LEVELS; i++) buf_free( &ivfc[i] );
    return ok;
}

/* The first 0xC00 bytes, as six XTS sectors under the console's header key. */
static void nca_finish( struct nca_header *header, u64 tid, int content_type, const u8 *header_key, struct buf *out )
{
    Aes128XtsContext ctx;
    u64 pos;
    u8 sector = 0;

    header->magic = NCA3_MAGIC;
    header->distribution_type = 0;       /* system, not a game card */
    header->content_type = content_type;
    header->program_id = tid;
    header->sdk_version = 0x000C1100;
    header->size = out->size;

    aes128XtsContextCreate( &ctx, header_key, header_key + 0x10, true );
    for (pos = 0; pos < 0xC00; pos += 0x200)
    {
        aes128XtsContextResetSector( &ctx, sector++, true );
        aes128XtsEncrypt( &ctx, (u8 *)header + pos, (const u8 *)header + pos, 0x200 );
    }
    out->offset = 0;
    buf_write( out, header, sizeof(*header) );
    out->offset = out->size;
}

/***********************************************************************
 * The NPDM, which is what decides the address space
 */

/* The first capability of this kind, which npdmtool wrote for the values in
 * hbl.json, replaced with ours. */
static int npdm_patch_capability( u8 *npdm, u32 offset, u32 size, u32 bits, u32 value )
{
    const u32 pattern = BIT( bits ) - 1;
    const u32 mask = BIT( bits ) | pattern;
    u32 i;

    for (i = 0; i < size; i += 4)
    {
        u32 capability;

        memcpy( &capability, npdm + offset + i, sizeof(capability) );
        if ((capability & mask) == pattern)
        {
            capability = value | pattern;
            memcpy( npdm + offset + i, &capability, sizeof(capability) );
            return 1;
        }
    }
    return 0;
}

/* Core 3's time-sliced workers need priority 63. */
static u32 npdm_kernel_flags( void )
{
    const u32 descriptor = (((3u << 8) | 0u) << 6 | 28u) << 6 | 63u;

    return descriptor << 4;
}

/* Atmosphere 1.8.0 moved the NPDM's debug flags for HOS 19: the bit npdmtool
 * writes for hbl.json's force_debug_prod is ForceDebugProd now, and what the
 * loader needs to map an NRO's code is ForceDebug, the bit after it. sphaira
 * asks Exosphere its version and picks the new bit when it is that new; so
 * does this, or the forwarder installs and then cannot start the NRO. */
static int exosphere_moved_debug_flags( void )
{
    const SplConfigItem ExosphereApiVersion = (SplConfigItem)65000;
    u64 version = 0;
    int moved = 0;

    if (R_FAILED( splInitialize() )) return 0;
    if (R_SUCCEEDED( splGetConfig( ExosphereApiVersion, &version ) ))
        moved = (version >> 40) >= MAKEHOSVERSION( 1, 8, 0 );
    splExit();
    return moved;
}

static int npdm_patch( u8 *npdm, size_t size, u64 tid )
{
    const u8 ADDRESS_SPACE_SHIFT = 1;
    const u8 ADDRESS_SPACE_MASK = 0x7 << 1;
    struct npdm_meta meta;
    struct npdm_aci0 aci0;
    struct npdm_acid acid;
    u32 flags = npdm_kernel_flags();
    int patched;

    if (size < sizeof(meta)) return 0;
    memcpy( &meta, npdm, sizeof(meta) );
    if (meta.aci0_offset + sizeof(aci0) > size || meta.acid_offset + sizeof(acid) > size) return 0;
    memcpy( &aci0, npdm + meta.aci0_offset, sizeof(aci0) );
    memcpy( &acid, npdm + meta.acid_offset, sizeof(acid) );

    snprintf( meta.title_name, sizeof(meta.title_name), "%s", "Application" );
    memset( meta.product_code, 0, sizeof(meta.product_code) );
    meta.flags = (meta.flags & ~ADDRESS_SPACE_MASK) | (3u << ADDRESS_SPACE_SHIFT);
    meta.sys_resource_size = 16 * 1024 * 1024;
    aci0.program_id = tid;
    acid.program_id_min = tid;
    acid.program_id_max = tid;

    patched = npdm_patch_capability( npdm, meta.aci0_offset + aci0.kac_offset, aci0.kac_size, 3, flags );
    patched &= npdm_patch_capability( npdm, meta.acid_offset + acid.kac_offset, acid.kac_size, 3, flags );
    /* The debug flags capability is the one ending in sixteen ones; it is
     * replaced whole, so only ForceDebug is left set in it. */
    if (exosphere_moved_debug_flags())
    {
        npdm_patch_capability( npdm, meta.aci0_offset + aci0.kac_offset, aci0.kac_size, 16, BIT( 19 ) );
        npdm_patch_capability( npdm, meta.acid_offset + acid.kac_offset, acid.kac_size, 16, BIT( 19 ) );
    }

    memcpy( npdm, &meta, sizeof(meta) );
    memcpy( npdm + meta.aci0_offset, &aci0, sizeof(aci0) );
    memcpy( npdm + meta.acid_offset, &acid, sizeof(acid) );
    return patched;
}

static void nacp_defaults( NacpStruct *nacp )
{
    memset( nacp, 0, sizeof(*nacp) );
    /* Not rated, which is 0xFF in every region; all zeroes is "rated 0". */
    memset( nacp->rating_age, 0xFF, sizeof(nacp->rating_age) );
    snprintf( nacp->display_version, sizeof(nacp->display_version), "%s", "1.0.0" );
}

/* What makes a NACP this forwarder's, applied to the NRO's own: the fields
 * sphaira's patch_nacp sets, and no others -- the rest are nacptool's, which
 * is what the forwarders that work on a console carry. */
static void nacp_build( NacpStruct *nacp, const char *name, const char *author, u64 tid )
{
    unsigned int i;

    /* The sixteen names and authors are the first thing in a NACP, whatever
     * libnx has called the field around them from one version to the next. */
    NacpLanguageEntry *titles = (NacpLanguageEntry *)nacp;

    for (i = 0; i < 16; i++)
    {
        snprintf( titles[i].name, sizeof(titles[i].name), "%s", name );
        snprintf( titles[i].author, sizeof(titles[i].author), "%s", author );
    }
    /* The one field changed that sphaira leaves alone. The home menu takes the
     * console's own language when the NACP says it is supported, and then asks
     * for that language's icon. nacptool leaves Portuguese out, so a console
     * set to it falls back to English and finds the one icon there is; saying
     * all sixteen are supported sent it after icon_Portuguese.dat, which does
     * not exist, and the tile waited for it for ever. English alone is the
     * language there is an icon for, so every console lands on it. */
    nacp->supported_language_flag = 1u << 0;    /* AmericanEnglish */
    nacp->startup_user_account = 0;             /* no profile to pick */
    nacp->user_account_switch_lock = 0;
    nacp->add_on_content_registration_type = 1; /* on demand */
    nacp->screenshot = 0;                       /* allowed */
    nacp->video_capture = 2;                    /* automatic */
    nacp->logo_type = 2;
    nacp->logo_handling = 0;
    nacp->data_loss_confirmation = 0;
    nacp->required_network_service_license_on_launch = 0;
    nacp->application_error_code_category = 0;
    memcpy( &nacp->application_error_code_category, "autorun", 7 );
    nacp->presence_group_id = tid;
    nacp->save_data_owner_id = tid;
    nacp->pseudo_device_id_seed = tid;
    nacp->add_on_content_base_id = tid ^ 0x1000;
    for (i = 0; i < sizeof(nacp->local_communication_id) / sizeof(nacp->local_communication_id[0]); i++)
        nacp->local_communication_id[i] = tid;
    nacp->play_log_policy = 0;
    nacp->play_log_query_capability = 0;
    /* No saves, so nothing to make room for. */
    nacp->user_account_save_data_size = 0;
    nacp->user_account_save_data_journal_size = 0;
    nacp->device_save_data_size = 0;
    nacp->device_save_data_journal_size = 0;
    nacp->user_account_save_data_size_max = 0;
    nacp->user_account_save_data_journal_size_max = 0;
    nacp->device_save_data_size_max = 0;
    nacp->device_save_data_journal_size_max = 0;
}


uint32_t pipensxBuildForwarder(uint64_t tid, const char *name, const char *nro,
    const char *args, const void *main_input, size_t main_size,
    const void *npdm_input, size_t npdm_size, const void *icon, size_t icon_size,
    void **output, size_t *output_size)
{
    struct buf program = {0}, control = {0}, meta = {0}, cnmt = {0}, package = {0};
    struct file_entry exefs[2], romfs[2], cnmt_entry, entries[3];
    struct nca_header *header = NULL;
    u8 *npdm = NULL;
    NacpStruct *nacp = NULL;
    char cnmt_name[40], names[3][42];
    struct cnmt_header cnmt_header;
    NcmApplicationMetaExtendedHeader extended;
    NcmPackagedContentInfo contents[2];
    u8 digest[0x20] = {0}, hashes[3][SHA256_HASH_SIZE];
    u8 header_kek[0x20], header_key[0x20];
    const NcmContentType types[2] = { NcmContentType_Program, NcmContentType_Control };
    const NcmStorageId storage_id = NcmStorageId_SdCard;
    Result rc = 0;
    int i;
    *output = NULL;
    *output_size = 0;
    if (!main_input || !main_size || !npdm_input || !npdm_size || !icon || !icon_size)
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    if (R_FAILED(rc = splCryptoInitialize())) return rc;
    if (R_SUCCEEDED(rc = splCryptoGenerateAesKek(HEADER_KEK_SRC, 0, 0, header_kek)) &&
        R_SUCCEEDED(rc = splCryptoGenerateAesKey(header_kek, HEADER_KEY_SRC, header_key)))
        rc = splCryptoGenerateAesKey(header_kek, HEADER_KEY_SRC + 16, header_key + 16);
    splCryptoExit();
    if (R_FAILED(rc)) return rc;
    if (!(header = calloc(1, sizeof(*header))) || !(nacp = calloc(1, sizeof(*nacp))) ||
        !(npdm = malloc(npdm_size)))
    { rc = MAKERESULT(Module_Libnx, LibnxError_OutOfMemory); goto done; }
    /* The program: the loader, and what it is to start. */

    memcpy( npdm, npdm_input, npdm_size );
    if (!npdm_patch( npdm, npdm_size, tid ))
    { rc = MAKERESULT( Module_Libnx, LibnxError_BadInput ); goto done; }
    exefs[0] = (struct file_entry){ "main", main_input, main_size };
    exefs[1] = (struct file_entry){ "main.npdm", npdm, npdm_size };
    romfs[0] = (struct file_entry){ "/nextArgv", args, strlen( args ) };
    romfs[1] = (struct file_entry){ "/nextNroPath", nro, strlen( nro ) };
    if (!buf_zero( &program, sizeof(*header) ) ||
        !nca_write_pfs0( header, 0, exefs, 2, PFS0_EXEFS_HASH_BLOCK, &program ) ||
        !nca_write_romfs( header, 1, romfs, 2, &program ))
    { rc = MAKERESULT( Module_Libnx, LibnxError_OutOfMemory ); goto done; }
    nca_finish( header, tid, NCA_CONTENT_PROGRAM, header_key, &program );

    /* The control: the name and the icon the home menu shows. */

    /* A fresh NACP: no save ownership or account requirements from the game. */
    nacp_defaults( nacp );
    nacp_build( nacp, name, "AutoRun / pipensx", tid );
    romfs[0] = (struct file_entry){ "/control.nacp", nacp, sizeof(*nacp) };
    romfs[1] = (struct file_entry){ "/icon_AmericanEnglish.dat", icon, icon_size };
    memset( header, 0, sizeof(*header) );
    if (!buf_zero( &control, sizeof(*header) ) || !nca_write_romfs( header, 0, romfs, 2, &control ))
    { rc = MAKERESULT( Module_Libnx, LibnxError_OutOfMemory ); goto done; }
    nca_finish( header, tid, NCA_CONTENT_CONTROL, header_key, &control );

    sha256CalculateHash( hashes[0], program.data, program.size );
    sha256CalculateHash( hashes[1], control.data, control.size );

    /* The meta: the list saying those two are this application. */

    memset( &cnmt_header, 0, sizeof(cnmt_header) );
    memset( &extended, 0, sizeof(extended) );
    memset( contents, 0, sizeof(contents) );
    cnmt_header.title_id = tid;
    cnmt_header.title_version = 0;
    cnmt_header.meta_type = NcmContentMetaType_Application;
    cnmt_header.meta_header.extended_header_size = sizeof(extended);
    cnmt_header.meta_header.content_count = 2;
    cnmt_header.meta_header.content_meta_count = 0;
    cnmt_header.meta_header.attributes = 0;
    cnmt_header.meta_header.storage_id = storage_id;
    extended.patch_id = tid | 0x800;
    for (i = 0; i < 2; i++)
    {
        memcpy( contents[i].hash, hashes[i], sizeof(contents[i].hash) );
        memcpy( &contents[i].info.content_id, hashes[i], sizeof(contents[i].info.content_id) );
        contents[i].info.content_type = types[i];
        ncmU64ToContentInfoSize( i ? control.size : program.size, &contents[i].info );
    }
    snprintf( cnmt_name, sizeof(cnmt_name), "Application_%016llX.cnmt", (unsigned long long)tid );
    if (!buf_write( &cnmt, &cnmt_header, sizeof(cnmt_header) ) ||
        !buf_write( &cnmt, &extended, sizeof(extended) ) ||
        !buf_write( &cnmt, contents, sizeof(contents) ) ||
        !buf_write( &cnmt, digest, sizeof(digest) ))
    { rc = MAKERESULT( Module_Libnx, LibnxError_OutOfMemory ); goto done; }
    cnmt_entry = (struct file_entry){ cnmt_name, cnmt.data, cnmt.size };
    memset( header, 0, sizeof(*header) );
    if (!buf_zero( &meta, sizeof(*header) ) ||
        !nca_write_pfs0( header, 0, &cnmt_entry, 1, PFS0_META_HASH_BLOCK, &meta ))
    { rc = MAKERESULT( Module_Libnx, LibnxError_OutOfMemory ); goto done; }
    nca_finish( header, tid, NCA_CONTENT_META, header_key, &meta );
    sha256CalculateHash( hashes[2], meta.data, meta.size );


    {
        const struct buf *ncas[3] = { &program, &control, &meta };
        static const char hex[] = "0123456789abcdef";
        for (i = 0; i < 3; ++i) {
            for (int j = 0; j < 16; ++j) {
                names[i][j*2] = hex[hashes[i][j] >> 4];
                names[i][j*2+1] = hex[hashes[i][j] & 15];
            }
            strcpy(names[i] + 32, i == 2 ? ".cnmt.nca" : ".nca");
            entries[i] = (struct file_entry){ names[i], ncas[i]->data, ncas[i]->size };
        }
    }
    if (!pfs0_build(entries, 3, &package))
    { rc = MAKERESULT(Module_Libnx, LibnxError_OutOfMemory); goto done; }
    *output = package.data;
    *output_size = package.size;
    package.data = NULL;
done:
    free(header); free(nacp); free(npdm);
    buf_free(&program); buf_free(&control); buf_free(&meta); buf_free(&cnmt); buf_free(&package);
    memset(header_key, 0, sizeof(header_key));
    memset(header_kek, 0, sizeof(header_kek));
    return rc;
}
