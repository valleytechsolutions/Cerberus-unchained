#pragma once

#include "sentinel_watch_profile.h"
#include "watch_receive_timing.h"

/* Copied values from one completed receiver bank. Each message family has its
 * own callback-age bound; none borrows the age of another family or receiver. */
typedef struct {
    sentinel_watch_profile_t values;
    cerberus_watch_receive_timing_t timing[SENTINEL_WATCH_PROFILE_GROUP_COUNT];
    uint64_t capture_epoch;
    bool available;
} cerberus_watch_profile_evidence_t;

static inline bool cerberus_watch_profile_evidence_valid(
    const cerberus_watch_profile_evidence_t *evidence, uint64_t receipt_ms)
{
    if (!evidence || !sentinel_watch_profile_valid(&evidence->values)) return false;
    if (evidence->available) {
        if (!evidence->capture_epoch) return false;
    } else if (evidence->capture_epoch || evidence->values.present) return false;
    for (unsigned i = 0; i < SENTINEL_WATCH_PROFILE_GROUP_COUNT; ++i) {
        const cerberus_watch_receive_timing_t *timing = &evidence->timing[i];
        if (!cerberus_watch_receive_timing_valid(timing, receipt_ms) ||
            (timing->valid && !(evidence->values.present & (1U << i)))) return false;
    }
    return true;
}

static inline bool cerberus_watch_profile_evidence_equal(
    const cerberus_watch_profile_evidence_t *a, const cerberus_watch_profile_evidence_t *b)
{
    if (!a || !b || a->available != b->available || a->capture_epoch != b->capture_epoch ||
        memcmp(&a->values, &b->values, sizeof(a->values))) return false;
    for (unsigned i = 0; i < SENTINEL_WATCH_PROFILE_GROUP_COUNT; ++i)
        if (!cerberus_watch_receive_timing_equal(&a->timing[i], &b->timing[i])) return false;
    return true;
}
