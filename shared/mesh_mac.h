/* Original Cerberus parser. References and limitations: docs/MESH_MAC_PARSER.md. */
#ifndef CERBERUS_MESH_MAC_H
#define CERBERUS_MESH_MAC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MESH_MAC_MAX_LENGTH 125U

typedef enum {
    MESH_MAC_HEADER_OK = 0,
    MESH_MAC_MALFORMED,
    MESH_MAC_UNSUPPORTED,
    MESH_MAC_INVALID_ARGUMENT
} mesh_mac_result_t;

typedef enum {
    MESH_MAC_ADDRESS_NONE = 0,
    MESH_MAC_ADDRESS_SHORT16 = 2,
    MESH_MAC_ADDRESS_EXT64 = 3
} mesh_mac_address_mode_t;

typedef enum {
    MESH_MAC_PAN_UNKNOWN = 0,
    MESH_MAC_PAN_EXPLICIT,
    MESH_MAC_PAN_COMPRESSED_SAME
} mesh_mac_pan_origin_t;

typedef struct {
    /* Numeric value of the complete little-endian wire address. NONE is zero. */
    uint64_t value;
    uint8_t mode;
    bool broadcast;       /* Short 0xffff only: not an independently heard node. */
    bool short_sentinel;  /* Short 0xfffe only: not a usable assigned short ID. */
} mesh_mac_address_t;

typedef struct {
    uint16_t value; /* UNKNOWN is canonical zero; explicit PAN 0 is distinct. */
    uint8_t origin;
} mesh_mac_pan_t;

typedef struct {
    mesh_mac_address_t source;
    mesh_mac_address_t destination;
    mesh_mac_pan_t source_pan;
    mesh_mac_pan_t destination_pan;
    uint16_t frame_control;
    uint16_t header_length;
    uint16_t payload_length; /* Opaque span, excluding MIC and normalized FCS. */
    uint32_t frame_counter;
    uint8_t key_identifier[9];
    uint8_t key_identifier_length;
    uint8_t frame_type;
    uint8_t version;
    uint8_t sequence;
    uint8_t security_level;
    uint8_t key_id_mode;
    uint8_t mic_length;
    uint8_t legacy_command_id;
    bool sequence_present;
    bool pan_compression;
    bool ack_requested;
    bool frame_pending;
    bool security_enabled;
    bool encrypted;
    bool frame_counter_present;
    bool asn_in_nonce;
    bool header_ies_present;
    bool payload_ies_indicated;
    bool legacy_command_id_present;
} mesh_mac_observation_t;

/*
 * Parse a normalized MAC span: no SDK length byte, no FCS/replacement RSSI/LQI.
 * HEADER_OK validates the supported clear header and MIC extent ONLY. Payload
 * bytes, payload IEs, security authentication and application protocol are not
 * validated. Header values are observations, not proof of device identity/role.
 *
 * The output is a copied value and remains byte-for-byte unchanged on failure.
 * Input and output must not overlap. No allocation, clock, radio or global state.
 */
mesh_mac_result_t mesh_mac_parse(const uint8_t *bytes, size_t length,
                                 mesh_mac_observation_t *out);

#ifdef __cplusplus
}
#endif
#endif
