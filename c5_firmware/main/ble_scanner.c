#include "ble_scanner.h"
#include "radio_test_service.h"

#include "ieee802154_scanner.h"
#include "wifi_scanner.h"
#include "cerberus_watch_capture.h"

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "dual_ble_protocol.h"
#include "ble_inventory_admission.h"
#include "ble_analyzer_counter.h"
#include "ble_family_wire.h"
#include "ble_response.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "host/ble_hs.h"
#include "host/ble_gatt.h"
#include "host/util/util.h"
#include "nimble/nimble_npl.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "os/os_mbuf.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#define BLE_SCAN_DURATION_MS 8000
#define BLE_CONNECT_TIMEOUT_MS 15000
#define BLE_CONNECTION_HANDLE_NONE 0xFFFFU
#define BLE_DIAGNOSTIC_MAGIC 0x4345424cUL

#if MYNEWT_VAL(BLE_EXT_ADV)
/* Extended HCI reports need the larger event pool even when old sdkconfig
 * files retain their former legacy-scan defaults. */
_Static_assert(CONFIG_BT_NIMBLE_TRANSPORT_EVT_SIZE >= 257,
    "Extended scans require full-size HCI event buffers");
#endif

typedef struct {
    uint8_t address[6];
    uint8_t address_type;
    int8_t rssi;
    uint8_t name_length;
    uint8_t uuid_length;
    uint8_t name[CERBERUS_BLE_NAME_MAX_LENGTH];
    uint8_t uuid[CERBERUS_BLE_UUID_MAX_LENGTH];
    uint16_t manufacturer_id;
    uint8_t flags;
    uint8_t advertisement[CERBERUS_BLE_ADVERTISEMENT_MAX_LENGTH];
    uint8_t advertisement_length;
    uint8_t advertisement_event_type;
    uint8_t advertisement_flags;
    uint16_t reports_seen;
    uint32_t original_observed_ms;
    int8_t latest_rssi;
    bool original_time_valid;
    cerberus_dual_ble_receive_metadata_t receive;
} cerberus_ble_record_t;
#include "ble_response_state.h"

static const char *TAG = "ble_scanner";
static SemaphoreHandle_t scanner_mutex;
static struct ble_npl_event start_event;
static struct ble_npl_event stop_event;
static struct ble_npl_event connect_event;
static struct ble_npl_event disconnect_event;
static struct ble_npl_event discover_services_event;
static struct ble_npl_event discover_characteristics_event;
static struct ble_npl_event read_characteristic_event;
static struct ble_npl_callout scan_timeout_callout;
static cerberus_ble_record_t scan_results[CERBERUS_BLE_SCAN_MAX_RESULTS];
static cerberus_ble_scan_status_payload_t scan_status;
static cerberus_watch_capture_t watch_capture;
static uint64_t watch_capture_epoch;
/* NimBLE's callback argument is pointer-width, as on the S3 scanner. This
 * private token is never reused during a boot and is independent of gen8. */
static uint32_t scan_callback_epoch;
static cerberus_ble_activity_capture_t activity_capture;
static cerberus_ble_connection_status_payload_t connection_status;
static cerberus_ble_gatt_service_result_payload_t
    gatt_services[CERBERUS_BLE_GATT_SERVICE_MAX_RESULTS];
static cerberus_ble_gatt_service_status_payload_t gatt_service_status;
static cerberus_ble_gatt_characteristic_result_payload_t
    gatt_characteristics[CERBERUS_BLE_GATT_CHARACTERISTIC_MAX_RESULTS];
static cerberus_ble_gatt_characteristic_status_payload_t gatt_characteristic_status;
static cerberus_ble_gatt_read_status_payload_t gatt_read_status;
static uint8_t gatt_read_value[CERBERUS_BLE_GATT_READ_MAX_LENGTH];
static bool host_synced;
static bool scan_cancelled;
static bool disconnect_requested;
static RTC_NOINIT_ATTR uint32_t diagnostic_magic;
static RTC_NOINIT_ATTR uint8_t diagnostic_boot_count;
static RTC_NOINIT_ATTR uint8_t diagnostic_phase;
static uint64_t dual_boot_id, dual_epoch_counter, dual_epoch;
static c5_response_context_t *dual_response;
static uint32_t dual_job_id, dual_job_highwater, dual_last_contact_ms, dual_track_ms, dual_track_reports;
static int32_t dual_error;
static uint8_t dual_mode, dual_generation, dual_address[6], dual_address_type, dual_version, dual_family;
static int8_t dual_track_rssi;
/* The SDK-active flag covers ordinary and companion receive scans alike. An
 * SDK cancellation error keeps it set until cancellation/completion/reset is
 * confirmed; public radio admission must not mistake ERROR for quiescence. */
static bool dual_attached, dual_radio_active, dual_track_seen;
static cerberus_ble_analyzer_counter_t dual_analyzer;
/* A slot remains reserved while its static event is queued OR dispatching.
 * Reset retires the session, but does not erase these outstanding reservations.
 * NimBLE's event queue holds pointers, not immutable copies of event arguments. */
static uint32_t scan_start_pending_epoch, scan_stop_pending_epoch, scan_sdk_epoch;
static int64_t scan_deadline_us;
static bool scan_epoch_retired;
static bool scan_epoch_current_locked(uint32_t epoch)
{ return epoch && epoch == scan_callback_epoch && !scan_epoch_retired; }
static bool scan_radio_owned_locked(void)
{
    return scan_start_pending_epoch || scan_stop_pending_epoch || dual_radio_active ||
        scan_status.state == CERBERUS_BLE_SCAN_RUNNING;
}
static cerberus_ble_inventory_admission_t scan_inventory;
static cerberus_dual_ble_family_wire_t scan_family[CERBERUS_DUAL_BLE_CAPACITY];
typedef struct {
    cerberus_dual_ble_snapshot_row_t rows[CERBERUS_DUAL_BLE_CAPACITY];
    cerberus_dual_ble_receive_metadata_t receive[CERBERUS_DUAL_BLE_CAPACITY];
    cerberus_dual_ble_family_wire_t family[CERBERUS_DUAL_BLE_CAPACITY];
    uint64_t counter, handle, epoch, request_id;
    uint32_t job_id, replaced, dropped;
    uint8_t count, flags;
    bool valid, inventory_enabled;
} dual_snapshot_store_t;
static dual_snapshot_store_t dual_snapshot;
static void dual_expire_locked(void);

static size_t bounded_size(size_t value, size_t limit)
{
    return value < limit ? value : limit;
}

static void reset_gatt_read_locked(uint8_t state, int32_t error_code)
{
    gatt_read_status.state = state;
    gatt_read_status.connection_generation = connection_status.generation;
    gatt_read_status.value_handle = 0;
    gatt_read_status.total_length = 0;
    gatt_read_status.stored_length = 0;
    gatt_read_status.error_code = error_code;
    memset(gatt_read_value, 0, sizeof(gatt_read_value));
}

static int find_record(const ble_addr_t *address)
{
    for (uint8_t index = 0; index < scan_status.stored_count; ++index) {
        if (scan_results[index].address_type == address->type &&
            memcmp(scan_results[index].address, address->val, sizeof(address->val)) == 0) {
            return index;
        }
    }
    return -1;
}

static void copy_advertisement_fields(
    cerberus_ble_record_t *record,
    const struct ble_hs_adv_fields *fields)
{
    if (fields->name != NULL && fields->name_len > 0U && record->name_length == 0U) {
        size_t name_length = bounded_size(fields->name_len, sizeof(record->name));
        memcpy(record->name, fields->name, name_length);
        record->name_length = (uint8_t)name_length;
    }

    if (record->uuid_length == 0U) {
        if (fields->uuids16 != NULL && fields->num_uuids16 > 0U) {
            memcpy(record->uuid, &fields->uuids16[0].value, sizeof(fields->uuids16[0].value));
            record->uuid_length = sizeof(fields->uuids16[0].value);
        } else if (fields->uuids32 != NULL && fields->num_uuids32 > 0U) {
            memcpy(record->uuid, &fields->uuids32[0].value, sizeof(fields->uuids32[0].value));
            record->uuid_length = sizeof(fields->uuids32[0].value);
        } else if (fields->uuids128 != NULL && fields->num_uuids128 > 0U) {
            memcpy(record->uuid, fields->uuids128[0].value, sizeof(fields->uuids128[0].value));
            record->uuid_length = sizeof(fields->uuids128[0].value);
        }
    }

    if (record->manufacturer_id == CERBERUS_BLE_MANUFACTURER_UNKNOWN &&
        fields->mfg_data != NULL && fields->mfg_data_len >= 2U) {
        record->manufacturer_id = (uint16_t)fields->mfg_data[0] |
            ((uint16_t)fields->mfg_data[1] << 8);
    }
}

static void copy_raw_advertisement(cerberus_ble_record_t *record,
    const struct ble_gap_disc_desc *discovery, bool parsed)
{
    const size_t length = bounded_size(discovery->length_data, sizeof(record->advertisement));
    memset(record->advertisement, 0, sizeof(record->advertisement));
    if (length != 0U) memcpy(record->advertisement, discovery->data, length);
    record->advertisement_length = (uint8_t)length;
    record->advertisement_event_type = discovery->event_type;
    record->advertisement_flags = parsed ? 0U : CERBERUS_BLE_ADVERTISEMENT_MALFORMED;
    if (discovery->length_data > sizeof(record->advertisement)) {
        record->advertisement_flags |= CERBERUS_BLE_ADVERTISEMENT_TRUNCATED;
    }
    /* Keep the original bytes, including malformed fields, for bounded parsing
       on S3. Never join fields from different packets into a synthetic payload. */
    for (size_t offset = 0U; offset < length;) {
        const uint8_t field_length = record->advertisement[offset];
        if (field_length == 0U) break;
        if ((size_t)field_length + 1U > length - offset) {
            record->advertisement_flags |= CERBERUS_BLE_ADVERTISEMENT_MALFORMED;
            break;
        }
        offset += (size_t)field_length + 1U;
    }
    if (record->reports_seen < UINT16_MAX) ++record->reports_seen;
}

#include "ble_response_impl.h"

static bool analyzer_observe(uint32_t epoch, uint8_t type, int8_t rssi,
    bool original, int64_t callback_us, uint8_t *family, const uint8_t address[6])
{
    xSemaphoreTake(scanner_mutex, portMAX_DELAY);
    if (family) *family = dual_attached && dual_mode == CERBERUS_DUAL_BLE_FAMILY &&
        scan_epoch_current_locked(epoch) && scan_status.state == CERBERUS_BLE_SCAN_RUNNING ? dual_family : 0U;
    const bool analyzer = dual_attached && dual_mode == CERBERUS_DUAL_BLE_ANALYZER;
    if (analyzer && original && scan_epoch_current_locked(epoch) &&
        scan_status.state == CERBERUS_BLE_SCAN_RUNNING)
        cerberus_ble_analyzer_counter_observe(&dual_analyzer, epoch, type, rssi, callback_us);
    /* The same early lock that selects the Analyzer/Family owner also gates
     * saved-address discovery. Unrelated reports never reach AD parsing,
     * activity accounting or the bounded inventory. NULL marks partial EXT. */
    bool filtered = dual_attached && dual_mode == CERBERUS_DUAL_BLE_REACQUIRE &&
        (!scan_epoch_current_locked(epoch) || scan_status.state != CERBERUS_BLE_SCAN_RUNNING ||
         !original || !address || type > 3U || memcmp(address, dual_address, 6U) || callback_us < 0 ||
         (scan_inventory.clock_valid && callback_us < scan_inventory.clock_highwater_us));
    xSemaphoreGive(scanner_mutex);
    return analyzer || filtered;
}

/* Full parsing is opt-in FAMILY work and runs outside scanner_mutex. Raw
 * preview flags retain their independent SDK/prefix meaning; a cut preview
 * does not invalidate the independently parsed complete delivered container. */
static void __attribute__((noinline)) family_classify_callback(
    const struct ble_gap_disc_desc *discovery,
    const cerberus_dual_ble_receive_metadata_t *receive, uint8_t code,
    cerberus_dual_ble_family_wire_t *out)
{
    cerberus_dual_ble_family_evidence_t evidence = {0};
    if (receive->source_kind == CERBERUS_DUAL_BLE_RECEIVE_UNKNOWN)
        cerberus_dual_ble_family_unavailable(&evidence, CERBERUS_DUAL_BLE_FAMILY_RECEIVE_UNKNOWN);
    else cerberus_dual_ble_family_parse_ad(discovery->data, discovery->length_data,
        discovery->addr.val, discovery->addr.type, &evidence);
    /* Invalid typed identities are rejected before retention. Parser failure
     * yields canonical unavailable evidence, never cached positive hints. */
    (void)cerberus_dual_ble_family_wire_encode(&evidence, code, out);
}

static void record_advertisement_with_metadata(const struct ble_gap_disc_desc *discovery,
    uint32_t epoch, int64_t callback_us, const cerberus_dual_ble_receive_metadata_t *receive)
{
    if (!discovery) return;
    if ((discovery->length_data != 0U && discovery->data == NULL) ||
        discovery->event_type == BLE_HCI_ADV_RPT_EVTYPE_SCAN_RSP) return;
    uint8_t family_code = 0U;
    if (analyzer_observe(epoch, discovery->addr.type, discovery->rssi,
            discovery->event_type <= 3U, callback_us, &family_code, discovery->addr.val)) return;
    struct ble_hs_adv_fields fields = {0};
    const bool parsed = discovery->length_data == 0U ||
        ble_hs_adv_parse_fields(&fields, discovery->data, discovery->length_data) == 0;
    /* Independently validate the complete delivered container once. NimBLE's
     * parse result still gates inventory qualification; metadata validation
     * describes full AD layout rather than a possibly cut31-byte preview. */
    const bool full_ad_valid = discovery->event_type <= 3U &&
        cerberus_ble_inventory_ad_valid(discovery->data, discovery->length_data);
    const bool qualifying = parsed && full_ad_valid;
    cerberus_dual_ble_receive_metadata_t normalized = {0};
    if (receive && receive->callback_length == discovery->length_data) {
        normalized = *receive;
        normalized.ad_validation = full_ad_valid ? CERBERUS_DUAL_BLE_AD_VALID : CERBERUS_DUAL_BLE_AD_INVALID;
        const uint8_t raw_length = discovery->length_data < 31U ? discovery->length_data : 31U;
        const uint8_t raw_flags = (discovery->length_data > 31U ? 1U : 0U) | (parsed ? 0U : 2U);
        if (!cerberus_dual_ble_receive_metadata_valid(&normalized, raw_length,
                discovery->event_type, raw_flags)) memset(&normalized, 0, sizeof(normalized));
    }
    cerberus_dual_ble_family_wire_t family = {0};
    if (family_code) family_classify_callback(discovery, &normalized, family_code, &family);


    xSemaphoreTake(scanner_mutex, portMAX_DELAY);
    if (!scan_epoch_current_locked(epoch) || scan_status.state != CERBERUS_BLE_SCAN_RUNNING) {
        xSemaphoreGive(scanner_mutex);
        return;
    }

    /* Parsing can yield to Stop, a new epoch or a newer callback. Preserve
     * exact selector and monotonic source time at publication as well. */
    if (dual_attached && dual_mode == CERBERUS_DUAL_BLE_REACQUIRE &&
        (discovery->addr.type > 3U || memcmp(discovery->addr.val, dual_address, 6U) ||
         callback_us < 0 || (scan_inventory.clock_valid && callback_us < scan_inventory.clock_highwater_us))) {
        xSemaphoreGive(scanner_mutex); return;
    }

    if (dual_attached && dual_mode == CERBERUS_DUAL_BLE_TRACK && callback_us >= 0 &&
        discovery->addr.type == dual_address_type &&
        memcmp(discovery->addr.val, dual_address, 6U) == 0 &&
        discovery->rssi >= -127 && discovery->rssi <= 20) {
        dual_track_seen = true;
        dual_track_rssi = discovery->rssi;
        dual_track_ms = (uint32_t)((uint64_t)callback_us / 1000U);
        if (dual_track_reports != UINT32_MAX) ++dual_track_reports;
    }

    /* Account for every delivered report before ordinary inventory capacity.
       Remote ID ID/location packets survive later packets from that address. */
    cerberus_ble_activity_observe(&activity_capture, discovery->addr.val,
        discovery->addr.type, discovery->data, discovery->length_data, true,
        (uint32_t)(esp_timer_get_time() / 1000LL));
    sentinel_watch_protocol_record_t protocol;
    bool matched = discovery->addr.type <= 3U && sentinel_watch_protocol_decode_ble(
        discovery->data, discovery->length_data, &protocol);
    if (callback_us >= 0)
        cerberus_watch_capture_observe_timed(&watch_capture, matched ? &protocol : NULL,
            discovery->addr.val, discovery->addr.type, discovery->rssi, 0U, (uint64_t)callback_us);
    else cerberus_watch_capture_observe(&watch_capture, matched ? &protocol : NULL,
        discovery->addr.val, discovery->addr.type, discovery->rssi, 0U);
    /* Distinct-address lower bound replaces repeated overflow-report counting. */
    if (activity_capture.status.identities > scan_status.total_count)
        scan_status.total_count = activity_capture.status.identities;
    if (discovery->addr.type > 3U) {
        xSemaphoreGive(scanner_mutex);
        return;
    }

    const bool focused = dual_attached && dual_mode == CERBERUS_DUAL_BLE_FAMILY;
    /* Parse outside the mutex, then bind it to the still-current owner/epoch.
     * Backward callback clocks cannot replace or refresh an original. These
     * remain historical retained rows; no new timing/proof is published. */
    if (focused && (family_code != dual_family || callback_us < 0 ||
            (scan_inventory.clock_valid && callback_us < scan_inventory.clock_highwater_us))) {
        xSemaphoreGive(scanner_mutex); return;
    }
    int existing_index = find_record(&discovery->addr);
    const bool family_match = family.version && (family.flags & CERBERUS_DUAL_BLE_FAMILY_WIRE_AVAILABLE) &&
        (family.categories & (1U << (family_code - 1U)));
    if (focused) {
        scan_inventory.clock_highwater_us = callback_us;
        scan_inventory.clock_valid = true;
        /* Unrelated originals never occupy slots or increment full-bank drop
         * counters. Existing typed rows are retained but dequalified below. */
        if (existing_index < 0 && !family_match) { xSemaphoreGive(scanner_mutex); return; }
        if (existing_index >= 0 && !family_match) scan_inventory.rank[existing_index] = 0U;
    }
    int admitted_index = cerberus_ble_inventory_admit(&scan_inventory,
        scan_status.stored_count, existing_index, focused ? family_match : qualifying, callback_us);
    bool connectable = discovery->event_type == BLE_HCI_ADV_RPT_EVTYPE_ADV_IND ||
        discovery->event_type == BLE_HCI_ADV_RPT_EVTYPE_DIR_IND;
    cerberus_ble_record_t *record = NULL;
    if (existing_index >= 0) {
        record = &scan_results[existing_index];
        if (discovery->rssi > record->rssi) {
            record->rssi = discovery->rssi;
        }
        if (connectable) {
            record->flags |= CERBERUS_BLE_RESULT_CONNECTABLE;
        }
    } else {
        if (admitted_index >= 0) {
            record = &scan_results[admitted_index];
            if (admitted_index == scan_status.stored_count) ++scan_status.stored_count;
            memset(record, 0, sizeof(*record));
            memcpy(record->address, discovery->addr.val, sizeof(record->address));
            record->address_type = discovery->addr.type;
            record->rssi = discovery->rssi;
            record->manufacturer_id = CERBERUS_BLE_MANUFACTURER_UNKNOWN;
            if (connectable) {
                record->flags |= CERBERUS_BLE_RESULT_CONNECTABLE;
            }
        } else if (scan_status.dropped_reports < UINT16_MAX) {
            ++scan_status.dropped_reports;
        }
    }

    if (record != NULL) {
        if (parsed) copy_advertisement_fields(record, &fields);
        copy_raw_advertisement(record, discovery, parsed);
        record->original_observed_ms = callback_us >= 0 ? (uint32_t)((uint64_t)callback_us / 1000U) : UINT32_MAX;
        record->latest_rssi = discovery->rssi;
        record->original_time_valid = callback_us >= 0;
        record->receive = normalized;
        if (focused) scan_family[record - scan_results] = family;
    }
    xSemaphoreGive(scanner_mutex);
}

/* Dispatch each SDK route once, before original-only admission. EXT dispatch
 * stays in gap_event so incomplete originals can invalidate response proof. */
static void record_advertisement(const struct ble_gap_disc_desc *discovery,
    uint32_t epoch, int64_t callback_us)
{
    if (!discovery) return;
    const cerberus_dual_ble_receive_metadata_t receive = {
        .source_kind = CERBERUS_DUAL_BLE_RECEIVE_LEGACY,
        .callback_length = discovery->length_data,
        .sid = 0xffU, .data_status = 0xffU,
    };
    if (response_receive_event(discovery,epoch,callback_us,&receive,true,
            discovery->event_type==BLE_HCI_ADV_RPT_EVTYPE_SCAN_RSP)) return;
    record_advertisement_with_metadata(discovery, epoch, callback_us, &receive);
}

static void finish_scan_for_epoch(int reason, uint32_t epoch)
{
    xSemaphoreTake(scanner_mutex, portMAX_DELAY);
    if (!scan_epoch_current_locked(epoch)) {
        xSemaphoreGive(scanner_mutex);
        return;
    }
    dual_radio_active = false;
    scan_sdk_epoch = 0U;
    scan_deadline_us = 0;
    ble_npl_callout_stop(&scan_timeout_callout);
    if (scan_status.state != CERBERUS_BLE_SCAN_RUNNING) {
        response_error_locked();
        xSemaphoreGive(scanner_mutex);
        return;
    }
    if (dual_attached && dual_mode == CERBERUS_DUAL_BLE_ANALYZER)
        cerberus_ble_analyzer_counter_finish(&dual_analyzer, esp_timer_get_time());
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000LL);
    uint8_t watch_state = scan_cancelled ? CERBERUS_WATCH_IDLE :
        reason != 0 ? CERBERUS_WATCH_ERROR : CERBERUS_WATCH_COMPLETE;
    cerberus_watch_capture_finish(&watch_capture, watch_state, now_ms);
    cerberus_ble_activity_finish(&activity_capture, watch_state, now_ms);
    if (scan_cancelled) {
        scan_status.state = CERBERUS_BLE_SCAN_IDLE;
        scan_status.total_count = 0;
        scan_status.stored_count = 0;
        scan_status.unnamed_count = 0;
        scan_status.dropped_reports = 0;
        scan_status.error_code = 0;
        scan_cancelled = false;
    } else if (reason != 0) {
        scan_status.state = CERBERUS_BLE_SCAN_ERROR;
        scan_status.error_code = reason;
        scan_status.total_count = 0;
        scan_status.stored_count = 0;
        scan_status.unnamed_count = 0;
        scan_status.dropped_reports = 0;
    } else {
        uint8_t unnamed_count = 0;
        for (uint8_t index = 0; index < scan_status.stored_count; ++index) {
            if (scan_results[index].name_length == 0U) {
                ++unnamed_count;
            }
        }
        scan_status.state = CERBERUS_BLE_SCAN_COMPLETE;
        scan_status.unnamed_count = unnamed_count;
        scan_status.error_code = 0;
        ESP_LOGI(TAG, "passive scan complete: total=%u stored=%u unnamed=%u reason=%d",
            scan_status.total_count, scan_status.stored_count, unnamed_count, reason);
    }
    response_error_locked();
    xSemaphoreGive(scanner_mutex);
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_DISC:
        record_advertisement(&event->disc, (uint32_t)(uintptr_t)arg, esp_timer_get_time());
        break;
#if MYNEWT_VAL(BLE_EXT_ADV)
    case BLE_GAP_EVENT_EXT_DISC: {
        int64_t callback_us = esp_timer_get_time();
        uint32_t epoch = (uint32_t)(uintptr_t)arg;
        const struct ble_gap_ext_disc_desc *extended = &event->ext_disc;
        struct ble_gap_disc_desc discovery = {
            .addr = extended->addr, .rssi = extended->rssi,
            .length_data = extended->length_data, .data = extended->data,
            .event_type = (extended->props & BLE_HCI_ADV_LEGACY_MASK) != 0U
                ? extended->legacy_event_type
                : ((extended->props & BLE_HCI_ADV_CONN_MASK) != 0U
                    ? BLE_HCI_ADV_RPT_EVTYPE_ADV_IND : BLE_HCI_ADV_RPT_EVTYPE_NONCONN_IND),
        };
        const cerberus_dual_ble_receive_metadata_t receive = {
            .source_kind = CERBERUS_DUAL_BLE_RECEIVE_EXTENDED,
            .properties = extended->props, .callback_length = extended->length_data,
            .sid = extended->sid, .primary_phy = extended->prim_phy,
            .secondary_phy = extended->sec_phy, .data_status = extended->data_status,
        };
        if (response_receive_event(&discovery,epoch,callback_us,&receive,
                extended->data_status==BLE_GAP_EXT_ADV_DATA_STATUS_COMPLETE,
                (extended->props & BLE_HCI_ADV_SCAN_RSP_MASK)!=0U)) break;
        if ((extended->props & BLE_HCI_ADV_SCAN_RSP_MASK) != 0U) break;
        if (extended->data_status != BLE_GAP_EXT_ADV_DATA_STATUS_COMPLETE) {
            /* Never concatenate partial reports or classify their prefixes. */
            if (analyzer_observe(epoch, extended->addr.type, extended->rssi,
                    !extended->length_data || extended->data != NULL, callback_us, NULL, NULL)) break;
            xSemaphoreTake(scanner_mutex, portMAX_DELAY);
            if (scan_epoch_current_locked(epoch) && scan_status.state == CERBERUS_BLE_SCAN_RUNNING) {
                cerberus_ble_activity_observe(&activity_capture, extended->addr.val,
                    extended->addr.type, extended->data, extended->length_data, false,
                    (uint32_t)(esp_timer_get_time() / 1000LL));
                cerberus_watch_capture_observe(&watch_capture, NULL, extended->addr.val,
                    extended->addr.type, extended->rssi, 0U);
            }
            xSemaphoreGive(scanner_mutex);
            break;
        }
        record_advertisement_with_metadata(&discovery, epoch, callback_us, &receive);
        break;
    }
#endif
    case BLE_GAP_EVENT_DISC_COMPLETE:
        if (arg) finish_scan_for_epoch(event->disc_complete.reason, (uint32_t)(uintptr_t)arg);
        break;
    case BLE_GAP_EVENT_CONNECT:
        xSemaphoreTake(scanner_mutex, portMAX_DELAY);
        if (event->connect.status == 0) {
            connection_status.state = CERBERUS_BLE_CONNECTION_CONNECTED;
            connection_status.connection_handle = event->connect.conn_handle;
            connection_status.error_code = 0;
            gatt_service_status.state = CERBERUS_BLE_GATT_SERVICE_IDLE;
            gatt_service_status.total_count = 0;
            gatt_service_status.stored_count = 0;
            gatt_service_status.connection_generation = connection_status.generation;
            gatt_service_status.error_code = 0;
            gatt_characteristic_status.state = CERBERUS_BLE_GATT_CHARACTERISTIC_IDLE;
            gatt_characteristic_status.total_count = 0;
            gatt_characteristic_status.stored_count = 0;
            gatt_characteristic_status.connection_generation = connection_status.generation;
            gatt_characteristic_status.error_code = 0;
            reset_gatt_read_locked(CERBERUS_BLE_GATT_READ_IDLE, 0);
            ESP_LOGI(TAG, "BLE connection established: handle=%u", event->connect.conn_handle);
        } else {
            connection_status.state = CERBERUS_BLE_CONNECTION_ERROR;
            connection_status.connection_handle = BLE_CONNECTION_HANDLE_NONE;
            connection_status.error_code = event->connect.status;
            gatt_service_status.state = CERBERUS_BLE_GATT_SERVICE_IDLE;
            gatt_service_status.total_count = 0;
            gatt_service_status.stored_count = 0;
            gatt_characteristic_status.state = CERBERUS_BLE_GATT_CHARACTERISTIC_IDLE;
            gatt_characteristic_status.total_count = 0;
            gatt_characteristic_status.stored_count = 0;
            reset_gatt_read_locked(CERBERUS_BLE_GATT_READ_IDLE, 0);
            ESP_LOGW(TAG, "BLE connection failed: status=%d", event->connect.status);
        }
        xSemaphoreGive(scanner_mutex);
        break;
    case BLE_GAP_EVENT_DISCONNECT: {
        int reason = event->disconnect.reason;
        xSemaphoreTake(scanner_mutex, portMAX_DELAY);
        bool requested = disconnect_requested ||
            connection_status.state == CERBERUS_BLE_CONNECTION_DISCONNECTING;
        connection_status.state = requested
            ? CERBERUS_BLE_CONNECTION_IDLE
            : CERBERUS_BLE_CONNECTION_ERROR;
        connection_status.connection_handle = BLE_CONNECTION_HANDLE_NONE;
        connection_status.error_code = requested ? 0 : reason;
        gatt_service_status.state = CERBERUS_BLE_GATT_SERVICE_IDLE;
        gatt_service_status.total_count = 0;
        gatt_service_status.stored_count = 0;
        gatt_service_status.error_code = 0;
        gatt_characteristic_status.state = CERBERUS_BLE_GATT_CHARACTERISTIC_IDLE;
        gatt_characteristic_status.total_count = 0;
        gatt_characteristic_status.stored_count = 0;
        gatt_characteristic_status.error_code = 0;
        reset_gatt_read_locked(CERBERUS_BLE_GATT_READ_IDLE, 0);
        disconnect_requested = false;
        xSemaphoreGive(scanner_mutex);
        ESP_LOGI(TAG, "BLE connection closed: reason=%d requested=%d", reason, requested);
        break;
    }
    case BLE_GAP_EVENT_TERM_FAILURE:
        xSemaphoreTake(scanner_mutex, portMAX_DELAY);
        connection_status.state = CERBERUS_BLE_CONNECTION_ERROR;
        connection_status.error_code = event->term_failure.status;
        disconnect_requested = false;
        xSemaphoreGive(scanner_mutex);
        ESP_LOGW(TAG, "BLE disconnect failed: status=%d", event->term_failure.status);
        break;
    default:
        break;
    }
    return 0;
}

static void host_reset(int reason)
{
    ESP_LOGW(TAG, "NimBLE host reset: reason=%d", reason);
    xSemaphoreTake(scanner_mutex, portMAX_DELAY);
    host_synced = false;
    scan_epoch_retired = true;
    dual_radio_active = false;
    scan_sdk_epoch = 0U;
    scan_deadline_us = 0;
    ble_npl_callout_stop(&scan_timeout_callout);
    cerberus_ble_analyzer_counter_reset(&dual_analyzer);
    if (dual_attached) dual_error = reason;
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000LL);
    cerberus_watch_capture_finish(&watch_capture, CERBERUS_WATCH_ERROR, now_ms);
    cerberus_ble_activity_finish(&activity_capture, CERBERUS_WATCH_ERROR, now_ms);
    scan_status.state = CERBERUS_BLE_SCAN_ERROR;
    scan_status.error_code = reason;
    response_error_locked();
    connection_status.state = CERBERUS_BLE_CONNECTION_ERROR;
    connection_status.connection_handle = BLE_CONNECTION_HANDLE_NONE;
    connection_status.error_code = reason;
    gatt_service_status.state = CERBERUS_BLE_GATT_SERVICE_ERROR;
    gatt_service_status.error_code = reason;
    gatt_characteristic_status.state = CERBERUS_BLE_GATT_CHARACTERISTIC_ERROR;
    gatt_characteristic_status.error_code = reason;
    reset_gatt_read_locked(CERBERUS_BLE_GATT_READ_ERROR, reason);
    xSemaphoreGive(scanner_mutex);
}

static void host_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    xSemaphoreTake(scanner_mutex, portMAX_DELAY);
    host_synced = rc == 0;
    scan_status.state = host_synced ? CERBERUS_BLE_SCAN_IDLE : CERBERUS_BLE_SCAN_ERROR;
    scan_status.error_code = rc;
    connection_status.state = host_synced
        ? CERBERUS_BLE_CONNECTION_IDLE
        : CERBERUS_BLE_CONNECTION_ERROR;
    connection_status.connection_handle = BLE_CONNECTION_HANDLE_NONE;
    connection_status.error_code = rc;
    gatt_service_status.state = host_synced
        ? CERBERUS_BLE_GATT_SERVICE_IDLE
        : CERBERUS_BLE_GATT_SERVICE_ERROR;
    gatt_service_status.error_code = rc;
    gatt_characteristic_status.state = host_synced
        ? CERBERUS_BLE_GATT_CHARACTERISTIC_IDLE
        : CERBERUS_BLE_GATT_CHARACTERISTIC_ERROR;
    gatt_characteristic_status.error_code = rc;
    reset_gatt_read_locked(
        host_synced ? CERBERUS_BLE_GATT_READ_IDLE : CERBERUS_BLE_GATT_READ_ERROR,
        rc);
    xSemaphoreGive(scanner_mutex);
    if (rc == 0) {
        ESP_LOGI(TAG, "passive BLE observer ready");
#ifdef CERBERUS_C5_PANIC_DIAGNOSTIC
        (void)cerberus_ble_scanner_start();
#endif
    } else {
        ESP_LOGE(TAG, "BLE identity setup failed: rc=%d", rc);
    }
}

static void host_task(void *arg)
{
    (void)arg;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static void set_control_error_for_epoch(uint32_t epoch, int rc, const char *operation)
{
    xSemaphoreTake(scanner_mutex, portMAX_DELAY);
    if (!scan_epoch_current_locked(epoch)) { xSemaphoreGive(scanner_mutex); return; }
    cerberus_ble_analyzer_counter_reset(&dual_analyzer);
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000LL);
    cerberus_watch_capture_finish(&watch_capture, CERBERUS_WATCH_ERROR, now_ms);
    cerberus_ble_activity_finish(&activity_capture, CERBERUS_WATCH_ERROR, now_ms);
    scan_status.state = CERBERUS_BLE_SCAN_ERROR;
    scan_status.error_code = rc;
    if (dual_attached) dual_error = rc;
    response_error_locked();
    xSemaphoreGive(scanner_mutex);
    ESP_LOGW(TAG, "%s failed: rc=%d", operation, rc);
}

static void scan_control_done(uint32_t epoch, bool start)
{
    xSemaphoreTake(scanner_mutex, portMAX_DELAY);
    uint32_t *slot = start ? &scan_start_pending_epoch : &scan_stop_pending_epoch;
    if (*slot == epoch) *slot = 0U;
    response_error_locked();
    xSemaphoreGive(scanner_mutex);
}

/* The caller retains a pending/dispatching control reservation through this
 * entire operation. EALREADY only proves quiescence when this exact SDK scope
 * is still current and the SDK also reports that discovery is inactive. */
static bool cancel_scan_for_epoch(uint32_t epoch, int *result)
{
    xSemaphoreTake(scanner_mutex, portMAX_DELAY);
    bool current = scan_epoch_current_locked(epoch);
    bool accepted = current && dual_radio_active && scan_sdk_epoch == epoch;
    xSemaphoreGive(scanner_mutex);
    if (!current) return false;
    int rc = accepted ? ble_gap_disc_cancel() : 0;
    if (rc == BLE_HS_EALREADY && !ble_gap_disc_active()) rc = 0;
    xSemaphoreTake(scanner_mutex, portMAX_DELAY);
    current = scan_epoch_current_locked(epoch);
    if (current && rc == 0) {
        dual_radio_active = false;
        scan_sdk_epoch = 0U;
        scan_deadline_us = 0;
    }
    xSemaphoreGive(scanner_mutex);
    *result = rc;
    return current;
}

static void start_scan_event(struct ble_npl_event *event)
{
    uint32_t epoch = (uint32_t)(uintptr_t)ble_npl_event_get_arg(event);
    xSemaphoreTake(scanner_mutex, portMAX_DELAY);
    bool reserved = epoch && scan_start_pending_epoch == epoch;
    bool current = reserved && scan_epoch_current_locked(epoch) && scan_status.state == CERBERUS_BLE_SCAN_RUNNING;
    bool cancelled = scan_cancelled;
    bool response_scan = dual_attached && cerberus_dual_ble_response_mode(dual_mode) && dual_response;
    xSemaphoreGive(scanner_mutex);
    if (!reserved) return;
    if (!current || cancelled) {
        if (current) finish_scan_for_epoch(0, epoch);
        scan_control_done(epoch, true);
        return;
    }
#ifdef CERBERUS_C5_PANIC_DIAGNOSTIC
    esp_rom_printf("C5-DIAG start event\r\n");
#endif
    diagnostic_phase = 2;
    uint8_t own_address_type = 0;
    diagnostic_phase = 3;
    int rc = ble_hs_id_infer_auto(0, &own_address_type);
    diagnostic_phase = 4;
    if (rc == 0) {
#if MYNEWT_VAL(BLE_EXT_ADV)
        /* Only the existing explicit response mode requests responses on 1M.
         * Coded PHY and all passive modes retain their existing parameters. */
        const struct ble_gap_ext_disc_params parameters = {
            .itvl = 160U, .window = 80U, .passive = response_scan ? 0U : 1U,
        };
        const struct ble_gap_ext_disc_params coded = {
            .itvl = 160U, .window = 80U, .passive = 1U,
        };
#else
        struct ble_gap_disc_params parameters = {
            .itvl = response_scan ? 160U : 0U,
            .window = response_scan ? 80U : 0U,
            .filter_policy = 0, .limited = 0,
            .passive = response_scan ? 0U : 1U, .filter_duplicates = 0,
        };
#endif
        xSemaphoreTake(scanner_mutex, portMAX_DELAY);
        current = scan_epoch_current_locked(epoch) && scan_status.state == CERBERUS_BLE_SCAN_RUNNING;
        cancelled = scan_cancelled;
        if (current && !cancelled) {
            uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000LL);
            watch_capture.started_ms = now_ms;
            activity_capture.started_ms = now_ms;
            if (dual_attached && dual_mode == CERBERUS_DUAL_BLE_ANALYZER)
                cerberus_ble_analyzer_counter_arm(&dual_analyzer, epoch, esp_timer_get_time());
        }
        xSemaphoreGive(scanner_mutex);
        if (!current || cancelled) {
            if (current) finish_scan_for_epoch(0, epoch);
            scan_control_done(epoch, true);
            return;
        }
        diagnostic_phase = 5;
#ifdef CERBERUS_C5_PANIC_DIAGNOSTIC
        esp_rom_printf("C5-DIAG calling ble_gap_disc\r\n");
#endif
#if MYNEWT_VAL(BLE_EXT_ADV)
        rc = ble_gap_ext_disc(own_address_type, 0U, 0U, 0U, 0U, 0U,
            &parameters, &coded, gap_event, (void *)(uintptr_t)epoch);
#else
        rc = ble_gap_disc(own_address_type, BLE_HS_FOREVER, &parameters,
            gap_event, (void *)(uintptr_t)epoch);
#endif
        diagnostic_phase = 6;
        const int64_t accepted_us = esp_timer_get_time();
        const bool clock_valid = accepted_us >= 0 && accepted_us <= INT64_MAX - BLE_SCAN_DURATION_MS * 1000LL;
        xSemaphoreTake(scanner_mutex, portMAX_DELAY);
        current = scan_epoch_current_locked(epoch) && scan_status.state == CERBERUS_BLE_SCAN_RUNNING;
        if (current && rc == 0) {
            dual_radio_active = true;
            scan_sdk_epoch = epoch;
            scan_deadline_us = clock_valid ? accepted_us + BLE_SCAN_DURATION_MS * 1000LL : 0;
        }
        if (current && dual_attached && dual_mode == CERBERUS_DUAL_BLE_ANALYZER)
            cerberus_ble_analyzer_counter_accept(&dual_analyzer, rc == 0);
        xSemaphoreGive(scanner_mutex);
        if (!current) { scan_control_done(epoch, true); return; }
#ifdef CERBERUS_C5_PANIC_DIAGNOSTIC
        esp_rom_printf("C5-DIAG ble_gap_disc rc=%d\r\n", rc);
#endif
        if (rc == 0 && (!clock_valid || ble_npl_callout_reset(&scan_timeout_callout,
                ble_npl_time_ms_to_ticks32(BLE_SCAN_DURATION_MS)) != BLE_NPL_OK)) {
            int cancel_result;
            (void)cancel_scan_for_epoch(epoch, &cancel_result);
            rc = BLE_HS_EOS;
        }
    }
    if (rc != 0) {
        diagnostic_phase = 0x80U;
        set_control_error_for_epoch(epoch, rc, "passive scan start");
    }
    scan_control_done(epoch, true);
}

static void stop_scan_event(struct ble_npl_event *event)
{
    uint32_t epoch = (uint32_t)(uintptr_t)ble_npl_event_get_arg(event);
    xSemaphoreTake(scanner_mutex, portMAX_DELAY);
    bool reserved = epoch && scan_stop_pending_epoch == epoch;
    bool current = reserved && scan_epoch_current_locked(epoch);
    xSemaphoreGive(scanner_mutex);
    if (!reserved) return;
    if (current) {
        ble_npl_callout_stop(&scan_timeout_callout);
        int rc;
        if (cancel_scan_for_epoch(epoch, &rc)) {
            if (rc == 0) finish_scan_for_epoch(0, epoch);
            else set_control_error_for_epoch(epoch, rc, "passive scan stop");
        }
    }
    scan_control_done(epoch, false);
}

static void scan_timeout_event(struct ble_npl_event *event)
{
    (void)event;
    /* ESP_TIMER stop/reset does not remove a queued callout event. Its mutable
     * argument cannot identify the expired sweep: validate the current accepted
     * SDK scope and its full deadline before reserving any cancellation. */
    const int64_t now_us = esp_timer_get_time();
    xSemaphoreTake(scanner_mutex, portMAX_DELAY);
    uint32_t epoch = scan_sdk_epoch;
    bool due = scan_epoch_current_locked(epoch) && dual_radio_active &&
        scan_deadline_us > 0 && now_us >= scan_deadline_us && !scan_stop_pending_epoch;
    if (due) scan_stop_pending_epoch = epoch;
    xSemaphoreGive(scanner_mutex);
    if (!due) return;
    int rc;
    if (cancel_scan_for_epoch(epoch, &rc)) {
        if (rc == 0) finish_scan_for_epoch(0, epoch);
        else set_control_error_for_epoch(epoch, rc, "passive scan timeout");
    }
    scan_control_done(epoch, false);
}

static void set_connection_error(int rc, const char *operation)
{
    xSemaphoreTake(scanner_mutex, portMAX_DELAY);
    connection_status.state = CERBERUS_BLE_CONNECTION_ERROR;
    connection_status.connection_handle = BLE_CONNECTION_HANDLE_NONE;
    connection_status.error_code = rc;
    disconnect_requested = false;
    xSemaphoreGive(scanner_mutex);
    ESP_LOGW(TAG, "%s failed: rc=%d", operation, rc);
}

static void start_connection_event(struct ble_npl_event *event)
{
    (void)event;
    ble_addr_t peer = {0};
    xSemaphoreTake(scanner_mutex, portMAX_DELAY);
    peer.type = connection_status.address_type;
    memcpy(peer.val, connection_status.address, sizeof(peer.val));
    xSemaphoreGive(scanner_mutex);

    uint8_t own_address_type = 0;
    int rc = ble_hs_id_infer_auto(0, &own_address_type);
    if (rc == 0) {
        rc = ble_gap_connect(
            own_address_type,
            &peer,
            BLE_CONNECT_TIMEOUT_MS,
            NULL,
            gap_event,
            NULL);
    }
    if (rc != 0) {
        set_connection_error(rc, "BLE connection start");
    }
}

static void stop_connection_event(struct ble_npl_event *event)
{
    (void)event;
    xSemaphoreTake(scanner_mutex, portMAX_DELAY);
    uint16_t connection_handle = connection_status.connection_handle;
    xSemaphoreGive(scanner_mutex);

    int rc = ble_gap_terminate(connection_handle, BLE_ERR_REM_USER_CONN_TERM);
    if (rc != 0) {
        set_connection_error(rc, "BLE disconnect");
    }
}

static void copy_gatt_uuid(
    uint8_t *uuid_length,
    uint8_t uuid_bytes[16],
    const ble_uuid_t *uuid)
{
    if (uuid->type == BLE_UUID_TYPE_16) {
        uint16_t value = BLE_UUID16(uuid)->value;
        *uuid_length = 2;
        uuid_bytes[0] = (uint8_t)value;
        uuid_bytes[1] = (uint8_t)(value >> 8U);
    } else if (uuid->type == BLE_UUID_TYPE_32) {
        uint32_t value = BLE_UUID32(uuid)->value;
        *uuid_length = 4;
        for (uint8_t byte = 0; byte < 4U; ++byte) {
            uuid_bytes[byte] = (uint8_t)(value >> (byte * 8U));
        }
    } else if (uuid->type == BLE_UUID_TYPE_128) {
        *uuid_length = 16;
        memcpy(uuid_bytes, BLE_UUID128(uuid)->value, 16U);
    }
}

static int gatt_service_discovered(
    uint16_t connection_handle,
    const struct ble_gatt_error *error,
    const struct ble_gatt_svc *service,
    void *arg)
{
    (void)arg;
    xSemaphoreTake(scanner_mutex, portMAX_DELAY);
    bool current = gatt_service_status.state == CERBERUS_BLE_GATT_SERVICE_RUNNING &&
        connection_status.state == CERBERUS_BLE_CONNECTION_CONNECTED &&
        connection_status.connection_handle == connection_handle &&
        gatt_service_status.connection_generation == connection_status.generation;
    if (!current) {
        xSemaphoreGive(scanner_mutex);
        return 0;
    }

    if (error->status == 0 && service != NULL) {
        if (gatt_service_status.total_count < UINT16_MAX) {
            ++gatt_service_status.total_count;
        }
        if (gatt_service_status.stored_count < CERBERUS_BLE_GATT_SERVICE_MAX_RESULTS) {
            cerberus_ble_gatt_service_result_payload_t *record =
                &gatt_services[gatt_service_status.stored_count++];
            memset(record, 0, sizeof(*record));
            record->start_handle = service->start_handle;
            record->end_handle = service->end_handle;
            copy_gatt_uuid(&record->uuid_length, record->uuid, &service->uuid.u);
        }
        xSemaphoreGive(scanner_mutex);
        return 0;
    }

    if (error->status == BLE_HS_EDONE) {
        gatt_service_status.state = CERBERUS_BLE_GATT_SERVICE_COMPLETE;
        gatt_service_status.error_code = 0;
        ESP_LOGI(TAG, "GATT service discovery complete: total=%u stored=%u",
            gatt_service_status.total_count,
            gatt_service_status.stored_count);
    } else {
        gatt_service_status.state = CERBERUS_BLE_GATT_SERVICE_ERROR;
        gatt_service_status.error_code = error->status;
        ESP_LOGW(TAG, "GATT service discovery failed: status=%u", error->status);
    }
    xSemaphoreGive(scanner_mutex);
    return 0;
}

static int gatt_characteristic_discovered(
    uint16_t connection_handle,
    const struct ble_gatt_error *error,
    const struct ble_gatt_chr *characteristic,
    void *arg)
{
    (void)arg;
    xSemaphoreTake(scanner_mutex, portMAX_DELAY);
    bool current = gatt_characteristic_status.state == CERBERUS_BLE_GATT_CHARACTERISTIC_RUNNING &&
        connection_status.state == CERBERUS_BLE_CONNECTION_CONNECTED &&
        connection_status.connection_handle == connection_handle &&
        gatt_characteristic_status.connection_generation == connection_status.generation;
    if (!current) {
        xSemaphoreGive(scanner_mutex);
        return 0;
    }

    if (error->status == 0 && characteristic != NULL) {
        if (gatt_characteristic_status.total_count < UINT16_MAX) {
            ++gatt_characteristic_status.total_count;
        }
        if (gatt_characteristic_status.stored_count < CERBERUS_BLE_GATT_CHARACTERISTIC_MAX_RESULTS) {
            cerberus_ble_gatt_characteristic_result_payload_t *record =
                &gatt_characteristics[gatt_characteristic_status.stored_count++];
            memset(record, 0, sizeof(*record));
            record->definition_handle = characteristic->def_handle;
            record->value_handle = characteristic->val_handle;
            record->properties = characteristic->properties;
            copy_gatt_uuid(&record->uuid_length, record->uuid, &characteristic->uuid.u);
        }
        xSemaphoreGive(scanner_mutex);
        return 0;
    }

    if (error->status == BLE_HS_EDONE) {
        gatt_characteristic_status.state = CERBERUS_BLE_GATT_CHARACTERISTIC_COMPLETE;
        gatt_characteristic_status.error_code = 0;
        ESP_LOGI(TAG, "GATT characteristic discovery complete: total=%u stored=%u range=%04x-%04x",
            gatt_characteristic_status.total_count,
            gatt_characteristic_status.stored_count,
            gatt_characteristic_status.start_handle,
            gatt_characteristic_status.end_handle);
    } else {
        gatt_characteristic_status.state = CERBERUS_BLE_GATT_CHARACTERISTIC_ERROR;
        gatt_characteristic_status.error_code = error->status;
        ESP_LOGW(TAG, "GATT characteristic discovery failed: status=%u", error->status);
    }
    xSemaphoreGive(scanner_mutex);
    return 0;
}

static int gatt_characteristic_read(
    uint16_t connection_handle,
    const struct ble_gatt_error *error,
    struct ble_gatt_attr *attribute,
    void *arg)
{
    (void)arg;
    xSemaphoreTake(scanner_mutex, portMAX_DELAY);
    bool current = gatt_read_status.state == CERBERUS_BLE_GATT_READ_RUNNING &&
        connection_status.state == CERBERUS_BLE_CONNECTION_CONNECTED &&
        connection_status.connection_handle == connection_handle &&
        gatt_read_status.connection_generation == connection_status.generation;
    if (!current) {
        xSemaphoreGive(scanner_mutex);
        return 0;
    }

    if (error->status == 0 && attribute != NULL && attribute->om != NULL) {
        uint16_t total_length = OS_MBUF_PKTLEN(attribute->om);
        uint16_t stored_length = (uint16_t)bounded_size(total_length, sizeof(gatt_read_value));
        int rc = os_mbuf_copydata(attribute->om, 0, stored_length, gatt_read_value);
        if (rc == 0) {
            gatt_read_status.total_length = total_length;
            gatt_read_status.stored_length = stored_length;
            gatt_read_status.state = CERBERUS_BLE_GATT_READ_COMPLETE;
            gatt_read_status.error_code = 0;
            ESP_LOGI(TAG, "GATT read complete: handle=%04x total=%u stored=%u",
                gatt_read_status.value_handle,
                total_length,
                stored_length);
        } else {
            gatt_read_status.state = CERBERUS_BLE_GATT_READ_ERROR;
            gatt_read_status.error_code = rc;
        }
    } else {
        gatt_read_status.state = CERBERUS_BLE_GATT_READ_ERROR;
        gatt_read_status.error_code = error->status != 0 ? error->status : BLE_HS_EINVAL;
        ESP_LOGW(TAG, "GATT read failed: handle=%04x status=%u",
            gatt_read_status.value_handle,
            error->status);
    }
    xSemaphoreGive(scanner_mutex);
    return 0;
}

static void start_gatt_service_discovery_event(struct ble_npl_event *event)
{
    (void)event;
    xSemaphoreTake(scanner_mutex, portMAX_DELAY);
    uint16_t connection_handle = connection_status.connection_handle;
    xSemaphoreGive(scanner_mutex);

    int rc = ble_gattc_disc_all_svcs(
        connection_handle,
        gatt_service_discovered,
        NULL);
    if (rc != 0) {
        xSemaphoreTake(scanner_mutex, portMAX_DELAY);
        gatt_service_status.state = CERBERUS_BLE_GATT_SERVICE_ERROR;
        gatt_service_status.error_code = rc;
        xSemaphoreGive(scanner_mutex);
        ESP_LOGW(TAG, "GATT service discovery start failed: rc=%d", rc);
    }
}

static void start_gatt_characteristic_discovery_event(struct ble_npl_event *event)
{
    (void)event;
    xSemaphoreTake(scanner_mutex, portMAX_DELAY);
    bool ready = connection_status.state == CERBERUS_BLE_CONNECTION_CONNECTED &&
        gatt_characteristic_status.state == CERBERUS_BLE_GATT_CHARACTERISTIC_RUNNING &&
        gatt_characteristic_status.connection_generation == connection_status.generation;
    uint16_t connection_handle = connection_status.connection_handle;
    uint16_t start_handle = gatt_characteristic_status.start_handle;
    uint16_t end_handle = gatt_characteristic_status.end_handle;
    xSemaphoreGive(scanner_mutex);
    if (!ready) {
        return;
    }

    int rc = ble_gattc_disc_all_chrs(
        connection_handle,
        start_handle,
        end_handle,
        gatt_characteristic_discovered,
        NULL);
    if (rc != 0) {
        xSemaphoreTake(scanner_mutex, portMAX_DELAY);
        gatt_characteristic_status.state = CERBERUS_BLE_GATT_CHARACTERISTIC_ERROR;
        gatt_characteristic_status.error_code = rc;
        xSemaphoreGive(scanner_mutex);
        ESP_LOGW(TAG, "GATT characteristic discovery start failed: rc=%d", rc);
    }
}

static void start_gatt_read_event(struct ble_npl_event *event)
{
    (void)event;
    xSemaphoreTake(scanner_mutex, portMAX_DELAY);
    bool ready = connection_status.state == CERBERUS_BLE_CONNECTION_CONNECTED &&
        gatt_read_status.state == CERBERUS_BLE_GATT_READ_RUNNING &&
        gatt_read_status.connection_generation == connection_status.generation;
    uint16_t connection_handle = connection_status.connection_handle;
    uint16_t value_handle = gatt_read_status.value_handle;
    xSemaphoreGive(scanner_mutex);
    if (!ready) {
        return;
    }

    int rc = ble_gattc_read(connection_handle, value_handle, gatt_characteristic_read, NULL);
    if (rc != 0) {
        xSemaphoreTake(scanner_mutex, portMAX_DELAY);
        gatt_read_status.state = CERBERUS_BLE_GATT_READ_ERROR;
        gatt_read_status.error_code = rc;
        xSemaphoreGive(scanner_mutex);
        ESP_LOGW(TAG, "GATT read start failed: handle=%04x rc=%d", value_handle, rc);
    }
}

esp_err_t cerberus_ble_scanner_init(void)
{
    dual_boot_id = ((uint64_t)esp_random() << 32U) | esp_random();
    if (!dual_boot_id) dual_boot_id = 1U;
    dual_snapshot.counter = dual_boot_id; /* Boot seed, then saturating monotonic handles. */
    dual_snapshot.valid = false;
    if (diagnostic_magic != BLE_DIAGNOSTIC_MAGIC) {
        diagnostic_magic = BLE_DIAGNOSTIC_MAGIC;
        diagnostic_boot_count = 1;
        diagnostic_phase = 0;
    } else if (diagnostic_boot_count != UINT8_MAX) {
        ++diagnostic_boot_count;
    }

    scanner_mutex = xSemaphoreCreateMutex();
    if (scanner_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }

    scan_status.state = CERBERUS_BLE_SCAN_ERROR;
    scan_status.error_code = ESP_ERR_INVALID_STATE;
    connection_status.state = CERBERUS_BLE_CONNECTION_ERROR;
    connection_status.connection_handle = BLE_CONNECTION_HANDLE_NONE;
    connection_status.error_code = ESP_ERR_INVALID_STATE;
    gatt_service_status.state = CERBERUS_BLE_GATT_SERVICE_ERROR;
    gatt_service_status.error_code = ESP_ERR_INVALID_STATE;
    gatt_characteristic_status.state = CERBERUS_BLE_GATT_CHARACTERISTIC_ERROR;
    gatt_characteristic_status.error_code = ESP_ERR_INVALID_STATE;
    gatt_read_status.state = CERBERUS_BLE_GATT_READ_ERROR;
    gatt_read_status.error_code = ESP_ERR_INVALID_STATE;
    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        scan_status.error_code = err;
        ESP_LOGE(TAG, "NimBLE initialization failed: %s", esp_err_to_name(err));
        return err;
    }

    ble_hs_cfg.reset_cb = host_reset;
    ble_hs_cfg.sync_cb = host_sync;
    ble_npl_event_init(&start_event, start_scan_event, NULL);
    ble_npl_event_init(&stop_event, stop_scan_event, NULL);
    ble_npl_event_init(&connect_event, start_connection_event, NULL);
    ble_npl_event_init(&disconnect_event, stop_connection_event, NULL);
    ble_npl_event_init(&discover_services_event, start_gatt_service_discovery_event, NULL);
    ble_npl_event_init(
        &discover_characteristics_event,
        start_gatt_characteristic_discovery_event,
        NULL);
    ble_npl_event_init(&read_characteristic_event, start_gatt_read_event, NULL);
    ble_npl_callout_init(
        &scan_timeout_callout,
        nimble_port_get_dflt_eventq(),
        scan_timeout_event,
        NULL);
    nimble_port_freertos_init(host_task);
    return ESP_OK;
}

uint8_t cerberus_ble_scanner_get_boot_count(void)
{
    return diagnostic_boot_count;
}

uint8_t cerberus_ble_scanner_get_phase(void)
{
    return diagnostic_phase;
}

static bool another_radio_operation_is_active(void)
{
    cerberus_wifi_scan_status_payload_t wifi_status;
    cerberus_wifi_connection_status_payload_t wifi_connection;
    cerberus_wifi_scanner_get_status(&wifi_status);
    cerberus_wifi_connection_get_status(&wifi_connection);
    return cerberus_ieee802154_scanner_is_running() ||
        cerberus_wifi_defense_survey_is_running() || cerberus_wifi_station_running() ||
        wifi_status.state == CERBERUS_WIFI_SCAN_RUNNING ||
        wifi_connection.state == CERBERUS_WIFI_CONNECTION_STAGING ||
        wifi_connection.state == CERBERUS_WIFI_CONNECTION_CONNECTING ||
        wifi_connection.state == CERBERUS_WIFI_CONNECTION_CONNECTED ||
        wifi_connection.state == CERBERUS_WIFI_CONNECTION_DISCONNECTING;
}

static esp_err_t start_scan(bool companion)
{
    if(cerberus_c5_test_running())return ESP_ERR_INVALID_STATE;
    if (scanner_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t preempt_error = cerberus_wifi_channel_survey_preempt();
    if (preempt_error != ESP_OK) return preempt_error;
    if (another_radio_operation_is_active()) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(scanner_mutex, portMAX_DELAY);
    dual_expire_locked();
    bool connection_active = connection_status.state == CERBERUS_BLE_CONNECTION_CONNECTING ||
        connection_status.state == CERBERUS_BLE_CONNECTION_CONNECTED ||
        connection_status.state == CERBERUS_BLE_CONNECTION_DISCONNECTING;
    if (!host_synced || scan_radio_owned_locked() || connection_active ||
        (dual_attached && !companion) ||
        (companion && cerberus_dual_ble_response_mode(dual_mode) && !dual_response) ||
        scan_callback_epoch == UINT32_MAX) {
        esp_err_t err = host_synced ? ESP_ERR_INVALID_STATE : ESP_ERR_NOT_FINISHED;
        xSemaphoreGive(scanner_mutex);
        return err;
    }
    ++scan_status.generation;
    if (companion) dual_generation = scan_status.generation;
    scan_status.state = CERBERUS_BLE_SCAN_RUNNING;
    scan_status.total_count = 0;
    scan_status.stored_count = 0;
    scan_status.unnamed_count = 0;
    scan_status.dropped_reports = 0;
    scan_status.error_code = 0;
    scan_cancelled = false;
    cerberus_ble_analyzer_counter_reset(&dual_analyzer);
    const int64_t started_us = esp_timer_get_time();
    uint32_t now_ms = (uint32_t)(started_us / 1000LL);
    cerberus_ble_inventory_admission_begin(&scan_inventory,
        companion && dual_attached && dual_version == CERBERUS_DUAL_BLE_VERSION &&
            cerberus_dual_ble_snapshot_mode(dual_mode), started_us);
    uint64_t epoch = watch_capture_epoch == UINT64_MAX ? 0U : ++watch_capture_epoch;
    cerberus_watch_capture_begin_with_epoch(&watch_capture, CERBERUS_WATCH_RADIO_BLE,
        scan_status.generation, now_ms, epoch);
    ++scan_callback_epoch;
    scan_epoch_retired = false;
    scan_start_pending_epoch = scan_callback_epoch;
    scan_sdk_epoch = 0U;
    scan_deadline_us = 0;
    ble_npl_event_set_arg(&start_event, (void *)(uintptr_t)scan_callback_epoch);
    cerberus_ble_activity_begin(&activity_capture, scan_status.generation, now_ms,
        MYNEWT_VAL(BLE_EXT_ADV) != 0);
    memset(scan_results, 0, sizeof(scan_results));
    if (companion && cerberus_dual_ble_family_mode(dual_mode))
        memset(scan_family, 0, sizeof(scan_family));
    if (companion && dual_response) memset(dual_response,0,sizeof(*dual_response));
    diagnostic_phase = 1;
    xSemaphoreGive(scanner_mutex);
    ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &start_event);
    return ESP_OK;
}

esp_err_t cerberus_ble_scanner_start(void) { return start_scan(false); }

esp_err_t cerberus_ble_watch_get_status(uint8_t generation, cerberus_watch_scan_status_payload_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    if (!scanner_mutex) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(scanner_mutex, portMAX_DELAY);
    bool valid = watch_capture.status.generation == generation && watch_capture.status.radio == CERBERUS_WATCH_RADIO_BLE;
    if (valid) *out = watch_capture.status;
    xSemaphoreGive(scanner_mutex);
    return valid ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t cerberus_ble_watch_get_data(const cerberus_watch_scan_data_request_t *request,
    cerberus_watch_scan_data_payload_t *out)
{
    if (!request || !out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    if (!scanner_mutex) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(scanner_mutex, portMAX_DELAY);
    bool valid = cerberus_watch_capture_get_chunk(&watch_capture, request->radio,
        request->generation, request->index, request->chunk, out);
    xSemaphoreGive(scanner_mutex);
    return valid ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t cerberus_ble_watch_get_timing(const cerberus_watch_scan_timing_request_t *request,
    cerberus_watch_scan_timing_payload_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    if (!request) return ESP_ERR_INVALID_ARG;
    if (!scanner_mutex) return ESP_ERR_INVALID_STATE;
    int64_t now_us = esp_timer_get_time();
    xSemaphoreTake(scanner_mutex, portMAX_DELAY);
    bool valid = cerberus_watch_capture_get_timing(&watch_capture, request->radio,
        request->generation, request->index, now_us >= 0 ? (uint64_t)now_us : 0U, out);
    if (valid && now_us < 0) { out->flags = 0U; out->source_age_ms = 0U; }
    xSemaphoreGive(scanner_mutex);
    return valid ? ESP_OK : ESP_ERR_INVALID_STATE;
}

static esp_err_t ble_watch_motion_snapshot(const cerberus_watch_scan_data_request_t *request,
    sentinel_watch_motion_t *motion, cerberus_watch_scan_timing_payload_t *timing)
{
    memset(motion, 0, sizeof(*motion));
    memset(timing, 0, sizeof(*timing));
    if (!request || request->chunk) return ESP_ERR_INVALID_ARG;
    if (!scanner_mutex) return ESP_ERR_INVALID_STATE;
    int64_t now_us = esp_timer_get_time();
    xSemaphoreTake(scanner_mutex, portMAX_DELAY);
    bool valid = cerberus_watch_capture_get_motion(&watch_capture, request->radio,
        request->generation, request->index, now_us >= 0 ? (uint64_t)now_us : 0U, motion, timing);
    if (valid && now_us < 0) { timing->flags = 0U; timing->source_age_ms = 0U; }
    xSemaphoreGive(scanner_mutex);
    return valid ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t cerberus_ble_watch_get_motion(const cerberus_watch_scan_data_request_t *request,
    cerberus_watch_scan_motion_payload_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    sentinel_watch_motion_t motion;
    cerberus_watch_scan_timing_payload_t timing;
    esp_err_t error = ble_watch_motion_snapshot(request, &motion, &timing);
    if (error != ESP_OK) return error;
    out->radio = timing.radio; out->generation = timing.generation; out->index = timing.index;
    out->capture_epoch = timing.capture_epoch; out->motion = motion;
    return ESP_OK;
}

esp_err_t cerberus_ble_watch_get_motion_timing(const cerberus_watch_scan_data_request_t *request,
    cerberus_watch_scan_timing_payload_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    sentinel_watch_motion_t motion;
    return ble_watch_motion_snapshot(request, &motion, out);
}

esp_err_t cerberus_ble_watch_get_activity(uint8_t generation, cerberus_ble_activity_payload_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    if (!scanner_mutex) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(scanner_mutex, portMAX_DELAY);
    bool valid = activity_capture.status.generation == generation && activity_capture.status.state == CERBERUS_WATCH_COMPLETE;
    if (valid) *out = activity_capture.status;
    xSemaphoreGive(scanner_mutex);
    return valid ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t cerberus_ble_connection_start(const cerberus_ble_connect_request_t *request)
{
    if(cerberus_c5_test_running())return ESP_ERR_INVALID_STATE;
    if (request == NULL || scanner_mutex == NULL || request->address_type > BLE_ADDR_RANDOM_ID) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t preempt_error = cerberus_wifi_channel_survey_preempt();
    if (preempt_error != ESP_OK) return preempt_error;
    if (another_radio_operation_is_active()) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(scanner_mutex, portMAX_DELAY);
    dual_expire_locked();
    bool connection_active = connection_status.state == CERBERUS_BLE_CONNECTION_CONNECTING ||
        connection_status.state == CERBERUS_BLE_CONNECTION_CONNECTED ||
        connection_status.state == CERBERUS_BLE_CONNECTION_DISCONNECTING;
    if (!host_synced || scan_radio_owned_locked() || connection_active || dual_attached) {
        esp_err_t err = host_synced ? ESP_ERR_INVALID_STATE : ESP_ERR_NOT_FINISHED;
        xSemaphoreGive(scanner_mutex);
        return err;
    }

    ++connection_status.generation;
    connection_status.state = CERBERUS_BLE_CONNECTION_CONNECTING;
    connection_status.address_type = request->address_type;
    memcpy(connection_status.address, request->address, sizeof(connection_status.address));
    connection_status.connection_handle = BLE_CONNECTION_HANDLE_NONE;
    connection_status.error_code = 0;
    disconnect_requested = false;
    gatt_service_status.state = CERBERUS_BLE_GATT_SERVICE_IDLE;
    gatt_service_status.total_count = 0;
    gatt_service_status.stored_count = 0;
    gatt_service_status.connection_generation = connection_status.generation;
    gatt_service_status.error_code = 0;
    memset(gatt_services, 0, sizeof(gatt_services));
    gatt_characteristic_status.state = CERBERUS_BLE_GATT_CHARACTERISTIC_IDLE;
    gatt_characteristic_status.total_count = 0;
    gatt_characteristic_status.stored_count = 0;
    gatt_characteristic_status.connection_generation = connection_status.generation;
    gatt_characteristic_status.start_handle = 0;
    gatt_characteristic_status.end_handle = 0;
    gatt_characteristic_status.error_code = 0;
    memset(gatt_characteristics, 0, sizeof(gatt_characteristics));
    reset_gatt_read_locked(CERBERUS_BLE_GATT_READ_IDLE, 0);
    xSemaphoreGive(scanner_mutex);
    ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &connect_event);
    return ESP_OK;
}

esp_err_t cerberus_ble_connection_disconnect(void)
{
    if (scanner_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(scanner_mutex, portMAX_DELAY);
    if (connection_status.state != CERBERUS_BLE_CONNECTION_CONNECTED) {
        xSemaphoreGive(scanner_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    connection_status.state = CERBERUS_BLE_CONNECTION_DISCONNECTING;
    connection_status.error_code = 0;
    disconnect_requested = true;
    xSemaphoreGive(scanner_mutex);
    ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &disconnect_event);
    return ESP_OK;
}

void cerberus_ble_connection_get_status(cerberus_ble_connection_status_payload_t *status)
{
    if (status == NULL) {
        return;
    }
    memset(status, 0, sizeof(*status));
    status->connection_handle = BLE_CONNECTION_HANDLE_NONE;
    if (scanner_mutex == NULL) {
        status->state = CERBERUS_BLE_CONNECTION_ERROR;
        status->error_code = ESP_ERR_INVALID_STATE;
        return;
    }
    xSemaphoreTake(scanner_mutex, portMAX_DELAY);
    *status = connection_status;
    xSemaphoreGive(scanner_mutex);
}

esp_err_t cerberus_ble_gatt_service_discovery_start(void)
{
    if (scanner_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(scanner_mutex, portMAX_DELAY);
    if (connection_status.state != CERBERUS_BLE_CONNECTION_CONNECTED ||
        gatt_service_status.state == CERBERUS_BLE_GATT_SERVICE_RUNNING ||
        gatt_characteristic_status.state == CERBERUS_BLE_GATT_CHARACTERISTIC_RUNNING ||
        gatt_read_status.state == CERBERUS_BLE_GATT_READ_RUNNING) {
        xSemaphoreGive(scanner_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    ++gatt_service_status.generation;
    gatt_service_status.state = CERBERUS_BLE_GATT_SERVICE_RUNNING;
    gatt_service_status.total_count = 0;
    gatt_service_status.stored_count = 0;
    gatt_service_status.connection_generation = connection_status.generation;
    gatt_service_status.error_code = 0;
    memset(gatt_services, 0, sizeof(gatt_services));
    gatt_characteristic_status.state = CERBERUS_BLE_GATT_CHARACTERISTIC_IDLE;
    gatt_characteristic_status.total_count = 0;
    gatt_characteristic_status.stored_count = 0;
    gatt_characteristic_status.connection_generation = connection_status.generation;
    gatt_characteristic_status.start_handle = 0;
    gatt_characteristic_status.end_handle = 0;
    gatt_characteristic_status.error_code = 0;
    memset(gatt_characteristics, 0, sizeof(gatt_characteristics));
    reset_gatt_read_locked(CERBERUS_BLE_GATT_READ_IDLE, 0);
    xSemaphoreGive(scanner_mutex);
    ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &discover_services_event);
    return ESP_OK;
}

void cerberus_ble_gatt_service_get_status(cerberus_ble_gatt_service_status_payload_t *status)
{
    if (status == NULL) {
        return;
    }
    memset(status, 0, sizeof(*status));
    if (scanner_mutex == NULL) {
        status->state = CERBERUS_BLE_GATT_SERVICE_ERROR;
        status->error_code = ESP_ERR_INVALID_STATE;
        return;
    }
    xSemaphoreTake(scanner_mutex, portMAX_DELAY);
    *status = gatt_service_status;
    xSemaphoreGive(scanner_mutex);
}

esp_err_t cerberus_ble_gatt_service_get_result(
    uint8_t generation,
    uint8_t index,
    cerberus_ble_gatt_service_result_payload_t *result)
{
    if (result == NULL || scanner_mutex == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(scanner_mutex, portMAX_DELAY);
    if (gatt_service_status.state != CERBERUS_BLE_GATT_SERVICE_COMPLETE ||
        generation != gatt_service_status.generation ||
        index >= gatt_service_status.stored_count) {
        xSemaphoreGive(scanner_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    *result = gatt_services[index];
    xSemaphoreGive(scanner_mutex);
    return ESP_OK;
}

esp_err_t cerberus_ble_gatt_characteristic_discovery_start(
    const cerberus_ble_gatt_characteristic_discover_request_t *request)
{
    if (request == NULL || scanner_mutex == NULL || request->start_handle == 0U ||
        request->start_handle > request->end_handle) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(scanner_mutex, portMAX_DELAY);
    bool known_service = false;
    if (connection_status.state == CERBERUS_BLE_CONNECTION_CONNECTED &&
        gatt_service_status.state == CERBERUS_BLE_GATT_SERVICE_COMPLETE &&
        gatt_service_status.connection_generation == connection_status.generation) {
        for (uint8_t index = 0; index < gatt_service_status.stored_count; ++index) {
            if (gatt_services[index].start_handle == request->start_handle &&
                gatt_services[index].end_handle == request->end_handle) {
                known_service = true;
                break;
            }
        }
    }
    if (!known_service ||
        gatt_characteristic_status.state == CERBERUS_BLE_GATT_CHARACTERISTIC_RUNNING ||
        gatt_read_status.state == CERBERUS_BLE_GATT_READ_RUNNING) {
        xSemaphoreGive(scanner_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    ++gatt_characteristic_status.generation;
    gatt_characteristic_status.state = CERBERUS_BLE_GATT_CHARACTERISTIC_RUNNING;
    gatt_characteristic_status.total_count = 0;
    gatt_characteristic_status.stored_count = 0;
    gatt_characteristic_status.connection_generation = connection_status.generation;
    gatt_characteristic_status.start_handle = request->start_handle;
    gatt_characteristic_status.end_handle = request->end_handle;
    gatt_characteristic_status.error_code = 0;
    memset(gatt_characteristics, 0, sizeof(gatt_characteristics));
    reset_gatt_read_locked(CERBERUS_BLE_GATT_READ_IDLE, 0);
    xSemaphoreGive(scanner_mutex);
    ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &discover_characteristics_event);
    return ESP_OK;
}

void cerberus_ble_gatt_characteristic_get_status(
    cerberus_ble_gatt_characteristic_status_payload_t *status)
{
    if (status == NULL) {
        return;
    }
    memset(status, 0, sizeof(*status));
    if (scanner_mutex == NULL) {
        status->state = CERBERUS_BLE_GATT_CHARACTERISTIC_ERROR;
        status->error_code = ESP_ERR_INVALID_STATE;
        return;
    }
    xSemaphoreTake(scanner_mutex, portMAX_DELAY);
    *status = gatt_characteristic_status;
    xSemaphoreGive(scanner_mutex);
}

esp_err_t cerberus_ble_gatt_characteristic_get_result(
    uint8_t generation,
    uint8_t index,
    cerberus_ble_gatt_characteristic_result_payload_t *result)
{
    if (result == NULL || scanner_mutex == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(scanner_mutex, portMAX_DELAY);
    if (gatt_characteristic_status.state != CERBERUS_BLE_GATT_CHARACTERISTIC_COMPLETE ||
        generation != gatt_characteristic_status.generation ||
        index >= gatt_characteristic_status.stored_count) {
        xSemaphoreGive(scanner_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    *result = gatt_characteristics[index];
    xSemaphoreGive(scanner_mutex);
    return ESP_OK;
}

esp_err_t cerberus_ble_gatt_read_start(const cerberus_ble_gatt_read_request_t *request)
{
    if (request == NULL || scanner_mutex == NULL || request->value_handle == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(scanner_mutex, portMAX_DELAY);
    bool known_readable_characteristic = false;
    if (connection_status.state == CERBERUS_BLE_CONNECTION_CONNECTED &&
        gatt_characteristic_status.state == CERBERUS_BLE_GATT_CHARACTERISTIC_COMPLETE &&
        gatt_characteristic_status.connection_generation == connection_status.generation) {
        for (uint8_t index = 0; index < gatt_characteristic_status.stored_count; ++index) {
            if (gatt_characteristics[index].value_handle == request->value_handle &&
                (gatt_characteristics[index].properties & BLE_GATT_CHR_PROP_READ) != 0U) {
                known_readable_characteristic = true;
                break;
            }
        }
    }
    if (!known_readable_characteristic ||
        gatt_read_status.state == CERBERUS_BLE_GATT_READ_RUNNING) {
        xSemaphoreGive(scanner_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    ++gatt_read_status.generation;
    gatt_read_status.state = CERBERUS_BLE_GATT_READ_RUNNING;
    gatt_read_status.connection_generation = connection_status.generation;
    gatt_read_status.value_handle = request->value_handle;
    gatt_read_status.total_length = 0;
    gatt_read_status.stored_length = 0;
    gatt_read_status.error_code = 0;
    memset(gatt_read_value, 0, sizeof(gatt_read_value));
    xSemaphoreGive(scanner_mutex);
    ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &read_characteristic_event);
    return ESP_OK;
}

void cerberus_ble_gatt_read_get_status(cerberus_ble_gatt_read_status_payload_t *status)
{
    if (status == NULL) {
        return;
    }
    memset(status, 0, sizeof(*status));
    if (scanner_mutex == NULL) {
        status->state = CERBERUS_BLE_GATT_READ_ERROR;
        status->error_code = ESP_ERR_INVALID_STATE;
        return;
    }
    xSemaphoreTake(scanner_mutex, portMAX_DELAY);
    *status = gatt_read_status;
    xSemaphoreGive(scanner_mutex);
}

esp_err_t cerberus_ble_gatt_read_get_data(
    uint8_t generation,
    uint8_t chunk,
    cerberus_ble_gatt_read_data_payload_t *result)
{
    if (result == NULL || scanner_mutex == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(scanner_mutex, portMAX_DELAY);
    size_t offset = (size_t)chunk * CERBERUS_BLE_GATT_READ_CHUNK_LENGTH;
    if (gatt_read_status.state != CERBERUS_BLE_GATT_READ_COMPLETE ||
        generation != gatt_read_status.generation || offset >= gatt_read_status.stored_length) {
        xSemaphoreGive(scanner_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    memset(result, 0, sizeof(*result));
    result->generation = generation;
    result->chunk = chunk;
    result->data_length = (uint8_t)bounded_size(
        gatt_read_status.stored_length - offset,
        CERBERUS_BLE_GATT_READ_CHUNK_LENGTH);
    memcpy(result->data, &gatt_read_value[offset], result->data_length);
    xSemaphoreGive(scanner_mutex);
    return ESP_OK;
}

static esp_err_t stop_scan(bool companion)
{
    if (scanner_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(scanner_mutex, portMAX_DELAY);
    dual_expire_locked();
    if ((!companion && dual_attached) || !scan_radio_owned_locked()) {
        xSemaphoreGive(scanner_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    scan_cancelled = true;
    if (scan_stop_pending_epoch || scan_epoch_retired) {
        xSemaphoreGive(scanner_mutex);
        return ESP_OK;
    }
    scan_stop_pending_epoch = scan_callback_epoch;
    ble_npl_event_set_arg(&stop_event, (void *)(uintptr_t)scan_stop_pending_epoch);
    xSemaphoreGive(scanner_mutex);
    ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &stop_event);
    return ESP_OK;
}

esp_err_t cerberus_ble_scanner_stop(void) { return stop_scan(false); }

void cerberus_ble_scanner_get_status(cerberus_ble_scan_status_payload_t *status)
{
    if (status == NULL) {
        return;
    }
    memset(status, 0, sizeof(*status));
    if (scanner_mutex == NULL) {
        status->state = CERBERUS_BLE_SCAN_ERROR;
        status->error_code = ESP_ERR_INVALID_STATE;
        return;
    }
    xSemaphoreTake(scanner_mutex, portMAX_DELAY);
    *status = scan_status;
    /* Keep other C5 radio owners excluded through queued controls and uncertain
     * SDK cancellation, while retaining the real diagnostic error_code. */
    if (scan_radio_owned_locked()) status->state = CERBERUS_BLE_SCAN_RUNNING;
    xSemaphoreGive(scanner_mutex);
}

esp_err_t cerberus_ble_scanner_get_result(
    uint8_t generation,
    uint8_t index,
    cerberus_ble_scan_result_payload_t *result)
{
    if (result == NULL || scanner_mutex == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(scanner_mutex, portMAX_DELAY);
    if (scan_status.state != CERBERUS_BLE_SCAN_COMPLETE ||
        generation != scan_status.generation || index >= scan_status.stored_count) {
        xSemaphoreGive(scanner_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    const cerberus_ble_record_t *record = &scan_results[index];
    memset(result, 0, sizeof(*result));
    result->generation = generation;
    result->index = index;
    result->rssi = record->rssi;
    result->address_type = record->address_type;
    result->name_length = record->name_length;
    result->uuid_length = record->uuid_length;
    memcpy(result->address, record->address, sizeof(result->address));
    result->manufacturer_id = record->manufacturer_id;
    result->flags = record->flags;
    result->advertisement_length = record->advertisement_length;
    result->advertisement_event_type = record->advertisement_event_type;
    result->advertisement_flags = record->advertisement_flags;
    result->reports_seen = record->reports_seen;
    xSemaphoreGive(scanner_mutex);
    return ESP_OK;
}

esp_err_t cerberus_ble_scanner_get_data_chunk(
    uint8_t generation,
    uint8_t index,
    uint8_t kind,
    uint8_t chunk,
    cerberus_ble_scan_data_payload_t *result)
{
    if (result == NULL || scanner_mutex == NULL ||
        (kind != CERBERUS_BLE_DATA_NAME && kind != CERBERUS_BLE_DATA_SERVICE_UUID &&
         kind != CERBERUS_BLE_DATA_RAW_ADVERTISEMENT)) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(scanner_mutex, portMAX_DELAY);
    if (scan_status.state != CERBERUS_BLE_SCAN_COMPLETE ||
        generation != scan_status.generation || index >= scan_status.stored_count) {
        xSemaphoreGive(scanner_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    const cerberus_ble_record_t *record = &scan_results[index];
    const uint8_t *source = kind == CERBERUS_BLE_DATA_NAME ? record->name :
        kind == CERBERUS_BLE_DATA_SERVICE_UUID ? record->uuid : record->advertisement;
    size_t source_length = kind == CERBERUS_BLE_DATA_NAME ? record->name_length :
        kind == CERBERUS_BLE_DATA_SERVICE_UUID ? record->uuid_length : record->advertisement_length;
    size_t offset = (size_t)chunk * CERBERUS_BLE_DATA_CHUNK_SIZE;
    if (offset > source_length) {
        xSemaphoreGive(scanner_mutex);
        return ESP_ERR_INVALID_ARG;
    }
    size_t byte_count = source_length - offset;
    if (byte_count > CERBERUS_BLE_DATA_CHUNK_SIZE) {
        byte_count = CERBERUS_BLE_DATA_CHUNK_SIZE;
    }

    memset(result, 0, sizeof(*result));
    result->generation = generation;
    result->index = index;
    result->kind = kind;
    result->chunk = chunk;
    result->byte_count = (uint8_t)byte_count;
    if (byte_count > 0U) {
        memcpy(result->bytes, source + offset, byte_count);
    }
    xSemaphoreGive(scanner_mutex);
    return ESP_OK;
}

#include "dual_ble_service_impl.h"

bool cerberus_ble_wardrive_ready(void)
{
    if(!scanner_mutex)return false;
    xSemaphoreTake(scanner_mutex,portMAX_DELAY);
    bool ready=host_synced&&!scan_radio_owned_locked()&&!dual_attached&&
        connection_status.state!=CERBERUS_BLE_CONNECTION_CONNECTING&&
        connection_status.state!=CERBERUS_BLE_CONNECTION_CONNECTED&&
        connection_status.state!=CERBERUS_BLE_CONNECTION_DISCONNECTING;
    xSemaphoreGive(scanner_mutex);return ready;
}
