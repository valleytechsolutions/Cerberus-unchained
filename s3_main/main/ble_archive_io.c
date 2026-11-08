#include "ble_archive_io.h"
#include "storage.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stddef.h>
#include <string.h>

#define BLE_ARCHIVE_IO_STACK_BYTES 6144U
#define BLE_ARCHIVE_IO_MIN_FREE_INTERNAL (24U * 1024U)
#define BLE_ARCHIVE_IO_MIN_LARGEST_INTERNAL (BLE_ARCHIVE_IO_STACK_BYTES + 2048U)

_Static_assert(CERBERUS_BLE_ARCHIVE_MAX_SIZE <= CERBERUS_STORAGE_BLE_ARCHIVE_MAX_BYTES,
    "Archive codec must fit its storage bound");
_Static_assert(CERBERUS_AIRTAG_JSON_MAX_BYTES <= CERBERUS_STORAGE_AIRTAG_JSON_MAX_BYTES,
    "JSON codec must fit its storage bound");
_Static_assert(CERBERUS_WIFI_ARCHIVE_MAX_SIZE <= CERBERUS_STORAGE_WIFI_ARCHIVE_MAX_BYTES,
    "Wi-Fi codec must fit its storage bound");
_Static_assert(CERBERUS_WIFI_MARAUDER_AP_MAX_BYTES <= CERBERUS_STORAGE_AP_JSON_MAX_BYTES &&
    CERBERUS_WIFI_MARAUDER_SSID_MAX_BYTES <= CERBERUS_STORAGE_SSID_TEXT_MAX_BYTES,
    "Wi-Fi interchange codecs must fit their storage bounds");
_Static_assert(CERBERUS_PROBE_SNIFF_REPORT_MAX_BYTES <= CERBERUS_STORAGE_PROBE_REPORT_MAX_BYTES,
    "Probe report must fit its storage bound");
_Static_assert(MESH_REPORT_MAX_BYTES <= CERBERUS_STORAGE_MESH_REPORT_MAX_BYTES,
    "Mesh report must fit its storage bound");
_Static_assert(MESH_ARCHIVE_MAX_BYTES <= CERBERUS_STORAGE_MESH_ARCHIVE_MAX_BYTES,
    "Native Mesh codec must fit its storage bound");
_Static_assert(MESH_LOCATE_REPORT_MAX_BYTES <= CERBERUS_STORAGE_MESH_REPORT_MAX_BYTES,
    "Locate report must fit the shared Mesh CSV storage bound");
_Static_assert(CERBERUS_BLE_ARCHIVE_IO_PATH_CAPACITY == CERBERUS_STORAGE_PATH_MAX &&
    CERBERUS_BLE_ARCHIVE_IO_NAME_CAPACITY == CERBERUS_STORAGE_NAME_MAX,
    "Archive paths must fit storage validators");

typedef struct {
    char relative_path[CERBERUS_BLE_ARCHIVE_IO_PATH_CAPACITY];
    char file_name[CERBERUS_BLE_ARCHIVE_IO_NAME_CAPACITY];
    union {
        struct {
            cerberus_ble_archive_t document;
            uint8_t bytes[CERBERUS_BLE_ARCHIVE_MAX_SIZE];
        } native;
        struct {
            cerberus_airtag_json_t document;
            /* Export source is immutable until conversion completes. Encoding
             * then reuses this aligned scratch; the codec arguments never
             * overlap, and no third document/buffer allocation is needed. */
            union {
                cerberus_ble_archive_t export_source;
                uint8_t bytes[CERBERUS_AIRTAG_JSON_MAX_BYTES];
            } buffer;
        } json;
        struct {
            cerberus_wifi_archive_t document;
            uint8_t bytes[CERBERUS_WIFI_ARCHIVE_MAX_SIZE];
        } wifi;
        struct {
            cerberus_wifi_marauder_ap_t document;
            union {
                cerberus_wifi_archive_t export_source;
                uint8_t bytes[CERBERUS_WIFI_MARAUDER_AP_MAX_BYTES];
            } buffer;
        } ap;
        struct {
            cerberus_wifi_marauder_ssid_t document;
            union {
                cerberus_wifi_archive_t export_source;
                uint8_t bytes[CERBERUS_WIFI_MARAUDER_SSID_MAX_BYTES];
            } buffer;
        } ssid;
        struct {
            cerberus_probe_sniff_snapshot_t snapshot;
            char bytes[CERBERUS_PROBE_SNIFF_REPORT_MAX_BYTES + 1U];
        } probe;
        struct {
            mesh_scan_t snapshot;
            char bytes[MESH_REPORT_MAX_BYTES + 1U];
        } mesh;
        struct {
            mesh_locate_t snapshot;
            char bytes[MESH_LOCATE_REPORT_MAX_BYTES + 1U];
        } locate;
        struct {
            mesh_archive_t document;
            uint8_t bytes[MESH_ARCHIVE_MAX_BYTES];
        } mesh_archive;
        struct {
            mesh_archive_t document;
            uint8_t bytes[MESH_ARCHIVE_MAX_BYTES];
            mesh_archive_t scratch;
        } mesh_archive_load;
        struct {
            mesh_archive_t document;
            char bytes[MESH_REPORT_MAX_BYTES + 1U];
        } mesh_archive_csv;
    } payload;
} ble_archive_io_context_t;
static portMUX_TYPE archive_io_lock = portMUX_INITIALIZER_UNLOCKED;
static ble_archive_io_context_t *archive_io_context;
static cerberus_ble_archive_io_status_t archive_io_status;
static uint64_t archive_io_last_job;
static bool archive_io_release_requested;
static uint16_t archive_io_readers;

static bool archive_io_loading(cerberus_ble_archive_io_operation_t operation)
{ return operation == CERBERUS_BLE_ARCHIVE_IO_LOAD ||
    operation == CERBERUS_BLE_ARCHIVE_IO_LOAD_AIRTAG_JSON || operation == CERBERUS_BLE_ARCHIVE_IO_LOAD_WIFI ||
    operation == CERBERUS_BLE_ARCHIVE_IO_LOAD_AP_JSON || operation == CERBERUS_BLE_ARCHIVE_IO_LOAD_SSID_TEXT ||
    operation == CERBERUS_BLE_ARCHIVE_IO_LOAD_MESH_ARCHIVE; }

static bool archive_io_json(cerberus_ble_archive_io_operation_t operation)
{ return operation == CERBERUS_BLE_ARCHIVE_IO_LOAD_AIRTAG_JSON ||
    operation == CERBERUS_BLE_ARCHIVE_IO_SAVE_AIRTAG_JSON || operation == CERBERUS_BLE_ARCHIVE_IO_EXPORT_AIRTAG_JSON; }

static bool archive_io_wifi(cerberus_ble_archive_io_operation_t operation)
{ return operation == CERBERUS_BLE_ARCHIVE_IO_SAVE_WIFI || operation == CERBERUS_BLE_ARCHIVE_IO_LOAD_WIFI; }

static bool archive_io_ap(cerberus_ble_archive_io_operation_t operation)
{ return operation == CERBERUS_BLE_ARCHIVE_IO_LOAD_AP_JSON ||
    operation == CERBERUS_BLE_ARCHIVE_IO_SAVE_AP_JSON || operation == CERBERUS_BLE_ARCHIVE_IO_EXPORT_AP_JSON; }

static bool archive_io_ssid(cerberus_ble_archive_io_operation_t operation)
{ return operation == CERBERUS_BLE_ARCHIVE_IO_LOAD_SSID_TEXT ||
    operation == CERBERUS_BLE_ARCHIVE_IO_SAVE_SSID_TEXT || operation == CERBERUS_BLE_ARCHIVE_IO_EXPORT_SSID_TEXT; }

static bool archive_io_mesh_archive(cerberus_ble_archive_io_operation_t operation)
{ return operation == CERBERUS_BLE_ARCHIVE_IO_SAVE_MESH_ARCHIVE ||
    operation == CERBERUS_BLE_ARCHIVE_IO_LOAD_MESH_ARCHIVE ||
    operation == CERBERUS_BLE_ARCHIVE_IO_EXPORT_MESH_ARCHIVE_CSV; }

static mesh_archive_t *archive_io_mesh_document(ble_archive_io_context_t *context,
    cerberus_ble_archive_io_operation_t operation)
{
    return operation == CERBERUS_BLE_ARCHIVE_IO_LOAD_MESH_ARCHIVE ? &context->payload.mesh_archive_load.document :
        operation == CERBERUS_BLE_ARCHIVE_IO_EXPORT_MESH_ARCHIVE_CSV ? &context->payload.mesh_archive_csv.document :
        &context->payload.mesh_archive.document;
}

static size_t archive_io_allocation_size(cerberus_ble_archive_io_operation_t operation)
{
    size_t payload_size;
    switch (operation) {
    case CERBERUS_BLE_ARCHIVE_IO_SAVE:
    case CERBERUS_BLE_ARCHIVE_IO_LOAD:
        payload_size = sizeof(((ble_archive_io_context_t *)0)->payload.native); break;
    case CERBERUS_BLE_ARCHIVE_IO_LOAD_AIRTAG_JSON:
    case CERBERUS_BLE_ARCHIVE_IO_SAVE_AIRTAG_JSON:
    case CERBERUS_BLE_ARCHIVE_IO_EXPORT_AIRTAG_JSON:
        payload_size = sizeof(((ble_archive_io_context_t *)0)->payload.json); break;
    case CERBERUS_BLE_ARCHIVE_IO_SAVE_WIFI:
    case CERBERUS_BLE_ARCHIVE_IO_LOAD_WIFI:
        payload_size = sizeof(((ble_archive_io_context_t *)0)->payload.wifi); break;
    case CERBERUS_BLE_ARCHIVE_IO_LOAD_AP_JSON:
    case CERBERUS_BLE_ARCHIVE_IO_SAVE_AP_JSON:
    case CERBERUS_BLE_ARCHIVE_IO_EXPORT_AP_JSON:
        payload_size = sizeof(((ble_archive_io_context_t *)0)->payload.ap); break;
    case CERBERUS_BLE_ARCHIVE_IO_LOAD_SSID_TEXT:
    case CERBERUS_BLE_ARCHIVE_IO_SAVE_SSID_TEXT:
    case CERBERUS_BLE_ARCHIVE_IO_EXPORT_SSID_TEXT:
        payload_size = sizeof(((ble_archive_io_context_t *)0)->payload.ssid); break;
    case CERBERUS_BLE_ARCHIVE_IO_SAVE_PROBE:
        payload_size = sizeof(((ble_archive_io_context_t *)0)->payload.probe); break;
    case CERBERUS_BLE_ARCHIVE_IO_SAVE_MESH:
        payload_size = sizeof(((ble_archive_io_context_t *)0)->payload.mesh); break;
    case CERBERUS_BLE_ARCHIVE_IO_SAVE_MESH_LOCATE:
        payload_size = sizeof(((ble_archive_io_context_t *)0)->payload.locate); break;
    case CERBERUS_BLE_ARCHIVE_IO_SAVE_MESH_ARCHIVE:
        payload_size = sizeof(((ble_archive_io_context_t *)0)->payload.mesh_archive); break;
    case CERBERUS_BLE_ARCHIVE_IO_LOAD_MESH_ARCHIVE:
        payload_size = sizeof(((ble_archive_io_context_t *)0)->payload.mesh_archive_load); break;
    case CERBERUS_BLE_ARCHIVE_IO_EXPORT_MESH_ARCHIVE_CSV:
        payload_size = sizeof(((ble_archive_io_context_t *)0)->payload.mesh_archive_csv); break;
    default: return 0U;
    }
    return offsetof(ble_archive_io_context_t, payload) + payload_size;
}

static esp_err_t archive_io_json_error(cerberus_airtag_json_result_t result)
{
    return result == CERBERUS_AIRTAG_JSON_OK ? ESP_OK :
        result == CERBERUS_AIRTAG_JSON_LIMIT ? ESP_ERR_INVALID_SIZE :
        result == CERBERUS_AIRTAG_JSON_NO_ELIGIBLE ? ESP_ERR_NOT_FOUND : ESP_ERR_INVALID_RESPONSE;
}

static esp_err_t archive_io_wifi_error(cerberus_wifi_marauder_result_t result)
{
    portENTER_CRITICAL(&archive_io_lock);
    archive_io_status.wifi_result = result;
    portEXIT_CRITICAL(&archive_io_lock);
    return result == CERBERUS_WIFI_MARAUDER_OK ? ESP_OK :
        result == CERBERUS_WIFI_MARAUDER_LIMIT ? ESP_ERR_INVALID_SIZE :
        result == CERBERUS_WIFI_MARAUDER_UNREPRESENTABLE ? ESP_ERR_NOT_SUPPORTED :
        result == CERBERUS_WIFI_MARAUDER_NO_ELIGIBLE ? ESP_ERR_NOT_FOUND : ESP_ERR_INVALID_RESPONSE;
}

static bool archive_io_string_fits(const char *value, size_t capacity)
{
    if (value == NULL) return false;
    for (size_t i = 0U; i < capacity; ++i) if (value[i] == '\0') return true;
    return false;
}

static bool archive_io_canceled(void)
{
    portENTER_CRITICAL(&archive_io_lock);
    bool canceled = archive_io_status.cancel_requested;
    portEXIT_CRITICAL(&archive_io_lock);
    return canceled;
}

static void archive_io_finish(ble_archive_io_context_t *context,
    cerberus_ble_archive_io_state_t state, esp_err_t error, size_t bytes)
{
    portENTER_CRITICAL(&archive_io_lock);
    /* Cancellation racing LOAD completion still prevents result publication.
     * A completed SAVE reports actual storage outcome; no rollback is implied. */
    if (state == CERBERUS_BLE_ARCHIVE_IO_COMPLETE &&
        archive_io_loading(archive_io_status.operation) &&
        archive_io_status.cancel_requested) state = CERBERUS_BLE_ARCHIVE_IO_CANCELED;
    archive_io_status.state = state;
    archive_io_status.error = error;
    archive_io_status.bytes = (uint32_t)bytes;
    archive_io_status.count = state == CERBERUS_BLE_ARCHIVE_IO_COMPLETE ?
        (archive_io_mesh_archive(archive_io_status.operation) ?
            archive_io_mesh_document(context, archive_io_status.operation)->model.address_count :
         archive_io_status.operation == CERBERUS_BLE_ARCHIVE_IO_SAVE_MESH_LOCATE ? context->payload.locate.snapshot.history_count :
         archive_io_status.operation == CERBERUS_BLE_ARCHIVE_IO_SAVE_MESH ? context->payload.mesh.snapshot.model.address_count :
         archive_io_status.operation == CERBERUS_BLE_ARCHIVE_IO_SAVE_PROBE ? context->payload.probe.snapshot.count :
            archive_io_ap(archive_io_status.operation) ? context->payload.ap.document.count :
            archive_io_ssid(archive_io_status.operation) ? context->payload.ssid.document.count :
            archive_io_json(archive_io_status.operation) ? context->payload.json.document.count :
            archive_io_wifi(archive_io_status.operation) ? context->payload.wifi.document.count :
            context->payload.native.document.count) : 0U;
    memcpy(archive_io_status.file_name, context->file_name, sizeof(context->file_name));
    archive_io_status.worker_active = false;
    bool dispose = archive_io_release_requested || state != CERBERUS_BLE_ARCHIVE_IO_COMPLETE;
    archive_io_status.result_ready = !dispose;
    if (dispose) archive_io_context = NULL;
    portEXIT_CRITICAL(&archive_io_lock);
    if (dispose) heap_caps_free(context);
}

static void ble_archive_io_worker(void *argument)
{
    ble_archive_io_context_t *context = argument;
    portENTER_CRITICAL(&archive_io_lock);
    cerberus_ble_archive_io_operation_t operation = archive_io_status.operation;
    bool save = !archive_io_loading(operation);
    bool json = archive_io_json(operation);
    bool wifi = archive_io_wifi(operation);
    bool ap = archive_io_ap(operation), ssid = archive_io_ssid(operation);
    bool probe = operation == CERBERUS_BLE_ARCHIVE_IO_SAVE_PROBE;
    bool mesh = operation == CERBERUS_BLE_ARCHIVE_IO_SAVE_MESH;
    bool locate = operation == CERBERUS_BLE_ARCHIVE_IO_SAVE_MESH_LOCATE;
    bool map = archive_io_mesh_archive(operation);
    bool map_load = operation == CERBERUS_BLE_ARCHIVE_IO_LOAD_MESH_ARCHIVE;
    bool map_csv = operation == CERBERUS_BLE_ARCHIVE_IO_EXPORT_MESH_ARCHIVE_CSV;
    archive_io_status.state = save ? CERBERUS_BLE_ARCHIVE_IO_SAVING : CERBERUS_BLE_ARCHIVE_IO_LOADING;
    portEXIT_CRITICAL(&archive_io_lock);
    esp_err_t error = ESP_OK;
    size_t length = 0U;
    bool canceled = archive_io_canceled();
    uint8_t *bytes = map_csv ? (uint8_t *)context->payload.mesh_archive_csv.bytes :
        map_load ? context->payload.mesh_archive_load.bytes : map ? context->payload.mesh_archive.bytes : locate ? (uint8_t *)context->payload.locate.bytes :
        mesh ? (uint8_t *)context->payload.mesh.bytes : probe ? (uint8_t *)context->payload.probe.bytes :
        ap ? context->payload.ap.buffer.bytes : ssid ? context->payload.ssid.buffer.bytes :
        json ? context->payload.json.buffer.bytes :
        wifi ? context->payload.wifi.bytes : context->payload.native.bytes;
    size_t capacity = map_csv ? sizeof(context->payload.mesh_archive_csv.bytes) :
        map_load ? sizeof(context->payload.mesh_archive_load.bytes) : map ? sizeof(context->payload.mesh_archive.bytes) : locate ? sizeof(context->payload.locate.bytes) :
        mesh ? sizeof(context->payload.mesh.bytes) : probe ? sizeof(context->payload.probe.bytes) :
        ap ? sizeof(context->payload.ap.buffer.bytes) : ssid ? sizeof(context->payload.ssid.buffer.bytes) :
        json ? sizeof(context->payload.json.buffer.bytes) :
        wifi ? sizeof(context->payload.wifi.bytes) : sizeof(context->payload.native.bytes);
    if (!canceled && operation == CERBERUS_BLE_ARCHIVE_IO_EXPORT_AIRTAG_JSON) {
        cerberus_airtag_json_export_stats_t stats = {0};
        error = archive_io_json_error(cerberus_airtag_json_from_archive(
            &context->payload.json.buffer.export_source, &context->payload.json.document, &stats));
        portENTER_CRITICAL(&archive_io_lock);
        archive_io_status.export_stats = stats;
        portEXIT_CRITICAL(&archive_io_lock);
    }
    if (!canceled && (operation == CERBERUS_BLE_ARCHIVE_IO_EXPORT_AP_JSON ||
        operation == CERBERUS_BLE_ARCHIVE_IO_EXPORT_SSID_TEXT)) {
        cerberus_wifi_marauder_export_stats_t stats = {0};
        error = archive_io_wifi_error(ap ? cerberus_wifi_marauder_ap_from_archive(
            &context->payload.ap.buffer.export_source, &context->payload.ap.document, &stats) :
            cerberus_wifi_marauder_ssid_from_archive(&context->payload.ssid.buffer.export_source,
                &context->payload.ssid.document, &stats));
        portENTER_CRITICAL(&archive_io_lock);
        archive_io_status.wifi_export_stats = stats;
        portEXIT_CRITICAL(&archive_io_lock);
    }
    if (!canceled && error == ESP_OK && save) {
        if (map_csv) {
            if (!mesh_archive_report_encode(archive_io_mesh_document(context, operation),
                (char *)bytes, capacity, &length)) error = ESP_ERR_INVALID_RESPONSE;
            else if (length > CERBERUS_STORAGE_MESH_REPORT_MAX_BYTES) error = ESP_ERR_INVALID_SIZE;
        }
        else if (map) {
            if (!mesh_archive_encode(archive_io_mesh_document(context, operation),
                bytes, capacity, &length)) error = ESP_ERR_INVALID_RESPONSE;
        }
        else if (locate) {
            if (!mesh_locate_report_encode(&context->payload.locate.snapshot,
                (char *)bytes, capacity, &length)) error = ESP_ERR_INVALID_RESPONSE;
            else if (length > CERBERUS_STORAGE_MESH_REPORT_MAX_BYTES) error = ESP_ERR_INVALID_SIZE;
        }
        else if (mesh) {
            if (!mesh_report_encode(&context->payload.mesh.snapshot,
                (char *)bytes, capacity, &length)) error = ESP_ERR_INVALID_RESPONSE;
            else if (length > CERBERUS_STORAGE_MESH_REPORT_MAX_BYTES) error = ESP_ERR_INVALID_SIZE;
        }
        else if (probe) {
            if (!cerberus_probe_sniff_report(&context->payload.probe.snapshot,
                (char *)bytes, capacity, &length)) error = ESP_ERR_INVALID_RESPONSE;
            else if (length > CERBERUS_STORAGE_PROBE_REPORT_MAX_BYTES) error = ESP_ERR_INVALID_SIZE;
        }
        else if (ap) error = archive_io_wifi_error(cerberus_wifi_marauder_ap_encode(
            &context->payload.ap.document, bytes, capacity, &length));
        else if (ssid) error = archive_io_wifi_error(cerberus_wifi_marauder_ssid_encode(
            &context->payload.ssid.document, bytes, capacity, &length));
        else if (json) error = archive_io_json_error(cerberus_airtag_json_encode(
            &context->payload.json.document, bytes, capacity, &length));
        else if (wifi) {
            if (!cerberus_wifi_archive_encode(&context->payload.wifi.document,
                bytes, capacity, &length)) error = ESP_ERR_INVALID_RESPONSE;
        }
        else if (!cerberus_ble_archive_encode(&context->payload.native.document,
            bytes, capacity, &length)) error = ESP_ERR_INVALID_RESPONSE;
    }
    canceled = canceled || archive_io_canceled();
    if (!canceled && error == ESP_OK) {
        if (save && (mesh || locate || map_csv)) error = cerberus_storage_mesh_report_save(bytes, length, context->file_name, sizeof(context->file_name));
        else if (save && map) error = cerberus_storage_mesh_archive_save(bytes, length, context->file_name, sizeof(context->file_name));
        else if (save && probe) error = cerberus_storage_probe_report_save(bytes, length, context->file_name, sizeof(context->file_name));
        else if (save && ap) error = cerberus_storage_ap_json_save(bytes, length, context->file_name, sizeof(context->file_name));
        else if (save && ssid) error = cerberus_storage_ssid_text_save(bytes, length, context->file_name, sizeof(context->file_name));
        else if (save) error = json ? cerberus_storage_airtag_json_save(bytes, length,
            context->file_name, sizeof(context->file_name)) : wifi ? cerberus_storage_wifi_archive_save(bytes, length,
            context->file_name, sizeof(context->file_name)) : cerberus_storage_ble_archive_save(bytes, length,
            context->file_name, sizeof(context->file_name));
        else if (map_load) error = cerberus_storage_mesh_archive_load(context->relative_path,
            context->file_name, bytes, capacity, &length);
        else if (ap) error = cerberus_storage_ap_json_load(context->relative_path,
            context->file_name, bytes, capacity, &length);
        else if (ssid) error = cerberus_storage_ssid_text_load(context->relative_path,
            context->file_name, bytes, capacity, &length);
        else error = json ? cerberus_storage_airtag_json_load(context->relative_path,
            context->file_name, bytes, capacity, &length) : wifi ? cerberus_storage_wifi_archive_load(context->relative_path,
            context->file_name, bytes, capacity, &length) : cerberus_storage_ble_archive_load(context->relative_path,
            context->file_name, bytes, capacity, &length);
        if (!save && archive_io_canceled()) canceled = true;
        if (!save && !canceled && error == ESP_OK) {
            if (map_load) {
                if (!mesh_archive_decode(bytes, length, &context->payload.mesh_archive_load.document,
                    &context->payload.mesh_archive_load.scratch)) error = ESP_ERR_INVALID_RESPONSE;
            }
            else if (ap) error = archive_io_wifi_error(cerberus_wifi_marauder_ap_decode(bytes, length, &context->payload.ap.document));
            else if (ssid) error = archive_io_wifi_error(cerberus_wifi_marauder_ssid_decode(bytes, length, &context->payload.ssid.document));
            else if (json) error = archive_io_json_error(cerberus_airtag_json_decode(bytes, length, &context->payload.json.document));
            else if (wifi) {
                if (!cerberus_wifi_archive_decode(bytes, length, &context->payload.wifi.document)) error = ESP_ERR_INVALID_RESPONSE;
            }
            else if (!cerberus_ble_archive_decode(bytes, length, &context->payload.native.document)) error = ESP_ERR_INVALID_RESPONSE;
        }
    }
    archive_io_finish(context, error != ESP_OK ? CERBERUS_BLE_ARCHIVE_IO_ERROR :
        canceled ? CERBERUS_BLE_ARCHIVE_IO_CANCELED : CERBERUS_BLE_ARCHIVE_IO_COMPLETE,
        error, error == ESP_OK && !canceled ? length : 0U);
    vTaskDelete(NULL);
}

static esp_err_t archive_io_start(cerberus_ble_archive_io_operation_t operation,
    const cerberus_ble_archive_t *document, const cerberus_airtag_json_t *json_document,
    const cerberus_wifi_archive_t *wifi_document,
    const cerberus_wifi_marauder_ap_t *ap_document, const cerberus_wifi_marauder_ssid_t *ssid_document,
    const cerberus_probe_sniff_snapshot_t *probe_snapshot, const mesh_scan_t *mesh_snapshot,
    const mesh_locate_t *locate_snapshot, const mesh_archive_t *mesh_document,
    const char *relative_path, const char *file_name, uint64_t *job_id)
{
    if (job_id != NULL) *job_id = 0U;
    bool save = !archive_io_loading(operation);
    if (job_id == NULL || !archive_io_allocation_size(operation) ||
        ((operation == CERBERUS_BLE_ARCHIVE_IO_SAVE_WIFI || operation == CERBERUS_BLE_ARCHIVE_IO_EXPORT_AP_JSON ||
            operation == CERBERUS_BLE_ARCHIVE_IO_EXPORT_SSID_TEXT) && wifi_document == NULL) ||
        (operation == CERBERUS_BLE_ARCHIVE_IO_SAVE_AP_JSON && ap_document == NULL) ||
        (operation == CERBERUS_BLE_ARCHIVE_IO_SAVE_SSID_TEXT && ssid_document == NULL) ||
        (operation == CERBERUS_BLE_ARCHIVE_IO_SAVE_PROBE && (probe_snapshot == NULL ||
            !probe_snapshot->frozen || !cerberus_probe_sniff_snapshot_valid(probe_snapshot))) ||
        (operation == CERBERUS_BLE_ARCHIVE_IO_SAVE_MESH && !mesh_report_valid(mesh_snapshot)) ||
        (operation == CERBERUS_BLE_ARCHIVE_IO_SAVE_MESH_LOCATE && !mesh_locate_report_valid(locate_snapshot)) ||
        ((operation == CERBERUS_BLE_ARCHIVE_IO_SAVE_MESH_ARCHIVE ||
          operation == CERBERUS_BLE_ARCHIVE_IO_EXPORT_MESH_ARCHIVE_CSV) && !mesh_archive_valid(mesh_document)) ||
        (!save && (!archive_io_string_fits(relative_path, CERBERUS_BLE_ARCHIVE_IO_PATH_CAPACITY) ||
        !archive_io_string_fits(file_name, CERBERUS_BLE_ARCHIVE_IO_NAME_CAPACITY))))
        return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&archive_io_lock);
    if (archive_io_status.worker_active || archive_io_context != NULL || archive_io_readers ||
        archive_io_last_job == UINT64_MAX) {
        portEXIT_CRITICAL(&archive_io_lock);
        return ESP_ERR_INVALID_STATE;
    }
    memset(&archive_io_status, 0, sizeof(archive_io_status));
    archive_io_status.job_id = ++archive_io_last_job;
    *job_id = archive_io_status.job_id;
    archive_io_status.operation = operation;
    archive_io_status.format = (operation == CERBERUS_BLE_ARCHIVE_IO_SAVE_MESH_ARCHIVE ||
        operation == CERBERUS_BLE_ARCHIVE_IO_LOAD_MESH_ARCHIVE) ? CERBERUS_BLE_ARCHIVE_IO_FORMAT_MESH_ARCHIVE :
        operation == CERBERUS_BLE_ARCHIVE_IO_EXPORT_MESH_ARCHIVE_CSV ? CERBERUS_BLE_ARCHIVE_IO_FORMAT_MESH_REPORT : operation == CERBERUS_BLE_ARCHIVE_IO_SAVE_MESH_LOCATE ? CERBERUS_BLE_ARCHIVE_IO_FORMAT_MESH_LOCATE_REPORT :
        operation == CERBERUS_BLE_ARCHIVE_IO_SAVE_MESH ? CERBERUS_BLE_ARCHIVE_IO_FORMAT_MESH_REPORT :
        operation == CERBERUS_BLE_ARCHIVE_IO_SAVE_PROBE ? CERBERUS_BLE_ARCHIVE_IO_FORMAT_PROBE_REPORT :
        archive_io_ap(operation) ? CERBERUS_BLE_ARCHIVE_IO_FORMAT_AP_JSON :
        archive_io_ssid(operation) ? CERBERUS_BLE_ARCHIVE_IO_FORMAT_SSID_TEXT : archive_io_json(operation) ? CERBERUS_BLE_ARCHIVE_IO_FORMAT_AIRTAG_JSON :
        archive_io_wifi(operation) ? CERBERUS_BLE_ARCHIVE_IO_FORMAT_WIFI : CERBERUS_BLE_ARCHIVE_IO_FORMAT_BLE;
    archive_io_status.state = CERBERUS_BLE_ARCHIVE_IO_STARTING;
    archive_io_status.worker_active = true;
    archive_io_release_requested = false;
    const char *path = !save ? relative_path : operation == CERBERUS_BLE_ARCHIVE_IO_SAVE_MESH_ARCHIVE ?
        CERBERUS_STORAGE_MESH_ARCHIVE_PATH : (operation == CERBERUS_BLE_ARCHIVE_IO_EXPORT_MESH_ARCHIVE_CSV || operation == CERBERUS_BLE_ARCHIVE_IO_SAVE_MESH ||
        operation == CERBERUS_BLE_ARCHIVE_IO_SAVE_MESH_LOCATE) ?
        CERBERUS_STORAGE_MESH_REPORT_PATH : operation == CERBERUS_BLE_ARCHIVE_IO_SAVE_PROBE ?
        CERBERUS_STORAGE_PROBE_REPORT_PATH :
        (archive_io_wifi(operation) || archive_io_ap(operation) || archive_io_ssid(operation)) ?
        CERBERUS_STORAGE_WIFI_ARCHIVE_PATH : CERBERUS_STORAGE_BLE_ARCHIVE_PATH;
    memcpy(archive_io_status.relative_path, path, strlen(path) + 1U);
    if (!save) memcpy(archive_io_status.file_name, file_name, strlen(file_name) + 1U);
    portEXIT_CRITICAL(&archive_io_lock);

    ble_archive_io_context_t *context = NULL;
    if (heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) >= BLE_ARCHIVE_IO_MIN_FREE_INTERNAL &&
        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) >= BLE_ARCHIVE_IO_MIN_LARGEST_INTERNAL)
        context = heap_caps_calloc(1U, archive_io_allocation_size(operation), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (context != NULL) {
        if (operation == CERBERUS_BLE_ARCHIVE_IO_SAVE) context->payload.native.document = *document;
        else if (operation == CERBERUS_BLE_ARCHIVE_IO_SAVE_AIRTAG_JSON) context->payload.json.document = *json_document;
        else if (operation == CERBERUS_BLE_ARCHIVE_IO_EXPORT_AIRTAG_JSON) context->payload.json.buffer.export_source = *document;
        else if (operation == CERBERUS_BLE_ARCHIVE_IO_SAVE_WIFI) context->payload.wifi.document = *wifi_document;
        else if (operation == CERBERUS_BLE_ARCHIVE_IO_SAVE_AP_JSON) context->payload.ap.document = *ap_document;
        else if (operation == CERBERUS_BLE_ARCHIVE_IO_SAVE_SSID_TEXT) context->payload.ssid.document = *ssid_document;
        else if (operation == CERBERUS_BLE_ARCHIVE_IO_EXPORT_AP_JSON) context->payload.ap.buffer.export_source = *wifi_document;
        else if (operation == CERBERUS_BLE_ARCHIVE_IO_EXPORT_SSID_TEXT) context->payload.ssid.buffer.export_source = *wifi_document;
        else if (operation == CERBERUS_BLE_ARCHIVE_IO_SAVE_PROBE) context->payload.probe.snapshot = *probe_snapshot;
        else if (operation == CERBERUS_BLE_ARCHIVE_IO_SAVE_MESH) context->payload.mesh.snapshot = *mesh_snapshot;
        else if (operation == CERBERUS_BLE_ARCHIVE_IO_SAVE_MESH_LOCATE) context->payload.locate.snapshot = *locate_snapshot;
        else if (save && archive_io_mesh_archive(operation))
            *archive_io_mesh_document(context, operation) = *mesh_document;
        memcpy(context->relative_path, path, strlen(path) + 1U);
        if (!save) memcpy(context->file_name, file_name, strlen(file_name) + 1U);
        portENTER_CRITICAL(&archive_io_lock);
        archive_io_context = context;
        portEXIT_CRITICAL(&archive_io_lock);
        if (xTaskCreate(ble_archive_io_worker, "ble_archive", BLE_ARCHIVE_IO_STACK_BYTES,
                context, 2U, NULL) == pdPASS) return ESP_OK;
    }
    portENTER_CRITICAL(&archive_io_lock);
    archive_io_context = NULL;
    archive_io_status.worker_active = false;
    archive_io_status.state = CERBERUS_BLE_ARCHIVE_IO_ERROR;
    archive_io_status.error = ESP_ERR_NO_MEM;
    portEXIT_CRITICAL(&archive_io_lock);
    heap_caps_free(context);
    return ESP_ERR_NO_MEM;
}

esp_err_t cerberus_ble_archive_io_save(const cerberus_ble_archive_t *document, uint64_t *job_id)
{
    if (document == NULL) { if (job_id != NULL) *job_id = 0U; return ESP_ERR_INVALID_ARG; }
    return archive_io_start(CERBERUS_BLE_ARCHIVE_IO_SAVE, document, NULL, NULL,
        NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, job_id);
}
esp_err_t cerberus_ble_archive_io_load(const char *relative_path, const char *file_name, uint64_t *job_id)
{ return archive_io_start(CERBERUS_BLE_ARCHIVE_IO_LOAD, NULL, NULL, NULL,
        NULL, NULL, NULL, NULL, NULL, NULL, relative_path, file_name, job_id); }

esp_err_t cerberus_ble_archive_io_load_airtag_json(const char *relative_path, const char *file_name, uint64_t *job_id)
{ return archive_io_start(CERBERUS_BLE_ARCHIVE_IO_LOAD_AIRTAG_JSON, NULL, NULL, NULL,
        NULL, NULL, NULL, NULL, NULL, NULL, relative_path, file_name, job_id); }
esp_err_t cerberus_ble_archive_io_save_airtag_json(const cerberus_airtag_json_t *document, uint64_t *job_id)
{
    if (!document) { if (job_id) *job_id = 0U; return ESP_ERR_INVALID_ARG; }
    return archive_io_start(CERBERUS_BLE_ARCHIVE_IO_SAVE_AIRTAG_JSON, NULL, document, NULL,
        NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, job_id);
}
esp_err_t cerberus_ble_archive_io_export_airtag_json(const cerberus_ble_archive_t *document, uint64_t *job_id)
{
    if (!document) { if (job_id) *job_id = 0U; return ESP_ERR_INVALID_ARG; }
    return archive_io_start(CERBERUS_BLE_ARCHIVE_IO_EXPORT_AIRTAG_JSON, document, NULL, NULL,
        NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, job_id);
}

esp_err_t cerberus_ble_archive_io_save_wifi(const cerberus_wifi_archive_t *document, uint64_t *job_id)
{
    if (!document) { if (job_id) *job_id = 0U; return ESP_ERR_INVALID_ARG; }
    return archive_io_start(CERBERUS_BLE_ARCHIVE_IO_SAVE_WIFI, NULL, NULL, document,
        NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, job_id);
}
esp_err_t cerberus_ble_archive_io_load_wifi(const char *relative_path, const char *file_name, uint64_t *job_id)
{ return archive_io_start(CERBERUS_BLE_ARCHIVE_IO_LOAD_WIFI, NULL, NULL, NULL,
        NULL, NULL, NULL, NULL, NULL, NULL, relative_path, file_name, job_id); }

esp_err_t cerberus_ble_archive_io_load_ap_json(const char *relative_path, const char *file_name, uint64_t *job_id)
{ return archive_io_start(CERBERUS_BLE_ARCHIVE_IO_LOAD_AP_JSON, NULL, NULL, NULL,
        NULL, NULL, NULL, NULL, NULL, NULL, relative_path, file_name, job_id); }
esp_err_t cerberus_ble_archive_io_save_ap_json(const cerberus_wifi_marauder_ap_t *document, uint64_t *job_id)
{ return archive_io_start(CERBERUS_BLE_ARCHIVE_IO_SAVE_AP_JSON, NULL, NULL, NULL,
        document, NULL, NULL, NULL, NULL, NULL, NULL, NULL, job_id); }
esp_err_t cerberus_ble_archive_io_export_ap_json(const cerberus_wifi_archive_t *document, uint64_t *job_id)
{ return archive_io_start(CERBERUS_BLE_ARCHIVE_IO_EXPORT_AP_JSON, NULL, NULL, document,
        NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, job_id); }

esp_err_t cerberus_ble_archive_io_load_ssid_text(const char *relative_path, const char *file_name, uint64_t *job_id)
{ return archive_io_start(CERBERUS_BLE_ARCHIVE_IO_LOAD_SSID_TEXT, NULL, NULL, NULL,
        NULL, NULL, NULL, NULL, NULL, NULL, relative_path, file_name, job_id); }
esp_err_t cerberus_ble_archive_io_save_ssid_text(const cerberus_wifi_marauder_ssid_t *document, uint64_t *job_id)
{ return archive_io_start(CERBERUS_BLE_ARCHIVE_IO_SAVE_SSID_TEXT, NULL, NULL, NULL,
        NULL, document, NULL, NULL, NULL, NULL, NULL, NULL, job_id); }
esp_err_t cerberus_ble_archive_io_export_ssid_text(const cerberus_wifi_archive_t *document, uint64_t *job_id)
{ return archive_io_start(CERBERUS_BLE_ARCHIVE_IO_EXPORT_SSID_TEXT, NULL, NULL, document,
        NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, job_id); }

esp_err_t cerberus_ble_archive_io_save_probe(const cerberus_probe_sniff_snapshot_t *snapshot, uint64_t *job_id)
{
    return archive_io_start(CERBERUS_BLE_ARCHIVE_IO_SAVE_PROBE, NULL, NULL, NULL,
        NULL, NULL, snapshot, NULL, NULL, NULL, NULL, NULL, job_id);
}

esp_err_t cerberus_ble_archive_io_save_mesh(const mesh_scan_t *scan, uint64_t *job_id)
{
    return archive_io_start(CERBERUS_BLE_ARCHIVE_IO_SAVE_MESH, NULL, NULL, NULL,
        NULL, NULL, NULL, scan, NULL, NULL, NULL, NULL, job_id);
}

esp_err_t cerberus_ble_archive_io_save_mesh_locate(const mesh_locate_t *locate, uint64_t *job_id)
{
    return archive_io_start(CERBERUS_BLE_ARCHIVE_IO_SAVE_MESH_LOCATE, NULL, NULL, NULL,
        NULL, NULL, NULL, NULL, locate, NULL, NULL, NULL, job_id);
}

esp_err_t cerberus_ble_archive_io_save_mesh_archive(const mesh_archive_t *document, uint64_t *job_id)
{
    return archive_io_start(CERBERUS_BLE_ARCHIVE_IO_SAVE_MESH_ARCHIVE, NULL, NULL, NULL,
        NULL, NULL, NULL, NULL, NULL, document, NULL, NULL, job_id);
}
esp_err_t cerberus_ble_archive_io_load_mesh_archive(const char *path, const char *name, uint64_t *job_id)
{
    return archive_io_start(CERBERUS_BLE_ARCHIVE_IO_LOAD_MESH_ARCHIVE, NULL, NULL, NULL,
        NULL, NULL, NULL, NULL, NULL, NULL, path, name, job_id);
}
esp_err_t cerberus_ble_archive_io_export_mesh_archive_csv(const mesh_archive_t *document, uint64_t *job_id)
{
    return archive_io_start(CERBERUS_BLE_ARCHIVE_IO_EXPORT_MESH_ARCHIVE_CSV, NULL, NULL, NULL,
        NULL, NULL, NULL, NULL, NULL, document, NULL, NULL, job_id);
}

void cerberus_ble_archive_io_get_status(cerberus_ble_archive_io_status_t *status)
{
    if (status == NULL) return;
    portENTER_CRITICAL(&archive_io_lock);
    *status = archive_io_status;
    portEXIT_CRITICAL(&archive_io_lock);
}
static ble_archive_io_context_t *archive_io_pin(uint64_t job_id, cerberus_ble_archive_io_format_t format)
{
    ble_archive_io_context_t *borrowed = NULL;
    portENTER_CRITICAL(&archive_io_lock);
    if (job_id && job_id == archive_io_status.job_id && !archive_io_status.worker_active &&
        archive_io_status.result_ready && archive_io_context != NULL && archive_io_status.format == format &&
        archive_io_status.state == CERBERUS_BLE_ARCHIVE_IO_COMPLETE && archive_io_readers != UINT16_MAX) {
        borrowed = archive_io_context;
        ++archive_io_readers;
    }
    portEXIT_CRITICAL(&archive_io_lock);
    return borrowed;
}
static void archive_io_unpin(ble_archive_io_context_t *borrowed)
{
    ble_archive_io_context_t *dispose = NULL;
    portENTER_CRITICAL(&archive_io_lock);
    --archive_io_readers;
    if (!archive_io_readers && archive_io_release_requested && archive_io_context == borrowed) {
        dispose = archive_io_context;
        archive_io_context = NULL;
        archive_io_status.result_ready = false;
    }
    portEXIT_CRITICAL(&archive_io_lock);
    if (dispose != NULL) heap_caps_free(dispose);
}
esp_err_t cerberus_ble_archive_io_copy_result(uint64_t job_id, cerberus_ble_archive_t *document)
{
    if (!document) return ESP_ERR_INVALID_ARG;
    ble_archive_io_context_t *borrowed = archive_io_pin(job_id, CERBERUS_BLE_ARCHIVE_IO_FORMAT_BLE);
    if (!borrowed) return ESP_ERR_INVALID_STATE;
    /* Typed immutable copies remain outside the interrupt-disabled lock. */
    memcpy(document, &borrowed->payload.native.document, sizeof(*document));
    archive_io_unpin(borrowed); return ESP_OK;
}
esp_err_t cerberus_ble_archive_io_copy_airtag_json(uint64_t job_id, cerberus_airtag_json_t *document)
{
    if (!document) return ESP_ERR_INVALID_ARG;
    ble_archive_io_context_t *borrowed = archive_io_pin(job_id, CERBERUS_BLE_ARCHIVE_IO_FORMAT_AIRTAG_JSON);
    if (!borrowed) return ESP_ERR_INVALID_STATE;
    memcpy(document, &borrowed->payload.json.document, sizeof(*document));
    archive_io_unpin(borrowed); return ESP_OK;
}
esp_err_t cerberus_ble_archive_io_copy_wifi(uint64_t job_id, cerberus_wifi_archive_t *document)
{
    if (!document) return ESP_ERR_INVALID_ARG;
    ble_archive_io_context_t *borrowed = archive_io_pin(job_id, CERBERUS_BLE_ARCHIVE_IO_FORMAT_WIFI);
    if (!borrowed) return ESP_ERR_INVALID_STATE;
    memcpy(document, &borrowed->payload.wifi.document, sizeof(*document));
    archive_io_unpin(borrowed); return ESP_OK;
}
esp_err_t cerberus_ble_archive_io_copy_ap_json(uint64_t job_id, cerberus_wifi_marauder_ap_t *document)
{
    if (!document) return ESP_ERR_INVALID_ARG;
    ble_archive_io_context_t *borrowed = archive_io_pin(job_id, CERBERUS_BLE_ARCHIVE_IO_FORMAT_AP_JSON);
    if (!borrowed) return ESP_ERR_INVALID_STATE;
    memcpy(document, &borrowed->payload.ap.document, sizeof(*document));
    archive_io_unpin(borrowed); return ESP_OK;
}
esp_err_t cerberus_ble_archive_io_copy_ssid_text(uint64_t job_id, cerberus_wifi_marauder_ssid_t *document)
{
    if (!document) return ESP_ERR_INVALID_ARG;
    ble_archive_io_context_t *borrowed = archive_io_pin(job_id, CERBERUS_BLE_ARCHIVE_IO_FORMAT_SSID_TEXT);
    if (!borrowed) return ESP_ERR_INVALID_STATE;
    memcpy(document, &borrowed->payload.ssid.document, sizeof(*document));
    archive_io_unpin(borrowed); return ESP_OK;
}
esp_err_t cerberus_ble_archive_io_copy_mesh_archive(uint64_t job_id, mesh_archive_t *document)
{
    if (!document) return ESP_ERR_INVALID_ARG;
    ble_archive_io_context_t *borrowed = archive_io_pin(job_id, CERBERUS_BLE_ARCHIVE_IO_FORMAT_MESH_ARCHIVE);
    if (!borrowed) return ESP_ERR_INVALID_STATE;
    memcpy(document, archive_io_mesh_document(borrowed, archive_io_status.operation), sizeof(*document));
    archive_io_unpin(borrowed); return ESP_OK;
}
void cerberus_ble_archive_io_cancel(uint64_t job_id)
{
    portENTER_CRITICAL(&archive_io_lock);
    if (job_id && job_id == archive_io_status.job_id && archive_io_status.worker_active)
        archive_io_status.cancel_requested = true;
    portEXIT_CRITICAL(&archive_io_lock);
}
void cerberus_ble_archive_io_release(uint64_t job_id)
{
    ble_archive_io_context_t *owned = NULL;
    portENTER_CRITICAL(&archive_io_lock);
    if (job_id && job_id == archive_io_status.job_id) {
        if (archive_io_status.worker_active) {
            archive_io_release_requested = true;
            archive_io_status.cancel_requested = true;
        } else if (archive_io_readers) {
            archive_io_release_requested = true;
            archive_io_status.result_ready = false;
        } else {
            owned = archive_io_context;
            archive_io_context = NULL;
            archive_io_status.result_ready = false;
        }
    }
    portEXIT_CRITICAL(&archive_io_lock);
    if (owned != NULL) heap_caps_free(owned);
}
const char *cerberus_ble_archive_io_state_name(cerberus_ble_archive_io_state_t state)
{
    static const char *const names[] = {"IDLE", "STARTING", "SAVING", "LOADING", "COMPLETE", "ERROR", "CANCELED"};
    return (unsigned)state < sizeof(names) / sizeof(names[0]) ? names[state] : "UNKNOWN";
}
