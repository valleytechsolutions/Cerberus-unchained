#pragma once

/* Caller-owned passive survey evidence. No allocation, locks or radio work.
 * Receiver snapshots remain separate; pages merge their stable row ranks. */
#include "wifi_ap_survey.h"

#define CERBERUS_AP_SURVEY_PAGE_ROWS 4U
enum { CERBERUS_AP_SURVEY_SOURCE_S3 = 1U, CERBERUS_AP_SURVEY_SOURCE_C5 = 2U };

typedef struct {
    uint64_t run_id, session_id, order;
    uint64_t request_started_us, receipt_us, copied_source_us;
    uint32_t revision;
    uint8_t source, kind;
    union {
        cerberus_ap_survey_ap_t ap;
        cerberus_ap_survey_relation_t client;
    } evidence;
} cerberus_ap_survey_record_t;

typedef struct {
    cerberus_ap_survey_record_t frozen, latest;
    bool valid, present, unavailable;
} cerberus_ap_survey_selection_t;

typedef struct {
    cerberus_ap_survey_snapshot_t snapshot;
    uint64_t run_id, request_started_us, receipt_us;
    uint64_t ap_order[CERBERUS_AP_SURVEY_AP_CAPACITY];
    uint64_t client_order[CERBERUS_AP_SURVEY_RELATION_CAPACITY];
    bool published, unavailable;
} cerberus_ap_survey_bank_t;

typedef struct {
    cerberus_ap_survey_bank_t banks[2];
    cerberus_ap_survey_selection_t selected;
    uint64_t next_order;
} cerberus_ap_survey_model_t;

typedef struct {
    cerberus_ap_survey_record_t rows[CERBERUS_AP_SURVEY_PAGE_ROWS];
    uint8_t total, copied, offset, kind;
    bool filtered;
} cerberus_ap_survey_page_t;

typedef enum {
    CERBERUS_AP_SURVEY_MODEL_APPLIED,
    CERBERUS_AP_SURVEY_MODEL_DUPLICATE,
    CERBERUS_AP_SURVEY_MODEL_STALE,
    CERBERUS_AP_SURVEY_MODEL_INVALID,
} cerberus_ap_survey_model_result_t;

void cerberus_ap_survey_model_init(cerberus_ap_survey_model_t *);
cerberus_ap_survey_model_result_t cerberus_ap_survey_model_publish(
    cerberus_ap_survey_model_t *, uint8_t source, uint64_t run_id,
    uint64_t request_started_us, uint64_t receipt_us,
    const cerberus_ap_survey_snapshot_t *);
void cerberus_ap_survey_model_unavailable(cerberus_ap_survey_model_t *, uint8_t source, bool);

/* Optional filter is the copied AP identity. It never requires that AP to be
 * retained in its current slot; client evidence has its own lifetime. */
bool cerberus_ap_survey_model_page(const cerberus_ap_survey_model_t *, uint8_t kind,
    const cerberus_ap_survey_record_t *ap_filter, size_t offset,
    cerberus_ap_survey_page_t *);
bool cerberus_ap_survey_record_equal(const cerberus_ap_survey_record_t *,
    const cerberus_ap_survey_record_t *);
bool cerberus_ap_survey_model_select(cerberus_ap_survey_model_t *,
    const cerberus_ap_survey_record_t *painted);
void cerberus_ap_survey_model_clear_selection(cerberus_ap_survey_model_t *);

/* Upper bound on receive-callback age, not association or RF hardware time.
 * direction is zero for an AP, TO_AP or FROM_AP for a client relationship. */
bool cerberus_ap_survey_record_age(const cerberus_ap_survey_record_t *, uint8_t direction,
    uint64_t now_us, uint64_t *age_ms);
