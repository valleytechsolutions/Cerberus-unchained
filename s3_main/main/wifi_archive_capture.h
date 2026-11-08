#pragma once
#include "dual_wifi_scan.h"
#include "wifi_archive.h"

/* Converts one immutable UI-task snapshot and the caller's one save-clock
 * sample. No allocation, transport, RF, scanner or clock calls. A negative or
 * backwards save clock preserves observations with unknown saved timing.
 * Input/output must not overlap; failure leaves output unchanged. */
bool cerberus_wifi_archive_capture(const cerberus_dual_wifi_snapshot_t *snapshot,
    int64_t now_us, cerberus_wifi_archive_t *out);
/* Same contract, with a selection mask and caller-owned immutable components.
 * UINT64_MAX selects all; other out-of-bank bits reject. All original rows are
 * preflighted, even when omitted; original coverage/counts remain explicit. */
bool cerberus_wifi_archive_capture_rows(const cerberus_wifi_archive_publication_t *publication,
    const cerberus_dual_wifi_row_t *rows, uint8_t count, int64_t now_us,
    uint64_t selected_rows, cerberus_wifi_archive_t *out);
