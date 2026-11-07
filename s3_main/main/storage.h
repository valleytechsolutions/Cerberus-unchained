#pragma once

#include "esp_err.h"
#include "sdmmc_cmd.h"
#include "storage_report_limits.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CERBERUS_STORAGE_MOUNT_PATH "/sdcard"
#define CERBERUS_STORAGE_NAME_MAX 256
#define CERBERUS_STORAGE_PATH_MAX 512
#define CERBERUS_STORAGE_BENCHMARK_BYTES (512U * 1024U)
#define CERBERUS_STORAGE_FIRMWARE_PATH "/CERBERUS/FIRMWARE"
#define CERBERUS_STORAGE_SCRIPTS_PATH "/CERBERUS/SCRIPTS"
#define CERBERUS_STORAGE_FIRMWARE_MAX_BYTES (4U * 1024U * 1024U)
#define CERBERUS_STORAGE_WIFI_CAPTURE_MAX_BYTES (64U * 1024U * 1024U)
#define CERBERUS_STORAGE_WARDRIVE_MAX_BYTES (64U * 1024U * 1024U)
#define CERBERUS_STORAGE_WARDRIVE_PATH "/CERBERUS/WARD"
#define CERBERUS_STORAGE_RECON_PATH "/CERBERUS/RECON"
#define CERBERUS_STORAGE_RECON_ROW_MAX 1024U
#define CERBERUS_STORAGE_RECON_MANIFEST_MAX 4096U
#define CERBERUS_STORAGE_RECON_MAX_BYTES (64U * 1024U * 1024U)
#define CERBERUS_STORAGE_RECON_NAME_CAPACITY 7U
#define CERBERUS_STORAGE_BLE_ARCHIVE_PATH "/CERBERUS/BLE"
/* v6: header32 + up to64 receiver-tagged rows of432 bytes. */
#define CERBERUS_STORAGE_BLE_ARCHIVE_MAX_BYTES 27680U
#define CERBERUS_STORAGE_AIRTAG_JSON_MAX_BYTES 49152U
#define CERBERUS_STORAGE_WIFI_ARCHIVE_PATH "/CERBERUS/WIFI"
/* v1: header96 + up to64 source-tagged rows of80 bytes. */
#define CERBERUS_STORAGE_WIFI_ARCHIVE_MAX_BYTES 5216U
#define CERBERUS_STORAGE_AP_JSON_MAX_BYTES 65536U
#define CERBERUS_STORAGE_SSID_TEXT_MAX_BYTES 2112U

typedef struct {
    bool mounted;
    uint64_t total_bytes;
    uint64_t free_bytes;
    uint32_t sector_size;
    char card_name[8];
} cerberus_storage_status_t;

typedef struct {
    char name[CERBERUS_STORAGE_NAME_MAX];
    uint64_t size_bytes;
    bool is_directory;
} cerberus_storage_entry_t;

typedef struct {
    uint32_t bytes_written;
    uint32_t bytes_read;
    uint64_t write_time_us;
    uint64_t read_time_us;
    bool data_verified;
} cerberus_storage_benchmark_result_t;

typedef enum {
    CERBERUS_LOG_INFO,
    CERBERUS_LOG_WARNING,
    CERBERUS_LOG_ERROR,
} cerberus_log_level_t;

typedef enum {
    CERBERUS_STORAGE_OWNER_NONE,
    CERBERUS_STORAGE_OWNER_FIRMWARE,
    CERBERUS_STORAGE_OWNER_USB,
} cerberus_storage_owner_t;

typedef struct {
    uint64_t size_bytes;
    uint8_t sha256[32];
    bool esp_image;
    uint8_t segment_count;
    uint8_t spi_mode;
    uint8_t spi_speed_size;
} cerberus_firmware_package_info_t;

esp_err_t cerberus_storage_mount(void);
esp_err_t cerberus_storage_unmount(void);
bool cerberus_storage_is_mounted(void);
/* Read under the storage mutex. Successful mount/unmount changes the token;
 * USB ownership, an unmounted card or a busy mutex returns an error. A caller
 * doing multiple chunk reads compares tokens before/after and also validates
 * file identity. This does not hold a filesystem lease or hash file contents. */
esp_err_t cerberus_storage_get_read_epoch(uint32_t *epoch);
/* UI cache guard: the same ownership/token check without waiting or creating
 * a mutex. Returns TIMEOUT while storage is busy, INVALID_STATE before mount
 * initialization or while unavailable; every failure clears the output. */
esp_err_t cerberus_storage_try_get_read_epoch(uint32_t *epoch);
cerberus_storage_owner_t cerberus_storage_get_owner(void);
esp_err_t cerberus_storage_reserve_for_usb(void);
esp_err_t cerberus_storage_release_from_usb(void);
esp_err_t cerberus_storage_acquire_usb_card(sdmmc_card_t **card);
esp_err_t cerberus_storage_release_usb_card(void);
esp_err_t cerberus_storage_get_status(cerberus_storage_status_t *status);
esp_err_t cerberus_storage_list_directory(
    const char *relative_path,
    cerberus_storage_entry_t *entries,
    size_t entry_capacity,
    size_t *entry_count);
esp_err_t cerberus_storage_read_file_chunk(
    const char *relative_path,
    const char *file_name,
    uint64_t offset,
    uint8_t *data,
    size_t data_capacity,
    size_t *bytes_read,
    uint64_t *file_size);
esp_err_t cerberus_storage_sha256_file(
    const char *relative_path,
    const char *file_name,
    uint8_t digest[32],
    uint64_t *file_size);
esp_err_t cerberus_storage_write_diagnostic_report(
    const char *data,
    size_t data_length,
    char *file_name,
    size_t file_name_capacity);
esp_err_t cerberus_storage_write_wifi_report(
    const char *data,
    size_t data_length,
    char *file_name,
    size_t file_name_capacity);
esp_err_t cerberus_storage_write_wifi_defense_report(
    const char *data,
    size_t data_length,
    char *file_name,
    size_t file_name_capacity);
esp_err_t cerberus_storage_write_portal_report(
    const char *data,
    size_t data_length,
    char *file_name,
    size_t file_name_capacity);
/* One streaming PCAPNG capture at a time. An open capture keeps the SD card
 * owned by firmware; USB handoff and unmount return INVALID_STATE until finish.
 * Records append to a .TMP file and finish atomically publishes .PCAPNG. */
esp_err_t cerberus_storage_wifi_capture_begin(const uint8_t *header, size_t header_length);
esp_err_t cerberus_storage_wifi_capture_append(const uint8_t *record, size_t record_length);
esp_err_t cerberus_storage_wifi_capture_finish(bool keep,
    char *file_name, size_t file_name_capacity, uint64_t *bytes_written);
/* Optional selected-target capture: exact job lease, two lazy per-radio files.
 * Requires already-mounted firmware storage; never takes over USB. Creates a
 * fresh Pnnnnn directory (bounded search), then exclusive S3.TMP/C5.TMP files.
 * Only checked write/flush/sync/close files publish as .PCAPNG. Failed partial
 * files remain .TMP; no atomic or power-loss durability guarantee is implied. */
#define CERBERUS_STORAGE_PACKET_COUNT_PATH "/CERBERUS/PCOUNT"
typedef struct {
    uint64_t bytes[2];
    esp_err_t error[2];
    char file_name[2][24];
} cerberus_storage_packet_count_result_t;
esp_err_t cerberus_storage_packet_count_begin(uint64_t job, char name[8]);
/* Monitor uses the same exclusive SD lease, with a separate MONITOR/Mnnnnn
 * directory and a source-clock CSV index beside the C5 PCAPNG. */
esp_err_t cerberus_storage_packet_monitor_begin(uint64_t job, char name[8]);
esp_err_t cerberus_storage_packet_count_open(uint64_t job, unsigned source,
    const uint8_t *header, size_t length);
esp_err_t cerberus_storage_packet_count_append(uint64_t job, unsigned source,
    const uint8_t *record, size_t length);
esp_err_t cerberus_storage_packet_monitor_append(uint64_t job, uint64_t session,
    uint32_t sequence, uint64_t source_us, uint8_t channel, int8_t rssi,
    bool rssi_known, uint16_t frame_bytes, const uint8_t *record, size_t length);
/* Exact job required. Waits for the mutex to close every owned handle and
 * release the lease even after earlier errors. Result must be nonnull. */
esp_err_t cerberus_storage_packet_count_finish(uint64_t job,
    cerberus_storage_packet_count_result_t *result);
/* Wardriving holds one SD lease with lazy UNLOC/WIGLE append streams. USB
 * handoff remains blocked until both streams close and the session ends.
 * Only a stream with no write/sync failure can publish CSV. Failed or
 * interrupted streams retain recoverable .TMP files, including a partial row. */
esp_err_t cerberus_storage_wardrive_session_begin(void);
esp_err_t cerberus_storage_wardrive_session_finish(void);
esp_err_t cerberus_storage_wardrive_begin(const char *header, size_t header_length);
esp_err_t cerberus_storage_wardrive_append(const char *row, size_t row_length);
esp_err_t cerberus_storage_wardrive_sync(void);
esp_err_t cerberus_storage_wardrive_finish(bool keep,
    char *file_name, size_t file_name_capacity, uint64_t *bytes_written);
esp_err_t cerberus_storage_wardrive_wigle_begin(const char *header, size_t header_length);
esp_err_t cerberus_storage_wardrive_wigle_append(const char *row, size_t row_length);
esp_err_t cerberus_storage_wardrive_wigle_sync(void);
esp_err_t cerberus_storage_wardrive_wigle_finish(bool keep,
    char *file_name, size_t file_name_capacity, uint64_t *bytes_written);
/* Worker-only journal: begin requires mounted firmware-owned storage and
 * returns R00001..R99999. Its lease excludes USB/unmount/capture/Wardriving and
 * mutations overlapping the active session until finish. Begin examines at most
 * 1024 candidate indices, starting from a cached rotating hint. Failed begin
 * releases its lease and retains a nonempty name if a fresh directory was
 * created, so partial files can be found. Failed append/sync requires
 * finish(..., false), including a lock
 * timeout. Finish waits for the mutex and releases both streams and the lease
 * even for invalid arguments or a prior write failure. Header/rows are bounded
 * by ROW_MAX, manifest by MANIFEST_MAX, and all bytes together by MAX_BYTES.
 * Observation data reserves MANIFEST_MAX bytes for a final error manifest;
 * reaching the observation limit fails the session rather than completing it.
 * Only fully written, flushed, synced and closed files are eligible for
 * OBS.CSV then SESSION.JSON publication. SESSION.JSON is the completion marker.
 * Failures retain partial files; a failed second rename leaves OBS.CSV
 * alongside SESSION.TMP without a completion marker. This is not atomic
 * publication or a power-loss guarantee. No caller may retry a partial append. */
esp_err_t cerberus_storage_recon_begin(const char *csv_header, size_t length,
    char *session_name, size_t capacity);
esp_err_t cerberus_storage_recon_append(const char *row, size_t length);
esp_err_t cerberus_storage_recon_sync(void);
esp_err_t cerberus_storage_recon_finish(const char *manifest, size_t length,
    bool successful);
/* Worker-only bounded archive operations. The existing storage mutex is held
 * for the entire operation, excluding USB/unmount/file tools; other capture,
 * Wardriving and Recon leases must be idle. Byte bounds do not bound SD latency.
 * Save searches at most 1024 names from a cached hint, creates Bnnnnn.TMP with
 * O_EXCL and publishes Bnnnnn.BLE only after full write, flush, sync and close.
 * Existing files are never overwritten; a failure retains an owned temp and
 * returns its name when created. No atomic/power-loss durability is claimed.
 * Load accepts safe relative directories and .BLE filenames, checks complete
 * bounded data/EOF/close, and clears data/length on failure. Codec validation is
 * the caller's responsibility before publishing a loaded document. */
esp_err_t cerberus_storage_ble_archive_save(const uint8_t *data, size_t length,
    char *file_name, size_t capacity);
esp_err_t cerberus_storage_ble_archive_load(const char *relative_path,
    const char *file_name, uint8_t *data, size_t capacity, size_t *length);
/* Same exclusive storage boundary as .BLE. JSON load allows safe .LOG/.JSON
 * names. Save chooses Annnnn.LOG via an exclusive Annnnn.TMP; it never writes
 * a caller-supplied destination or overwrites upstream /Airtags_0.log. */
esp_err_t cerberus_storage_airtag_json_save(const uint8_t *data, size_t length,
    char *file_name, size_t capacity);
esp_err_t cerberus_storage_airtag_json_load(const char *relative_path,
    const char *file_name, uint8_t *data, size_t capacity, size_t *length);
/* Same checked exclusive transaction as .BLE, with an independent generated
 * Wnnnnn.WFI destination in /CERBERUS/WIFI and Wnnnnn.TMP failure evidence. */
esp_err_t cerberus_storage_wifi_archive_save(const uint8_t *data, size_t length,
    char *file_name, size_t capacity);
esp_err_t cerberus_storage_wifi_archive_load(const char *relative_path,
    const char *file_name, uint8_t *data, size_t capacity, size_t *length);
/* Native historical maps: exclusive Mxxxxx.TMP -> Mxxxxx.MSH publication
 * under /CERBERUS/MESH. Load accepts only safe .MSH names, complete bounded
 * data/EOF/close and clears output on failure. All caller ranges must be
 * disjoint; alias rejection preserves every range unchanged. Never mounts or
 * takes over SD. */
#define CERBERUS_STORAGE_MESH_ARCHIVE_PATH "/CERBERUS/MESH"
#define CERBERUS_STORAGE_MESH_ARCHIVE_MAX_BYTES 18332U
esp_err_t cerberus_storage_mesh_archive_save(const uint8_t *data, size_t length,
    char *file_name, size_t capacity);
esp_err_t cerberus_storage_mesh_archive_load(const char *relative_path,
    const char *file_name, uint8_t *data, size_t capacity, size_t *length);
/* Bounded passive Probe Sniff evidence reports; legacy Wi-Fi report callers
 * retain their existing behavior. No automatic mount or USB/stream takeover. */
#define CERBERUS_STORAGE_PROBE_REPORT_PATH "/CERBERUS/LOGS"
#define CERBERUS_STORAGE_PROBE_REPORT_MAX_BYTES 8192U
#define CERBERUS_STORAGE_MESH_REPORT_PATH "/CERBERUS/LOGS"
#define CERBERUS_STORAGE_MESH_REPORT_MAX_BYTES 98304U
esp_err_t cerberus_storage_mesh_report_save(const uint8_t *data, size_t length,
    char *file_name, size_t capacity);
/* Requires nonempty data and capacity>=11; input/output buffers are disjoint.
 * Under the firmware-owned mutex, publish P00001.CSV..P99999.CSV only after
 * checked exclusive TMP creation, full write, flush/sync, close and final
 * absence check. Failed created TMP is retained and returned by name. No
 * existing file is replaced; no power-loss/durable-rename guarantee is made. */
esp_err_t cerberus_storage_probe_report_save(const uint8_t *data, size_t length,
    char *file_name, size_t capacity);

/* Offline interchange shares the Wi-Fi directory but has independent names:
 * Pnnnnn.LOG (AP JSON) / Snnnnn.TXT (SSID bytes), each via exclusive .TMP.
 * Inputs accept safe .LOG/.JSON or .LOG/.TXT respectively. Only SSID text
 * permits zero bytes, representing an empty list; EOF is still checked. */
esp_err_t cerberus_storage_ap_json_save(const uint8_t *data, size_t length,
    char *file_name, size_t capacity);
esp_err_t cerberus_storage_ap_json_load(const char *relative_path,
    const char *file_name, uint8_t *data, size_t capacity, size_t *length);
esp_err_t cerberus_storage_ssid_text_save(const uint8_t *data, size_t length,
    char *file_name, size_t capacity);
esp_err_t cerberus_storage_ssid_text_load(const char *relative_path,
    const char *file_name, uint8_t *data, size_t capacity, size_t *length);
esp_err_t cerberus_storage_write_qr_bmp(
    const uint8_t *modules,
    uint16_t module_count,
    char *file_name,
    size_t file_name_capacity);
esp_err_t cerberus_storage_write_config_backup(
    const uint8_t *data,
    size_t data_length,
    char *file_name,
    size_t file_name_capacity);
esp_err_t cerberus_storage_read_latest_config_backup(
    uint8_t *data,
    size_t data_capacity,
    size_t *data_length,
    char *file_name,
    size_t file_name_capacity);
esp_err_t cerberus_storage_log_event(
    cerberus_log_level_t level,
    const char *component,
    const char *event,
    const char *detail);
esp_err_t cerberus_storage_delete_diagnostic_report(const char *file_name);
esp_err_t cerberus_storage_run_benchmark(cerberus_storage_benchmark_result_t *result);
esp_err_t cerberus_storage_create_directory(const char *relative_path, const char *name);
esp_err_t cerberus_storage_create_empty_file(const char *relative_path, const char *name);
esp_err_t cerberus_storage_rename_entry(const char *relative_path, const char *old_name, const char *new_name);
/* Copy under the firmware-owned storage lock through a destination-adjacent
 * .CERBTMP file. Existing destination/temp entries or uncertain existence checks
 * are rejected. Rename is attempted only after clean EOF, complete writes,
 * flush/fsync and both successful closes. A failure preserves the source and
 * attempts to remove only the temp created by this call; failed cleanup may
 * leave that temp for inspection. This is not an atomic or power-loss guarantee. */
esp_err_t cerberus_storage_copy_file(
    const char *source_path,
    const char *source_name,
    const char *destination_path,
    const char *destination_name);
esp_err_t cerberus_storage_move_entry(
    const char *source_path,
    const char *source_name,
    const char *destination_path,
    const char *destination_name);
esp_err_t cerberus_storage_delete_entry(const char *relative_path, const char *name);
esp_err_t cerberus_storage_inspect_firmware_package(
    const char *file_name,
    cerberus_firmware_package_info_t *info);
esp_err_t cerberus_storage_write_firmware_package(
    const char *file_name,
    const uint8_t *data,
    size_t data_length,
    cerberus_firmware_package_info_t *info);

#ifdef __cplusplus
}
#endif
