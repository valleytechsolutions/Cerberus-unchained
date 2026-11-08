#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CERBERUS_DEVICE_IDENTITY_MAGIC 0x31494443U
#define CERBERUS_DEVICE_IDENTITY_MIN_FIRMWARE 80U
#define CERBERUS_ID_FLASH (1U << 0)
#define CERBERUS_ID_PARTITION (1U << 1)
#define CERBERUS_ID_BASE_MAC (1U << 2)
#define CERBERUS_ID_STA_MAC (1U << 3)
#define CERBERUS_ID_AP_MAC (1U << 4)
#define CERBERUS_ID_STA_CURRENT (1U << 5)
#define CERBERUS_ID_AP_CURRENT (1U << 6)

/* Fixed-size metadata, not an attestation or a claim that two images form a
 * matched release. ELF SHA-256 is the IDF app descriptor's build fingerprint;
 * it is not the flash-image SHA-256 used by the package installer. */
typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t capability_version; /* C5 reports its API; S3 reports the API it expects. */
    uint16_t chip_model;
    uint16_t chip_revision;
    uint8_t cores;
    uint8_t flags;
    uint8_t partition_subtype;
    uint8_t reserved;
    uint32_t chip_features;
    uint32_t flash_bytes;
    uint32_t partition_address;
    uint32_t partition_bytes;
    char partition_label[16];
    char project[32];
    char version[32];
    char idf[32];
    char date[12];
    char time[12];
    uint8_t elf_sha256[32];
    uint8_t base_mac[6];
    uint8_t sta_mac[6];
    uint8_t ap_mac[6];
    uint8_t reserved_tail[2];
} cerberus_device_identity_t;

_Static_assert(sizeof(cerberus_device_identity_t) == 220U, "Device identity wire size");

bool cerberus_device_identity_valid(const cerberus_device_identity_t *identity);
/* Read-only SDK metadata. Does not initialize or change either radio. */
void cerberus_device_identity_collect(cerberus_device_identity_t *identity,
    uint32_t capability_version);
