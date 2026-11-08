#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "dual_wifi_scan.c"

static uint32_t clock_ms, local_job, local_complete_at, c5_complete_at;
static uint64_t epoch, lease;
static uint8_t requested_band;
static cerberus_s3_wifi_scan_status_t local_status;
static cerberus_wifi_scan_status_payload_t remote_status;
static cerberus_s3_wifi_scan_row_t local_rows[32];
static cerberus_c5_wifi_result_t remote_rows[32];
static unsigned local_starts, local_stops, remote_starts, remote_stops, info_reads, status_reads;
static unsigned proof_reads, row_reads;
static uint8_t peer_boot;
static bool regress_proof_clock;
static esp_err_t info_error, local_start_error, start_reply_error, status_error, row_error, proof_error;
static bool allocation_fails, reject_start, fail_local_cleanup, keep_remote_running;
static bool replace_local_on_row, replace_bank_on_proof, change_status_fence;

int64_t esp_timer_get_time(void) { return (int64_t)clock_ms * 1000; }
cerberus_s3_wifi_owner_t cerberus_s3_wifi_owner(void)
{ return local_status.radio_owned ? CERBERUS_S3_WIFI_SCAN : CERBERUS_S3_WIFI_NONE; }
/* Aggregate publication is covered with the real module by the diagnostics suite. */
void cerberus_dual_wifi_diagnostics_publish(
    const cerberus_dual_wifi_diagnostic_values_t *value, int64_t now_us)
{ assert(value && now_us == (int64_t)clock_ms * 1000); }
void *heap_caps_calloc(size_t count, size_t size, unsigned caps)
{ assert(caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)); return allocation_fails ? NULL : calloc(count, size); }

static void refresh_local(void)
{
    if (local_status.state == CERBERUS_S3_WIFI_SCAN_RUNNING && clock_ms >= local_complete_at) {
        local_status.state = CERBERUS_S3_WIFI_SCAN_COMPLETE;
        local_status.generation = local_status.job_id;
        local_status.completed_ms = local_complete_at;
        local_status.radio_owned = false;
    }
}
esp_err_t cerberus_s3_wifi_scanner_start(cerberus_s3_wifi_scan_status_t *out)
{
    ++local_starts; memset(out, 0, sizeof(*out));
    if (local_start_error != ESP_OK) return local_start_error;
    local_status.job_id = ++local_job;
    local_status.state = CERBERUS_S3_WIFI_SCAN_RUNNING;
    local_status.radio_owned = true;
    local_status.cleanup_error = ESP_OK;
    *out = local_status; return ESP_OK;
}
esp_err_t cerberus_s3_wifi_scanner_get_status(cerberus_s3_wifi_scan_status_t *out)
{ refresh_local(); *out = local_status; return ESP_OK; }
esp_err_t cerberus_s3_wifi_scanner_stop(uint32_t job)
{
    assert(job == local_status.job_id); ++local_stops;
    if (fail_local_cleanup) { local_status.cleanup_error = ESP_FAIL; return ESP_FAIL; }
    local_status.state = CERBERUS_S3_WIFI_SCAN_CANCELLED;
    local_status.radio_owned = false; return ESP_OK;
}
esp_err_t cerberus_s3_wifi_scanner_get_result(uint32_t generation, uint8_t index,
    cerberus_s3_wifi_scan_row_t *out)
{
    if (generation != local_status.generation || index >= local_status.count) return ESP_ERR_INVALID_STATE;
    *out = local_rows[index];
    if (replace_local_on_row) { ++local_status.job_id; ++local_status.generation; }
    return ESP_OK;
}
static void refresh_remote(void)
{
    if (remote_status.state == CERBERUS_WIFI_SCAN_RUNNING && !keep_remote_running && clock_ms >= c5_complete_at)
        remote_status.state = CERBERUS_WIFI_SCAN_COMPLETE;
}
esp_err_t c5_spi_link_wifi_scan_band_info(cerberus_wifi_scan_band_response_t *out)
{
    ++info_reads; memset(out, 0, sizeof(*out));
    out->session_id = epoch; out->status = remote_status;
    return info_error;
}
esp_err_t c5_spi_link_start_wifi_scan_band(uint8_t band, uint64_t session,
    cerberus_wifi_scan_status_payload_t *out)
{
    ++remote_starts; assert(local_starts || store->status.rebinding);
    assert(session == epoch + 1U);
    requested_band = band; memset(out, 0, sizeof(*out));
    if (reject_start) return start_reply_error;
    epoch = lease = session; ++remote_status.generation;
    remote_status.state = CERBERUS_WIFI_SCAN_RUNNING;
    *out = remote_status; return start_reply_error;
}
esp_err_t c5_spi_link_wifi_scan_band_status(uint8_t band, uint64_t session,
    cerberus_wifi_scan_status_payload_t *out)
{
    ++status_reads; memset(out, 0, sizeof(*out));
    if (status_error) return status_error;
    if (session != lease || session != epoch || band != requested_band) return ESP_ERR_NOT_FOUND;
    refresh_remote(); *out = remote_status; return ESP_OK;
}
esp_err_t c5_spi_link_stop_wifi_scan_band(uint8_t band, uint64_t session,
    cerberus_wifi_scan_status_payload_t *out)
{
    assert(session == lease && session == epoch && band == requested_band);
    ++remote_stops;
    remote_status.state = CERBERUS_WIFI_SCAN_IDLE;
    *out = remote_status; return ESP_OK;
}
esp_err_t c5_spi_link_get_wifi_scan_status_once(uint32_t deadline,
    cerberus_wifi_scan_status_payload_t *out)
{
    assert(clock_ms < deadline); *out = remote_status;
    if (change_status_fence) ++out->generation;
    return ESP_OK;
}
esp_err_t c5_spi_link_wifi_relation_begin_check(cerberus_c5_wifi_relation_reader_t *reader,
    uint8_t generation, uint32_t deadline)
{
    memset(reader, 0, sizeof(*reader)); reader->generation = generation; reader->deadline_ms = deadline;
    reader->pending.status.epoch = epoch;
    reader->pending.status.generation = generation;
    reader->pending.request_started_us = (uint64_t)clock_ms * 1000;
    reader->pending.source_now_us = 777000;
    reader->pending.peer_uptime_ms = 900;
    reader->pending.boot_count = peer_boot;
    reader->pending.clock_valid = true; return ESP_OK;
}
esp_err_t c5_spi_link_wifi_relation_begin_check_expected(cerberus_c5_wifi_relation_reader_t *reader,
    const cerberus_c5_wifi_relation_bank_t *expected, uint32_t deadline)
{
    memset(reader, 0, sizeof(*reader)); reader->deadline_ms = deadline;
    reader->expect_bank = true; reader->expected = *expected; return ESP_OK;
}
esp_err_t c5_spi_link_wifi_relation_step(cerberus_c5_wifi_relation_reader_t *reader,
    cerberus_c5_wifi_relation_bank_t *proof, bool *complete,
    cerberus_c5_relation_continue_fn callback, void *context)
{
    (void)callback; (void)context; ++proof_reads;
    assert(clock_ms < reader->deadline_ms); *complete = false;
    if (proof_error) return proof_error;
    if (reader->expect_bank) {
        if (replace_bank_on_proof) ++epoch;
        if (reader->expected.status.epoch != epoch || reader->expected.boot_count != peer_boot || regress_proof_clock) return ESP_ERR_INVALID_STATE;
        *proof = reader->expected;
    } else *proof = reader->pending;
    *complete = true; return ESP_OK;
}
void c5_spi_link_wifi_relation_cancel(cerberus_c5_wifi_relation_reader_t *reader)
{ memset(reader, 0, sizeof(*reader)); }
esp_err_t c5_spi_link_wifi_scan_result_begin(cerberus_c5_wifi_scan_reader_t *reader,
    uint8_t generation, uint8_t index, uint32_t deadline)
{
    memset(reader, 0, sizeof(*reader)); reader->generation = generation; reader->index = index;
    reader->deadline_ms = deadline; return ESP_OK;
}
esp_err_t c5_spi_link_wifi_scan_result_step(cerberus_c5_wifi_scan_reader_t *reader,
    cerberus_c5_wifi_result_t *out, cerberus_s3_wifi_observation_t *observation, bool *complete)
{
    ++row_reads; assert(clock_ms < reader->deadline_ms && reader->index < remote_status.stored_count);
    *complete = false; if (row_error) return row_error;
    if (++reader->phase < 3U) return ESP_OK; /* Incremental, not one blocking bank read. */
    *out = remote_rows[reader->index];
    observation->valid = true; observation->request_started_us = (uint64_t)clock_ms * 1000;
    *complete = true; return ESP_OK;
}
void c5_spi_link_wifi_scan_result_cancel(cerberus_c5_wifi_scan_reader_t *reader)
{ memset(reader, 0, sizeof(*reader)); }

static void reset(void)
{
    free(store); store = NULL; clock_ms = 1000; epoch = 100; lease = 0; local_job = 100;
    local_complete_at = 1300; c5_complete_at = 1400;
    requested_band = 255; proof_reads = row_reads = 0; peer_boot = 7; regress_proof_clock = false;
    local_starts = local_stops = remote_starts = remote_stops = info_reads = status_reads = 0;
    info_error = local_start_error = start_reply_error = status_error = row_error = proof_error = ESP_OK;
    allocation_fails = reject_start = fail_local_cleanup = keep_remote_running = false;
    replace_local_on_row = replace_bank_on_proof = change_status_fence = false;
    memset(&local_status, 0, sizeof(local_status)); memset(&remote_status, 0, sizeof(remote_status));
    memset(local_rows, 0, sizeof(local_rows)); memset(remote_rows, 0, sizeof(remote_rows));
    local_status.count = remote_status.stored_count = 32;
    local_status.total_count = remote_status.total_count = 40;
    for (uint8_t i = 0; i < 32; ++i) {
        local_rows[i].bssid[0] = 2; local_rows[i].bssid[5] = i;
        local_rows[i].ssid_length = 3; memcpy(local_rows[i].ssid, "S3x", 3);
        local_rows[i].channel = 1; local_rows[i].security = CERBERUS_WIFI_SECURITY_WPA2;
        local_rows[i].source = CERBERUS_S3_WIFI_SCAN_SOURCE_S3;
        remote_rows[i].bssid[0] = 4; remote_rows[i].bssid[5] = i;
        strcpy(remote_rows[i].ssid, "C5x"); remote_rows[i].channel = 36;
        remote_rows[i].security = CERBERUS_WIFI_SECURITY_WPA2;
    }
}
static void tick(void) { clock_ms += 20; cerberus_dual_wifi_scan_step(); }
static void drain(void) { unsigned limit = 1500; while (dual_active() && limit--) tick(); assert(!dual_active()); }
static void until_phase(uint8_t phase)
{ unsigned limit = 1500; while (store->phase != phase && dual_active() && limit--) tick(); assert(store->phase == phase); }
static cerberus_dual_wifi_status_t status(void)
{ cerberus_dual_wifi_status_t result; cerberus_dual_wifi_scan_get_status(&result); return result; }
static void verify_retained(uint32_t generation, unsigned count)
{ assert(status().generation == generation && status().count == count); }

static cerberus_c5_wifi_result_t exported_rows[32];
static cerberus_s3_wifi_observation_t exported_observations[32];
static cerberus_wifi_scan_status_payload_t exported_status;
static cerberus_c5_wifi_relation_bank_t exported_proof;
static cerberus_dual_wifi_handoff_t exported_handoff;
static uint8_t exported_index;
static esp_err_t export_bank(void)
{
    memset(exported_rows, 0xa5, sizeof(exported_rows));
    memset(exported_observations, 0xa5, sizeof(exported_observations));
    memset(&exported_handoff, 0xa5, sizeof(exported_handoff));
    return cerberus_dual_wifi_scan_copy_legacy_bank(exported_rows, exported_observations, 32,
        &exported_status, &exported_proof, &exported_index, &exported_handoff);
}
static cerberus_dual_wifi_row_t completed_c5(bool fallback)
{
    reset();
    if (fallback) local_start_error = ESP_ERR_INVALID_STATE;
    assert(cerberus_dual_wifi_scan_start() == ESP_OK); drain();
    cerberus_dual_wifi_row_t selected;
    assert(cerberus_dual_wifi_scan_get_row((uint8_t)(status().s3_count + 7U), &selected) == ESP_OK);
    assert(selected.source == CERBERUS_DUAL_WIFI_SOURCE_C5 && store->bank_eligible);
    return selected;
}
static void no_new_rf(void)
{ assert(remote_starts == 1U && local_starts == 1U && !remote_stops && !local_stops); }
static void check_cached_handoff(void)
{
    for (unsigned fallback = 0; fallback < 2U; ++fallback) {
        cerberus_dual_wifi_row_t selected = completed_c5(fallback != 0);
        cerberus_dual_wifi_row_t original[32];
        memcpy(original, &store->pending[32], sizeof(original));
        cerberus_c5_wifi_relation_bank_t proof = store->proof;
        cerberus_wifi_scan_status_payload_t bank = store->c5_status;
        int64_t received = store->bank_received_us;
        uint32_t completed = status().completed_ms;
        unsigned read_count = row_reads, infos = info_reads, proofs = proof_reads;
        clock_ms += 10000; /* Reopening cannot freshen the original observation. */
        assert(cerberus_dual_wifi_scan_begin_rebind(&selected) == ESP_OK);
        assert(status().rebind_cached && status().rebinding && !status().cleanup_pending);
        assert(status().rebind_source == CERBERUS_DUAL_WIFI_SOURCE_C5);
        drain(); no_new_rf();
        assert(status().rebind_ready && info_reads == infos && row_reads == read_count && proof_reads == proofs + 1U);
        assert(export_bank() == ESP_OK && exported_index == 7U && exported_handoff.reused);
        assert(exported_handoff.received_us == received && received < (int64_t)clock_ms * 1000);
        assert(exported_handoff.session_id == epoch && exported_handoff.band ==
            (fallback ? CERBERUS_WIFI_SCAN_BAND_ALL : CERBERUS_WIFI_SCAN_BAND_5G));
        assert(!memcmp(&bank, &exported_status, sizeof(bank)) && !memcmp(&proof, &exported_proof, sizeof(proof)));
        for (unsigned i = 0; i < 32; ++i) {
            assert(!memcmp(&exported_rows[i], &original[i].ap, sizeof(exported_rows[i])));
            assert(!memcmp(&exported_observations[i], &original[i].observation, sizeof(exported_observations[i])));
        }
        assert(status().completed_ms == completed && status().generation == 1U);
        assert(status().s3_count == (fallback ? 0U : 32U) && status().c5_count == 32U && status().c5_total == 40U);
        assert(cerberus_dual_wifi_scan_begin_rebind(&selected) == ESP_OK); drain();
        assert(export_bank() == ESP_OK && exported_handoff.received_us == received); no_new_rf();
        assert(cerberus_dual_wifi_scan_copy_legacy_bank(exported_rows, exported_observations, 32,
            &exported_status, &exported_proof, &exported_index, NULL) == ESP_ERR_INVALID_ARG);
    }
    /* Stop at every validation boundary releases no RF and permits a clean
     * validation retry of the untouched bank. */
    const uint8_t phases[] = {DUAL_PHASE_CACHED_OWNER, DUAL_PHASE_FINAL_PROOF, DUAL_PHASE_FINAL_OWNER};
    for (unsigned i = 0; i < sizeof(phases); ++i) {
        cerberus_dual_wifi_row_t selected = completed_c5(false);
        int64_t received = store->bank_received_us;
        assert(cerberus_dual_wifi_scan_begin_rebind(&selected) == ESP_OK); until_phase(phases[i]);
        cerberus_dual_wifi_scan_stop(); drain(); no_new_rf();
        assert(!status().rebind_ready && !status().cleanup_pending && store->bank_eligible);
        assert(export_bank() == ESP_ERR_INVALID_STATE && !exported_handoff.session_id);
        assert(cerberus_dual_wifi_scan_begin_rebind(&selected) == ESP_OK); drain();
        assert(export_bank() == ESP_OK && exported_handoff.received_us == received); no_new_rf();
    }
    /* Stale/uncertain validation is terminal and cannot adopt or stop another
     * session, even if its eight-bit generation is unchanged. */
    for (unsigned failure = 0; failure < 9U; ++failure) {
        cerberus_dual_wifi_row_t selected = completed_c5(false);
        assert(cerberus_dual_wifi_scan_begin_rebind(&selected) == ESP_OK);
        if (failure == 0) epoch += 256U; /* Same gen8; different owned epoch. */
        else if (failure == 1) { ++peer_boot; } /* Peer reboot, even with reused token. */
        else if (failure == 2) replace_bank_on_proof = true;
        else if (failure == 3) proof_error = ESP_ERR_TIMEOUT;
        else if (failure == 4) status_error = ESP_ERR_TIMEOUT;
        else if (failure == 5) ++remote_status.total_count;
        else if (failure == 6) regress_proof_clock = true;
        else if (failure == 7) { until_phase(DUAL_PHASE_FINAL_OWNER); ++epoch; }
        else clock_ms = 0U;
        drain(); no_new_rf();
        assert(status().state == CERBERUS_DUAL_WIFI_ERROR && !status().rebind_ready &&
            !status().cleanup_pending && !store->bank_eligible);
        assert(export_bank() == ESP_ERR_INVALID_STATE && !exported_status.stored_count &&
            !exported_handoff.session_id && !exported_observations[7].valid);
        verify_retained(1U, 64U);
    }
    {
        cerberus_dual_wifi_row_t selected = completed_c5(false);
        assert(cerberus_dual_wifi_scan_begin_rebind(&selected) == ESP_OK); drain();
        ++epoch; /* Replacement after validation, just before export. */
        assert(export_bank() == ESP_ERR_NOT_FOUND && !exported_handoff.session_id &&
            !exported_status.stored_count && !exported_observations[7].valid); no_new_rf();
    }
    /* Actual legacy generation wrap is accepted only within the full-width
     * current lease. It does not disable the ownership fence. */
    reset(); remote_status.generation = UINT8_MAX;
    assert(cerberus_dual_wifi_scan_start() == ESP_OK); drain();
    cerberus_dual_wifi_row_t selected;
    assert(cerberus_dual_wifi_scan_get_row(39U, &selected) == ESP_OK && !selected.source_generation);
    assert(cerberus_dual_wifi_scan_begin_rebind(&selected) == ESP_OK); drain();
    assert(export_bank() == ESP_OK && !exported_status.generation); no_new_rf();

    /* A canceled replacement or even failed INFO invalidates the old staging
     * eligibility; only a new explicit rebind may reacquire an all-band bank. */
    for (unsigned failed_info = 0; failed_info < 2U; ++failed_info) {
        selected = completed_c5(false);
        if (failed_info) info_error = ESP_ERR_TIMEOUT;
        esp_err_t error = cerberus_dual_wifi_scan_start();
        assert(error == (failed_info ? ESP_ERR_TIMEOUT : ESP_OK));
        assert(status().rebind_source == 0U);
        if (!failed_info) cerberus_dual_wifi_scan_stop();
        drain(); assert(!store->bank_eligible); info_error = ESP_OK;
        assert(cerberus_dual_wifi_scan_begin_rebind(&selected) == ESP_OK && !status().rebind_cached);
        drain(); assert(remote_starts == 2U && requested_band == CERBERUS_WIFI_SCAN_BAND_ALL);
        assert(export_bank() == ESP_OK && !exported_handoff.reused);
    }
    /* A copied AP identity cannot substitute a display/source index, source
     * generation or receipt from a different row; changed/duplicate identity
     * also takes the existing fresh rebind path rather than cached approval. */
    for (unsigned changed = 0; changed < 7U; ++changed) {
        selected = completed_c5(false);
        if (changed == 0) ++selected.source_index;
        else if (changed == 1) ++selected.source_generation;
        else if (changed == 2) ++selected.received_ms;
        else if (changed == 3) selected.ap.ssid[0] = 'X';
        else if (changed == 4) ++selected.ap.bssid[5];
        else if (changed == 5) selected.ap.security = CERBERUS_WIFI_SECURITY_OPEN;
        else store->pending[40].ap = selected.ap;
        assert(cerberus_dual_wifi_scan_begin_rebind(&selected) == ESP_OK && !status().rebind_cached);
        assert(store->phase == DUAL_PHASE_LOCAL && !store->bank_eligible);
        cerberus_dual_wifi_scan_stop(); drain(); no_new_rf();
    }
    puts("Cached C5 handoff: zero RF/retransfer, original receipt/proof/band, wrap/reboot/stale fences, cancellation retry and staging invalidation passed.");
}

static void check_archive_publication(void)
{
    static cerberus_dual_wifi_snapshot_t frozen, copied;
    static cerberus_wifi_archive_t saved, before;
    reset();memset(&saved,0x55,sizeof(saved));before=saved;
    assert(cerberus_dual_wifi_scan_capture_archive(0,UINT64_MAX,&saved)==ESP_ERR_NOT_FOUND);
    assert(!memcmp(&saved,&before,sizeof(saved)));
    assert(cerberus_dual_wifi_scan_start()==ESP_OK);drain();
    assert(cerberus_dual_wifi_scan_copy_snapshot(&frozen)==ESP_OK);
    assert(frozen.count==64&&frozen.publication.split&&frozen.publication.s3_count==32&&frozen.publication.c5_count==32);
    unsigned io=info_reads+status_reads+proof_reads+row_reads+local_starts+local_stops+remote_starts+remote_stops;
    assert(cerberus_dual_wifi_scan_capture_archive(esp_timer_get_time(),UINT64_MAX,&saved)==ESP_OK);
    assert(cerberus_wifi_archive_validate(&saved)&&saved.count==64);
    assert(saved.publication.received_us==(uint64_t)store->bank_received_us);
    assert(cerberus_dual_wifi_scan_capture_archive(esp_timer_get_time(),UINT64_C(1)<<63,&saved)==ESP_OK);
    assert(saved.count==1&&saved.subset&&saved.rows[0].source_index==31&&saved.publication.s3_count==32);
    assert(io==info_reads+status_reads+proof_reads+row_reads+local_starts+local_stops+remote_starts+remote_stops);
    assert(cerberus_dual_wifi_scan_copy_snapshot((cerberus_dual_wifi_snapshot_t*)store)==ESP_ERR_INVALID_ARG);
    assert(cerberus_dual_wifi_scan_capture_archive(0,0,(cerberus_wifi_archive_t*)store)==ESP_ERR_INVALID_ARG);
    cerberus_dual_wifi_row_t cached=frozen.rows[32];
    assert(cerberus_dual_wifi_scan_begin_rebind(&cached)==ESP_OK);drain();
    assert(status().rebind_cached&&status().rebind_ready);
    assert(cerberus_dual_wifi_scan_copy_snapshot(&copied)==ESP_OK&&!memcmp(&copied,&frozen,sizeof(copied)));
    /* A failed INFO and a queued/cancelled retry must retain complete metadata. */
    info_error=ESP_ERR_TIMEOUT;assert(cerberus_dual_wifi_scan_start()==ESP_ERR_TIMEOUT);
    assert(cerberus_dual_wifi_scan_copy_snapshot(&copied)==ESP_OK&&!memcmp(&copied,&frozen,sizeof(copied)));
    info_error=ESP_OK;assert(cerberus_dual_wifi_scan_start()==ESP_OK);
    assert(status().s3_count==0&&status().c5_count==0);
    assert(cerberus_dual_wifi_scan_copy_snapshot(&copied)==ESP_OK&&!memcmp(&copied,&frozen,sizeof(copied)));
    assert(cerberus_dual_wifi_scan_capture_archive(esp_timer_get_time(),UINT64_MAX,&saved)==ESP_OK);
    assert(saved.publication.split&&saved.publication.s3_total==40&&saved.publication.c5_total==40);
    cerberus_dual_wifi_scan_stop();drain();
    assert(cerberus_dual_wifi_scan_copy_snapshot(&copied)==ESP_OK&&!memcmp(&copied,&frozen,sizeof(copied)));
    /* A fresh rebind owns a different all-band bank without relabeling retained64. */
    cerberus_dual_wifi_row_t target=frozen.rows[0];remote_rows[0]=target.ap;
    assert(cerberus_dual_wifi_scan_begin_rebind(&target)==ESP_OK);drain();assert(status().rebind_ready);
    assert(cerberus_dual_wifi_scan_copy_snapshot(&copied)==ESP_OK&&!memcmp(&copied,&frozen,sizeof(copied)));
    assert(cerberus_dual_wifi_scan_capture_archive(esp_timer_get_time(),UINT64_MAX,&saved)==ESP_OK);
    assert(saved.publication.c5_session_id==frozen.publication.c5_session_id&&saved.publication.c5_coverage==2);
    /* An interrupted transfer cannot publish partially replaced rows or metadata. */
    assert(cerberus_dual_wifi_scan_start()==ESP_OK);until_phase(DUAL_PHASE_ROWS);
    cerberus_dual_wifi_scan_stop();drain();
    assert(cerberus_dual_wifi_scan_copy_snapshot(&copied)==ESP_OK&&!memcmp(&copied,&frozen,sizeof(copied)));
    local_start_error=ESP_ERR_INVALID_STATE;
    assert(cerberus_dual_wifi_scan_start()==ESP_OK);drain();
    assert(cerberus_dual_wifi_scan_copy_snapshot(&copied)==ESP_OK);
    assert(copied.count==32&&copied.publication.fallback&&!copied.publication.split&&copied.publication.generation==2);
    assert(copied.publication.s3_error==ESP_ERR_INVALID_STATE&&copied.publication.c5_coverage==3);
    assert(cerberus_dual_wifi_scan_capture_archive(esp_timer_get_time(),UINT64_MAX,&saved)==ESP_OK&&saved.count==32);
    assert(cerberus_dual_wifi_scan_capture_archive(esp_timer_get_time(),UINT64_C(1)<<32,&saved)==ESP_ERR_INVALID_RESPONSE);
    /* Generation exhaustion leaves the last archive source available. */
    frozen=copied;store->status.generation=UINT32_MAX;
    assert(cerberus_dual_wifi_scan_start()==ESP_ERR_INVALID_STATE);
    assert(cerberus_dual_wifi_scan_copy_snapshot(&copied)==ESP_OK&&!memcmp(&copied,&frozen,sizeof(copied)));
    reset();local_status.count=remote_status.stored_count=0;
    assert(cerberus_dual_wifi_scan_start()==ESP_OK);drain();
    assert(cerberus_dual_wifi_scan_capture_archive(esp_timer_get_time(),UINT64_MAX,&saved)==ESP_OK);
    assert(saved.count==0&&!saved.subset&&saved.publication.generation==1);
    puts("Archive publication: no-I/O coherent copy, masks, retry/Stop/rebind preservation, fallback, generation exhaustion and empty bank passed.");
}

static void retrying_local_cleanup(void)
{
    reset();assert(cerberus_dual_wifi_scan_start()==ESP_OK);until_phase(DUAL_PHASE_WAIT);
    local_status.state=CERBERUS_S3_WIFI_SCAN_STOPPING;local_status.cleanup_error=ESP_FAIL;
    for(unsigned i=0;i<60;++i){
        tick();assert(status().error==ESP_OK&&status().split&&dual_active());
        assert(!local_stops&&!remote_stops);
    }
    local_status.state=CERBERUS_S3_WIFI_SCAN_COMPLETE;local_status.cleanup_error=ESP_OK;
    local_status.radio_owned=false;local_status.generation=local_status.job_id;
    local_status.completed_ms=local_complete_at;drain();
    assert(status().state==CERBERUS_DUAL_WIFI_COMPLETE&&status().count==64&&!status().fallback);

    for(unsigned expire=0;expire<2;++expire){
        reset();assert(cerberus_dual_wifi_scan_start()==ESP_OK);until_phase(DUAL_PHASE_WAIT);
        local_status.state=CERBERUS_S3_WIFI_SCAN_STOPPING;local_status.cleanup_error=ESP_FAIL;
        fail_local_cleanup=true;
        if(expire){clock_ms=store->started_ms+DUAL_WIFI_DEADLINE_MS;tick();}
        else cerberus_dual_wifi_scan_stop();
        for(unsigned i=0;i<5;++i)tick();
        assert(local_stops>0&&status().cleanup_pending&&local_status.radio_owned);
        assert(status().error==(expire?ESP_ERR_TIMEOUT:ESP_OK));
        assert(cerberus_dual_wifi_scan_start()==ESP_ERR_INVALID_STATE);
        fail_local_cleanup=false;drain();assert(!status().cleanup_pending&&!local_status.radio_owned);
        local_status.cleanup_error=ESP_OK;
        assert(cerberus_dual_wifi_scan_start()==ESP_OK);drain();
        assert(status().state==CERBERUS_DUAL_WIFI_COMPLETE&&status().split);
    }
    puts("S3 cleanup retries: wait without cancelling C5, forward Stop despite cleanup error, retain owner past deadline, recover and rescan passed.");
}
int main(void)
{
    retrying_local_cleanup();
    reset(); allocation_fails = true;
    assert(cerberus_dual_wifi_scan_start() == ESP_ERR_NO_MEM && !info_reads && !local_starts && !remote_starts);
    reset(); info_error = ESP_ERR_NOT_SUPPORTED;
    assert(cerberus_dual_wifi_scan_start() == ESP_ERR_NOT_SUPPORTED && !local_starts && !remote_starts);
    reset(); assert(cerberus_dual_wifi_scan_start() == ESP_OK); drain();
    assert(status().state == CERBERUS_DUAL_WIFI_COMPLETE && status().split && !status().cleanup_pending);
    assert(requested_band == CERBERUS_WIFI_SCAN_BAND_5G && local_starts == 1 && remote_starts == 1);
    assert(status().s3_count == 32 && status().c5_count == 32 && status().s3_total == 40 && status().c5_total == 40);
    verify_retained(1, 64);
    for (uint8_t i = 0; i < 64; ++i) {
        cerberus_dual_wifi_row_t row; assert(cerberus_dual_wifi_scan_get_row(i, &row) == ESP_OK);
        assert(row.source == (i < 32 ? CERBERUS_DUAL_WIFI_SOURCE_S3 : CERBERUS_DUAL_WIFI_SOURCE_C5));
        assert(row.source_index == i % 32 && row.observation.valid == (i >= 32));
        if (i < 32) assert(row.received_ms == local_complete_at);
    }
    cerberus_dual_wifi_row_t selected, retained;
    assert(cerberus_dual_wifi_scan_get_row(4, &selected) == ESP_OK); retained = selected;
    remote_rows[7] = selected.ap; /* Fresh C5 bank has an independently obtained index. */
    assert(cerberus_dual_wifi_scan_begin_rebind(&selected) == ESP_OK); drain();
    assert(status().rebind_ready && !status().rebinding && !status().cleanup_pending);
    assert(status().rebind_source == CERBERUS_DUAL_WIFI_SOURCE_S3 && !status().rebind_cached);
    assert(local_starts == 1 && requested_band == CERBERUS_WIFI_SCAN_BAND_ALL); verify_retained(1, 64);
    assert(cerberus_dual_wifi_scan_get_row(4, &selected) == ESP_OK && !memcmp(&selected, &retained, sizeof(selected)));
    cerberus_c5_wifi_result_t rows[32]; cerberus_s3_wifi_observation_t observations[32];
    cerberus_wifi_scan_status_payload_t bank; cerberus_c5_wifi_relation_bank_t proof; uint8_t selected_index; cerberus_dual_wifi_handoff_t handoff;
    assert(cerberus_dual_wifi_scan_copy_legacy_bank(rows, observations, 32, &bank, &proof, &selected_index, &handoff) == ESP_OK);
    assert(!handoff.reused && handoff.band == CERBERUS_WIFI_SCAN_BAND_ALL && handoff.received_us > 0 && handoff.session_id == epoch);
    assert(selected_index == 7 && observations[7].valid && !memcmp(&rows[7], &selected.ap, sizeof(rows[7])));
    ++epoch; /* Copy fence must not export a replaced lease even with gen8 unchanged. */
    assert(cerberus_dual_wifi_scan_copy_legacy_bank(rows, observations, 32, &bank, &proof, &selected_index, &handoff) == ESP_ERR_NOT_FOUND);
    assert(!status().rebind_ready && !bank.stored_count && !observations[7].valid);
    assert(cerberus_dual_wifi_scan_start() == ESP_OK); cerberus_dual_wifi_scan_stop();
    assert(!dual_active() && !status().cleanup_pending); verify_retained(1, 64);
    store->status.generation = UINT32_MAX; unsigned old_starts = remote_starts;
    assert(cerberus_dual_wifi_scan_start() == ESP_ERR_INVALID_STATE && remote_starts == old_starts);

    reset(); local_start_error = ESP_ERR_INVALID_STATE;
    assert(cerberus_dual_wifi_scan_start() == ESP_OK); drain();
    assert(status().fallback && !status().split && requested_band == CERBERUS_WIFI_SCAN_BAND_ALL);
    verify_retained(1, 32); assert(!local_stops);

    reset(); start_reply_error = ESP_ERR_TIMEOUT;
    assert(cerberus_dual_wifi_scan_start() == ESP_OK); drain();
    assert(status().state == CERBERUS_DUAL_WIFI_COMPLETE && remote_starts == 1 && !remote_stops);
    assert(status().c5_error == ESP_OK); verify_retained(1, 64);
    reset(); start_reply_error = ESP_ERR_INVALID_RESPONSE;
    assert(cerberus_dual_wifi_scan_start() == ESP_OK); drain(); verify_retained(1, 64);
    assert(remote_starts == 1 && !remote_stops);
    reset(); start_reply_error = ESP_ERR_TIMEOUT; reject_start = true;
    assert(cerberus_dual_wifi_scan_start() == ESP_OK); drain();
    assert(status().state == CERBERUS_DUAL_WIFI_ERROR && !remote_stops && local_stops == 1); verify_retained(0, 0);

    reset(); assert(cerberus_dual_wifi_scan_start() == ESP_OK); until_phase(DUAL_PHASE_ROWS);
    cerberus_dual_wifi_scan_stop(); drain(); verify_retained(0, 0);
    assert(!remote_stops && !status().cleanup_pending);
    reset(); assert(cerberus_dual_wifi_scan_start() == ESP_OK); until_phase(DUAL_PHASE_WAIT);
    ++epoch; /* Foreign scan may be RUNNING; our stale Stop must never reach it. */
    cerberus_dual_wifi_scan_stop(); drain(); assert(!remote_stops && local_stops == 1);

    reset(); local_complete_at = UINT32_MAX; fail_local_cleanup = true; keep_remote_running = true;
    assert(cerberus_dual_wifi_scan_start() == ESP_OK); until_phase(DUAL_PHASE_WAIT);
    cerberus_dual_wifi_scan_stop(); for (unsigned i = 0; i < 30; ++i) tick();
    assert(status().cleanup_pending && status().s3_error == ESP_FAIL && remote_stops == 1);
    assert(cerberus_dual_wifi_scan_start() == ESP_ERR_INVALID_STATE);

    reset(); replace_local_on_row = true;
    assert(cerberus_dual_wifi_scan_start() == ESP_OK); drain();
    assert(status().state == CERBERUS_DUAL_WIFI_ERROR && !local_stops); verify_retained(0, 0);
    reset(); replace_bank_on_proof = true;
    assert(cerberus_dual_wifi_scan_start() == ESP_OK); drain();
    assert(status().state == CERBERUS_DUAL_WIFI_ERROR); verify_retained(0, 0);
    reset(); change_status_fence = true;
    assert(cerberus_dual_wifi_scan_start() == ESP_OK); drain();
    assert(status().state == CERBERUS_DUAL_WIFI_ERROR); verify_retained(0, 0);
    reset(); row_error = ESP_ERR_INVALID_RESPONSE;
    assert(cerberus_dual_wifi_scan_start() == ESP_OK); drain(); verify_retained(0, 0);
    assert(status().state == CERBERUS_DUAL_WIFI_ERROR);

    reset(); assert(cerberus_dual_wifi_scan_start() == ESP_OK); drain();
    assert(cerberus_dual_wifi_scan_get_row(0, &selected) == ESP_OK);
    assert(cerberus_dual_wifi_scan_begin_rebind(&selected) == ESP_OK); drain();
    assert(status().error == ESP_ERR_NOT_FOUND && !status().rebind_ready && !status().rebinding); verify_retained(1, 64);
    remote_rows[2] = remote_rows[3] = selected.ap;
    assert(cerberus_dual_wifi_scan_begin_rebind(&selected) == ESP_OK); drain();
    assert(status().error == ESP_ERR_INVALID_RESPONSE && !status().rebind_ready); verify_retained(1, 64);

    check_cached_handoff();
    check_archive_publication();
    printf("Coordinator PSRAM model: %zu bytes; status %zu; handoff %zu\n", sizeof(dual_wifi_store_t), sizeof(cerberus_dual_wifi_status_t), sizeof(cerberus_dual_wifi_handoff_t));
    puts("Dual Wi-Fi coordinator: split64, fallback, provenance, lost START recovery, owned cleanup, stale fences, retained banks and exact AP rebind passed.");
    free(store); return 0;
}
