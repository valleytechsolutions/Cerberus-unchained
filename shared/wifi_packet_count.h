#pragma once
/* Original receive-only selected-address counters. The upstream Packet Count
 * workflow is the reference, not its AP-first accounting implementation.
 * Counts describe SDK callbacks, not unique packets, association or RF loss.
 * No frame payload is retained by this model. Addresses use on-air byte order. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CERBERUS_WIFI_PACKET_COUNT_LIMIT 16U
#define CERBERUS_WIFI_PACKET_COUNT_CHANNELS 42U
#define CERBERUS_WIFI_PACKET_COUNT_DWELL_MIN_MS 50U
#define CERBERUS_WIFI_PACKET_COUNT_DWELL_MAX_MS 1000U
#define CERBERUS_WIFI_PACKET_COUNT_FRAME_MAX 65535U /* delivered length; raw queue has its own4096B cap */

enum { CERBERUS_WIFI_PACKET_COUNT_AP=1U, CERBERUS_WIFI_PACKET_COUNT_STA=2U };
enum { CERBERUS_WIFI_PACKET_COUNT_MGMT=0U, CERBERUS_WIFI_PACKET_COUNT_DATA=1U,
    CERBERUS_WIFI_PACKET_COUNT_OTHER=2U };
enum { CERBERUS_WIFI_PACKET_COUNT_DECODE_OK=0U,
    CERBERUS_WIFI_PACKET_COUNT_DECODE_MALFORMED=1U,
    CERBERUS_WIFI_PACKET_COUNT_DECODE_UNSUPPORTED=2U };
enum { CERBERUS_WIFI_PACKET_COUNT_ROLE_RA=1U, CERBERUS_WIFI_PACKET_COUNT_ROLE_TA=2U,
    CERBERUS_WIFI_PACKET_COUNT_ROLE_BSSID=4U };
enum { CERBERUS_WIFI_PACKET_COUNT_ROW_TIME=1U, CERBERUS_WIFI_PACKET_COUNT_ROW_TX_TIME=2U,
    CERBERUS_WIFI_PACKET_COUNT_ROW_RSSI=4U, CERBERUS_WIFI_PACKET_COUNT_ROW_SATURATED=8U };
enum { CERBERUS_WIFI_PACKET_COUNT_STARTED_TIME=1U, CERBERUS_WIFI_PACKET_COUNT_STOPPED_TIME=2U,
    CERBERUS_WIFI_PACKET_COUNT_COPIED_TIME=4U, CERBERUS_WIFI_PACKET_COUNT_STOPPED=8U,
    CERBERUS_WIFI_PACKET_COUNT_SATURATED=16U, CERBERUS_WIFI_PACKET_COUNT_CLOCK_INVALID=32U };

typedef struct {
    uint8_t role,channel_hint,mac[6]; /* Hint is provenance/plan input, never a match filter. */
} cerberus_wifi_packet_count_target_t;

typedef struct {
    uint8_t ra[6],ta[6],bssid[6],roles,kind,subtype;
    uint16_t header_bytes;
    bool retry,protected_frame,fragmented;
} cerberus_wifi_packet_count_frame_t;

typedef struct {
    cerberus_wifi_packet_count_target_t target;
    uint32_t observations,transmitted,addressed,bssid_context;
    uint32_t management,data,retry,protected_frames,fragmented;
    uint64_t first_us,last_us,last_tx_us;
    int8_t rssi;
    uint8_t last_channel,flags;
} cerberus_wifi_packet_count_row_t;

typedef struct {
    uint64_t started_source_us,stopped_source_us,copied_source_us;
    uint64_t requested_channels,attempted_channels,successful_channels;
    uint32_t callbacks,invalid_metadata,malformed_frames,unsupported_frames;
    uint32_t clock_dropped_frames,outside_window_frames,valid_frames,matched_frames;
    uint32_t channel_failures,revision;
    uint16_t dwell_ms,flags;
    uint8_t count;
} cerberus_wifi_packet_count_summary_t;

typedef struct {
    cerberus_wifi_packet_count_summary_t summary;
    cerberus_wifi_packet_count_row_t rows[CERBERUS_WIFI_PACKET_COUNT_LIMIT];
    uint64_t channel_window_us[CERBERUS_WIFI_PACKET_COUNT_CHANNELS];
    uint64_t clock_floor,window_started_us;
    uint8_t window_channel;
    bool window_open;
} cerberus_wifi_packet_count_table_t;

/* Canonical Probe-compatible map: 1..14;36..64/4;100..144/4;149..177/4. */
uint8_t cerberus_wifi_packet_count_channel(unsigned index);
int cerberus_wifi_packet_count_channel_index(uint8_t channel);
uint64_t cerberus_wifi_packet_count_channel_bit(uint8_t channel);
bool cerberus_wifi_packet_count_mask_valid(uint64_t mask);
bool cerberus_wifi_packet_count_target_valid(const cerberus_wifi_packet_count_target_t *target);
bool cerberus_wifi_packet_count_targets_valid(const cerberus_wifi_packet_count_target_t *targets,unsigned count);
uint64_t cerberus_wifi_packet_count_target_channels(const cerberus_wifi_packet_count_target_t *targets,unsigned count);

/* Header-only parser, bytes exclude FCS. MGMT/DATA only; opaque protected,
 * fragmented and retried payloads remain observations. A malformed/unsupported
 * decode clears output; no unbounded pointer or payload is returned. */
unsigned cerberus_wifi_packet_count_decode(const uint8_t *frame,size_t length,
    unsigned sdk_kind,cerberus_wifi_packet_count_frame_t *out);
/* Preflight before mutation; target input must not overlap table. */
bool cerberus_wifi_packet_count_init(cerberus_wifi_packet_count_table_t *table,
    const cerberus_wifi_packet_count_target_t *targets,unsigned count,
    uint64_t channel_mask,uint16_t dwell_ms);
void cerberus_wifi_packet_count_attempt(cerberus_wifi_packet_count_table_t *table,uint8_t channel,bool success);
bool cerberus_wifi_packet_count_window_begin(cerberus_wifi_packet_count_table_t *table,uint8_t channel,int64_t now_us);
bool cerberus_wifi_packet_count_window_end(cerberus_wifi_packet_count_table_t *table,int64_t now_us);
void cerberus_wifi_packet_count_finish(cerberus_wifi_packet_count_table_t *table,int64_t now_us);
/* Returns the primary RA/TA target bitmask. BSSID-only context is counted
 * separately and does not request optional matched-frame recording. Metadata
 * and monotonic clocks are checked before any address match. */
uint16_t cerberus_wifi_packet_count_observe(cerberus_wifi_packet_count_table_t *table,
    const uint8_t *frame,size_t length,unsigned sdk_kind,bool rx_ok,
    uint8_t channel,int rssi,int64_t callback_us);
/* Caller serializes model access. Summary/row copies are independently sampled;
 * only a stopped, immutable table supplies a coherent complete final bank. */
bool cerberus_wifi_packet_count_copy_summary(const cerberus_wifi_packet_count_table_t *table,
    int64_t now_us,cerberus_wifi_packet_count_summary_t *out);
bool cerberus_wifi_packet_count_copy_row(const cerberus_wifi_packet_count_table_t *table,
    unsigned index,cerberus_wifi_packet_count_row_t *out);
bool cerberus_wifi_packet_count_copy_channel(const cerberus_wifi_packet_count_table_t *table,
    unsigned index,int64_t now_us,uint64_t *window_us);
