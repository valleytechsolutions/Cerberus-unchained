/* Private shared-discovery integration. UI task owns this on-demand PSRAM store;
 * producer callbacks only touch the separately locked scanner snapshot. */
#include "s3_ble_discovery_inventory.h"
#include "s3_ble_discovery_session.h"
#include "ble_discovery_list.h"
#include "dual_ble_family.h"

_Static_assert((int)CERBERUS_DUAL_BLE_FAMILY_IBEACON == (int)CERBERUS_S3_BLE_FOCUS_IBEACON &&
    (int)CERBERUS_DUAL_BLE_FAMILY_FIND_MY == (int)CERBERUS_S3_BLE_FOCUS_FIND_MY &&
    (int)CERBERUS_DUAL_BLE_FAMILY_FLIPPER == (int)CERBERUS_S3_BLE_FOCUS_FLIPPER &&
    (int)CERBERUS_DUAL_BLE_FAMILY_FLOCK == (int)CERBERUS_S3_BLE_FOCUS_FLOCK &&
    (int)CERBERUS_DUAL_BLE_FAMILY_META_HINT == (int)CERBERUS_S3_BLE_FOCUS_META_HINT &&
    (int)CERBERUS_DUAL_BLE_FAMILY_HC_NAME_HINT == (int)CERBERUS_S3_BLE_FOCUS_HC_NAME_HINT,
    "Receiver family category mapping");
_Static_assert(CERBERUS_S3_BLE_FILTER_ALL == 0 && CERBERUS_S3_BLE_FILTER_IBEACON == 1 &&
    CERBERUS_S3_BLE_FILTER_FIND_MY == 2 && CERBERUS_S3_BLE_FILTER_FLIPPER == 3 &&
    CERBERUS_S3_BLE_FILTER_FLOCK == 4 && CERBERUS_S3_BLE_FILTER_META_HINT == 5 &&
    CERBERUS_S3_BLE_FILTER_HC_NAME_HINT == 6,
    "C5 focused-family wire selector mapping");

typedef struct {
    cerberus_ble_discovery_session_t session;
    cerberus_ble_discovery_ops_t ops;
    cerberus_s3_ble_discovery_inventory_t inventory;
    cerberus_s3_ble_discovery_bank_t staging;
    cerberus_s3_ble_discovery_entry_t painted[4], selected;
    /* Only four displayed C5 proofs, not a third observation bank. All of
     * this optional presentation state shares the existing PSRAM lifetime. */
    cerberus_dual_ble_row_t painted_c5[4];
    cerberus_dual_ble_provenance_t painted_c5_provenance[4];
    cerberus_ble_discovery_list_ref_t order[CERBERUS_BLE_DISCOVERY_LIST_CAPACITY];
    cerberus_ble_discovery_list_source_t source_filter;
    uint8_t painted_source[4], built_count;
    uint64_t order_s3_epoch, order_s3_revision, order_c5_boot, order_c5_epoch;
    uint32_t order_c5_snapshots, order_s3_mask, order_c5_mask;
    uint32_t c5_classifiable, c5_oui_only, monitor_s3_valid;
    uint8_t c5_family_bits[CERBERUS_DUAL_BLE_CAPACITY];
    uint8_t c5_response_bits[CERBERUS_DUAL_BLE_CAPACITY];
    bool c5_family_cache_valid, monitor_s3_cache_valid;
    uint8_t order_s3_count, order_c5_count, order_count, order_sort, order_source, order_family;
    bool order_valid;
    live_scan_record_t presentation;
    uint64_t last_draw_ms;
    uint16_t binding_overflows, producer_replacements;
    uint8_t painted_count, selected_source;
    uint8_t destination; /* 1: tools, 2: Find My monitor, after release only. */
    bool selection_pending, visible, dirty;
} ble_discovery_live_store_t;

static ble_discovery_live_store_t *ble_discovery_live;
static esp_err_t ble_discovery_open_error;
/* User intent survives either receiver failure, including optional-store OOM. */
static bool ble_discovery_run_requested;
/* Only used when the optional shared store could not be allocated. */
static uint8_t ble_discovery_unallocated_destination;
static uint64_t ble_discovery_unallocated_last_draw_ms;
static void live_make_view(void);
static esp_err_t live_paint(bool full);

static bool ble_discovery_monitor_page(void)
{
    return !ui_capability_blocked && current_page == UI_PAGE_BLE &&
        ble_mode == UI_BLE_MODE_FINDMY_MONITOR && !ble_attack_selecting;
}

static cerberus_s3_ble_filter_t ble_discovery_family(void)
{
    return ble_discovery_monitor_page() ? CERBERUS_S3_BLE_FILTER_FIND_MY : ble_filter;
}

static bool ble_discovery_ordinary_page(void)
{
    return !ui_capability_blocked && current_page == UI_PAGE_BLE &&
        (ble_mode == UI_BLE_MODE_LIST || ble_mode == UI_BLE_MODE_C5) && !ble_attack_selecting;
}

static bool ble_discovery_requested(void)
{
    return ble_discovery_run_requested;
}

static bool ble_discovery_page(void)
{
    return ble_discovery_ordinary_page() || ble_discovery_monitor_page();
}

static bool ble_discovery_presenting(void)
{
    return ble_discovery_page() && ble_mode == UI_BLE_MODE_LIST && ble_discovery_live != NULL;
}

static bool ble_discovery_unified(void)
{
    /* Each receiver supplies its own family proof; the combined references
     * never turn C5 original hints into S3 history or response eligibility. */
    return ble_discovery_presenting();
}

static bool ble_discovery_keeps_awake(void)
{
    return ble_discovery_page() && (ble_discovery_requested() ||
        (ble_discovery_live && (ble_discovery_live->session.enabled || ble_discovery_live->session.epoch != 0U)));
}

/* The local BLE list must not wait for optional, synchronous SPI telemetry.
 * Any outstanding C5 work is still polled by its owning live session. */
static bool ble_discovery_defers_c5_telemetry(void)
{
    return ble_discovery_page();
}

/* Intent cancellation is pure: the existing global ticks drain each owned
 * radio. Pending frozen selection/destination survives until that drain ends. */
static void ble_discovery_request_stop(void)
{
    ble_discovery_run_requested = false;
    if (ble_discovery_live)
        cerberus_ble_discovery_session_stop(&ble_discovery_live->session);
    dual_ble_ui_cancel();
}

static void ble_discovery_cancel_navigation(void)
{
    dual_ble_ui_cancel_selection();
    ble_discovery_unallocated_destination = 0U;
    if (!ble_discovery_live) return;
    ble_discovery_live->selection_pending = false;
    ble_discovery_live->selected_source = ble_discovery_live->destination = 0U;
    ble_discovery_live->painted_count = 0U;
}

static void ble_discovery_leave(void)
{
    ble_discovery_request_stop();
    ble_discovery_cancel_navigation();
}

static void ble_discovery_request_destination(uint8_t destination)
{
    if (!ble_discovery_ordinary_page() || destination < 1U || destination > 2U) return;
    dual_ble_ui_cancel_selection();
    ble_discovery_unallocated_destination = ble_discovery_live ? 0U : destination;
    if (ble_discovery_live) {
        ble_discovery_live->selection_pending = false;
        ble_discovery_live->selected_source = ble_discovery_live->painted_count = 0U;
        ble_discovery_live->destination = destination;
    }
    ble_discovery_request_stop();
}

_Static_assert(CERBERUS_BLE_DISCOVERY_FAMILY_COUNT == CERBERUS_S3_BLE_FILTER_COUNT,
    "Discovery family mapping");
_Static_assert((int)CERBERUS_BLE_DISCOVERY_RUNNING == (int)CERBERUS_BLE_SCAN_RUNNING &&
    (int)CERBERUS_BLE_DISCOVERY_COMPLETE == (int)CERBERUS_BLE_SCAN_COMPLETE &&
    (int)CERBERUS_BLE_DISCOVERY_ERROR == (int)CERBERUS_BLE_SCAN_ERROR, "Discovery state mapping");

static cerberus_ble_discovery_operation_t ble_discovery_operation(esp_err_t error)
{
    return (cerberus_ble_discovery_operation_t){error, error == ESP_ERR_NOT_FOUND};
}

static bool ble_discovery_observation(const cerberus_s3_ble_discovery_meta_t *meta,
    cerberus_ble_discovery_observation_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!meta->attached || !meta->scan_epoch ||
        meta->family >= CERBERUS_S3_BLE_FILTER_COUNT) return false;
    *out = (cerberus_ble_discovery_observation_t){
        .scan_epoch = meta->scan_epoch, .revision = meta->revision,
        .error = meta->status.error_code, .family = (uint8_t)meta->family,
        .state = meta->status.state, .owned = meta->owned, .stopping = meta->stopping};
    return true;
}

static cerberus_ble_discovery_operation_t ble_discovery_start(void *context,
    uint8_t family, uint64_t previous, cerberus_ble_discovery_observation_t *out)
{
    (void)context;
    cerberus_s3_ble_discovery_meta_t meta = {0};
    esp_err_t error = cerberus_s3_ble_scanner_start_discovery(
        (cerberus_s3_ble_filter_t)family, previous, &meta);
    if (!ble_discovery_observation(&meta, out) && error == ESP_OK)
        error = ESP_ERR_INVALID_RESPONSE;
    return ble_discovery_operation(error);
}

static cerberus_ble_discovery_operation_t ble_discovery_poll(void *context,
    uint64_t epoch, cerberus_ble_discovery_observation_t *out)
{
    (void)context;
    cerberus_s3_ble_discovery_meta_t meta = {0};
    esp_err_t error = cerberus_s3_ble_scanner_discovery_status(epoch, &meta);
    if (!ble_discovery_observation(&meta, out) && error == ESP_OK)
        error = ESP_ERR_INVALID_RESPONSE;
    return ble_discovery_operation(error);
}

static cerberus_ble_discovery_operation_t ble_discovery_stop(void *context,
    uint64_t epoch, cerberus_ble_discovery_observation_t *out)
{
    (void)context;
    cerberus_s3_ble_discovery_meta_t meta = {0};
    esp_err_t error = cerberus_s3_ble_scanner_stop_discovery(epoch, &meta);
    if (!ble_discovery_observation(&meta, out) && error == ESP_OK)
        error = ESP_ERR_INVALID_RESPONSE;
    return ble_discovery_operation(error);
}

static cerberus_ble_discovery_operation_t ble_discovery_release(void *context,
    uint64_t epoch)
{
    (void)context;
    return ble_discovery_operation(cerberus_s3_ble_scanner_release_discovery(epoch));
}

static cerberus_ble_discovery_operation_t ble_discovery_copy(void *context,
    uint64_t epoch, uint8_t family, cerberus_ble_discovery_observation_t *out)
{
    ble_discovery_live_store_t *store = context;
    esp_err_t error = cerberus_s3_ble_scanner_copy_discovery(epoch, &store->staging);
    const cerberus_s3_ble_discovery_meta_t *meta = &store->staging.meta;
    memset(out, 0, sizeof(*out));
    if (error != ESP_OK) return ble_discovery_operation(error);
    if (meta->scan_epoch != epoch || meta->family != family ||
        !ble_discovery_observation(meta, out) ||
        !cerberus_s3_ble_discovery_bank_valid(&store->staging))
        return ble_discovery_operation(ESP_ERR_INVALID_RESPONSE);
    if (store->inventory.family != meta->family)
        (void)cerberus_s3_ble_discovery_inventory_init(&store->inventory, meta->family);
    cerberus_s3_ble_discovery_apply_t applied =
        cerberus_s3_ble_discovery_inventory_apply(&store->inventory, &store->staging);
    if (applied == CERBERUS_BLE_INVENTORY_INVALID)
        return ble_discovery_operation(ESP_ERR_INVALID_RESPONSE);
    if (applied == CERBERUS_BLE_INVENTORY_APPLIED) {
        store->presentation.captured = true;
        store->presentation.saved_generation = meta->status.generation;
        store->presentation.captured_ms = (uint32_t)meta->source_now_ms;
        store->presentation.count = store->inventory.count;
        store->binding_overflows = meta->binding_overflows;
        store->producer_replacements = meta->replacements;
        store->dirty = live_ble_order_dirty = true;
    }
    return ble_discovery_operation(ESP_OK);
}

static bool ble_discovery_acquire(void)
{
    if (ble_discovery_live) return true;
    if (ble_discovery_open_error != ESP_OK) return false;
    ble_discovery_live = heap_caps_calloc(1U, sizeof(*ble_discovery_live),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ble_discovery_live) {
        ble_discovery_open_error = ESP_ERR_NO_MEM;
        live_records[SENTINEL_SCAN_BLE].error = ESP_ERR_NO_MEM;
        live_records[SENTINEL_SCAN_BLE].valid = true;
        return false;
    }
    ble_discovery_open_error = ESP_OK;
    cerberus_ble_discovery_session_init(&ble_discovery_live->session, (uint8_t)ble_discovery_family());
    (void)cerberus_s3_ble_discovery_inventory_init(&ble_discovery_live->inventory, ble_discovery_family());
    ble_discovery_live->ops = (cerberus_ble_discovery_ops_t){
        .context = ble_discovery_live, .start = ble_discovery_start,
        .poll = ble_discovery_poll, .copy = ble_discovery_copy,
        .stop = ble_discovery_stop, .release = ble_discovery_release};
    ble_discovery_live->presentation.valid = true;
    return true;
}

static esp_err_t ble_discovery_request_start(void)
{
    if (!ble_discovery_ordinary_page()) return ESP_ERR_INVALID_STATE;
    /* A new explicit run also retries C5, even if S3 allocation fails. */
    dual_ble_ui_retry();
    ble_discovery_run_requested = true;
    ble_discovery_unallocated_destination = 0U;
    ble_discovery_open_error = ESP_OK;
    dual_ble_ui_cancel_selection();
    if (!ble_discovery_acquire()) return ble_discovery_open_error;
    ble_discovery_live_store_t *store = ble_discovery_live;
    store->selection_pending = false;
    store->selected_source = store->destination = store->painted_count = 0U;
    (void)cerberus_ble_discovery_session_select(&store->session, (uint8_t)ble_discovery_family());
    (void)cerberus_ble_discovery_session_start(&store->session);
    store->dirty = true;
    return ESP_OK;
}

static bool ble_discovery_s3_retryable(void)
{
    return ble_discovery_requested() && ble_discovery_ordinary_page() &&
        (ble_discovery_open_error != ESP_OK || (ble_discovery_live &&
         ble_discovery_live->session.faulted));
}

static esp_err_t ble_discovery_retry_s3(void)
{
    if (!ble_discovery_s3_retryable()) return ESP_ERR_INVALID_STATE;
    ble_discovery_open_error = ESP_OK;
    if (!ble_discovery_acquire()) return ble_discovery_open_error;
    (void)cerberus_ble_discovery_session_start(&ble_discovery_live->session);
    ble_discovery_live->dirty = true;
    return ESP_OK;
}

/* Monitor intent is controlled separately by its page. Allocation and return
 * to retained results must never implicitly arm either receiver. */
static esp_err_t ble_discovery_monitor_start(void)
{
    if (!ble_discovery_monitor_page()) return ESP_ERR_INVALID_STATE;
    ble_discovery_run_requested = false;
    ble_discovery_unallocated_destination = 0U;
    ble_discovery_open_error = ESP_OK;
    if (!ble_discovery_acquire()) return ble_discovery_open_error;
    ble_discovery_live_store_t *store = ble_discovery_live;
    store->selection_pending = false;
    store->selected_source = store->destination = store->painted_count = 0U;
    (void)cerberus_ble_discovery_session_select(&store->session, CERBERUS_S3_BLE_FILTER_FIND_MY);
    (void)cerberus_ble_discovery_session_start(&store->session);
    store->dirty = true;
    return ESP_OK;
}

/* Freeze the last successfully painted proof before requesting owner release.
 * Existing selected slots stay independent of subsequent bank replacement. */
static bool ble_discovery_monitor_freeze_selection(unsigned row)
{
    ble_discovery_live_store_t *store = ble_discovery_live;
    if (!ble_discovery_monitor_page() || !store || row >= store->painted_count || row >= 4U)
        return false;
    uint8_t source = store->painted_source[row];
    if (source == CERBERUS_BLE_DISCOVERY_LIST_S3)
        store->selected = store->painted[row];
    else if (source == CERBERUS_BLE_DISCOVERY_LIST_C5)
        dual_ble_ui_select_frozen_from(&store->painted_c5[row], &store->painted_c5_provenance[row], true);
    else return false;
    store->selected_source = source;
    store->painted_count = 0U;
    store->selection_pending = false;
    store->destination = 0U;
    cerberus_ble_discovery_session_stop(&store->session);
    return true;
}

static bool ble_discovery_monitor_apply_selection(void)
{
    ble_discovery_live_store_t *store = ble_discovery_live;
    if (!ble_discovery_monitor_page() || !store ||
        !cerberus_ble_discovery_session_released(&store->session) || !dual_ble_ui_released())
        return false;
    if (store->selected_source == CERBERUS_BLE_DISCOVERY_LIST_C5) {
        ble_mode = UI_BLE_MODE_C5;
    } else if (store->selected_source == CERBERUS_BLE_DISCOVERY_LIST_S3) {
        const cerberus_s3_ble_discovery_record_t *proof = &store->selected.record;
        cerberus_findmy_monitor_record_t frozen = {
            .peer = proof->peer, .advertisement = proof->advertisement,
            .focus = proof->focus, .generation = proof->generation,
            .index = proof->index, .observed_ms = proof->observed_ms};
        ble_select_frozen_live(&frozen);
        ble_selected_from_monitor = true;
        ble_selected_original_time64_valid = true;
        ble_selected_scan_epoch = store->selected.epoch;
        ble_selected_original_ms64 = store->selected.original_source_ms;
        ble_selected_source_ceiling_ms64 = store->selected.source_now_ms;
    } else return false;
    store->selected_source = 0U;
    return true;
}

static void ble_discovery_select_painted(unsigned row)
{
    ble_discovery_live_store_t *store = ble_discovery_live;
    if (!store || !ble_discovery_presenting() || store->selection_pending || store->destination ||
        row >= store->painted_count || row >= 4U) return;
    uint8_t source = store->painted_source[row];
    if (source == CERBERUS_BLE_DISCOVERY_LIST_C5)
        dual_ble_ui_select_frozen(&store->painted_c5[row], &store->painted_c5_provenance[row]);
    else if (source == CERBERUS_BLE_DISCOVERY_LIST_S3)
        store->selected = store->painted[row];
    else return;
    store->selected_source = source;
    store->selection_pending = true;
    store->painted_count = store->destination = 0U;
    ble_discovery_request_stop();
    store->dirty = true;
}

static void ble_discovery_toggle(void)
{
    if (ble_discovery_requested() || !dual_ble_ui_released() ||
        (ble_discovery_live && !cerberus_ble_discovery_session_released(&ble_discovery_live->session)))
        ble_discovery_leave();
    else (void)ble_discovery_request_start();
}

/* An unavailable S3 store cannot own the repaint clock. Keep only this small
 * UI timestamp outside PSRAM so cached C5 progress remains visible. Painting
 * does not allocate the shared store or advance either radio controller. */
static void ble_discovery_paint_unallocated(uint64_t now_ms)
{
    if (!ble_discovery_ordinary_page() || ble_mode != UI_BLE_MODE_LIST || ble_discovery_live ||
        now_ms - ble_discovery_unallocated_last_draw_ms < 200U) return;
    ble_discovery_unallocated_last_draw_ms = now_ms;
    ++live_frame;
    live_make_view();
    ESP_ERROR_CHECK(live_paint(false));
}

static void ble_discovery_tick(uint64_t now_ms, bool asleep)
{
    bool visible = ble_discovery_page() && !asleep;
    /* A hidden ordinary request must be canceled even when its allocation
     * failed. Do not cancel a monitor/Fox/Analyzer request that replaced it. */
    if ((!ble_discovery_ordinary_page() || asleep) &&
        (ble_discovery_requested() || ble_discovery_unallocated_destination || (ble_discovery_live &&
         (ble_discovery_live->selection_pending || ble_discovery_live->destination))))
        ble_discovery_leave();
    if (visible && ble_discovery_unallocated_destination) {
        if (dual_ble_ui_released() && (!ble_discovery_live ||
            cerberus_ble_discovery_session_released(&ble_discovery_live->session))) {
            uint8_t destination = ble_discovery_unallocated_destination;
            ble_discovery_unallocated_destination = 0U;
            if (destination == 2U) ble_findmy_monitor_open();
            else ble_mode = UI_BLE_MODE_MENU;
            ESP_ERROR_CHECK(draw_current_page());
        } else ble_discovery_paint_unallocated(now_ms);
        return;
    }
    if (visible && !ble_discovery_acquire()) {
        ble_discovery_paint_unallocated(now_ms);
        return;
    }
    ble_discovery_live_store_t *store = ble_discovery_live;
    if (!store) return;
    if (!visible) {
        store->selection_pending = false;
        store->destination = 0U;
        cerberus_ble_discovery_session_stop(&store->session);
    } else if (store->session.requested_family != (uint8_t)ble_discovery_family()) {
        store->selection_pending = false;
        store->destination = 0U;
        (void)cerberus_ble_discovery_session_select(&store->session, (uint8_t)ble_discovery_family());
        store->dirty = true;
    }
    if (ble_discovery_monitor_page() && !ble_findmy_monitor_requested())
        cerberus_ble_discovery_session_stop(&store->session);
    store->visible = visible;
    /* First drain any legacy Watch sweep. Local discovery does not require a
     * successful C5 poll, and it never adopts a generic scanner's claim. */
    if (store->session.epoch || (live_ble_session.active == SENTINEL_SCAN_NONE && live_ble_recon_released()))
        cerberus_ble_discovery_session_tick(&store->session, now_ms, &store->ops);
    store->presentation.valid = true;
    store->presentation.error = store->session.error;
    store->presentation.state = store->session.enabled ||
        !cerberus_ble_discovery_session_released(&store->session) ?
        SENTINEL_SCAN_RUNNING : SENTINEL_SCAN_COMPLETE;
    if (visible && ble_discovery_presenting() && store->selection_pending &&
        cerberus_ble_discovery_session_released(&store->session) && dual_ble_ui_released()) {
        if (store->selected_source == CERBERUS_BLE_DISCOVERY_LIST_C5) {
            store->selection_pending = false;
            store->selected_source = 0U;
            ble_mode = UI_BLE_MODE_C5;
            ESP_ERROR_CHECK(draw_current_page());
            return;
        }
        const cerberus_s3_ble_discovery_record_t *proof = &store->selected.record;
        cerberus_findmy_monitor_record_t frozen = {
            .peer = proof->peer, .advertisement = proof->advertisement,
            .focus = proof->focus, .generation = proof->generation,
            .index = proof->index, .observed_ms = proof->observed_ms};
        store->selection_pending = false;
        store->selected_source = 0U;
        ble_select_frozen_live(&frozen);
        ble_selected_original_time64_valid = true;
        ble_selected_scan_epoch = store->selected.epoch;
        ble_selected_original_ms64 = store->selected.original_source_ms;
        ble_selected_source_ceiling_ms64 = store->selected.source_now_ms;
        return;
    }
    if (visible && ble_discovery_presenting() && store->destination &&
        cerberus_ble_discovery_session_released(&store->session) && dual_ble_ui_released()) {
        uint8_t destination = store->destination;
        store->destination = 0U;
        if (destination == 2U) ble_findmy_monitor_open();
        else ble_mode = UI_BLE_MODE_MENU;
        ESP_ERROR_CHECK(draw_current_page());
        return;
    }
    if (visible && ble_discovery_presenting() && (store->dirty || now_ms - store->last_draw_ms >= 200U)) {
        store->last_draw_ms = now_ms;
        ++live_frame;
        live_make_view();
        ESP_ERROR_CHECK(live_paint(false));
        store->dirty = false;
    }
}

/* The monitor horizon follows the actual hint's full source clock. It never
 * reinterprets a fresh copy time, a 32-bit wrap, or an old category as a hint. */
static bool ble_discovery_monitor_hint_recent(
    const cerberus_s3_ble_discovery_entry_t *entry, uint64_t now_ms)
{
    if (!entry || !entry->epoch || now_ms < entry->source_now_ms ||
        entry->source_now_ms < entry->original_source_ms ||
        entry->source_now_ms - entry->original_source_ms > INT32_MAX) return false;
    const cerberus_s3_ble_discovery_evidence_t *hints = &entry->record.focus.discovery;
    const uint32_t observed[2] = {hints->advertisement_observed_ms, hints->scan_response_observed_ms};
    const uint8_t bits[2] = {hints->findmy_advertisement.bits, hints->findmy_scan_response.bits};
    for (unsigned i = 0U; i < 2U; ++i) {
        if (!bits[i]) continue;
        uint32_t delta = (uint32_t)entry->source_now_ms - observed[i];
        if (entry->source_now_ms < delta) continue;
        uint64_t hint_ms = entry->source_now_ms - delta;
        if (hint_ms >= entry->original_source_ms && hint_ms <= now_ms &&
            now_ms - hint_ms < CERBERUS_FINDMY_MONITOR_EXPIRE_MS) return true;
    }
    return false;
}

static uint32_t ble_discovery_visible_mask(uint64_t now_ms)
{
    const cerberus_s3_ble_filter_t family = ble_discovery_family();
    if (!ble_discovery_live) return 0U;
    uint32_t mask = 0U;
    const cerberus_s3_ble_discovery_inventory_t *inventory = &ble_discovery_live->inventory;
    for (uint8_t i = 0U; i < inventory->count; ++i) {
        const cerberus_s3_ble_discovery_entry_t *entry = &inventory->entries[i];
        uint8_t ignored;
        if (!cerberus_s3_ble_focus_page(&entry->record.focus.category_bits, 1U,
                family, 0U, &ignored, 1U)) continue;
        if (ble_discovery_monitor_page() &&
            (!(ble_discovery_live->monitor_s3_valid & (UINT32_C(1) << i)) ||
             !ble_discovery_monitor_hint_recent(entry, now_ms))) continue;
        if (!ble_discovery_monitor_page() && live_ble_recent_only && cerberus_s3_ble_recent_supported(family) &&
            !cerberus_s3_ble_discovery_entry_recent(entry, family, now_ms)) continue;
        mask |= UINT32_C(1) << i;
    }
    return mask;
}

static uint8_t ble_discovery_combined_order(void)
{
    const cerberus_s3_ble_filter_t family = ble_discovery_family();
    ble_discovery_live_store_t *store = ble_discovery_live;
    if (!store) return 0U;
    const cerberus_s3_ble_discovery_inventory_t *s3 =
        store->inventory.family == family ? &store->inventory : NULL;
    const cerberus_dual_ble_scan_t *c5 = dual_ble_ui_store;
    uint64_t epoch = s3 ? s3->epoch : 0U, revision = s3 ? s3->revision : 0U;
    uint64_t boot = c5 ? c5->published_boot_id : 0U, c5_epoch = c5 ? c5->published_epoch : 0U;
    uint32_t snapshots = c5 ? c5->snapshots : 0U;
    uint8_t s3_count = s3 ? s3->count : 0U;
    uint8_t c5_count = c5 && c5->count <= CERBERUS_DUAL_BLE_CAPACITY ? c5->count : 0U;
    if (!ble_discovery_monitor_page()) store->monitor_s3_cache_valid = false;
    else if (!store->monitor_s3_cache_valid || !store->order_valid || revision == UINT64_MAX ||
        store->order_s3_epoch != epoch || store->order_s3_revision != revision ||
        store->order_s3_count != s3_count) {
        store->monitor_s3_valid = 0U;
        for (uint8_t i = 0U; i < s3_count; ++i) {
            const cerberus_s3_ble_discovery_entry_t *entry = &s3->entries[i];
            const cerberus_s3_ble_discovery_record_t *record = &entry->record;
            if (entry->epoch && (uint32_t)entry->original_source_ms == record->observed_ms &&
                entry->source_now_ms >= entry->original_source_ms &&
                entry->source_now_ms - entry->original_source_ms <= INT32_MAX &&
                cerberus_findmy_monitor_evidence_valid(&record->peer, &record->advertisement,
                    &record->focus, record->generation, record->observed_ms,
                    (uint32_t)entry->source_now_ms))
                store->monitor_s3_valid |= UINT32_C(1) << i;
        }
        store->monitor_s3_cache_valid = true;
    }
    bool c5_changed = !store->order_valid || snapshots == UINT32_MAX ||
        store->order_c5_boot != boot || store->order_c5_epoch != c5_epoch ||
        store->order_c5_snapshots != snapshots || store->order_c5_count != c5_count;
    if (c5_changed) store->c5_family_cache_valid = false;
    if (family != CERBERUS_S3_BLE_FILTER_ALL && !store->c5_family_cache_valid) {
        store->c5_classifiable = store->c5_oui_only = 0U;
        memset(store->c5_family_bits, 0, sizeof(store->c5_family_bits));
        memset(store->c5_response_bits, 0, sizeof(store->c5_response_bits));
        for (uint8_t i = 0U; i < c5_count; ++i) {
            cerberus_dual_ble_family_evidence_t evidence;
            const cerberus_dual_ble_provenance_t provenance = {
                .boot_id = boot, .epoch = c5_epoch, .generation = c5->published_generation, .index = i};
            if (cerberus_dual_ble_family_classify(&c5->rows[i], &provenance, &evidence)) {
                store->c5_classifiable |= UINT32_C(1) << i;
                store->c5_family_bits[i] = evidence.categories;
                if (evidence.flock_oui && !cerberus_ble_flock_hints_level(&evidence.flock))
                    store->c5_oui_only |= UINT32_C(1) << i;
            }
            if (cerberus_dual_ble_response_classify(&c5->rows[i], &provenance, &evidence)) {
                store->c5_classifiable |= UINT32_C(1) << i;
                store->c5_response_bits[i] = evidence.categories;
            }
        }
        store->c5_family_cache_valid = true;
    }
    const uint64_t now_ms = (uint64_t)(esp_timer_get_time() / 1000LL);
    uint32_t s3_mask = s3 ? ble_discovery_visible_mask(now_ms) : 0U, c5_mask = 0U;
    uint8_t wanted = family == CERBERUS_S3_BLE_FILTER_ALL ? 0U :
        (uint8_t)(1U << ((unsigned)family - 1U));
    for (uint8_t i = 0U; i < c5_count; ++i) {
        if (wanted && !((store->c5_family_bits[i] | store->c5_response_bits[i]) & wanted)) continue;
        if (!ble_discovery_monitor_page() && live_ble_recent_only && cerberus_s3_ble_recent_supported(family)) {
            uint32_t upper;
            bool recent_adv = (!wanted || (store->c5_family_bits[i] & wanted)) &&
                cerberus_dual_ble_list_age(&c5->rows[i], now_ms, &upper) && upper <= CERBERUS_DUAL_BLE_RECENT_MS;
            bool recent_rsp = wanted && (store->c5_response_bits[i] & wanted) &&
                cerberus_dual_ble_response_age(&c5->rows[i], now_ms, &upper) && upper <= CERBERUS_DUAL_BLE_RECENT_MS;
            if (!recent_adv && !recent_rsp) continue;
        }
        c5_mask |= UINT32_C(1) << i;
    }
    /* Classify only committed source changes. Age-only repaints recompute
     * masks, never raw hints; a saturated publication counter cannot prove an
     * unchanged bank and deliberately forces conservative cache rebuilding. */
    if (!store->order_valid || c5_changed || revision == UINT64_MAX ||
        store->order_s3_epoch != epoch || store->order_s3_revision != revision ||
        store->order_s3_count != s3_count || store->order_sort != (uint8_t)live_ble_sort ||
        store->order_source != (uint8_t)store->source_filter || store->order_family != (uint8_t)family ||
        store->order_s3_mask != s3_mask || store->order_c5_mask != c5_mask) {
        store->order_count = cerberus_ble_discovery_list_order_masked(s3, c5 ? c5->rows : NULL, c5_count,
            store->source_filter,
            live_ble_sort == CERBERUS_S3_BLE_SORT_SIGNAL_DESC ? CERBERUS_BLE_DISCOVERY_LIST_SIGNAL :
            live_ble_sort == CERBERUS_S3_BLE_SORT_NAME_ASC ? CERBERUS_BLE_DISCOVERY_LIST_NAME :
            CERBERUS_BLE_DISCOVERY_LIST_SOURCE_ORDER, s3_mask, c5_mask, store->order);
        store->order_s3_epoch = epoch; store->order_s3_revision = revision;
        store->order_c5_boot = boot; store->order_c5_epoch = c5_epoch;
        store->order_c5_snapshots = snapshots;
        store->order_s3_count = s3_count; store->order_c5_count = c5_count;
        store->order_sort = (uint8_t)live_ble_sort; store->order_source = (uint8_t)store->source_filter;
        store->order_family = (uint8_t)family;
        store->order_s3_mask = s3_mask; store->order_c5_mask = c5_mask;
        store->order_valid = true;
    }
    return store->order_count;
}

/* S3 family evidence keeps its original ADV/matched-response/history semantics.
 * The caller passes the already copied painted entry, never a new bank slot. */
static void ble_discovery_s3_family_badge(unsigned row,
    const cerberus_s3_ble_discovery_entry_t *entry, uint64_t now_ms)
{
    const cerberus_s3_ble_filter_t family = ble_discovery_family();
    const cerberus_s3_ble_focus_t *focus = &entry->record.focus;
    const cerberus_s3_ble_discovery_evidence_t *hints = &focus->discovery;
    char *out = live_badges[row]; size_t capacity = sizeof(live_badges[row]);
    if (family == CERBERUS_S3_BLE_FILTER_FIND_MY)
        cerberus_findmy_hint_sources_line(&hints->findmy_advertisement, &hints->findmy_scan_response, out, capacity);
    else if (family == CERBERUS_S3_BLE_FILTER_FLIPPER)
        cerberus_flipper_hint_sources_line(&hints->flipper_advertisement, &hints->flipper_scan_response, out, capacity);
    else if (family == CERBERUS_S3_BLE_FILTER_META_HINT)
        cerberus_meta_hint_sources_line(&hints->meta_advertisement, &hints->meta_scan_response, out, capacity);
    else if (family == CERBERUS_S3_BLE_FILTER_IBEACON) {
        cerberus_ibeacon_source_line(focus->ibeacon_scan_response, cerberus_s3_ble_ibeacon_is_current(focus),
            (uint32_t)now_ms - focus->ibeacon_observed_ms, true, out, capacity);
        ble_hint_age64_line(out, capacity, focus->ibeacon_observed_ms, entry->source_now_ms, (int64_t)now_ms);
    } else if (family == CERBERUS_S3_BLE_FILTER_FLOCK)
        cerberus_flock_sources_line(&hints->flock_advertisement, &hints->flock_scan_response, focus->flock_oui_clue, out, capacity);
    else if (family == CERBERUS_S3_BLE_FILTER_HC_NAME_HINT)
        cerberus_hc_sources_line(hints->hc_advertisement, hints->hc_scan_response, out, capacity);
}

/* Freeze the four displayed proofs while building their strings. A later
 * inventory update cannot change the row authorized by a successful paint. */
static bool ble_discovery_build_unified_row(unsigned row, size_t position, uint64_t now_ms)
{
    const cerberus_s3_ble_filter_t family = ble_discovery_family();
    ble_discovery_live_store_t *store = ble_discovery_live;
    if (!store || row >= 4U || position >= store->order_count ||
        position >= CERBERUS_BLE_DISCOVERY_LIST_CAPACITY) return false;
    const cerberus_ble_discovery_list_ref_t ref = store->order[position];
    sentinel_scan_row_t *out = &live_view.rows[row];
    char identifier[24];
    if (ref.source == CERBERUS_BLE_DISCOVERY_LIST_S3) {
        if (!cerberus_s3_ble_discovery_inventory_copy(&store->inventory, ref.index,
                &store->painted[row])) return false;
        const cerberus_s3_ble_discovery_entry_t *entry = &store->painted[row];
        const cerberus_c5_ble_result_t *peer = &entry->record.peer;
        cerberus_privacy_format_ble_identifier(peer->address, privacy_addresses_masked(), identifier, sizeof(identifier));
        if (peer->name[0]) (void)cerberus_s3_ble_list_label(peer, live_names[row], sizeof(live_names[row]));
        else snprintf(live_names[row], sizeof(live_names[row]), "%s",
            privacy_addresses_masked() ? "Unnamed device" : identifier);
        bool earlier = !cerberus_s3_ble_discovery_inventory_current(&store->inventory, ref.index) ||
            entry->epoch != store->session.epoch;
        snprintf(live_details[row], sizeof(live_details[row]), "%s T%u%s", identifier,
            peer->address_type, earlier ? " / EARLIER" : "");
        uint64_t age;
        if (cerberus_s3_ble_discovery_entry_age_ms(entry, now_ms, &age))
            snprintf(live_badges[row], sizeof(live_badges[row]), "S3 PEAK / ADV %llus",
                (unsigned long long)(age / 1000U));
        else snprintf(live_badges[row], sizeof(live_badges[row]), "S3 PEAK / ADV AGE UNKNOWN");
        if (family != CERBERUS_S3_BLE_FILTER_ALL) {
            snprintf(live_details[row], sizeof(live_details[row]), "S3 PEAK %s T%u%s",
                identifier, peer->address_type, earlier ? " OLD" : "");
            ble_discovery_s3_family_badge(row, entry, now_ms);
        }
        out->rssi = peer->rssi;
        out->signal_valid = cerberus_ble_rssi_valid(peer->rssi);
        out->marked = cerberus_settings_ble_is_favorite(peer->address, peer->address_type);
    } else if (ref.source == CERBERUS_BLE_DISCOVERY_LIST_C5) {
        cerberus_dual_ble_row_t *peer = &store->painted_c5[row];
        if (!cerberus_dual_ble_copy_row(dual_ble_ui_store, ref.index, peer,
                &store->painted_c5_provenance[row])) return false;
        cerberus_privacy_format_ble_identifier(peer->address, privacy_addresses_masked(), identifier, sizeof(identifier));
        bool response_name = cerberus_dual_ble_list_response_name(peer);
        if (peer->name[0] || response_name) cerberus_dual_ble_list_label(peer, live_names[row]);
        else snprintf(live_names[row], sizeof(live_names[row]), "%s",
            privacy_addresses_masked() ? "Unnamed device" : identifier);
        snprintf(live_details[row], sizeof(live_details[row]), "%s T%u%s", identifier, peer->address_type,
            response_name ? " / RSP NAME" : "");
        uint32_t age;
        if (cerberus_dual_ble_list_age(peer, now_ms, &age)) {
            uint32_t seconds = age / 1000U + (age % 1000U != 0U);
            snprintf(live_badges[row], sizeof(live_badges[row]), "C5 LAST / AGE<=%lus%s",
                (unsigned long)seconds, age > CERBERUS_DUAL_BLE_RECENT_MS ? " RECENCY?" : "");
        } else snprintf(live_badges[row], sizeof(live_badges[row]), "C5 LAST / ADV AGE UNKNOWN");
        if (family != CERBERUS_S3_BLE_FILTER_ALL) {
            snprintf(live_details[row], sizeof(live_details[row]), "C5 LAST %s T%u", identifier, peer->address_type);
            uint8_t wanted = (uint8_t)(1U << ((unsigned)family - 1U));
            bool adv = (store->c5_family_bits[ref.index] & wanted) != 0U;
            bool rsp = (store->c5_response_bits[ref.index] & wanted) != 0U;
            const char *kind = adv && rsp ? "C5 ADV+RSP" : rsp ? "C5 RSP HINT" :
                family == CERBERUS_S3_BLE_FILTER_FLOCK &&
                (store->c5_oui_only & (UINT32_C(1) << ref.index)) ? "C5 ADDR CLUE" :
                peer->family.version ? "C5 FULL HINT" : "C5 ADV HINT";
            uint32_t response_age;
            bool age_valid = adv && cerberus_dual_ble_list_age(peer, now_ms, &age);
            bool response_age_valid = rsp && cerberus_dual_ble_response_age(peer, now_ms, &response_age);
            bool use_response = response_age_valid && (!age_valid || response_age < age);
            if (use_response) age = response_age;
            if (age_valid || response_age_valid) {
                uint32_t seconds = age / 1000U + (age % 1000U != 0U);
                snprintf(live_badges[row], sizeof(live_badges[row]), "%s %s<=%lus%s",
                    kind, adv && rsp ? (use_response ? "RSP" : "ADV") : "", (unsigned long)seconds,
                    age > CERBERUS_DUAL_BLE_RECENT_MS ? " RECENCY?" : "");
            } else snprintf(live_badges[row], sizeof(live_badges[row]), "%s / AGE UNKNOWN", kind);
            if (response_name) snprintf(live_details[row], sizeof(live_details[row]),
                "C5 LAST %s T%u RSP NAME", identifier, peer->address_type);
        }
        out->rssi = peer->rssi;
        out->signal_valid = cerberus_dual_ble_list_rssi_valid(peer);
        out->marked = cerberus_settings_ble_is_favorite(peer->address, peer->address_type);
    } else return false;
    store->painted_source[row] = ref.source;
    out->name = live_names[row]; out->detail = live_details[row]; out->badge = live_badges[row];
    ++store->built_count;
    return true;
}
