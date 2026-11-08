#pragma once

#include "wardrive_core.h"

#define CERBERUS_WARDRIVE_RECENT_HISTORY 24U
#define CERBERUS_WARDRIVE_RECENT_VISIBLE 10U

typedef enum {
    CERBERUS_WARDRIVE_RECENT_PENDING = 0,
    CERBERUS_WARDRIVE_RECENT_DUPLICATE,
    CERBERUS_WARDRIVE_RECENT_APPENDED_UNLOCATED,
    CERBERUS_WARDRIVE_RECENT_APPENDED_WIGLE,
    CERBERUS_WARDRIVE_RECENT_ERROR,
    CERBERUS_WARDRIVE_RECENT_NOT_LOGGED,
} cerberus_wardrive_recent_outcome_t;

typedef struct {
    uint64_t session_id, sequence;
} cerberus_wardrive_recent_ticket_t;

typedef struct {
    cerberus_wardrive_observation_t observation;
    uint64_t session_id, sequence, receipt_ms;
    cerberus_wardrive_recent_outcome_t outcome;
    bool receipt_valid;
} cerberus_wardrive_recent_row_t;

/* Owner synchronizes access. No heap, SDK, clock reads, storage or RF calls.
 * Initialize once to zero; begin clears history only for a strictly newer
 * nonzero session ID. IDs cannot wrap or be reused within this model's life.
 * Rows are full value copies, not pointers into producer/worker storage. */
typedef struct {
    cerberus_wardrive_recent_row_t rows[CERBERUS_WARDRIVE_RECENT_HISTORY];
    uint64_t session_id, revision, last_sequence, event_count, last_receipt_ms;
    uint32_t overwritten, invalid_receipts;
    uint8_t count, next;
    bool has_receipt;
    bool all_observations; /* Streaming Wardriving shows repeats as separate rows. */
} cerberus_wardrive_recent_t;

void cerberus_wardrive_recent_init(cerberus_wardrive_recent_t *state);
bool cerberus_wardrive_recent_begin(cerberus_wardrive_recent_t *state,
    uint64_t session_id);

/* Call once per validated AP/BLE observation before SD/GPS/dedupe gates.
 * Caller assigns a strictly increasing nonzero sequence and retains the
 * returned ticket across retries; reoffering a sequence never refreshes it.
 * Receipt is S3 processing time, NOT RF observation time or UTC. An unavailable
 * or backwards clock admits the observation with receipt_valid=false/time=0;
 * it does not lower the last valid clock high-water mark. A later recovered
 * sample can be valid. IDs/revision/event-count exhaustion rejects mutation.
 * Failure clears ticket; input/output must not overlap state or each other. */
bool cerberus_wardrive_recent_observe(cerberus_wardrive_recent_t *state,
    uint64_t session_id, uint64_t sequence,
    const cerberus_wardrive_observation_t *observation,
    uint64_t receipt_ms, bool receipt_valid,
    cerberus_wardrive_recent_ticket_t *ticket);

/* PENDING -> one terminal result. Repeating the same result is idempotent;
 * changing a terminal result is rejected. The exact session/sequence ticket
 * must still be retained. Neither result changes receipt, payload or order.
 * APPENDED means accepted by the append API, not flushed/closed/durable. */
bool cerberus_wardrive_recent_set_outcome(cerberus_wardrive_recent_t *state,
    cerberus_wardrive_recent_ticket_t ticket,
    cerberus_wardrive_recent_outcome_t outcome);

/* Newest at most ten events when all_observations is set; otherwise distinct
 * identities among the last 24 events, preserving
 * each latest event's complete value. Key: AP BSSID or BLE address+type, with
 * record kind separating radios. Radio slot is source metadata, not identity.
 * This deliberately has an event horizon, not a persistent unique-device set.
 * Zero capacity permits NULL output. No other output overlaps state. */
size_t cerberus_wardrive_recent_copy_recent(const cerberus_wardrive_recent_t *state,
    cerberus_wardrive_recent_row_t *out, size_t capacity);
/* Same distinct horizon/count without allocating or copying a row bank. */
size_t cerberus_wardrive_recent_visible_count(const cerberus_wardrive_recent_t *state);

/* Display-only processing age. Invalid clock, invalid receipt or backwards
 * display time returns false and zero; embedded RF evidence remains separate. */
bool cerberus_wardrive_recent_age(const cerberus_wardrive_recent_row_t *row,
    uint64_t now_ms, bool now_valid, uint64_t *age_ms);
