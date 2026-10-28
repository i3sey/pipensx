/*
 * Just enough of libnx to build source/forwarder.c on this machine, so that the
 * three NCAs it makes can be taken apart and checked before one is written to a
 * console. Nothing here talks to a console: the content store is a directory.
 */
#ifndef WINE_NX_TEST_SWITCH_SHIM_H
#define WINE_NX_TEST_SWITCH_SHIM_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <stdbool.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;
typedef uint32_t Result;

#define NX_PACKED __attribute__((packed))
#define BIT(n) (1u << (n))
#define SHA256_HASH_SIZE 0x20
#define R_FAILED(rc) ((rc) != 0)
#define R_SUCCEEDED(rc) ((rc) == 0)
#define MAKERESULT(module, desc) ((Result)((module) | ((desc) << 9)))
#define Module_Libnx 345
#define LibnxError_OutOfMemory 1
#define LibnxError_BadInput 2

typedef struct { u8 c[0x10]; } FsRightsId;
/* libnx's own NACP, copied so every field is where the console reads it. */
typedef struct {
    char name[0x200];
    char author[0x100];
} NacpLanguageEntry;

/// ApplicationNeighborDetectionGroupConfiguration
typedef struct {
    u64 group_id;                                                                              ///< GroupId
    u8 key[0x10];
} NacpApplicationNeighborDetectionGroupConfiguration;

/// NeighborDetectionClientConfiguration
typedef struct {
    NacpApplicationNeighborDetectionGroupConfiguration send_group_configuration;               ///< SendGroupConfiguration
    NacpApplicationNeighborDetectionGroupConfiguration receivable_group_configurations[0x10];  ///< ReceivableGroupConfigurations
} NacpNeighborDetectionClientConfiguration;

/// ApplicationJitConfiguration
typedef struct {
    u64 flags;                                                                           ///< Flags
    u64 memory_size;                                                                     ///< MemorySize
} NacpApplicationJitConfiguration;

/// ApplicationControlDataCondition
typedef struct {
    u8 type[8];                                                                          ///< Type
    struct {
        u8 priority;                                                                     ///< Priority
        u8 reserved_x1[0x7];                                                             ///< Reserved
        u16 aoc_index;                                                                   ///< AocIndex
        u8 reserved_xa[0x6];                                                             ///< Reserved
    } data[8];
    u8 count;                                                                            ///< Count
} NacpApplicationControlDataCondition;

typedef union {
    NacpLanguageEntry lang[16];                                                          ///< Use only if TitlesDataFormat == 0, \ref NacpLanguageEntry
    struct {
        u16 buffer_size;
        u8 buffer[0x2FFE];                                                               ///< Compressed using DEFLATE with wbits = -15
    } compressed_data;                                                                   ///< Use only if TitlesDataFormat == 1, uncompressed data matches NacpLanguageEntry[32]
} NacpLanguageEntryData;

/// ns ApplicationControlProperty
typedef struct {
    NacpLanguageEntryData lang_data;                                                     ///< \ref NacpLanguageEntryData
    u8 isbn[0x25];                                                                       ///< Isbn
    u8 startup_user_account;                                                             ///< StartupUserAccount
    u8 user_account_switch_lock;                                                         ///< UserAccountSwitchLock
    u8 add_on_content_registration_type;                                                 ///< AddOnContentRegistrationType
    u32 attribute_flag;                                                                  ///< AttributeFlag
    u32 supported_language_flag;                                                         ///< SupportedLanguageFlag
    u32 parental_control_flag;                                                           ///< ParentalControlFlag
    u8 screenshot;                                                                       ///< Screenshot
    u8 video_capture;                                                                    ///< VideoCapture
    u8 data_loss_confirmation;                                                           ///< DataLossConfirmation
    u8 play_log_policy;                                                                  ///< PlayLogPolicy
    u64 presence_group_id;                                                               ///< PresenceGroupId
    s8 rating_age[0x20];                                                                 ///< RatingAge
    char display_version[0x10];                                                          ///< DisplayVersion
    u64 add_on_content_base_id;                                                          ///< AddOnContentBaseId
    u64 save_data_owner_id;                                                              ///< SaveDataOwnerId
    u64 user_account_save_data_size;                                                     ///< UserAccountSaveDataSize
    u64 user_account_save_data_journal_size;                                             ///< UserAccountSaveDataJournalSize
    u64 device_save_data_size;                                                           ///< DeviceSaveDataSize
    u64 device_save_data_journal_size;                                                   ///< DeviceSaveDataJournalSize
    u64 bcat_delivery_cache_storage_size;                                                ///< BcatDeliveryCacheStorageSize
    u64 application_error_code_category;                                                 ///< ApplicationErrorCodeCategory
    u64 local_communication_id[0x8];                                                     ///< LocalCommunicationId
    u8 logo_type;                                                                        ///< LogoType
    u8 logo_handling;                                                                    ///< LogoHandling
    u8 runtime_add_on_content_install;                                                   ///< RuntimeAddOnContentInstall
    u8 runtime_parameter_delivery;                                                       ///< RuntimeParameterDelivery
    u8 appropriate_age_for_china;                                                        ///< AppropriateAgeForChina
    u8 reserved_x30f5;                                                                   ///< Reserved
    u8 crash_report;                                                                     ///< CrashReport
    u8 hdcp;                                                                             ///< Hdcp
    u64 pseudo_device_id_seed;                                                           ///< SeedForPseudoDeviceId
    char bcat_passphrase[0x41];                                                          ///< BcatPassphrase
    u8 startup_user_account_option;                                                      ///< StartupUserAccountOption
    u8 reserved_for_user_account_save_data_operation[0x6];                               ///< ReservedForUserAccountSaveDataOperation
    u64 user_account_save_data_size_max;                                                 ///< UserAccountSaveDataSizeMax
    u64 user_account_save_data_journal_size_max;                                         ///< UserAccountSaveDataJournalSizeMax
    u64 device_save_data_size_max;                                                       ///< DeviceSaveDataSizeMax
    u64 device_save_data_journal_size_max;                                               ///< DeviceSaveDataJournalSizeMax
    u64 temporary_storage_size;                                                          ///< TemporaryStorageSize
    u64 cache_storage_size;                                                              ///< CacheStorageSize
    u64 cache_storage_journal_size;                                                      ///< CacheStorageJournalSize
    u64 cache_storage_data_and_journal_size_max;                                         ///< CacheStorageDataAndJournalSizeMax
    u16 cache_storage_index_max;                                                         ///< CacheStorageIndexMax
    u8 reserved_x318a;                                                                   ///< Reserved
    u8 runtime_upgrade;                                                                  ///< RuntimeUpgrade
    u32 supporting_limited_applications_licenses;                                        ///< SupportingLimitedApplicationLicenses
    u64 play_log_queryable_application_id[0x10];                                         ///< PlayLogQueryableApplicationId
    u8 play_log_query_capability;                                                        ///< PlayLogQueryCapability
    u8 repair_flag;                                                                      ///< RepairFlag
    u8 program_index;                                                                    ///< ProgramIndex
    u8 required_network_service_license_on_launch;                                       ///< RequiredNetworkServiceLicenseOnLaunchFlag
    u8 application_error_code_prefix;                                                    ///< [20.0.0+] ApplicationErrorCodePrefix
    u8 titles_data_format;                                                               ///< [21.0.0+] TitlesDataFormat
    u8 acd_index;                                                                        ///< [20.0.0+] AcdIndex
    u8 apparent_platform;                                                                ///< [20.0.0+] ApparentPlatform
    NacpNeighborDetectionClientConfiguration neighbor_detection_client_configuration;    ///< NeighborDetectionClientConfiguration
    NacpApplicationJitConfiguration jit_configuration;                                   ///< JitConfiguration
    u16 required_addon_contents_set_binary_descriptor[0x20];                             ///< RequiredAddOnContentsSetBinaryDescriptor
    u8 play_report_permission;                                                           ///< PlayReportPermission
    u8 crash_screenshot_for_prod;                                                        ///< CrashScreenshotForProd
    u8 crash_screenshot_for_dev;                                                         ///< CrashScreenshotForDev
    u8 contents_availability_transition_policy;                                          ///< ContentsAvailabilityTransitionPolicy
    u8 supported_language_flag_for_nx_addon;                                             ///< [21.0.0+] SupportedLanguageFlagForNxAddon
    u64 accessible_launch_required_version[0x8];                                         ///< AccessibleLaunchRequiredVersion
    NacpApplicationControlDataCondition application_control_data_condition;              ///< [20.0.0+] ApplicationControlDataCondition
    u8 initial_program_index;                                                            ///< [20.0.0+] InitialProgramIndex
    u8 reserved_x34d2;                                                                   ///< Reserved
    u32 accessible_program_index_flags;                                                  ///< [20.0.0+] AccessibleProgramIndexFlags
    u8 album_file_export;                                                                ///< [20.0.0+] AlbumFileExport
    u8 reserved_x34d9[0x7];                                                              ///< Reserved
    u8 save_data_certificate_bytes[0x80];                                                ///< [20.0.0+] SaveDataCertificateBytes
    u8 has_in_game_voice_char;                                                           ///< [20.0.0+] HasInGameVoiceChat
    u8 reserved_x3561[0x3];                                                              ///< Reserved
    u32 supported_extra_addon_content_flag;                                              ///< [20.0.0+] SupportedExtraAddOnContentFlag
    u8 has_karaoke_feature;                                                              ///< [21.0.0+] HasKaraokeFeature
    u8 reserved_x3569[0x697];                                                            ///< Reserved
    u8 platform_specific_region[0x400];                                                  ///< [20.0.0+] PlatformSpecificRegion
} NacpStruct;

typedef struct { u64 headerSize, dirHashTableOff, dirHashTableSize, dirTableOff, dirTableSize,
                     fileHashTableOff, fileHashTableSize, fileTableOff, fileTableSize, fileDataOff; } romfs_header;
typedef struct { u32 parent, sibling, childDir, childFile, nextHash, nameLen; u8 name[]; } romfs_dir;
typedef struct { u32 parent, sibling; u64 dataOff, dataSize; u32 nextHash, nameLen; u8 name[]; } romfs_file;

typedef enum { NcmContentType_Meta = 0, NcmContentType_Program = 1, NcmContentType_Control = 3 } NcmContentType;
typedef enum { NcmContentMetaType_Application = 0x80 } NcmContentMetaType;
typedef enum { NcmContentInstallType_Full = 0 } NcmContentInstallType;
typedef enum { NcmStorageId_BuiltInUser = 3, NcmStorageId_SdCard = 5 } NcmStorageId;

typedef struct { u8 c[0x10]; } NcmContentId;
typedef struct { u8 c[0x10]; } NcmPlaceHolderId;
typedef struct { NcmContentId content_id; u32 size_low; u8 size_high; u8 attr; u8 content_type; u8 id_offset; }
    NcmContentInfo;
typedef struct { u8 hash[0x20]; NcmContentInfo info; } NcmPackagedContentInfo;
typedef struct { u16 extended_header_size; u16 content_count; u16 content_meta_count;
                 u8 attributes; u8 storage_id; } NcmContentMetaHeader;
typedef struct { u64 patch_id; u32 required_system_version; u32 required_application_version; }
    NcmApplicationMetaExtendedHeader;
typedef struct { u64 id; u32 version; u8 type; u8 install_type; u8 padding[2]; } NcmContentMetaKey;
typedef struct { int handle; } NcmContentStorage;
typedef struct { int handle; } NcmContentMetaDatabase;
typedef struct { int handle; } Service;
typedef int SplConfigItem;
#define MAKEHOSVERSION(major, minor, micro) (((u32)(major) << 16) | ((u32)(minor) << 8) | (u32)(micro))
Result splInitialize( void );
void splExit( void );
Result splGetConfig( SplConfigItem item, u64 *out );

static inline void ncmU64ToContentInfoSize( u64 size, NcmContentInfo *info )
{
    info->size_low = size & 0xFFFFFFFF;
    info->size_high = (size >> 32) & 0xFF;
}

Result splCryptoInitialize(void);
void splCryptoExit(void);
Result splCryptoGenerateAesKek(const void *, u32, u32, void *);
Result splCryptoGenerateAesKey(const void *, const void *, void *);
void sha256CalculateHash(void *, const void *, size_t);
typedef struct { u8 key[0x20]; u64 sector; } Aes128XtsContext;
void aes128XtsContextCreate( Aes128XtsContext *ctx, const void *key0, const void *key1, int encrypt );
void aes128XtsContextResetSector( Aes128XtsContext *ctx, u64 sector, int nintendo );
size_t aes128XtsEncrypt( Aes128XtsContext *ctx, void *dst, const void *src, size_t size );


#endif
