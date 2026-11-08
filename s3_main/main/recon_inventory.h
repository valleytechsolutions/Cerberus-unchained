#pragma once

/* Completed-sweep address inventory, not a callback stream or a count of
 * physical devices. The owner serializes access and supplies S3 uptime.
 * No allocation, RTOS, storage or radio operations. */
#include "recon_journal.h"

#define CERBERUS_RECON_INVENTORY_PER_RADIO 32U
#define CERBERUS_RECON_INVENTORY_CAPACITY 64U
#define CERBERUS_RECON_INVENTORY_BUCKETS 64U
#define CERBERUS_RECON_INVENTORY_EXPIRE_US UINT64_C(120000000)
#define CERBERUS_RECON_INVENTORY_ALL 2U

typedef enum {
    CERBERUS_RECON_INVENTORY_AGE_SWEEP = 0,
    CERBERUS_RECON_INVENTORY_AGE_SOURCE,
} cerberus_recon_inventory_age_basis_t;

typedef struct {
    cerberus_recon_record_t record;
    uint64_t first_consumed_us, last_consumed_us;
    /* Age can retain earlier genuine source evidence when a later row has no
     * source time. It is independent of the latest row's aggregate RSSI. */
    /* C5 age is an upper bound at request start, not a translated C5 clock:
     * age = age_offset_us + (now - age_anchor_us). Other bases use offset 0. */
    uint64_t age_anchor_us, age_offset_us, revision;
    uint8_t age_basis, age_source_kind;
    bool occupied;
} cerberus_recon_inventory_entry_t;

typedef struct {
    uint64_t second;
    uint32_t added, expired;
} cerberus_recon_inventory_bucket_t;

typedef struct {
    uint32_t accepted, added, expired, replaced, invalid;
} cerberus_recon_inventory_counters_t;

typedef struct {
    cerberus_recon_inventory_entry_t entries[CERBERUS_RECON_INVENTORY_CAPACITY];
    cerberus_recon_record_t staged[CERBERUS_RECON_INVENTORY_PER_RADIO];
    cerberus_recon_inventory_bucket_t history[CERBERUS_RECON_INVENTORY_BUCKETS];
    cerberus_recon_inventory_counters_t counters;
    uint64_t publication[2], clock_us, revision, batch_started_us, batch_last_us;
    uint8_t count[2], staged_count, batch_radio, history_head, history_count;
    bool initialized, batch_active, batch_failed;
} cerberus_recon_inventory_t;

void cerberus_recon_inventory_init(cerberus_recon_inventory_t *state, uint64_t now_us);
/* begin reserves the nonzero, strictly increasing publication for this radio.
 * Even aborted publications cannot be replayed. At most one batch is active.
 * Offer accepts payload records before recorder session/sequence assignment.
 * Invalid payload, mixed generations, duplicate typed identity, future/ambiguous source or
 * backward exact S3 source time poisons the batch. C5 upper age bounds may
 * change across requests and are not compared as exact source timestamps.
 * end publishes atomically;
 * abort/end failure leave every inventory row and graph unchanged. */
bool cerberus_recon_inventory_begin(cerberus_recon_inventory_t *state,
    uint8_t radio, uint64_t publication, uint64_t now_us);
bool cerberus_recon_inventory_offer(cerberus_recon_inventory_t *state,
    const cerberus_recon_record_t *record);
bool cerberus_recon_inventory_end(cerberus_recon_inventory_t *state, uint64_t now_us);
void cerberus_recon_inventory_abort(cerberus_recon_inventory_t *state);
/* Advances at most 64 one-second graph buckets, never expires inventory.
 * Before the first completed batch there are no graph samples. Rollback is
 * rejected. Pause/failed sweeps do not constitute evidence of absence.
 * Expiry means the retained age basis reached policy TTL, not proven absence;
 * the C5 upper-bound basis can reach that threshold conservatively. */
bool cerberus_recon_inventory_tick(cerberus_recon_inventory_t *state, uint64_t now_us);
/* Returns all matching row count, copying only the capacity prefix. Newest
 * accepted consumption first; equal times preserve slot order. Invalid
 * arguments return 0 without modifying output. ALL includes both radios. */
size_t cerberus_recon_inventory_indexes(const cerberus_recon_inventory_t *state,
    uint8_t radio_filter, uint8_t *indexes, size_t capacity);
/* Copies existing chronological buckets; returns their total count. */
size_t cerberus_recon_inventory_buckets(const cerberus_recon_inventory_t *state,
    cerberus_recon_inventory_bucket_t *out, size_t capacity);
bool cerberus_recon_inventory_entry_age(const cerberus_recon_inventory_entry_t *entry,
    uint64_t now_us, uint64_t *age_us);
