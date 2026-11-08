#pragma once

#include "mesh_mac.h"

#define MESH_OBSERVATION_PANS 16U
#define MESH_OBSERVATION_ADDRESSES 64U
#define MESH_OBSERVATION_EDGES 64U
#define MESH_OBSERVATION_WITNESSES 64U
#define MESH_OBSERVATION_NO_WITNESS UINT8_MAX
#define MESH_OBSERVATION_MAX_SEQUENCE UINT32_C(0x7fffffff)

enum {
    MESH_OBSERVATION_TIME_VALID = 1U,
    MESH_OBSERVATION_RSSI_VALID = 2U,
    MESH_OBSERVATION_LQI_VALID = 4U
};

/* A complete copied callback record, not a wire ABI. The caller supplies the
 * exact accepted session and nonzero FIFO sequence. Invalid metadata fields,
 * reserved bytes and unused MAC tail bytes must be zero; MAC length is 2..125.
 * Time is C5 callback
 * monotonic microseconds, never UTC or the S3 clock. RSSI belongs to the source. */
typedef struct {
    uint64_t session, callback_us;
    uint32_t sequence;
    uint8_t channel, flags, length;
    int8_t rssi;
    uint8_t lqi, reserved[3];
    uint8_t mac[MESH_MAC_MAX_LENGTH];
} mesh_observation_input_t;

/* A PAN context is a session-local channel/value bucket, not proof of a network.
 * UNKNOWN is distinct from known PAN0. Explicit and compressed-same origins
 * share known values; origin_bits preserves both parser provenance values. */
typedef struct {
    uint16_t value;
    uint8_t channel;
    bool known;
} mesh_observation_pan_key_t;

typedef struct {
    uint64_t value;
    mesh_observation_pan_key_t pan;
    uint8_t mode;
} mesh_observation_address_key_t;

typedef struct {
    uint64_t first_us, last_us;
    uint32_t first_sequence, last_sequence, frames;
    uint8_t first_witness, last_witness;
    bool first_time_valid, last_time_valid;
} mesh_observation_seen_t;

typedef struct {
    mesh_observation_pan_key_t key;
    mesh_observation_seen_t seen;
    uint8_t origin_bits;
} mesh_observation_pan_t;

typedef struct {
    mesh_observation_address_key_t key;
    /* Any source/destination appearance, not necessarily a received transmitter.
     * The separate last_source fields identify the actual signal observation. */
    mesh_observation_seen_t seen;
    uint64_t last_source_us;
    uint32_t source_frames, destination_frames, last_source_sequence;
    int8_t source_rssi;
    uint8_t source_lqi, source_flags, origin_bits, last_source_witness;
} mesh_observation_address_t;

/* Directed MAC addressing observations, not delivery, adjacency or routing.
 * Endpoint indexes are stable retained address slots. Repeated frame sequence
 * numbers are ordinary evidence; only transport sequences suppress replay. */
typedef struct {
    mesh_observation_seen_t seen;
    uint8_t source, destination, frame_type_bits, last_frame_type;
    uint8_t last_sequence, last_signal_flags;
    int8_t last_source_rssi;
    uint8_t last_source_lqi;
    uint32_t secured_frames, encrypted_frames;
    bool last_sequence_present;
} mesh_observation_edge_t;

typedef struct {
    mesh_observation_input_t input;
    mesh_mac_result_t parse_result;
} mesh_observation_witness_t;

typedef struct {
    uint32_t records, headers, malformed, unsupported, invalid_inputs, invalid_clock;
    uint32_t wrong_session, replayed, delivery_sequence_gaps;
    /* Omission attempts, not distinct identities or unseen RF frames. */
    uint32_t pan_omissions, address_omissions, edge_omissions, witness_omissions;
    bool saturated;
} mesh_observation_counters_t;

/* Caller-owned optional workspace. No allocation, globals, radio, clock or IO.
 * One owner serializes ingest/copy/reset. Slots retain first appearance and are
 * never evicted. First64 valid-metadata records retain complete raw witnesses,
 * including malformed/unsupported records; later latest_witness is explicitly
 * unavailable even if an earlier witness for that identity remains retained. */
typedef struct {
    uint64_t session, last_valid_us;
    uint32_t last_sequence;
    uint8_t pan_count, address_count, edge_count, witness_count;
    bool time_seen;
    mesh_observation_counters_t counters;
    mesh_observation_pan_t pans[MESH_OBSERVATION_PANS];
    mesh_observation_address_t addresses[MESH_OBSERVATION_ADDRESSES];
    mesh_observation_edge_t edges[MESH_OBSERVATION_EDGES];
    mesh_observation_witness_t witnesses[MESH_OBSERVATION_WITNESSES];
} mesh_observation_t;

typedef enum {
    MESH_OBSERVATION_OK = 0,
    MESH_OBSERVATION_INVALID,
    MESH_OBSERVATION_WRONG_SESSION,
    MESH_OBSERVATION_REPLAY,
    MESH_OBSERVATION_MALFORMED,
    MESH_OBSERVATION_UNSUPPORTED
} mesh_observation_result_t;

typedef enum {
    MESH_OBSERVATION_SELECT_PAN,
    MESH_OBSERVATION_SELECT_ADDRESS,
    MESH_OBSERVATION_SELECT_EDGE,
    MESH_OBSERVATION_SELECT_WITNESS
} mesh_observation_kind_t;

typedef struct {
    uint64_t session;
    mesh_observation_kind_t kind;
    uint8_t index;
    union {
        mesh_observation_pan_t pan;
        mesh_observation_address_t address;
        mesh_observation_edge_t edge;
    } record;
    bool witness_valid;
    mesh_observation_witness_t witness;
} mesh_observation_selection_t;

/* Explicit reset only. A zero session rejects without touching the workspace. */
bool mesh_observation_init(mesh_observation_t *model, uint64_t session);
/* Bad metadata changes only invalid_inputs; wrong-session/replay attempts have
 * their own rejection counters. Valid metadata consumes its sequence once even
 * if parsing fails. Backward valid callback time increments invalid_clock only.
 * Sequence gaps report delivery gaps, not the C5's separately counted queue loss.
 * Input must not overlap the workspace; overlap rejects without any mutation. */
mesh_observation_result_t mesh_observation_ingest(mesh_observation_t *model,
                                               const mesh_observation_input_t *input);
/* Value-only frozen detail. This copies the latest addressing observation, not
 * necessarily the last source/RSSI observation: an address also seen only as a
 * later destination retains its earlier source fields and last_source_witness.
 * Latest witness may be absent. Failure and overlap
 * leave output untouched; caller must not mutate the model during this copy. */
bool mesh_observation_select(const mesh_observation_t *model,
                             mesh_observation_kind_t kind, uint8_t index,
                             mesh_observation_selection_t *out);
