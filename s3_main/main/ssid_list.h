#pragma once

#include "wifi_marauder.h"

/* Working names only: no AP identity, radio ownership or filesystem state. */
typedef struct {
    cerberus_wifi_marauder_ssid_t document;
    uint64_t selected, revision;
    bool dirty;
} cerberus_ssid_list_t;

typedef enum {
    CERBERUS_SSID_LIST_OK,
    CERBERUS_SSID_LIST_INVALID,
    CERBERUS_SSID_LIST_LIMIT,
    CERBERUS_SSID_LIST_UNREPRESENTABLE,
    CERBERUS_SSID_LIST_EXHAUSTED,
    CERBERUS_SSID_LIST_STALE,
} cerberus_ssid_list_result_t;

#define CERBERUS_SSID_LIST_ENTROPY_PER_NAME 4U

/* Single owner. Init is clean and empty at revision 1. Actual changes,
 * including selection and a successful dirty-to-clean save, advance revision.
 * Content changes mark dirty; selection alone does not. Revisions never wrap.
 * Failed operations and semantic no-ops leave all bytes unchanged. */
void cerberus_ssid_list_init(cerberus_ssid_list_t *);
bool cerberus_ssid_list_valid(const cerberus_ssid_list_t *);
cerberus_ssid_list_result_t cerberus_ssid_list_add(cerberus_ssid_list_t *, const uint8_t *, size_t);
cerberus_ssid_list_result_t cerberus_ssid_list_edit(cerberus_ssid_list_t *, uint8_t index, const uint8_t *, size_t);
cerberus_ssid_list_result_t cerberus_ssid_list_remove(cerberus_ssid_list_t *, uint8_t index);
cerberus_ssid_list_result_t cerberus_ssid_list_remove_selected(cerberus_ssid_list_t *);
cerberus_ssid_list_result_t cerberus_ssid_list_clear(cerberus_ssid_list_t *);
cerberus_ssid_list_result_t cerberus_ssid_list_select(cerberus_ssid_list_t *, uint8_t index, bool selected);
cerberus_ssid_list_result_t cerberus_ssid_list_select_all(cerberus_ssid_list_t *, bool selected);

/* Documents must be disjoint from the model. Replace preserves input framing
 * and clears selection. Append retains existing selection; imported rows are
 * unselected. unique skips byte-identical names in the existing list and
 * earlier imported rows, preserving first occurrence order. Otherwise order
 * and duplicates are preserved. No partial append on invalid data or capacity.
 * Manual mutations/append emit a final LF when nonempty. Names allow NUL, CR,
 * non-UTF8 and zero length; embedded LF is unrepresentable in SSID text. */
cerberus_ssid_list_result_t cerberus_ssid_list_replace(cerberus_ssid_list_t *, const cerberus_wifi_marauder_ssid_t *);
cerberus_ssid_list_result_t cerberus_ssid_list_append(cerberus_ssid_list_t *, const cerberus_wifi_marauder_ssid_t *, bool unique);
/* Appends CERB- plus eight uppercase hex digits per four supplied bytes.
 * Entropy must be disjoint and exactly count * ENTROPY_PER_NAME bytes. No
 * hidden random source, uniqueness promise, allocation or radio operation. */
cerberus_ssid_list_result_t cerberus_ssid_list_generate(cerberus_ssid_list_t *, size_t count, const uint8_t *entropy, size_t length);
/* Both output regions must be supplied and disjoint from each other/model.
 * Copy excludes selection. Save acknowledgement clears dirty only when its
 * immutable snapshot revision still equals the working revision. */
cerberus_ssid_list_result_t cerberus_ssid_list_copy(const cerberus_ssid_list_t *, cerberus_wifi_marauder_ssid_t *, uint64_t *revision);
cerberus_ssid_list_result_t cerberus_ssid_list_saved(cerberus_ssid_list_t *, uint64_t revision);
