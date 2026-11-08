#include "usb_console.h"
#include "battery.h"
#include "dual_ble_diagnostics.h"

#include "c5_spi_link.h"
#include "csi_link.h"
#include "mac_monitor_link.h"
#include "s3_ble_mac_monitor.h"
#include "c5_event_pump.h"
#include "cerberus_protocol.h"
#include "display.h"
#include "event_bus.h"
#include "recovery.h"
#include "storage.h"
#include "recon_store.h"
#include "recon_recorder.h"
#include "ble_archive_io.h"
#include "remote_id_diagnostics.h"
#include "wifi_readout_diagnostics.h"
#include "dual_wifi_diagnostics.h"
#include "wifi_packet_count_diagnostics.h"
#include "wifi_multissid_session.h"
#include "wifi_pinescan_session.h"
#include "ble_test.h"
#include "s3_ble_attack.h"
#include "s3_ble_host.h"
#include "s3_ble_scanner.h"
#include "s3_ble_central.h"
#include "s3_wifi_receiver.h"
#include "usb_mass_storage.h"
#include "system_health.h"
#include "wardrive_service.h"
#include "gps_recorder.h"
#include "wardrive_gps_uart.h"
#include "wardrive_upload_settings.h"
#include "wardrive_log_manager.h"

#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_app_desc.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_mac.h"
#include "esp_private/usb_phy.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#define USB_CONSOLE_LINE_CAPACITY 64U
#define USB_CONSOLE_OUTPUT_CAPACITY 768U
#define USB_CONSOLE_TASK_STACK_BYTES 4096U
#define USB_CONSOLE_TASK_PRIORITY 4U

static const char *TAG = "usb_console";
static TaskHandle_t console_task_handle;
static usb_phy_handle_t console_phy;
/* Only the console task formats into this buffer. Lifecycle calls are
 * serialized by the UI; stop retains it until the task publishes quiescence.
 * Keep large output off both the task stack and scarce internal RAM. */
static char *console_output;
static atomic_bool console_stop_requested;
static atomic_bool console_task_stopped = true;

/* Keep command-local snapshots out of the permanent console task frame.
 * With size optimization, otherwise even an unrelated command inherits the
 * largest inlined snapshot and can exhaust the 4096-byte task stack. */
static __attribute__((noinline)) void print_help(void);
static __attribute__((noinline)) void print_info(void);
static __attribute__((noinline)) void print_health(void);
static __attribute__((noinline)) void print_memory(void);
static __attribute__((noinline)) void print_c5(void);
static __attribute__((noinline)) void print_storage(void);
static __attribute__((noinline)) void print_resources(void);
static __attribute__((noinline)) void print_capture(void);
static __attribute__((noinline)) void print_usb(void);
static __attribute__((noinline)) void print_network(void);
static __attribute__((noinline)) void print_wifi_seen(void);
static __attribute__((noinline)) void print_wifi_readout(void);
static __attribute__((noinline)) void print_wifi_dual(void);
static __attribute__((noinline)) void print_packet_count(void);
static __attribute__((noinline)) void print_recon_record(void);
static __attribute__((noinline)) void print_ble_archive(void);
static __attribute__((noinline)) void print_remote_id(void);
static __attribute__((noinline)) void print_stations(void);
static __attribute__((noinline)) void print_reports(void);
static __attribute__((noinline)) void print_radio(void);
static __attribute__((noinline)) void print_gps(void);
static __attribute__((noinline)) void print_upload_profile(void);
static __attribute__((noinline)) void print_upload_status(void);
static __attribute__((noinline)) void print_wardrive_logs(void);
static __attribute__((noinline)) void print_dual_ble(void);
static __attribute__((noinline)) void print_ble_memory(void);
static __attribute__((noinline)) void print_battery(void);

static void console_write(const char *text)
{
    if (text == NULL) {
        return;
    }

    size_t remaining = strlen(text);
    const char *cursor = text;
    while (remaining > 0U && !atomic_load(&console_stop_requested) &&
           usb_serial_jtag_is_connected()) {
        int written = usb_serial_jtag_write_bytes(
            cursor,
            remaining,
            pdMS_TO_TICKS(100));
        if (written <= 0) {
            break;
        }
        cursor += written;
        remaining -= (size_t)written;
    }
}

static void console_printf(const char *format, ...)
{
    char *output = console_output;
    if (output == NULL) {
        return;
    }
    va_list arguments;
    va_start(arguments, format);
    int length = vsnprintf(output, USB_CONSOLE_OUTPUT_CAPACITY, format, arguments);
    va_end(arguments);
    if (length <= 0) {
        return;
    }
    output[USB_CONSOLE_OUTPUT_CAPACITY - 1U] = '\0';
    console_write(output);
}

static void print_banner(void)
{
    console_write(
        "\r\nCERBERUS READ-ONLY DIAGNOSTIC CONSOLE\r\n"
        "No shell, radio control, HID actions, or firmware writes.\r\n"
        "Type 'help' for commands.\r\n");
}

static void print_help(void)
{
    console_write(
        "help     Show this command list\r\n"
        "info     Show firmware and protocol information\r\n"
        "health   Show reset, memory, stack, and crash status\r\n"
        "memory   Show S3 internal/PSRAM capacity and C5 heap headroom\r\n"
        "c5       Show onboard C5 link health and capabilities\r\n"
        "storage  Show microSD health and capacity\r\n"
        "resources Verify SD Recon database and SHA-256\r\n"
        "recon_record Read SD Recon recording state and counters\r\n"
        "ble_archive Read saved BLE snapshot worker state\r\n"
        "capture   Read passive capture, MultiSSID and PineScan state\r\n"
        "usb      Show USB storage handover status\r\n"
        "network  Show onboard C5 network scan status\r\n"
        "wifi_seen Read completed Wi-Fi source-age coverage without scanning\r\n"
        "wifi_readout Read local UI AP transfer metrics without SPI or scanning\r\n"
        "wifi_dual Read UI-published split Wi-Fi state without SPI or scanning\r\n"
        "packet_count Read copied Count setup/progress without radio access\r\n"
        "remote_id Read published Remote ID session counts without scanning\r\n"
        "stations Read passive station capture without renewing its lease\r\n"
        "reports  Read the first 32 SD log names and network CSVs\r\n"
        "radio    Show C5, S3 BLE/Wi-Fi and Wardriving status without renewing leases\r\n"
        "gps      Show receiver, baud and fix health without coordinates\r\n"
        "upload_profile Show saved upload readiness without credentials\r\n"
        "upload_status Show log operation and numeric transfer diagnostics\r\n"
        "ward_logs    List finalized wardrive filenames and sizes, read-only\r\n"
        "dual_ble Read cached two-receiver BLE/Fox state without SPI or scanning\r\n"
        "ble_memory Read cached S3 BLE lifecycle heap samples without heap walks\r\n"
        "battery  Show cached onboard battery gauge status\r\n"
        "All commands are read-only and accept no arguments.\r\n");
}

static void print_info(void)
{
    const esp_app_desc_t *description = esp_app_get_description();
    uint8_t mac[6];
    char mac_text[18] = "unavailable";
    if (esp_efuse_mac_get_default(mac) == ESP_OK) {
        (void)snprintf(mac_text, sizeof(mac_text), "%02x:%02x:%02x:%02x:%02x:%02x",
                mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }
    console_printf(
        "product=Cerberus Unchained\r\n"
        "s3_mac=%s\r\n"
        "project=%s\r\n"
        "version=%s\r\n"
        "idf=%s\r\n"
        "build=%s %s\r\n"
        "s3_c5_protocol=%u\r\n"
        "s3_rom_download=%s\r\n"
        "screen_buffered=%s\r\n"
        "interface=native USB Serial/JTAG\r\n"
        "access=read-only diagnostics\r\n",
        mac_text,
        description->project_name,
        description->version,
        description->idf_ver,
        description->date,
        description->time,
        CERBERUS_PROTOCOL_VERSION,
        cerberus_recovery_download_mode_available() ? "available" : "blocked-by-efuse",
        cerberus_display_buffering_enabled() ? "yes" : "no");
}

static void print_memory(void)
{
    const uint32_t caps[]={MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT};
    const char *names[]={"S3_INTERNAL","S3_PSRAM"};
    for(unsigned i=0;i<2;++i) {
        console_printf("memory=%s\r\ntotal_bytes=%u\r\nfree_bytes=%u\r\nminimum_free_bytes=%u\r\nlargest_free_block_bytes=%u\r\n",
            names[i],(unsigned)heap_caps_get_total_size(caps[i]),
            (unsigned)heap_caps_get_free_size(caps[i]),
            (unsigned)heap_caps_get_minimum_free_size(caps[i]),
            (unsigned)heap_caps_get_largest_free_block(caps[i]));
    }
    console_printf("s3_task_count=%u\r\n",(unsigned)uxTaskGetNumberOfTasks());
    /* ESP-IDF reports this minimum remaining task stack in bytes. Query this
     * command again after a diagnostic sequence to include its exercised paths. */
    console_printf("usb_console_stack_low_water_bytes=%u\r\n",
        (unsigned)uxTaskGetStackHighWaterMark(NULL));
    cerberus_c5_status_t status;
    esp_err_t err=c5_spi_link_get_status(&status);
    console_printf("memory=C5_INTERNAL\r\ntransport=%s\r\n",esp_err_to_name(err));
    if(err==ESP_OK)console_printf("free_kib=%u\r\nminimum_free_kib=%u\r\nlargest_free_block_bytes=unavailable\r\n",
        status.free_heap_kib,status.minimum_free_heap_kib);
}

static void print_health(void)
{
    cerberus_system_health_t health;
    cerberus_system_health_snapshot(&health);
    console_printf(
        "reset=%s\r\n"
        "previous_reset_fault=%s\r\n"
        "uptime_ms=%" PRIu64 "\r\n"
        "heap_free_bytes=%u\r\n"
        "heap_low_water_bytes=%u\r\n"
        "main_stack_low_water_bytes=%u\r\n",
        cerberus_reset_reason_label(health.reset_reason),
        health.previous_reset_fault ? "yes" : "no",
        health.uptime_ms,
        (unsigned)health.free_internal_bytes,
        (unsigned)health.minimum_internal_bytes,
        (unsigned)health.main_task_minimum_stack_bytes);

    cerberus_crash_record_t crash_record;
    esp_err_t err = cerberus_crash_record_inspect(&crash_record);
    if (err == ESP_ERR_NOT_FOUND || !crash_record.present) {
        console_write("crash_record=none\r\n");
    } else if (err == ESP_OK && crash_record.valid) {
        console_printf(
            "crash_record=valid\r\n"
            "crash_size_bytes=%u\r\n"
            "crash_task=%s\r\n",
            (unsigned)crash_record.size_bytes,
            crash_record.task_name);
    } else {
        console_printf(
            "crash_record=invalid\r\n"
            "crash_status=%s\r\n"
            "crash_size_bytes=%u\r\n",
            esp_err_to_name(err),
            (unsigned)crash_record.size_bytes);
    }

    cerberus_event_bus_status_t event_status;
    err = cerberus_event_bus_get_status(&event_status);
    if (err == ESP_OK) {
        console_printf(
            "event_attempts=%" PRIu32 "\r\n"
            "event_dispatched=%" PRIu32 "\r\n"
            "event_pending=%" PRIu32 "\r\n"
            "event_dropped=%" PRIu32 "\r\n"
            "event_subscribers=%u\r\n",
            event_status.publish_attempts,
            event_status.dispatched_events,
            event_status.pending_events,
            event_status.dropped_events,
            event_status.subscriber_count);
    } else {
        console_printf("event_bus=unavailable\r\nevent_error=%s\r\n", esp_err_to_name(err));
    }
}

static void print_c5(void)
{
    cerberus_c5_status_t status;
    esp_err_t err = c5_spi_link_get_status(&status);
    cerberus_c5_transport_diagnostics_t transport = {0};
    c5_spi_link_get_transport_diagnostics(&transport);
    console_printf(
        "spi_exchanges=%" PRIu32 "\r\n"
        "spi_mutex_timeouts=%" PRIu32 "\r\n"
        "spi_ready_initial_timeouts=%" PRIu32 "\r\n"
        "spi_ready_fall_timeouts=%" PRIu32 "\r\n"
        "spi_ready_high_timeouts=%" PRIu32 "\r\n"
        "spi_ready_latched_pulses=%" PRIu32 "\r\n"
        "spi_ready_fall_edges=%" PRIu32 "\r\n"
        "spi_legacy_fallbacks=%" PRIu32 "\r\n"
        "spi_driver_errors=%" PRIu32 "\r\n"
        "spi_max_mutex_wait_us=%" PRIu32 "\r\n"
        "spi_max_mutex_hold_us=%" PRIu32 "\r\n"
        "spi_max_exchange_us=%" PRIu32 "\r\n",
        transport.exchanges, transport.mutex_timeouts,
        transport.ready_initial_timeouts, transport.ready_fall_timeouts,
        transport.ready_high_timeouts, transport.ready_latched_pulses,
        transport.ready_fall_edges, transport.legacy_fallbacks,
        transport.spi_errors, transport.max_mutex_wait_us,
        transport.max_mutex_hold_us, transport.max_exchange_us);
    if (err != ESP_OK) {
        console_printf("status=unavailable\r\nerror=%s\r\n", esp_err_to_name(err));
        return;
    }

    console_printf(
        "status=online\r\n"
        "spi_clock_hz=%" PRIu32 "\r\n"
        "protocol=%u\r\n"
        "firmware=%" PRIu32 "\r\n"
        "uptime_ms=%" PRIu32 "\r\n"
        "reset=%s\r\n"
        "boot_count=%u\r\n"
        "heap_free_kib=%u\r\n"
        "heap_low_water_kib=%u\r\n"
        "radio_state=0x%02x\r\n"
        "capabilities=0x%08" PRIx32 "\r\n",
        c5_spi_link_clock_hz(),
        status.protocol_version,
        status.firmware_version,
        status.uptime_ms,
        cerberus_reset_reason_label((esp_reset_reason_t)status.reset_reason),
        status.boot_count,
        status.free_heap_kib,
        status.minimum_free_heap_kib,
        status.radio_state,
        status.capabilities);

    if ((status.capabilities & CERBERUS_C5_CAP_WIFI_RADIO_STATS) != 0U) {
        cerberus_wifi_radio_stats_payload_t stats;
        err = c5_spi_link_get_wifi_radio_stats(&stats);
        if (err == ESP_OK) {
            console_printf(
                "wifi_stats_generation=%u\r\n"
                "wifi_stats_total=%u\r\n"
                "wifi_stats_analyzed=%u\r\n"
                "wifi_stats_24_ghz=%u\r\n"
                "wifi_stats_5_ghz=%u\r\n"
                "wifi_stats_open=%u\r\n"
                "wifi_stats_secured=%u\r\n"
                "wifi_stats_hidden=%u\r\n"
                "wifi_stats_duration_ms=%" PRIu32 "\r\n"
                "wifi_stats_successful_scans=%u\r\n"
                "wifi_stats_failed_scans=%u\r\n",
                stats.generation,
                stats.total_count,
                stats.analyzed_count,
                stats.count_24_ghz,
                stats.count_5_ghz,
                stats.open_count,
                stats.secured_count,
                stats.hidden_count,
                stats.scan_duration_ms,
                stats.successful_scans,
                stats.failed_scans);
        } else {
            console_printf("wifi_stats=unavailable\r\nwifi_stats_error=%s\r\n", esp_err_to_name(err));
        }
    }

    if ((status.capabilities & CERBERUS_C5_CAP_WIFI_DEFENSE_SURVEY) != 0U) {
        cerberus_wifi_defense_status_payload_t defense;
        err = c5_spi_link_get_wifi_defense_status(&defense);
        if (err == ESP_OK) {
            console_printf(
                "wifi_defense_state=%u\r\n"
                "wifi_defense_generation=%u\r\n"
                "wifi_defense_completed=%u\r\n"
                "wifi_defense_channels=%u\r\n"
                "wifi_defense_flagged=%u\r\n"
                "wifi_defense_current_channel=%u\r\n"
                "wifi_defense_elapsed_ms=%" PRIu32 "\r\n"
                "wifi_defense_scope=aggregate_metadata_only\r\n",
                defense.state, defense.generation, defense.completed_count,
                defense.result_count, defense.flagged_count, defense.current_channel,
                defense.elapsed_ms);
            for (uint8_t index = 0U; index < defense.completed_count; ++index) {
                cerberus_wifi_defense_result_payload_t item;
                if (c5_spi_link_get_wifi_defense_result(
                        defense.generation, index, &item) == ESP_OK) {
                    console_printf(
                        "wifi_defense_channel=%u flags=0x%02x mgmt=%u ctrl=%u data=%u "
                        "beacon_per_sec=%u deauth=%u disassoc=%u noise=%d peak=%d\r\n",
                        item.channel, item.assessment_flags, item.management_frames,
                        item.control_frames, item.data_frames, item.beacon_rate_per_sec,
                        item.deauth_frames, item.disassoc_frames,
                        item.average_noise_floor, item.strongest_rssi);
                }
            }
        } else {
            console_printf("wifi_defense=unavailable\r\nwifi_defense_error=%s\r\n",
                esp_err_to_name(err));
        }
    }

    cerberus_c5_event_pump_status_t event_status;
    err = cerberus_c5_event_pump_get_status(&event_status);
    if (err == ESP_OK && event_status.supported) {
        console_printf(
            "event_queue=supported\r\n"
            "event_pending=%u\r\n"
            "event_capacity=%u\r\n"
            "event_produced=%" PRIu32 "\r\n"
            "event_consumed=%" PRIu32 "\r\n"
            "event_remote_dropped=%u\r\n"
            "event_flow_control=%s\r\n"
            "event_flow_configured=%s\r\n"
            "event_flow_enabled=%s\r\n"
            "event_backpressure=%s\r\n"
            "event_low_watermark=%u\r\n"
            "event_high_watermark=%u\r\n"
            "event_deferred=%u\r\n"
            "event_control_generation=%u\r\n"
            "event_batch=%s\r\n"
            "event_batch_transfers=%" PRIu32 "\r\n"
            "event_batch_failures=%" PRIu32 "\r\n"
            "event_single_transfers=%" PRIu32 "\r\n"
            "event_received=%" PRIu32 "\r\n"
            "event_local_publish_failures=%" PRIu32 "\r\n"
            "event_last_sequence=%u\r\n"
            "event_last_source=%u\r\n"
            "event_last_state=%u\r\n",
            event_status.remote.pending,
            event_status.remote.capacity,
            event_status.remote.produced,
            event_status.remote.consumed,
            event_status.remote.dropped,
            event_status.flow_supported ? "supported" : "unsupported",
            event_status.flow_configured ? "yes" : "no",
            (event_status.remote.flow_flags & CERBERUS_C5_EVENT_FLOW_FLAG_ENABLED) != 0U
                ? "yes"
                : "no",
            (event_status.remote.flow_flags & CERBERUS_C5_EVENT_FLOW_FLAG_ACTIVE) != 0U
                ? "active"
                : "clear",
            event_status.remote.low_watermark,
            event_status.remote.high_watermark,
            event_status.remote.deferred,
            event_status.remote.control_generation,
            event_status.batch_supported ? "supported" : "unsupported",
            event_status.batch_transfers,
            event_status.batch_failures,
            event_status.single_event_transfers,
            event_status.received_events,
            event_status.local_publish_failures,
            event_status.last_event.event_sequence,
            event_status.last_event.source,
            event_status.last_event.state);
    } else if ((status.capabilities & CERBERUS_C5_CAP_EVENT_QUEUE) != 0U) {
        console_write("event_queue=starting\r\n");
    } else {
        console_write("event_queue=unsupported\r\n");
    }

    if ((status.capabilities & CERBERUS_C5_CAP_FRAGMENTED_OBJECTS) != 0U) {
        cerberus_c5_diagnostic_snapshot_t snapshot;
        uint16_t snapshot_length = 0U;
        uint32_t snapshot_crc32 = 0U;
        err = c5_spi_link_read_fragmented_object(
            CERBERUS_FRAGMENT_OBJECT_C5_DIAGNOSTIC_SNAPSHOT,
            &snapshot,
            sizeof(snapshot),
            &snapshot_length,
            &snapshot_crc32);
        if (err == ESP_OK && snapshot_length == sizeof(snapshot) &&
            snapshot.firmware_version == status.firmware_version) {
            console_printf(
                "fragment_transfer=supported\r\n"
                "fragment_object=c5_diagnostic_snapshot\r\n"
                "fragment_bytes=%u\r\n"
                "fragment_chunks=%u\r\n"
                "fragment_crc32=%08" PRIx32 "\r\n"
                "snapshot_uptime_ms=%" PRIu32 "\r\n"
                "snapshot_heap_free_bytes=%" PRIu32 "\r\n"
                "snapshot_event_produced=%" PRIu32 "\r\n"
                "snapshot_event_consumed=%" PRIu32 "\r\n",
                snapshot_length,
                (unsigned)((snapshot_length + CERBERUS_FRAGMENT_DATA_SIZE - 1U) /
                    CERBERUS_FRAGMENT_DATA_SIZE),
                snapshot_crc32,
                snapshot.uptime_ms,
                snapshot.free_heap_bytes,
                snapshot.event_produced,
                snapshot.event_consumed);
        } else {
            console_printf(
                "fragment_transfer=unavailable\r\nfragment_error=%s\r\n",
                esp_err_to_name(err != ESP_OK ? err : ESP_ERR_INVALID_RESPONSE));
        }
    } else {
        console_write("fragment_transfer=unsupported\r\n");
    }
}

static void print_storage(void)
{
    cerberus_storage_status_t status;
    esp_err_t err = cerberus_storage_get_status(&status);
    if (err != ESP_OK || !status.mounted) {
        console_printf("status=unavailable\r\nerror=%s\r\n", esp_err_to_name(err));
        return;
    }

    console_printf(
        "status=mounted\r\n"
        "card=%s\r\n"
        "capacity_bytes=%" PRIu64 "\r\n"
        "free_bytes=%" PRIu64 "\r\n"
        "sector_bytes=%" PRIu32 "\r\n",
        status.card_name,
        status.total_bytes,
        status.free_bytes,
        status.sector_size);
}

static void print_battery(void)
{
    cerberus_battery_status_t status;
    cerberus_battery_get_status(&status);
    if (!status.valid) {
        console_printf("status=unavailable\r\nerror=%s\r\nlast_attempt_ms=%" PRId64 "\r\n",
            esp_err_to_name(status.last_error), status.last_attempt_ms);
        return;
    }
    console_printf("status=available\r\ngauge=MAX17048\r\npercent=%u\r\nvoltage_mv=%u\r\n"
        "gauge_version=0x%04x\r\nlast_read_ms=%" PRId64 "\r\ncharging=unknown\r\n",
        (unsigned)status.percent, (unsigned)status.voltage_mv,
        (unsigned)status.version, status.last_read_ms);
}

static void print_dual_ble(void)
{
    cerberus_dual_ble_diagnostics_t d;
    cerberus_dual_ble_diagnostics_get(&d);
    console_printf("dual_ble_available=%u\r\ndual_ble_version=%u\r\ndual_ble_sequence=%" PRIu64
        "\r\ndual_ble_sequence_saturated=%u\r\ndual_ble_published_ms=%" PRIu64
        "\r\ndual_ble_clock_valid=%u\r\ndual_ble_time_scope=S3_UI_publication_not_RF_time\r\n",
        d.available, d.version, d.sequence, d.sequence_saturated, d.published_ms, d.clock_valid);
    if (!d.available) return;
    console_printf("dual_ble_mode=%d\r\ndual_ble_awake=%u\r\ndual_ble_allocated=%u\r\n"
        "dual_ble_enabled=%u\r\ndual_ble_attached=%u\r\ndual_ble_receiving=%u\r\n"
        "dual_ble_reading=%u\r\ndual_ble_stopping=%u\r\ndual_ble_released=%u\r\n"
        "dual_ble_c5_phase=%u\r\ndual_ble_c5_state=%u\r\ndual_ble_c5_generation=%u\r\n"
        "dual_ble_c5_sweeps=%" PRIu32 "\r\ndual_ble_c5_rows=%u\r\ndual_ble_c5_read_index=%u\r\n"
        "dual_ble_c5_extended_configured=%u\r\ndual_ble_c5_error=%" PRId32 "\r\n",
        d.requested_mode, d.awake, d.allocated, d.enabled, d.attached, d.receiving,
        d.reading, d.stopping, d.released, d.c5_phase, d.c5_state, d.c5_generation,
        d.c5_sweeps, d.c5_rows, d.c5_read_index, d.extended_configured, d.c5_error);
    console_printf("dual_ble_requested_family=%u\r\ndual_ble_c5_requested_family=%u\r\n"
        "dual_ble_c5_published_family=%u\r\n"
        "dual_ble_family_scope=UI_desired_controller_request_and_retained_bank_not_RF_identity\r\n",
        d.requested_family, d.c5_requested_family, d.c5_published_family);
    console_printf("dual_ble_c5_live_updates=%u\r\ndual_ble_c5_snapshots=%" PRIu32
        "\r\ndual_ble_c5_has_first_result=%u\r\ndual_ble_c5_first_result_ms=%" PRIu32 "\r\n"
        "dual_ble_snapshot_scope=coherent_bank_publications_in_current_discovery_not_RF_sweeps\r\n"
        "dual_ble_sweeps_scope=completed_RF_sweeps_in_current_public_receiver_run\r\n"
        "dual_ble_first_result_scope=reader_latency_to_first_new_nonempty_bank_not_RF_time\r\n",
        (unsigned)d.c5_live_updates, d.c5_snapshots, (unsigned)d.c5_has_first_result, d.c5_first_result_ms);
    console_printf("dual_ble_c5_inventory_known=%u\r\ndual_ble_c5_inventory_replacement_enabled=%u\r\n"
        "dual_ble_c5_inventory_replaced=%" PRIu32 "\r\ndual_ble_c5_inventory_dropped=%" PRIu32 "\r\n"
        "dual_ble_c5_inventory_scope=last_published_bank_not_unique_devices_or_RF_loss\r\n",
        (unsigned)d.c5_inventory_known, (unsigned)d.c5_inventory_replacement_enabled,
        d.c5_inventory_replaced, d.c5_inventory_dropped);
    console_printf("dual_ble_s3_initialized=%u\r\ndual_ble_s3_synced=%u\r\ndual_ble_s3_owner=%u\r\n"
        "dual_ble_s3_scan_cached=%u\r\ndual_ble_s3_state=%u\r\n"
        "dual_ble_s3_scan_error=%" PRId32 "\r\ndual_ble_s3_discovery_enabled=%u\r\n"
        "dual_ble_s3_discovery_attached=%u\r\ndual_ble_s3_discovery_owned=%u\r\n"
        "dual_ble_fox_open=%u\r\ndual_ble_fox_pending=%u\r\ndual_ble_fox_stopping=%u\r\n"
        "dual_ble_fox_error=%" PRId32 "\r\ndual_ble_s3_track_error=%" PRId32 "\r\n",
        d.s3_initialized, d.s3_synced, d.s3_owner, d.s3_scan_cached, d.s3_state,
        d.s3_scan_error, d.s3_discovery_enabled, d.s3_discovery_attached, d.s3_discovery_owned,
        d.fox_open, d.fox_pending, d.fox_stopping, d.fox_error, d.s3_track_error);
    console_printf("dual_ble_s3_target_matches=%u\r\ndual_ble_s3_seen=%u\r\ndual_ble_s3_fresh=%u\r\n"
        "dual_ble_s3_rssi=%d\r\ndual_ble_s3_age_ms=%" PRIu32 "\r\ndual_ble_s3_reports=%" PRIu32 "\r\n"
        "dual_ble_c5_target_matches=%u\r\ndual_ble_c5_seen=%u\r\ndual_ble_c5_fresh=%u\r\n"
        "dual_ble_c5_rssi=%d\r\ndual_ble_c5_age_upper_ms=%" PRIu32 "\r\ndual_ble_c5_reports=%" PRIu32 "\r\n"
        "dual_ble_count_scope=independent_received_reports_not_unique_packets_or_devices\r\n"
        "dual_ble_c5_reports_scope=last_sampled_C5_sweep\r\n",
        d.s3.target_matches, d.s3.seen, d.s3.fresh, d.s3.rssi, d.s3.age_ms, d.s3.reports,
        d.c5.target_matches, d.c5.seen, d.c5.fresh, d.c5.rssi, d.c5.age_ms, d.c5.reports);
}

static void print_ble_memory(void)
{
    cerberus_s3_ble_memory_diagnostics_t snapshot = {0};
    cerberus_s3_ble_host_memory_diagnostics(&snapshot);
    static const char *const phases[CERBERUS_S3_BLE_MEMORY_STAGE_COUNT] = {
        "before_init", "after_init", "after_sync", "released", "refused"
    };
    console_printf("ble_memory_scope=cached_process_wide_phase_samples_not_ble_allocation_totals\r\n");
    for (unsigned i = 0U; i < CERBERUS_S3_BLE_MEMORY_STAGE_COUNT; ++i) {
        const cerberus_s3_ble_memory_sample_t *sample = &snapshot.samples[i];
        console_printf(
            "ble_memory_phase=%s valid=%u sequence=%" PRIu32 " receipt_us=%" PRId64
            " requested_owner=%u initialized=%u synced=%u error=%" PRId32
            " internal_free=%" PRIu32 " internal_minimum=%" PRIu32
            " psram_free=%" PRIu32 " psram_minimum=%" PRIu32 "\r\n",
            phases[i], sample->valid ? 1U : 0U, sample->sequence, sample->receipt_us,
            (unsigned)sample->requested_owner, sample->initialized ? 1U : 0U,
            sample->synced ? 1U : 0U, (int32_t)sample->error,
            sample->internal_free, sample->internal_minimum,
            sample->psram_free, sample->psram_minimum);
    }
}

static void print_resources(void)
{
    uint8_t digest[32];
    uint64_t bytes = 0;
    esp_err_t err = cerberus_storage_sha256_file("/CERBERUS", "RECON.DB", digest, &bytes);
    console_printf("path=/CERBERUS/RECON.DB\r\nbytes=%" PRIu64 "\r\nerror=%s\r\n", bytes, esp_err_to_name(err));
    if (err != ESP_OK) return;
    char hex[65];
    for (unsigned i = 0; i < 32; ++i) snprintf(hex + i * 2, 3, "%02x", digest[i]);
    console_printf("sha256=%s\r\n", hex);
    const uint8_t known_prefix[6] = {0, 0, 0x0c, 0, 0, 1};
    char vendor[RECON_DB_VENDOR_SIZE];
    recon_db_result_t result = recon_store_vendor(known_prefix, vendor);
    console_printf("lookup_00000c=%u\r\nvendor=%s\r\n", (unsigned)result, vendor);
}

static void print_usb(void)
{
    cerberus_usb_mass_storage_status_t status;
    cerberus_usb_mass_storage_get_status(&status);
    console_printf("msc_active=%u\r\nhost_attached=%u\r\nsafe_to_release=%u\r\nlast_error=%s\r\n",
        status.active, status.host_attached, status.safe_to_release, esp_err_to_name(status.last_error));
}

static void print_wardrive_logs(void)
{
    /* Use the same catalog and exclusive SD lease as the touchscreen. No CSV
     * contents, network connection, deletion, or receipt mutation is allowed. */
    const size_t capacity = CERBERUS_WARD_LOG_CAPACITY;
    cerberus_ward_log_entry_t *entries = heap_caps_calloc(capacity, sizeof(*entries),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint64_t lease = 0;
    size_t count = 0;
    cerberus_ward_log_catalog_info_t info = {0};
    esp_err_t error = entries ? cerberus_ward_log_storage_begin(&lease) : ESP_ERR_NO_MEM;
    if (error == ESP_OK) error = cerberus_ward_log_storage_list_info(lease, entries, capacity, &count, &info);
    esp_err_t release = ESP_OK;
    if (lease) {
        do {
            release = cerberus_ward_log_storage_end(lease);
            if (release == ESP_ERR_TIMEOUT) vTaskDelay(pdMS_TO_TICKS(20));
        } while (release == ESP_ERR_TIMEOUT);
        if (error == ESP_OK) error = release;
    }
    console_printf("ward_logs_error=%d\r\nward_logs_release_error=%d\r\nward_logs_count=%u\r\n",
        error, release, (unsigned)count);
    console_printf("ward_logs_directory_present=%u\r\nward_logs_directory_entries=%lu\r\nward_logs_csv_entries=%lu\r\nward_logs_temporary_entries=%lu\r\nward_logs_unrecognized_csv=%lu\r\n",
        info.directory_present,(unsigned long)info.entries,(unsigned long)info.csv_entries,
        (unsigned long)info.temporary_entries,(unsigned long)info.unrecognized_csv);
    if(error==ESP_OK&&info.first_unrecognized_csv[0]){
        /* A bounded, hex-encoded basename cannot inject console fields. */
        char encoded[CERBERUS_WARD_LOG_NAME_CAPACITY*2U]={0};
        for(size_t i=0;info.first_unrecognized_csv[i];++i)
            snprintf(encoded+i*2U,sizeof(encoded)-i*2U,"%02x",(unsigned char)info.first_unrecognized_csv[i]);
        console_printf("ward_logs_first_unrecognized_hex=%s\r\n",encoded);
    }
    for (size_t i = 0; error == ESP_OK && i < count && !atomic_load(&console_stop_requested); ++i)
        console_printf("ward_log=%s bytes=%" PRIu64 "\r\n", entries[i].name, entries[i].bytes);
    heap_caps_free(entries);
}

static void print_reports(void)
{
    const size_t capacity = 32;
    cerberus_storage_entry_t *entries = heap_caps_calloc(capacity, sizeof(*entries), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!entries) entries = calloc(capacity, sizeof(*entries));
    if (!entries) { console_write("error=ESP_ERR_NO_MEM\r\n"); return; }
    size_t count = 0;
    esp_err_t err = cerberus_storage_list_directory("/CERBERUS/LOGS", entries, capacity, &count);
    console_printf("error=%s\r\nentries=%u\r\nlisting_may_be_truncated=%u\r\n", esp_err_to_name(err), (unsigned)count, count == capacity);
    for (size_t i = 0; err == ESP_OK && i < count && !atomic_load(&console_stop_requested); ++i) {
        const char *name = entries[i].name;
        /* Only generated 8.3 network report names are emitted or read. */
        bool report = strlen(name) == 12 && !memcmp(name, "WIFI", 4) && !strcmp(name + 8, ".CSV");
        for (unsigned j = 4; report && j < 8; ++j) report = name[j] >= '0' && name[j] <= '9';
        if (!report || entries[i].is_directory) continue;
        console_printf("report=%s bytes=%" PRIu64 "\r\n", name, entries[i].size_bytes);
        if (entries[i].size_bytes > CERBERUS_STORAGE_WIFI_REPORT_MAX_BYTES) { console_write("read=skipped_oversized\r\n"); continue; }
        uint64_t offset = 0;
        while (offset < entries[i].size_bytes && !atomic_load(&console_stop_requested)) {
            uint8_t data[256]; size_t got = 0; uint64_t size = 0;
            err = cerberus_storage_read_file_chunk("/CERBERUS/LOGS", name, offset, data, sizeof(data)-1, &got, &size);
            if (err != ESP_OK || !got || size != entries[i].size_bytes) break;
            for (size_t j = 0; j < got; ++j) if ((data[j] < 32 && data[j] != '\n' && data[j] != '\r') || data[j] > 126) data[j] = '.';
            data[got] = 0; console_write((char *)data); offset += got;
        }
        console_printf("\r\nread_bytes=%" PRIu64 " error=%s\r\n", offset, esp_err_to_name(err));
    }
    free(entries);
}

static void print_stations(void)
{
    cerberus_c5_status_t radio;
    esp_err_t error=c5_spi_link_get_status(&radio);
    if(error!=ESP_OK||!(radio.capabilities&CERBERUS_C5_CAP_WIFI_STATIONS)){
        console_printf("stations=unavailable\r\ntransport=%s\r\n",esp_err_to_name(error));return;
    }
    cerberus_wifi_station_status_t status={0};
    error=c5_spi_link_station_status(0,&status);
    console_printf("transport=%s\r\njob_id=%u\r\nstate=%u\r\nretained=%u\r\nobserved_frames=%lu\r\nmatched_frames=%lu\r\noverflow_frames=%lu\r\nelapsed_ms=%u\r\njob_error=%ld\r\n",
        esp_err_to_name(error),status.job_id,status.state,status.count,(unsigned long)status.observed_frames,
        (unsigned long)status.matched_frames,(unsigned long)status.overflow_frames,status.elapsed_ms,(long)status.error_code);
    for(uint8_t i=0;error==ESP_OK&&i<status.count&&!atomic_load(&console_stop_requested);++i){
        cerberus_wifi_station_result_t result;
        error=c5_spi_link_station_result(status.job_id,i,&result);
        if(error!=ESP_OK){console_printf("result_error=%s\r\n",esp_err_to_name(error));break;}
        console_printf("station=%02X:%02X:%02X:%02X:%02X:%02X channel=%u frames=%lu from_station=%u to_station=%u rssi=%d signal_source=%s flags=0x%02X last_seen_ms=%u\r\n",
            result.mac[0],result.mac[1],result.mac[2],result.mac[3],result.mac[4],result.mac[5],result.channel,
            (unsigned long)result.frames,result.from_station,result.to_station,result.rssi,
            result.from_station?"station":"ap_only",result.flags,result.last_seen_ms);
    }
}
static void print_network(void)
{
    cerberus_net_scan_status_t status = {0};
    esp_err_t err = c5_spi_link_network_status(&status);
    console_printf("transport=%s\r\njob_id=%u\r\nstate=%u\r\ntested=%u\r\ntotal=%u\r\n"
        "open=%u\r\nclosed=%u\r\nno_reply=%u\r\nretained=%u\r\nelapsed_ms=%lu\r\njob_error=%ld\r\n",
        esp_err_to_name(err), status.job_id, status.state, status.tested, status.total,
        status.open, status.closed, status.filtered, status.result_count,
        (unsigned long)status.elapsed_ms, (long)status.error_code);
}

static void print_wifi_readout(void)
{
    cerberus_wifi_readout_diagnostics_t snapshot;
    cerberus_wifi_readout_diagnostics_get(&snapshot);
    console_printf("wifi_readout_scope=s3_ap_readout_excludes_rf_scan_display_touch\r\n"
        "wifi_readout_state=%u\r\nwifi_readout_phase=%u\r\n"
        "wifi_readout_rows_total=%u\r\nwifi_readout_rows_copied=%u\r\n"
        "wifi_readout_attempts=%" PRIu32 "\r\nwifi_readout_completed=%" PRIu32 "\r\n"
        "wifi_readout_failed=%" PRIu32 "\r\nwifi_readout_cancelled=%" PRIu32 "\r\n"
        "wifi_readout_step_calls=%" PRIu32 "\r\nwifi_readout_error=%" PRId32 "\r\n"
        "wifi_readout_cancel_reason=%u\r\nwifi_readout_timing_valid=%u\r\n"
        "wifi_readout_counters_saturated=%u\r\nwifi_readout_sampled_us=%" PRIu64 "\r\n"
        "wifi_readout_elapsed_us=%" PRIu64 "\r\nwifi_readout_max_step_us=%" PRIu64 "\r\n"
        "wifi_readout_max_tick_gap_us=%" PRIu64 "\r\n",
        (unsigned)snapshot.state, (unsigned)snapshot.phase,
        (unsigned)snapshot.rows_total, (unsigned)snapshot.rows_copied,
        snapshot.attempts, snapshot.completed, snapshot.failed, snapshot.cancelled,
        snapshot.step_calls, snapshot.error, (unsigned)snapshot.cancel_reason,
        snapshot.timing_valid ? 1U : 0U, snapshot.counters_saturated ? 1U : 0U,
        snapshot.sampled_us, snapshot.elapsed_us, snapshot.max_step_us, snapshot.max_tick_gap_us);
}

static void print_wifi_dual(void)
{
    cerberus_dual_wifi_diagnostics_t snapshot;
    cerberus_dual_wifi_diagnostics_get(&snapshot);
    const cerberus_dual_wifi_diagnostic_values_t *v = &snapshot.value;
    console_printf("wifi_dual_scope=ui_published_aggregates_no_spi\r\n"
        "wifi_dual_available=%u\r\nwifi_dual_publication_sequence=%" PRIu64 "\r\n"
        "wifi_dual_published_us=%" PRIu64 "\r\nwifi_dual_time_valid=%u\r\n"
        "wifi_dual_sequence_saturated=%u\r\n",
        snapshot.available ? 1U : 0U, snapshot.publication_sequence,
        snapshot.published_us, snapshot.time_valid ? 1U : 0U,
        snapshot.sequence_saturated ? 1U : 0U);
    console_printf("wifi_dual_state=%u\r\nwifi_dual_phase=%u\r\n"
        "wifi_dual_generation=%" PRIu32 "\r\nwifi_dual_retained=%u\r\n"
        "wifi_dual_split=%u\r\nwifi_dual_fallback=%u\r\n"
        "wifi_dual_elapsed_ms=%" PRIu32 "\r\nwifi_dual_cleanup_pending=%u\r\n"
        "wifi_dual_rebinding=%u\r\nwifi_dual_rebind_ready=%u\r\n",
        (unsigned)v->state, (unsigned)v->phase, v->generation, (unsigned)v->count,
        v->split ? 1U : 0U, v->fallback ? 1U : 0U, v->elapsed_ms,
        v->cleanup_pending ? 1U : 0U, v->rebinding ? 1U : 0U, v->rebind_ready ? 1U : 0U);
    console_printf("wifi_dual_rebind_cached=%u\r\n", v->rebind_cached ? 1U : 0U);
    console_printf("wifi_dual_rebind_source=%u\r\n", (unsigned)v->rebind_source);
    console_printf("wifi_dual_s3_count=%u\r\nwifi_dual_s3_total=%u\r\n"
        "wifi_dual_c5_count=%u\r\nwifi_dual_c5_total=%u\r\n"
        "wifi_dual_error=%" PRId32 "\r\nwifi_dual_ui_error=%" PRId32 "\r\n"
        "wifi_dual_s3_error=%" PRId32 "\r\nwifi_dual_c5_error=%" PRId32 "\r\n",
        (unsigned)v->s3_count, (unsigned)v->s3_total, (unsigned)v->c5_count, (unsigned)v->c5_total,
        v->error, v->ui_error, v->s3_error, v->c5_error);
    console_printf("wifi_dual_s3_status_valid=%u\r\nwifi_dual_s3_job=%" PRIu32 "\r\n"
        "wifi_dual_scan_s3_job=%" PRIu32 "\r\nwifi_dual_s3_state=%u\r\n"
        "wifi_dual_s3_wifi_owner=%u\r\nwifi_dual_s3_radio_owned=%u\r\n"
        "wifi_dual_scan_s3_owned=%u\r\nwifi_dual_s3_cleanup_error=%" PRId32 "\r\n"
        "wifi_dual_c5_may_own=%u\r\n",
        v->s3_status_valid ? 1U : 0U, v->s3_job, v->scan_s3_job, (unsigned)v->s3_state,
        (unsigned)v->s3_wifi_owner, v->s3_radio_owned ? 1U : 0U,
        v->scan_s3_owned ? 1U : 0U, v->s3_cleanup_error, v->c5_may_own ? 1U : 0U);
}

static void print_packet_count(void)
{
    wifi_packet_count_diagnostics_t snapshot;
    wifi_packet_count_diagnostics_get(&snapshot);
    console_printf("packet_count_scope=ui_published_scalars_no_radio\r\n"
        "packet_count_available=%u\r\npacket_count_allocated=%u\r\n"
        "packet_count_publication_sequence=%" PRIu64 "\r\npacket_count_published_ms=%" PRIu64 "\r\n"
        "packet_count_time_valid=%u\r\npacket_count_sequence_saturated=%u\r\n",
        snapshot.available ? 1U : 0U, snapshot.allocated ? 1U : 0U,
        snapshot.publication_sequence, snapshot.published_ms,
        snapshot.time_valid ? 1U : 0U, snapshot.sequence_saturated ? 1U : 0U);
    if (!snapshot.available || !snapshot.allocated) return;
    console_printf("packet_count_last_tick_ms=%" PRIu64 "\r\npacket_count_start_pending=%u\r\n"
        "packet_count_requested=%u\r\npacket_count_closing=%u\r\npacket_count_retuning=%u\r\n"
        "packet_count_frozen=%u\r\npacket_count_fallback=%u\r\npacket_count_recording=%u\r\n"
        "packet_count_targets=%u\r\npacket_count_dwell_ms=%u\r\npacket_count_channel_mask=%016" PRIx64 "\r\n",
        snapshot.last_tick_ms, snapshot.start_pending ? 1U : 0U, snapshot.requested ? 1U : 0U,
        snapshot.closing ? 1U : 0U, snapshot.retuning ? 1U : 0U, snapshot.frozen ? 1U : 0U,
        snapshot.fallback ? 1U : 0U, snapshot.recording ? 1U : 0U,
        (unsigned)snapshot.target_count, (unsigned)snapshot.dwell_ms, snapshot.channel_mask);
    console_printf("packet_count_segments=%" PRIu32 "\r\npacket_count_interruptions=%" PRIu32 "\r\n"
        "packet_count_tick_gap_valid=%u\r\npacket_count_tick_gap_ms=%" PRIu64 "\r\n"
        "packet_count_max_tick_gap_ms=%" PRIu64 "\r\n"
        "packet_count_ui_error=%" PRId32 "\r\npacket_count_error=%" PRId32 "\r\n"
        "packet_count_s3_admission_error=%" PRId32 "\r\npacket_count_job=%" PRIu64 "\r\n"
        "packet_count_record_valid=%u\r\n",
        snapshot.segments, snapshot.interruptions, snapshot.tick_gap_valid ? 1U : 0U,
        snapshot.tick_gap_ms, snapshot.max_tick_gap_ms, snapshot.ui_error, snapshot.error,
        snapshot.s3_admission_error, snapshot.job, snapshot.record_valid ? 1U : 0U);
    if (snapshot.record_valid)
        console_printf("packet_count_record_busy=%u\r\npacket_count_record_ready=%u\r\n"
            "packet_count_record_finishing=%u\r\npacket_count_record_error=%" PRId32 "\r\n",
            snapshot.record_busy ? 1U : 0U, snapshot.record_ready ? 1U : 0U,
            snapshot.record_finishing ? 1U : 0U, snapshot.record_error);
    else console_printf("packet_count_record_state=unknown\r\n");
    static const char *const phases[] = {
        "idle", "info", "begin", "targets", "start", "poll", "peek", "read", "final_poll", "release"};
    for (unsigned i = 0; i < WIFI_PACKET_COUNT_SOURCES; ++i) {
        const char *name = i == WIFI_PACKET_COUNT_SOURCE_S3 ? "s3" : "c5";
        const wifi_packet_count_source_diagnostics_t *s = &snapshot.source[i];
        console_printf("packet_count_%s_phase=%u:%s\r\npacket_count_%s_requested_session=%016" PRIx64 "\r\n"
            "packet_count_%s_accepted_session=%016" PRIx64 "\r\npacket_count_%s_channel_mask=%016" PRIx64 "\r\n"
            "packet_count_%s_attempted=%u\r\npacket_count_%s_accepted=%u\r\npacket_count_%s_start_attempted=%u\r\n"
            "packet_count_%s_control_valid=%u\r\n",
            name, (unsigned)s->phase, s->phase < sizeof(phases)/sizeof(phases[0]) ? phases[s->phase] : "unknown",
            name, s->requested_session, name, s->accepted_session, name, s->channel_mask,
            name, s->attempted ? 1U : 0U, name, s->accepted ? 1U : 0U,
            name, s->start_attempted ? 1U : 0U, name, s->control_valid ? 1U : 0U);
        if (s->control_valid)
            console_printf("packet_count_%s_state=%u\r\npacket_count_%s_flags=%02X\r\n"
                "packet_count_%s_rx_active=%u\r\npacket_count_%s_channel=%u\r\n"
                "packet_count_%s_configured=%04X\r\npacket_count_%s_remote_error=%" PRId32 "\r\n",
                name, (unsigned)s->state, name, (unsigned)s->flags,
                name, (s->flags & CERBERUS_PACKET_COUNT_RX_ACTIVE) ? 1U : 0U,
                name, (unsigned)s->channel, name, (unsigned)s->configured, name, s->remote_error);
        else console_printf("packet_count_%s_control=unknown\r\n", name);
        console_printf("packet_count_%s_last_status_ms=%" PRIu64 "\r\npacket_count_%s_next_action_ms=%" PRIu64 "\r\n"
            "packet_count_%s_last_progress_ms=%" PRIu64 "\r\npacket_count_%s_progress_time_valid=%u\r\n"
            "packet_count_%s_pin=%08" PRIx32 "\r\npacket_count_%s_part=%u\r\npacket_count_%s_kind=%u\r\n"
            "packet_count_%s_index=%u\r\npacket_count_%s_upload_index=%u\r\npacket_count_%s_targets=%u\r\n"
            "packet_count_%s_revision_floor=%" PRIu32 "\r\npacket_count_%s_error=%" PRId32 "\r\n",
            name, s->last_status_ms, name, s->next_action_ms, name, s->last_progress_ms,
            name, s->progress_time_valid ? 1U : 0U, name, s->pin, name, (unsigned)s->part,
            name, (unsigned)s->kind, name, (unsigned)s->index, name, (unsigned)s->upload_index,
            name, (unsigned)s->target_count, name, s->revision_floor, name, s->error);
        console_printf("packet_count_%s_stop=%u\r\npacket_count_%s_close=%u\r\npacket_count_%s_faulted=%u\r\n"
            "packet_count_%s_clock_invalid=%u\r\npacket_count_%s_record_bound=%u\r\n"
            "packet_count_%s_published_valid=%u\r\n",
            name, s->stop ? 1U : 0U, name, s->close ? 1U : 0U, name, s->faulted ? 1U : 0U,
            name, s->clock_invalid ? 1U : 0U, name, s->record_bound ? 1U : 0U, name, s->published_valid ? 1U : 0U);
        if (s->published_valid) {
            console_printf("packet_count_%s_published_session=%016" PRIx64 "\r\npacket_count_%s_published_revision=%" PRIu32 "\r\n"
                "packet_count_%s_published_frozen=%u\r\npacket_count_%s_callbacks=%" PRIu32 "\r\n"
                "packet_count_%s_matched=%" PRIu32 "\r\npacket_count_%s_invalid_metadata=%" PRIu32 "\r\n"
                "packet_count_%s_raw_dropped_full=%" PRIu32 "\r\npacket_count_%s_raw_dropped_other=%" PRIu32 "\r\n"
                "packet_count_%s_published_error=%" PRId32 "\r\n",
                name, s->published_session, name, s->published_revision, name, s->published_frozen ? 1U : 0U,
                name, s->callbacks, name, s->matched, name, s->invalid_metadata,
                name, s->raw_dropped_full, name, s->raw_dropped_other, name, s->published_error);
            console_printf("packet_count_%s_valid_frames=%" PRIu32 "\r\n"
                "packet_count_%s_malformed_frames=%" PRIu32 "\r\n"
                "packet_count_%s_unsupported_frames=%" PRIu32 "\r\n"
                "packet_count_%s_outside_window_frames=%" PRIu32 "\r\n"
                "packet_count_%s_clock_dropped_frames=%" PRIu32 "\r\n",
                name, s->valid_frames, name, s->malformed_frames, name, s->unsupported_frames,
                name, s->outside_window_frames, name, s->clock_dropped_frames);
        }
        else console_printf("packet_count_%s_counters=unknown\r\n", name);
        if (snapshot.record_valid)
            console_printf("packet_count_%s_record_session=%016" PRIx64 "\r\npacket_count_%s_written=%" PRIu32 "\r\n"
                "packet_count_%s_discarded=%" PRIu32 "\r\npacket_count_%s_transport_errors=%" PRIu32 "\r\n"
                "packet_count_%s_recorder_bound=%u\r\npacket_count_%s_recorder_draining=%u\r\n"
                "packet_count_%s_recorder_drained=%u\r\npacket_count_%s_record_loss_unknown=%u\r\n"
                "packet_count_%s_record_error=%" PRId32 "\r\n",
                name, s->record_session, name, s->written, name, s->discarded, name, s->transport_errors,
                name, s->recorder_bound ? 1U : 0U, name, s->recorder_draining ? 1U : 0U,
                name, s->recorder_drained ? 1U : 0U, name, s->record_loss_unknown ? 1U : 0U, name, s->record_error);
    }
}

static void print_ble_archive(void)
{
    cerberus_ble_archive_io_status_t snapshot;
    cerberus_ble_archive_io_get_status(&snapshot);
    console_printf("ble_archive_state=%s\r\nble_archive_active=%u\r\n"
        "ble_archive_error=%" PRId32 "\r\nble_archive_job_id=%" PRIu64 "\r\n"
        "ble_archive_operation=%u\r\nble_archive_cancel_requested=%u\r\n"
        "ble_archive_result_ready=%u\r\nble_archive_bytes=%" PRIu32 "\r\n"
        "ble_archive_rows=%u\r\n",
        cerberus_ble_archive_io_state_name(snapshot.state), snapshot.worker_active ? 1U : 0U,
        (int32_t)snapshot.error, snapshot.job_id, (unsigned)snapshot.operation,
        snapshot.cancel_requested ? 1U : 0U, snapshot.result_ready ? 1U : 0U,
        snapshot.bytes, (unsigned)snapshot.count);
    const char *format = snapshot.format == CERBERUS_BLE_ARCHIVE_IO_FORMAT_BLE ? "BLE" :
        snapshot.format == CERBERUS_BLE_ARCHIVE_IO_FORMAT_AIRTAG_JSON ? "AIRTAG_JSON" :
        snapshot.format == CERBERUS_BLE_ARCHIVE_IO_FORMAT_WIFI ? "WIFI_NATIVE" :
        snapshot.format == CERBERUS_BLE_ARCHIVE_IO_FORMAT_AP_JSON ? "AP_JSON" :
        snapshot.format == CERBERUS_BLE_ARCHIVE_IO_FORMAT_SSID_TEXT ? "SSID_TEXT" :
        snapshot.format == CERBERUS_BLE_ARCHIVE_IO_FORMAT_PROBE_REPORT ? "PROBE_REPORT" :
        snapshot.format == CERBERUS_BLE_ARCHIVE_IO_FORMAT_MESH_REPORT ? "MESH_REPORT" :
        snapshot.format == CERBERUS_BLE_ARCHIVE_IO_FORMAT_MESH_LOCATE_REPORT ? "MESH_LOCATE_REPORT" :
        snapshot.format == CERBERUS_BLE_ARCHIVE_IO_FORMAT_MESH_ARCHIVE ? "MESH_ARCHIVE" : "UNKNOWN";
    console_printf("ble_archive_format=%s\r\n", format);
    /* The union belongs to the captured format. Never interpret a different
     * operation's counters as Wi-Fi conversion evidence. */
    if (snapshot.format == CERBERUS_BLE_ARCHIVE_IO_FORMAT_AP_JSON ||
        snapshot.format == CERBERUS_BLE_ARCHIVE_IO_FORMAT_SSID_TEXT) {
        console_printf("ble_archive_wifi_codec_result=%u\r\n"
            "ble_archive_wifi_loss_flags=%" PRIu32 "\r\n"
            "ble_archive_wifi_records=%u\r\nble_archive_wifi_security_unmapped=%u\r\n"
            "ble_archive_wifi_rssi_omitted=%u\r\nble_archive_wifi_names_absent=%u\r\n",
            (unsigned)snapshot.wifi_result, snapshot.wifi_export_stats.loss_flags,
            (unsigned)snapshot.wifi_export_stats.records,
            (unsigned)snapshot.wifi_export_stats.security_unmapped,
            (unsigned)snapshot.wifi_export_stats.rssi_omitted,
            (unsigned)snapshot.wifi_export_stats.names_absent);
    } else if (snapshot.format == CERBERUS_BLE_ARCHIVE_IO_FORMAT_AIRTAG_JSON) {
        console_printf("ble_archive_json_eligible=%u\r\nble_archive_json_filtered=%u\r\n"
            "ble_archive_json_incomplete=%u\r\n", (unsigned)snapshot.export_stats.eligible,
            (unsigned)snapshot.export_stats.filtered, (unsigned)snapshot.export_stats.incomplete);
    }
    /* Each bounded path fits the unchanged formatter capacity independently;
     * their combined maximum plus scalar fields would truncate one call. */
    console_printf("ble_archive_path=%s\r\n", snapshot.relative_path);
    console_printf("ble_archive_file=%s\r\n", snapshot.file_name);
}

static void print_recon_record(void)
{
    cerberus_recon_recorder_status_t snapshot;
    cerberus_recon_recorder_get_status(&snapshot);
    console_printf("recon_record_state=%s\r\nrecon_record_active=%u\r\n"
        "recon_record_error=%" PRId32 "\r\nrecon_record_session=%s\r\n"
        "recon_record_session_id=%" PRIu64 "\r\nrecon_record_started_us=%" PRIu64 "\r\n"
        "recon_record_ended_us=%" PRIu64 "\r\nrecon_record_queued=%u\r\n"
        "recon_record_accepted=%" PRIu32 "\r\nrecon_record_written=%" PRIu32 "\r\n"
        "recon_record_dropped=%" PRIu32 "\r\nrecon_record_invalid=%" PRIu32 "\r\n"
        "recon_record_abandoned=%" PRIu32 "\r\n"
        "recon_record_accepted_wifi_rows=%" PRIu32 "\r\n"
        "recon_record_accepted_ble_rows=%" PRIu32 "\r\n"
        "recon_record_accepted_ibeacon_rows=%" PRIu32 "\r\n"
        "recon_record_written_wifi_rows=%" PRIu32 "\r\n"
        "recon_record_written_ble_rows=%" PRIu32 "\r\n"
        "recon_record_written_ibeacon_rows=%" PRIu32 "\r\n",
        cerberus_recon_recorder_state_name(snapshot.state), snapshot.worker_active ? 1U : 0U,
        (int32_t)snapshot.error, snapshot.session_name, snapshot.session_id,
        snapshot.started_us, snapshot.ended_us, (unsigned)snapshot.queued,
        snapshot.counters.accepted, snapshot.counters.written, snapshot.counters.dropped,
        snapshot.counters.invalid, snapshot.counters.abandoned,
        snapshot.counters.accepted_wifi_rows, snapshot.counters.accepted_ble_rows,
        snapshot.counters.accepted_ibeacon_rows, snapshot.counters.written_wifi_rows,
        snapshot.counters.written_ble_rows, snapshot.counters.written_ibeacon_rows);
}

static void print_wifi_seen_error(const char *stage, esp_err_t error)
{
    console_printf("wifi_seen=unavailable\r\nwifi_seen_stage=%s\r\nwifi_seen_error=%s\r\n",
        stage, esp_err_to_name(error));
}

/* Inventory evidence only. No RF operation, allocation or retained state.
 * Keep one bounded timing array so every row uses the same final S3 time. */
static void print_wifi_seen(void)
{
    cerberus_c5_status_t peer = {0}, peer_after = {0};
    esp_err_t error = c5_spi_link_get_status(&peer);
    if (error != ESP_OK) { print_wifi_seen_error("peer_status", error); return; }
    cerberus_wifi_scan_status_payload_t scan = {0}, after = {0};
    error = c5_spi_link_get_wifi_scan_status(&scan);
    if (error != ESP_OK) { print_wifi_seen_error("scan_status", error); return; }
    if (scan.state != CERBERUS_WIFI_SCAN_COMPLETE || scan.error_code != ESP_OK) {
        console_printf("wifi_seen=not_complete\r\nwifi_seen_state=%u\r\nwifi_seen_radio_error=%" PRId32 "\r\n",
            (unsigned)scan.state, scan.error_code);
        return;
    }
    if (scan.stored_count > CERBERUS_WIFI_SCAN_MAX_RESULTS || scan.stored_count > scan.total_count) {
        print_wifi_seen_error("scan_bounds", ESP_ERR_INVALID_RESPONSE); return;
    }
    cerberus_s3_wifi_observation_t observations[CERBERUS_WIFI_SCAN_MAX_RESULTS] = {0};
    cerberus_c5_wifi_result_t row;
    for (uint8_t i = 0U; i < scan.stored_count; ++i) {
        error = c5_spi_link_get_wifi_scan_observed_result(scan.generation, i, &row, &observations[i]);
        if (error != ESP_OK) { print_wifi_seen_error("result", error); return; }
    }
    error = c5_spi_link_get_wifi_scan_status(&after);
    if (error != ESP_OK) { print_wifi_seen_error("scan_recheck", error); return; }
    if (after.state != CERBERUS_WIFI_SCAN_COMPLETE || after.error_code != ESP_OK ||
        after.generation != scan.generation || after.stored_count != scan.stored_count ||
        after.total_count != scan.total_count) {
        print_wifi_seen_error("snapshot_changed", ESP_ERR_INVALID_STATE); return;
    }
    error = c5_spi_link_get_status(&peer_after);
    if (error != ESP_OK) { print_wifi_seen_error("peer_recheck", error); return; }
    if (peer_after.firmware_version != peer.firmware_version ||
        peer_after.boot_count != peer.boot_count || peer_after.reset_reason != peer.reset_reason ||
        peer_after.uptime_ms < peer.uptime_ms) {
        /* A C5 restart can reuse a scan generation. Uptime wrap conservatively
         * rejects this diagnostic sample too; a subsequent read can succeed. */
        print_wifi_seen_error("peer_changed", ESP_ERR_INVALID_STATE); return;
    }
    int64_t now = esp_timer_get_time();
    if (now < 0) { print_wifi_seen_error("clock", ESP_ERR_INVALID_STATE); return; }
    unsigned available = 0U, recent = 0U, not_recent = 0U, clock_invalid = 0U;
    bool supported = peer.firmware_version >= CERBERUS_WIFI_OBSERVATION_MIN_FIRMWARE;
    for (uint8_t i = 0U; i < scan.stored_count; ++i) {
        const cerberus_s3_wifi_observation_t *observation = &observations[i];
        if (!supported || !observation->valid) continue;
        ++available;
        if ((uint64_t)now < observation->request_started_us) ++clock_invalid;
        else if (cerberus_s3_wifi_observation_recent(observation, (uint64_t)now)) ++recent;
        else ++not_recent;
    }
    console_printf("wifi_seen=completed_snapshot\r\nwifi_seen_firmware=%" PRIu32 "\r\n"
        "wifi_seen_timing=%s\r\nwifi_seen_generation=%u\r\nwifi_seen_total=%u\r\n"
        "wifi_seen_stored=%u\r\nwifi_seen_available=%u\r\nwifi_seen_unavailable=%u\r\n"
        "wifi_seen_recent=%u\r\nwifi_seen_not_recent=%u\r\nwifi_seen_clock_invalid=%u\r\n"
        "wifi_seen_window_ms=%u\r\nwifi_seen_scope=retained_matching_frame_presence\r\n",
        peer.firmware_version, supported ? "supported" : "unavailable_peer_firmware",
        (unsigned)scan.generation, (unsigned)scan.total_count, (unsigned)scan.stored_count,
        available, (unsigned)scan.stored_count - available, recent, not_recent, clock_invalid,
        (unsigned)CERBERUS_S3_WIFI_RECENT_WINDOW_MS);
}

static void print_scan_tools(void)
{
    cerberus_wifi_packet_monitor_status_payload_t monitor = {0};
    esp_err_t error = c5_spi_link_wifi_packet_monitor_status(&monitor);
    console_printf("processor=C5_PACKET_MONITOR\r\ntransport=%s\r\nstate=%u\r\ngeneration=%u\r\nchannel=%u\r\nstored_samples=%u\r\nelapsed_ms=%lu\r\nerror=%ld\r\n",
        esp_err_to_name(error), monitor.state, monitor.generation, monitor.channel,
        monitor.stored_count, (unsigned long)monitor.elapsed_ms, (long)monitor.error_code);
    if (error == ESP_OK && monitor.stored_count != 0U) {
        cerberus_wifi_packet_monitor_sample_payload_t sample = {0};
        error = c5_spi_link_wifi_packet_monitor_sample(monitor.generation,
            monitor.latest_sequence, &sample);
        console_printf("sample_transport=%s\r\nsample_sequence=%u\r\nbeacons=%u\r\ndisconnections=%u\r\nprobe_requests=%u\r\n",
            esp_err_to_name(error), sample.sequence, sample.beacons,
            sample.disconnections, sample.probe_requests);
    }
    cerberus_s3_ble_track_status_t track = {0};
    error = cerberus_s3_ble_scanner_track_status(&track);
    console_printf("processor=S3_FOX_HUNT\r\ntransport=%s\r\nenabled=%u\r\nseen=%u\r\nfresh=%u\r\nreports=%lu\r\nrssi=%d\r\nage_ms=%lu\r\n",
        esp_err_to_name(error), track.enabled, track.seen, track.fresh,
        (unsigned long)track.reports, track.rssi_dbm, (unsigned long)track.age_ms);
    cerberus_ble_analyzer_view_t analyzer = {0};
    error = cerberus_s3_ble_scanner_analyzer_status(&analyzer);
    console_printf("processor=S3_BLE_ANALYZER\r\ntransport=%s\r\nactive=%u\r\nevents=%lu\r\nmalformed=%lu\r\nmeta_hints=%lu\r\nhc_name_hints=%lu\r\nflock_hints=%lu\r\n",
        esp_err_to_name(error), analyzer.active, (unsigned long)analyzer.total_events,
        (unsigned long)analyzer.malformed_reports, (unsigned long)analyzer.meta_hint_events,
        (unsigned long)analyzer.hc_name_hint_events, (unsigned long)analyzer.flock_hint_events);
    cerberus_s3_ble_sound_status_t sound = {0};
    error = cerberus_s3_ble_central_sound_get_status(&sound);
    console_printf("processor=S3_FINDMY_SOUND\r\ntransport=%s\r\nstate=%u\r\nprofile=%u\r\ngeneration=%u\r\nwrite_accepted=%u\r\nresponse_received=%u\r\ncompletion_reported=%u\r\nfailed_stage=%u\r\nerror=%ld\r\nhost_rc=%ld\r\n",
        esp_err_to_name(error), (unsigned)sound.state, (unsigned)sound.profile,
        sound.generation, sound.write_accepted, sound.response_received,
        sound.completion_reported, (unsigned)sound.failed_stage,
        (long)sound.error_code, (long)sound.last_host_rc);
}

/* Each diagnostic pair keeps its field context. These are the unique pairs
 * across a completed sweep, not authenticated device/vendor identities. */
static void print_ble_meta_pairs(const char *source,
    const cerberus_ble_meta_evidence_t *evidence)
{
    static const char *const contexts[] = {
        "MFG16", "LIST16", "LIST32", "LIST128", "DATA16", "DATA32", "DATA128",
    };
    uint8_t count = cerberus_ble_meta_evidence_count(evidence);
    console_printf("meta_%s_unique_pairs=%u\r\n", source, count);
    for (uint8_t ordinal = 0U; ordinal < count; ++ordinal) {
        uint16_t identifier;
        cerberus_ble_meta_context_t context;
        if (cerberus_ble_meta_evidence_at(evidence, ordinal, &identifier, &context))
            console_printf("meta_%s_pair=%04X/%s\r\n", source,
                (unsigned)identifier, contexts[context]);
    }
}

/* Summarize only a completed immutable sweep. Never print a partial mixture if
 * another caller starts scanning while these bounded getter calls run. */
static void print_ble_discovery_hints(const cerberus_ble_scan_status_payload_t *scan)
{
    if (scan->state != CERBERUS_BLE_SCAN_COMPLETE) {
        console_write("findmy_evidence=not_complete\r\nflipper_evidence=not_complete\r\n"
            "meta_evidence=not_complete\r\nibeacon_evidence=not_complete\r\n"
            "flock_evidence=not_complete\r\nhc_evidence=not_complete\r\n");
        return;
    }
    unsigned adv_rows = 0U, response_rows = 0U, mixed_rows = 0U;
    unsigned raw_truncated_rows = 0U, widths = 0U;
    unsigned flipper_adv_rows = 0U, flipper_response_rows = 0U, flipper_mixed_rows = 0U;
    unsigned flipper_named_rows = 0U, flipper_data_rows = 0U, flipper_codes = 0U;
    unsigned flipper_widths = 0U, flipper_raw_truncated_rows = 0U;
    unsigned meta_adv_rows = 0U, meta_response_rows = 0U, meta_filter_rows = 0U;
    unsigned meta_suppressed_rows = 0U, meta_raw_truncated_rows = 0U;
    cerberus_ble_meta_evidence_t meta_adv = {0}, meta_response = {0};
    unsigned ibeacon_rows = 0U, ibeacon_adv_rows = 0U, ibeacon_response_rows = 0U;
    unsigned ibeacon_earlier_rows = 0U;
    unsigned flock_adv_levels[4] = {0}, flock_response_levels[4] = {0};
    unsigned flock_specific_rows = 0U, flock_weak_rows = 0U, flock_oui_rows = 0U;
    unsigned flock_oui_only_rows = 0U;
    const uint8_t hc_codes[] = {CERBERUS_BLE_HC_03, CERBERUS_BLE_HC_05, CERBERUS_BLE_HC_06};
    unsigned hc_adv_counts[3] = {0}, hc_response_counts[3] = {0};
    unsigned hc_rows = 0U, hc_different_name_rows = 0U, hc_raw_truncated_rows = 0U;
    bool coherent = true;
    for (uint8_t i = 0U; i < scan->stored_count &&
         i < CERBERUS_BLE_SCAN_MAX_RESULTS; ++i) {
        cerberus_s3_ble_focus_t focus = {0};
        if (cerberus_s3_ble_scanner_get_focus(scan->generation, i, &focus) != ESP_OK) {
            coherent = false;
            break;
        }
        const cerberus_s3_ble_discovery_evidence_t *e = &focus.discovery;
        uint8_t adv = (e->flags & CERBERUS_S3_BLE_EVIDENCE_ADV_VALID) ?
            e->findmy_advertisement.bits : 0U;
        uint8_t response = (e->flags & CERBERUS_S3_BLE_EVIDENCE_RESPONSE_VALID) ?
            e->findmy_scan_response.bits : 0U;
        uint8_t combined = adv | response;
        adv_rows += adv != 0U;
        response_rows += response != 0U;
        mixed_rows += combined && (combined & (combined - 1U));
        if (combined && !(e->flags & CERBERUS_S3_BLE_EVIDENCE_RAW_COMPLETE))
            ++raw_truncated_rows;
        if (adv) widths |= e->findmy_advertisement.fmna_uuid_widths;
        if (response) widths |= e->findmy_scan_response.fmna_uuid_widths;
        adv = (e->flags & CERBERUS_S3_BLE_EVIDENCE_ADV_VALID) ?
            e->flipper_advertisement.bits : 0U;
        response = (e->flags & CERBERUS_S3_BLE_EVIDENCE_RESPONSE_VALID) ?
            e->flipper_scan_response.bits : 0U;
        combined = adv | response;
        flipper_adv_rows += adv != 0U;
        flipper_response_rows += response != 0U;
        flipper_mixed_rows += combined && (combined & (combined - 1U));
        flipper_codes |= combined;
        if (combined && !(e->flags & CERBERUS_S3_BLE_EVIDENCE_RAW_COMPLETE))
            ++flipper_raw_truncated_rows;
        uint8_t flags = (adv ? e->flipper_advertisement.flags : 0U) |
            (response ? e->flipper_scan_response.flags : 0U);
        flipper_named_rows += (flags & CERBERUS_BLE_FLIPPER_NAME) != 0U;
        flipper_data_rows += (flags & CERBERUS_BLE_FLIPPER_SERVICE_DATA) != 0U;
        flipper_widths |= flags & (CERBERUS_BLE_FLIPPER_UUID16 |
            CERBERUS_BLE_FLIPPER_UUID32 | CERBERUS_BLE_FLIPPER_UUID128);
        bool meta_has_adv = (e->flags & CERBERUS_S3_BLE_EVIDENCE_ADV_VALID) &&
            cerberus_ble_meta_evidence_count(&e->meta_advertisement);
        bool meta_has_response = (e->flags & CERBERUS_S3_BLE_EVIDENCE_RESPONSE_VALID) &&
            cerberus_ble_meta_evidence_count(&e->meta_scan_response);
        meta_adv_rows += meta_has_adv;
        meta_response_rows += meta_has_response;
        meta_filter_rows += (focus.category_bits & CERBERUS_S3_BLE_FOCUS_META_HINT) != 0U;
        meta_suppressed_rows += focus.meta_suppressed;
        if ((meta_has_adv || meta_has_response) &&
            !(e->flags & CERBERUS_S3_BLE_EVIDENCE_RAW_COMPLETE)) ++meta_raw_truncated_rows;
        for (size_t byte = 0U; byte < sizeof(meta_adv.pairs); ++byte) {
            if (meta_has_adv) meta_adv.pairs[byte] |= e->meta_advertisement.pairs[byte];
            if (meta_has_response) meta_response.pairs[byte] |= e->meta_scan_response.pairs[byte];
        }
        if (focus.ibeacon_valid) {
            ++ibeacon_rows;
            if (!cerberus_s3_ble_ibeacon_is_current(&focus)) ++ibeacon_earlier_rows;
            else if (focus.ibeacon_scan_response) ++ibeacon_response_rows;
            else ++ibeacon_adv_rows;
        }
        unsigned flock_adv = (e->flags & CERBERUS_S3_BLE_EVIDENCE_ADV_VALID) ?
            cerberus_ble_flock_hints_level(&e->flock_advertisement) : CERBERUS_BLE_FLOCK_LEVEL_NONE;
        unsigned flock_response = (e->flags & CERBERUS_S3_BLE_EVIDENCE_RESPONSE_VALID) ?
            cerberus_ble_flock_hints_level(&e->flock_scan_response) : CERBERUS_BLE_FLOCK_LEVEL_NONE;
        ++flock_adv_levels[flock_adv];
        ++flock_response_levels[flock_response];
        /* Taking the stronger independently parsed source never combines a
         * name in one packet with manufacturer/service facts in another. */
        unsigned flock_level = flock_adv > flock_response ? flock_adv : flock_response;
        flock_specific_rows += flock_level >= CERBERUS_BLE_FLOCK_LEVEL_SIGNATURE;
        flock_weak_rows += flock_level == CERBERUS_BLE_FLOCK_LEVEL_CLUE ||
            (!flock_level && focus.flock_oui_clue);
        flock_oui_rows += focus.flock_oui_clue;
        flock_oui_only_rows += !flock_level && focus.flock_oui_clue;
        uint8_t hc_adv = (e->flags & CERBERUS_S3_BLE_EVIDENCE_ADV_VALID) ?
            e->hc_advertisement : CERBERUS_BLE_HC_NONE;
        uint8_t hc_response = (e->flags & CERBERUS_S3_BLE_EVIDENCE_RESPONSE_VALID) ?
            e->hc_scan_response : CERBERUS_BLE_HC_NONE;
        bool hc_has_adv = false, hc_has_response = false;
        for (size_t code = 0U; code < sizeof(hc_codes); ++code) {
            hc_adv_counts[code] += hc_adv == hc_codes[code];
            hc_response_counts[code] += hc_response == hc_codes[code];
            hc_has_adv |= hc_adv == hc_codes[code];
            hc_has_response |= hc_response == hc_codes[code];
        }
        hc_rows += hc_has_adv || hc_has_response;
        hc_different_name_rows += hc_has_adv && hc_has_response && hc_adv != hc_response;
        hc_raw_truncated_rows += (hc_has_adv || hc_has_response) &&
            !(e->flags & CERBERUS_S3_BLE_EVIDENCE_RAW_COMPLETE);
    }
    cerberus_ble_scan_status_payload_t after = {0};
    coherent = coherent && cerberus_s3_ble_scanner_get_status(&after) == ESP_OK &&
        after.state == CERBERUS_BLE_SCAN_COMPLETE &&
        after.generation == scan->generation && after.stored_count == scan->stored_count;
    if (!coherent) {
        console_write("findmy_evidence=changed_during_read\r\n"
            "flipper_evidence=changed_during_read\r\nmeta_evidence=changed_during_read\r\n"
            "ibeacon_evidence=changed_during_read\r\nflock_evidence=changed_during_read\r\n"
            "hc_evidence=changed_during_read\r\n");
        return;
    }
    console_printf("findmy_evidence=completed_sweep\r\nfindmy_adv_hint_rows=%u\r\n"
        "findmy_response_hint_rows=%u\r\nfindmy_mixed_hint_rows=%u\r\n"
        "findmy_raw_truncated_hint_rows=%u\r\nfindmy_fmna_uuid_width_bits=%u\r\n",
        adv_rows, response_rows, mixed_rows, raw_truncated_rows, widths);
    console_printf("flipper_evidence=completed_sweep\r\nflipper_adv_hint_rows=%u\r\n"
        "flipper_response_hint_rows=%u\r\nflipper_mixed_hint_rows=%u\r\n"
        "flipper_name_corroborated_rows=%u\r\nflipper_service_data_rows=%u\r\n"
        "flipper_code_bits=%u\r\nflipper_uuid_width_bits=%u\r\n"
        "flipper_raw_truncated_hint_rows=%u\r\n",
        flipper_adv_rows, flipper_response_rows, flipper_mixed_rows,
        flipper_named_rows, flipper_data_rows, flipper_codes, flipper_widths,
        flipper_raw_truncated_rows);
    console_printf("meta_evidence=completed_sweep\r\nmeta_adv_evidence_rows=%u\r\n"
        "meta_response_evidence_rows=%u\r\nmeta_filter_rows=%u\r\n"
        "meta_sweep_suppressed_rows=%u\r\nmeta_raw_truncated_evidence_rows=%u\r\n",
        meta_adv_rows, meta_response_rows, meta_filter_rows, meta_suppressed_rows,
        meta_raw_truncated_rows);
    print_ble_meta_pairs("adv", &meta_adv);
    print_ble_meta_pairs("response", &meta_response);
    console_printf("ibeacon_evidence=completed_sweep\r\nibeacon_format_rows=%u\r\n"
        "ibeacon_current_adv_rows=%u\r\nibeacon_current_response_rows=%u\r\n"
        "ibeacon_earlier_match_rows=%u\r\n", ibeacon_rows, ibeacon_adv_rows,
        ibeacon_response_rows, ibeacon_earlier_rows);
    console_printf("flock_evidence=completed_sweep\r\nflock_specific_signature_rows=%u\r\n"
        "flock_weak_only_rows=%u\r\nflock_public_oui_clue_rows=%u\r\nflock_oui_only_rows=%u\r\n"
        "flock_adv_clue_rows=%u\r\nflock_adv_signature_rows=%u\r\nflock_adv_corroborated_rows=%u\r\n"
        "flock_response_clue_rows=%u\r\nflock_response_signature_rows=%u\r\n"
        "flock_response_corroborated_rows=%u\r\n", flock_specific_rows, flock_weak_rows,
        flock_oui_rows, flock_oui_only_rows, flock_adv_levels[CERBERUS_BLE_FLOCK_LEVEL_CLUE],
        flock_adv_levels[CERBERUS_BLE_FLOCK_LEVEL_SIGNATURE],
        flock_adv_levels[CERBERUS_BLE_FLOCK_LEVEL_CORROBORATED],
        flock_response_levels[CERBERUS_BLE_FLOCK_LEVEL_CLUE],
        flock_response_levels[CERBERUS_BLE_FLOCK_LEVEL_SIGNATURE],
        flock_response_levels[CERBERUS_BLE_FLOCK_LEVEL_CORROBORATED]);
    console_printf("hc_evidence=completed_sweep\r\nhc_name_hint_rows=%u\r\n"
        "hc_different_adv_response_name_rows=%u\r\nhc_raw_truncated_hint_rows=%u\r\n"
        "hc_adv_03_rows=%u\r\nhc_adv_05_rows=%u\r\nhc_adv_06_rows=%u\r\n"
        "hc_response_03_rows=%u\r\nhc_response_05_rows=%u\r\nhc_response_06_rows=%u\r\n",
        hc_rows, hc_different_name_rows, hc_raw_truncated_rows,
        hc_adv_counts[0], hc_adv_counts[1], hc_adv_counts[2],
        hc_response_counts[0], hc_response_counts[1], hc_response_counts[2]);
}

/* BEGIN BLE CENTRAL DIAGNOSTICS: aggregate status only, no peer identifiers. */
static void print_ble_central(void)
{
    cerberus_s3_ble_central_diagnostics_t status = {0};
    esp_err_t error = cerberus_s3_ble_central_get_diagnostics(&status);
    console_printf("processor=S3_BLE_CENTRAL\r\ntransport=%s\r\n", esp_err_to_name(error));
    if (error != ESP_OK) return;
    console_printf("state=%u\r\ngeneration=%u\r\nhost_owner=%u\r\nhost_epoch=%lu\r\nhandle_present=%u\r\nerror=%ld\r\nservice_state=%u\r\ncharacteristic_state=%u\r\nread_state=%u\r\n",
        status.state, status.generation, status.host_owner, (unsigned long)status.host_epoch,
        status.handle_present ? 1U : 0U, (long)status.error_code,
        status.service_state, status.characteristic_state, status.read_state);
}
/* END BLE CENTRAL DIAGNOSTICS. */

/* BEGIN BLE INVENTORY DIAGNOSTICS: completed-bank retention, not RF loss. */
static void print_ble_inventory(const cerberus_ble_scan_status_payload_t *scan)
{
    if (scan->state != CERBERUS_BLE_SCAN_COMPLETE) {
        console_printf("inventory=not_complete\r\n");
        return;
    }
    cerberus_ble_scan_status_payload_t status = {0};
    uint16_t replacements = 0U;
    esp_err_t error = cerberus_s3_ble_scanner_get_inventory_status(scan->generation,
        &status, &replacements);
    console_printf("inventory_transport=%s\r\n", esp_err_to_name(error));
    if (error != ESP_OK) return;
    console_printf("inventory_scope=completed_sweep_retention_not_unique_devices_or_RF_loss\r\ninventory_generation=%u\r\ninventory_stored=%u\r\ninventory_replacements=%u\r\ninventory_rejected_reports=%u\r\ninventory_counter_limit=65535\r\n",
        status.generation, status.stored_count, replacements, status.dropped_reports);
}
/* END BLE INVENTORY DIAGNOSTICS. */

/* BEGIN CSI DIAGNOSTICS: aggregate receive state only, never starts capture. */
static void print_csi(void)
{
    cerberus_csi_info_t info;
    cerberus_csi_observation_t observation;
    esp_err_t error = cerberus_csi_link_diagnostics(&info, &observation);
    console_printf("processor=C5_CSI\r\ntransport=%s\r\n", esp_err_to_name(error));
    if (error != ESP_OK || !observation.status_valid) return;
    console_printf("state=%u\r\nowned=%u\r\nreceiving=%u\r\nchannel=%u\r\nqueued=%u\r\nerror=%ld\r\n",
        observation.status.state, (observation.status.flags & CERBERUS_CSI_FLAG_OWNED) != 0,
        (observation.status.flags & CERBERUS_CSI_FLAG_RECEIVING) != 0,
        observation.status.channel, observation.status.queued, (long)observation.status.error_code);
    console_printf("format=%u\r\ncapacity=%u\r\ntelemetry_valid=%u\r\n",
        info.format, info.capacity, observation.timing_valid ? 1U : 0U);
    if (observation.timing_valid)
        console_printf("accepted=%" PRIu32 "\r\ninvalid=%" PRIu32 "\r\noverflow=%" PRIu32 "\r\nacked=%" PRIu32 "\r\npending=%u\r\n",
            observation.clock.accepted, observation.counters.invalid, observation.counters.overflow,
            observation.counters.acked, observation.clock.pending);
}
/* END CSI DIAGNOSTICS. */

/* BEGIN LIVE BLE DISCOVERY DIAGNOSTICS: aggregate reads never start RF. */
static void print_ble_discovery_live(void)
{
    cerberus_s3_ble_discovery_meta_t meta = {0};
    esp_err_t error = cerberus_s3_ble_scanner_discovery_diagnostics(&meta);
    console_printf("processor=S3_BLE_DISCOVERY\r\ntransport=%s\r\n", esp_err_to_name(error));
    if (error != ESP_OK) return;
    console_printf("attached=%u\r\nowned=%u\r\nstopping=%u\r\nstate=%u\r\nfamily=%u\r\nstored=%u\r\nreplacements=%u\r\nbinding_overflows=%u\r\nerror=%ld\r\n",
        meta.attached, meta.owned, meta.stopping, meta.status.state, (unsigned)meta.family,
        meta.status.stored_count, meta.replacements, meta.binding_overflows,
        (long)meta.status.error_code);
}
/* END LIVE BLE DISCOVERY DIAGNOSTICS. */

/* BEGIN MAC MONITOR DIAGNOSTICS: read-only, without pinning or renewing. */
static void print_mac_monitor(void)
{
    cerberus_mac_info_t info;
    cerberus_mac_status_t status;
    cerberus_mac_counters_t counters;
    esp_err_t error = cerberus_mac_c5_diagnostics(&info, &status, &counters);
    console_printf("processor=C5_MAC\r\ntransport=%s\r\n", esp_err_to_name(error));
    if (error == ESP_OK) {
        console_printf("state=%u\r\nowned=%u\r\nreceiving=%u\r\nchannel=%u\r\nerror=%ld\r\ncapacity=%u\r\nrow_bytes=%u\r\ntotals_valid=%u\r\n",
            status.state, !!(status.flags & CERBERUS_MAC_OWNED), !!(status.flags & CERBERUS_MAC_RECEIVING),
            status.channel, (long)status.error_code, info.capacity, info.row_bytes, status.epoch != 0U);
        console_printf("observations=%" PRIu32 "\r\ninvalid=%" PRIu32 "\r\nreplacements=%" PRIu32 "\r\n",
            counters.observations, counters.invalid, counters.replacements);
    }
    cerberus_s3_ble_scanner_mac_diagnostics(&status, &counters);
    console_printf("processor=S3_MAC\r\nstate=%u\r\nowned=%u\r\nreceiving=%u\r\nerror=%ld\r\nobservations=%" PRIu32 "\r\ninvalid=%" PRIu32 "\r\nreplacements=%" PRIu32 "\r\n",
        status.state, !!(status.flags & CERBERUS_MAC_OWNED), !!(status.flags & CERBERUS_MAC_RECEIVING),
        (long)status.error_code, counters.observations, counters.invalid, counters.replacements);
}
/* END MAC MONITOR DIAGNOSTICS. */

/* BEGIN WIFI RELATION DIAGNOSTICS: metadata only, no rows or RF work. */
static void print_wifi_relations(void)
{
    console_write("processor=C5_WIFI_RELATION\r\n");
    int64_t started = esp_timer_get_time();
    cerberus_wifi_scan_status_payload_t scan = {0};
    esp_err_t error = started < 0 ? ESP_ERR_INVALID_STATE : c5_spi_link_get_wifi_scan_status(&scan);
    cerberus_c5_wifi_relation_bank_t proof = {0};
    if (error == ESP_OK && (scan.state > CERBERUS_WIFI_SCAN_ERROR ||
        scan.stored_count > CERBERUS_WIFI_SCAN_MAX_RESULTS || scan.stored_count > scan.total_count ||
        (scan.state == CERBERUS_WIFI_SCAN_COMPLETE && scan.error_code != ESP_OK)))
        error = ESP_ERR_INVALID_RESPONSE;
    if (error == ESP_OK) error = c5_spi_link_get_wifi_relation_bank(scan.generation,
        (uint32_t)((uint64_t)started / 1000U) + 300U, &proof);
    console_printf("transport=%s\r\n", esp_err_to_name(error));
    if (error != ESP_OK) {
        console_printf("relation_evidence=%s\r\n", error == ESP_ERR_NOT_SUPPORTED ? "unsupported" :
            error == ESP_ERR_NOT_FINISHED ? "not_complete" : "read_error");
        return;
    }
    if (!(proof.status.flags & CERBERUS_WIFI_RELATION_AVAILABLE)) {
        console_write("relation_evidence=unavailable\r\n");
        return;
    }
    console_printf("relation_evidence=completed_bank\r\n"
        "relation_scope=DS_header_pairs_per_channel_not_association\r\n"
        "generation=%u\r\nstored=%u\r\ncapacity=32\r\n"
        "callbacks=%" PRIu32 "\r\ncapacity_drops=%" PRIu32 "\r\noverflow=%u\r\n",
        proof.status.generation, proof.status.count, proof.status.matched_frames,
        proof.status.dropped_frames, !!(proof.status.flags & CERBERUS_WIFI_RELATION_OVERFLOW));
}

static void print_wardrive_relations(const cerberus_wardrive_status_t *status)
{
    console_printf("relation_scope=DS_header_pairs_per_channel_not_association\r\n"
        "relation_state=%u\r\nrelation_observations=%lu\r\nrelation_logged=%lu\r\n"
        "relation_frame_scope=matched_callbacks_in_fully_drained_sweeps\r\n"
        "relation_frames=%lu\r\nrelation_dropped=%lu\r\nrelation_unavailable_sweeps=%lu\r\n",
        (unsigned)status->c5_relation_state,
        (unsigned long)status->c5_relation_observations,
        (unsigned long)status->c5_relation_logged,
        (unsigned long)status->c5_relation_frames,
        (unsigned long)status->c5_relation_dropped,
        (unsigned long)status->c5_relation_unavailable_sweeps);
}
/* END WIFI RELATION DIAGNOSTICS. */

static void print_upload_status(void)
{
    cerberus_log_manager_status_t s;cerberus_log_manager_status(&s);
    console_printf("upload_active=%u\r\nupload_busy=%u\r\nupload_closing=%u\r\nupload_action=%u\r\nupload_service=%u\r\nupload_error=%d\r\nupload_stage=%s\r\nupload_profile_saved=%u\r\n",
        (unsigned)s.active,(unsigned)s.busy,(unsigned)s.closing,(unsigned)s.action,s.service,
        (int)s.error,s.stage,(unsigned)s.profile_saved);
    console_printf("upload_attempted=%u\r\nupload_transport_error=%d\r\nupload_http_status=%u\r\nupload_response_bytes=%lu\r\nupload_response_complete=%u\r\n",
        (unsigned)s.transfer.attempted,(int)s.transfer.error,s.transfer.http_status,
        (unsigned long)s.transfer.response_bytes,(unsigned)s.transfer.response_complete);
    console_printf("upload_socket_errno=%d\r\nupload_tls_error=%d\r\nupload_tls_code=%d\r\nupload_tls_flags=%d\r\n",
        s.transfer.socket_errno,(int)s.transfer.tls_error,s.transfer.tls_code,s.transfer.tls_flags);
    console_printf("upload_log_name=%s\r\n",s.name);
    for(unsigned i=0;i<3;++i)console_printf("upload_destination_%u_result=%u\r\nupload_destination_%u_complete=%u\r\n",
        i,(unsigned)s.destination[i].result,i,(unsigned)s.destination_complete[i]);
}

static void print_upload_profile(void)
{
    cerberus_upload_config_t config={0};
    esp_err_t error=cerberus_upload_settings_load(&config);
    console_printf("upload_profile_error=%d\r\nupload_profile_home_ready=%u\r\nupload_profile_wigle_ready=%u\r\nupload_profile_wdgwars_ready=%u\r\nupload_profile_wardrift_ready=%u\r\nupload_profile_destinations=0x%02x\r\n",
        (int)error,(unsigned)(error==ESP_OK&&cerberus_upload_config_wifi_ready(&config)),
        (unsigned)(error==ESP_OK&&cerberus_upload_config_service_ready(&config,CERBERUS_UPLOAD_WIGLE)),
        (unsigned)(error==ESP_OK&&cerberus_upload_config_service_ready(&config,CERBERUS_UPLOAD_WDGWARS)),
        (unsigned)(error==ESP_OK&&cerberus_upload_config_service_ready(&config,CERBERUS_UPLOAD_WARDRIFT)),
        (unsigned)(error==ESP_OK?config.destinations:0));
    cerberus_upload_config_clear(&config,sizeof(config));
}

static void print_gps(void)
{
    cerberus_wardrive_gps_uart_state_t state={0};
    esp_err_t error=cerberus_wardrive_gps_uart_get_state(&state);
    console_printf("processor=GPS\r\nread_error=%ld\r\nprofile=%u\r\nrx_gpio=%u\r\ntx_assignment=%d\r\npps_assignment=%d\r\n"
        "baud=%lu\r\ndetecting=%u\r\nreader=%u\r\nbytes=%lu\r\nstate=%u\r\nsatellites=%u\r\nfix_age_ms=%lu\r\n"
        "valid_sentences=%lu\r\nbad_checksums=%lu\r\nutc_valid=%u\r\nerror=%ld\r\n",
        (long)error,state.config.profile,state.config.rx_gpio,state.config.tx_gpio,state.config.pps_gpio,
        (unsigned long)state.active_baud,state.detecting_baud,state.reader_running,(unsigned long)state.received_bytes,
        state.gps.state,state.gps.satellites,(unsigned long)state.gps.fix_age_ms,(unsigned long)state.gps.valid_sentences,
        (unsigned long)state.gps.bad_checksums,state.gps.utc_valid,(long)state.error);
}


/* Compact read-only view: never place the ten-row bank on the console stack. */
__attribute__((noinline))
static void print_wardrive_recent(void)
{
    cerberus_wardrive_recent_status_t recent;
    cerberus_wardrive_get_recent_status(&recent);
    console_printf("processor=WARDRIVE_RECENT\r\nstate=%u\r\nworker_active=%u\r\n"
        "available=%u\r\nerror=%ld\r\nview_error=%ld\r\n"
        "session_id=%" PRIu64 "\r\nrevision=%" PRIu64 "\r\nevents=%" PRIu64 "\r\n"
        "rows=%u\r\nretained_events=%u\r\nclock_faults=%" PRIu32 "\r\n"
        "receipt_scope=S3_processing_uptime_not_RF_time\r\n"
        "append_scope=API_accepted_not_SD_durability\r\n",
        (unsigned)recent.state, recent.worker_active ? 1U : 0U,
        recent.available ? 1U : 0U, (long)recent.error, (long)recent.view_error,
        recent.session_id, recent.revision, recent.events,
        (unsigned)recent.count, (unsigned)recent.retained_events, recent.clock_faults);
}

static void print_radio(void)
{
    cerberus_gps_recorder_status_t gps_log;
    cerberus_gps_recorder_status(&gps_log);
    console_printf("processor=GPS_LOG\r\nstate=%u\r\nactive=%u\r\nmode=%u\r\njob=%llu\r\n"
        "fix_ready=%u\r\nwritten=%lu\r\nqueued=%lu\r\nrejected=%lu\r\ndiscarded=%lu\r\n"
        "segments=%lu\r\nbytes=%llu\r\nfile_saved=%u\r\nerror=%ld\r\n",
        (unsigned)gps_log.state,gps_log.busy,(unsigned)gps_log.mode,(unsigned long long)gps_log.job,
        gps_log.fix_ready,(unsigned long)gps_log.written,(unsigned long)gps_log.queued,
        (unsigned long)gps_log.rejected,(unsigned long)gps_log.discarded,(unsigned long)gps_log.track.segments,
        (unsigned long long)gps_log.bytes,gps_log.file_name[0]!=0,(long)gps_log.error);
    print_wifi_relations();
    print_ble_discovery_live();
    print_mac_monitor();
    print_csi();
    print_ble_central();
    for(unsigned processor=0;processor<2;++processor){
        cerberus_radio_test_status_t status={0};esp_err_t err=ESP_OK;
        if(processor==0)err=c5_spi_link_test_status(0,&status);else cerberus_ble_test_status(0,&status);
        console_printf("processor=%s\r\ntransport=%s\r\njob_id=%u\r\nstate=%u\r\nmode=%u\r\nchannel=%u\r\nseconds=%u\r\nelapsed_ms=%lu\r\ndriver_accepted_frames=%lu\r\ncounts_valid=%u\r\nerror=%ld\r\nminimum_heap_kib=%u\r\n",
            processor==0?"C5":"S3",esp_err_to_name(err),status.job_id,status.state,status.mode,status.channel,status.seconds,
            (unsigned long)status.elapsed_ms,(unsigned long)status.accepted_frames,status.counts_valid,(long)status.error_code,status.minimum_heap_kib);
        if(processor==0&&status.mode==CERBERUS_TEST_WIFI_AP)
            console_printf("failure_stage=%u\r\nfailure_stage_name=%s\r\n",
                status.name_length,cerberus_ap_stage_name(status.name_length));
    }
    cerberus_s3_rx_status_t rx;cerberus_s3_rx_status(0,&rx);
    console_printf("processor=S3_RX\r\njob_id=%u\r\nstate=%u\r\nchannel=%u\r\nreserved=%u\r\nelapsed_ms=%lu\r\nobserved_frames=%lu\r\nap_beacons=%lu\r\nmatching_frames=%lu\r\nlast_match_rssi=%d\r\nerror=%ld\r\nminimum_heap_kib=%u\r\n",
        rx.job_id,rx.state,rx.channel,rx.reserved,(unsigned long)rx.elapsed_ms,(unsigned long)rx.observed_frames,
        (unsigned long)rx.target_beacons,(unsigned long)rx.matching_frames,rx.last_match_rssi,(long)rx.error_code,rx.minimum_heap_kib);
    cerberus_ble_scan_status_payload_t ble_scan = {0};
    esp_err_t ble_err = cerberus_s3_ble_scanner_get_status(&ble_scan);
    console_printf("processor=S3_BLE_SCAN\r\ntransport=%s\r\nhost_initialized=%s\r\nhost_synced=%s\r\nhost_owner=%u\r\ngeneration=%u\r\nstate=%u\r\nfound=%u\r\nstored=%u\r\ndropped=%u\r\nerror=%ld\r\n",
        esp_err_to_name(ble_err), cerberus_s3_ble_host_initialized() ? "yes" : "no",
        cerberus_s3_ble_host_synced() ? "yes" : "no",
        (unsigned)cerberus_s3_ble_host_owner(), (unsigned)ble_scan.generation,
        (unsigned)ble_scan.state, (unsigned)ble_scan.total_count,
        (unsigned)ble_scan.stored_count, (unsigned)ble_scan.dropped_reports,
        (long)ble_scan.error_code);
    if (ble_err == ESP_OK) print_ble_discovery_hints(&ble_scan);
    if (ble_err == ESP_OK) print_ble_inventory(&ble_scan);
    cerberus_s3_ble_attack_status_t ble_attack = {0};
    cerberus_s3_ble_attack_get_status(&ble_attack);
    console_printf("processor=S3_BLE_ADV\r\njob_id=%u\r\nstate=%u\r\nmode=%u\r\nelapsed_ms=%lu\r\neffective_interval_units=%u\r\nlast_payload=%u\r\ncontroller_accepted_bursts=%lu\r\ncontroller_rejected_bursts=%lu\r\ncompleted_bursts=%lu\r\nconnections_accepted=%lu\r\nconnections_closed=%lu\r\nconnection_terminate_errors=%lu\r\nconnection_active=%u\r\ncontroller_rc=%ld\r\nerror=%ld\r\nminimum_heap_kib=%u\r\n",
        ble_attack.job_id, (unsigned)ble_attack.state, (unsigned)ble_attack.mode,
        (unsigned long)ble_attack.elapsed_ms,
        (unsigned)ble_attack.effective_interval_units,
        (unsigned)ble_attack.last_payload,
        (unsigned long)ble_attack.controller_accepted_bursts,
        (unsigned long)ble_attack.controller_rejected_bursts,
        (unsigned long)ble_attack.completed_bursts,
        (unsigned long)ble_attack.connections_accepted,
        (unsigned long)ble_attack.connections_closed,
        (unsigned long)ble_attack.connection_terminate_errors,
        ble_attack.connection_active ? 1U : 0U,
        (long)ble_attack.last_controller_rc, (long)ble_attack.error,
        ble_attack.minimum_free_heap_kib);
    print_scan_tools();
    cerberus_wardrive_status_t wardrive = {0};
    cerberus_wardrive_get_status(&wardrive);
    console_printf("processor=WARDRIVE\r\nstate=%u\r\nelapsed_ms=%lu\r\nc5_wifi_sweeps=%lu\r\nc5_status_timeouts=%lu\r\ns3_ble_sweeps=%lu\r\nexpansion_present_mask=%u\r\nsd_rows=%lu\r\nfiles_saved=%lu\r\nerror=%ld\r\nc5_error=%ld\r\ns3_ble_error=%ld\r\nexpansion_error=%ld\r\nsd_error=%ld\r\ngps_requested=%u\r\ngps_error=%ld\r\ngps_state=%u\r\n",
        (unsigned)wardrive.state, (unsigned long)wardrive.elapsed_ms,
        (unsigned long)wardrive.c5_wifi_sweeps,
        (unsigned long)wardrive.c5_wifi_status_timeouts,
        (unsigned long)wardrive.s3_ble_sweeps,
        (unsigned)wardrive.expansion_present_mask, (unsigned long)wardrive.sd_rows,
        (unsigned long)wardrive.files_saved, (long)wardrive.error_code,
        (long)wardrive.c5_error, (long)wardrive.s3_ble_error,
        (long)wardrive.expansion_error, (long)wardrive.sd_error,
        wardrive.gps_requested ? 1U : 0U, (long)wardrive.gps_error,
        (unsigned)wardrive.gps.state);
    console_printf("geotagged_rows=%lu\r\nunlocated_rows=%lu\r\ngps_fallback_rows=%lu\r\n",
        (unsigned long)wardrive.geotagged_rows,
        (unsigned long)wardrive.unlocated_rows,
        (unsigned long)wardrive.gps_fallback_rows);
    console_printf("csv_rows=%lu\r\ncsv_devices=%lu\r\ncompacted_rows=%lu\r\ncompacting=%u\r\n",
        (unsigned long)wardrive.csv_rows, (unsigned long)wardrive.csv_devices,
        (unsigned long)wardrive.compacted_rows, wardrive.compacting ? 1U : 0U);
    console_printf("skipped_no_fix=%lu\r\nskipped_time=%lu\r\nskipped_format=%lu\r\n",
        (unsigned long)wardrive.skipped_no_fix,
        (unsigned long)wardrive.skipped_time,
        (unsigned long)wardrive.skipped_format);
    console_printf("ble_hint_scope=processed_typed_rows_per_sweep_not_unique_devices\r\n"
        "ble_flock_observations=%lu\r\nble_flock_clue=%lu\r\n"
        "ble_flock_signature=%lu\r\nble_flock_corroborated=%lu\r\n",
        (unsigned long)wardrive.s3_ble_flock_observations,
        (unsigned long)wardrive.s3_ble_flock_clue,
        (unsigned long)wardrive.s3_ble_flock_signature,
        (unsigned long)wardrive.s3_ble_flock_corroborated);
    console_printf("probe_scope=matching_transmitter_requested_ssid_rows_not_APs_or_association\r\n"
        "probe_state=%u\r\nprobe_observations=%lu\r\nprobe_logged=%lu\r\n"
        "probe_frame_scope=matched_callbacks_in_fully_drained_sweeps\r\n"
        "probe_frames=%lu\r\nprobe_dropped=%lu\r\nprobe_unavailable_sweeps=%lu\r\n",
        (unsigned)wardrive.c5_probe_state,
        (unsigned long)wardrive.c5_probe_observations,
        (unsigned long)wardrive.c5_probe_logged,
        (unsigned long)wardrive.c5_probe_frames,
        (unsigned long)wardrive.c5_probe_dropped,
        (unsigned long)wardrive.c5_probe_unavailable_sweeps);
    print_wardrive_relations(&wardrive);
    console_printf("wardrive_mode=%u\r\nstream_s3_wifi=%lu\r\nstream_s3_ble=%lu\r\nstream_c5_wifi=%lu\r\nstream_c5_ble=%lu\r\n"
        "stream_s3_pending=%u\r\nstream_c5_pending=%u\r\nstream_s3_drops=%lu\r\nstream_c5_drops=%lu\r\n"
        "stream_s3_channel=%u\r\nstream_c5_channel=%u\r\nstream_s3_channels=%u\r\nstream_c5_channels=%u\r\n"
        "stream_s3_held=%u\r\nstream_c5_held=%u\r\nstream_s3_error=%ld\r\nstream_c5_error=%ld\r\n",
        wardrive.mode,(unsigned long)wardrive.s3_stream.wifi_reports,(unsigned long)wardrive.s3_stream.ble_reports,
        (unsigned long)wardrive.c5_stream.wifi_reports,(unsigned long)wardrive.c5_stream.ble_reports,
        wardrive.s3_stream.pending,wardrive.c5_stream.pending,(unsigned long)wardrive.s3_stream.dropped,
        (unsigned long)wardrive.c5_stream.dropped,wardrive.s3_stream.channel,wardrive.c5_stream.channel,
        wardrive.s3_stream.valid_channels,wardrive.c5_stream.valid_channels,wardrive.s3_stream.held,wardrive.c5_stream.held,
        (long)wardrive.s3_stream.error,(long)wardrive.c5_stream.error);
    print_wardrive_recent();
}

static void print_pinescan(void)
{
    cerberus_s3_pinescan_status_t analysis;
    esp_err_t error = cerberus_s3_pinescan_status(&analysis);
    console_printf("processor=S3_PINESCAN\r\ntransport=%s\r\n", esp_err_to_name(error));
    if (error != ESP_OK) return;
    console_printf("generation=%lu\r\nfollowing=%u\r\ncandidates=%u\r\ncapacity=%u\r\n"
        "valid_beacons=%lu\r\nmatching_beacons=%lu\r\nrejected_frames=%lu\r\n"
        "capacity_drops=%lu\r\nstale_frames=%lu\r\nreceived_24ghz=%lu\r\nreceived_5ghz=%lu\r\n",
        (unsigned long)analysis.generation, analysis.following ? 1U : 0U,
        (unsigned)analysis.candidates, (unsigned)analysis.capacity,
        (unsigned long)analysis.valid_beacons, (unsigned long)analysis.matching_beacons,
        (unsigned long)analysis.rejected_frames, (unsigned long)analysis.capacity_drops,
        (unsigned long)analysis.stale_frames, (unsigned long)analysis.received_24ghz,
        (unsigned long)analysis.received_5ghz);
    cerberus_s3_pinescan_follow_t follow;
    error = cerberus_s3_pinescan_follow_status(&follow);
    if (error == ESP_OK)
        console_printf("follow_valid=%u\r\nfollow_generation=%lu\r\nfollow_beacons=%lu\r\n"
            "last_received_age_ms=%lu\r\nsource_observed_us=%llu\r\n",
            follow.valid ? 1U : 0U, (unsigned long)follow.generation,
            (unsigned long)follow.matching_beacons, (unsigned long)follow.last_received_age_ms,
            (unsigned long long)follow.latest.observed_us);
}

static void print_capture(void)
{
    cerberus_s3_capture_status_t capture;
    cerberus_s3_capture_status(&capture);
    console_printf("processor=S3_CAPTURE\r\nstate=%u\r\nbusy=%u\r\ngeneration=%lu\r\n"
        "hopping=%u\r\nsave_pcap=%u\r\nchannel=%u\r\nmode=%u\r\npending=%u\r\n"
        "received=%lu\r\ndropped_full=%lu\r\ndropped_other=%lu\r\nwritten=%lu\r\n"
        "bytes_written=%llu\r\nelapsed_ms=%lu\r\nerror=%ld\r\nfile=%s\r\n",
        (unsigned)capture.state, capture.busy ? 1U : 0U,
        (unsigned long)capture.generation, capture.hopping ? 1U : 0U,
        capture.save_pcap ? 1U : 0U, (unsigned)capture.channel,
        (unsigned)capture.mode_mask, (unsigned)capture.pending,
        (unsigned long)capture.received, (unsigned long)capture.dropped_full,
        (unsigned long)capture.dropped_other, (unsigned long)capture.written,
        (unsigned long long)capture.bytes_written, (unsigned long)capture.elapsed_ms,
        (long)capture.error_code, capture.file_name);
    cerberus_s3_multissid_status_t analysis;
    esp_err_t error = cerberus_s3_multissid_status(&analysis);
    console_printf("processor=S3_MULTISSID\r\ntransport=%s\r\n", esp_err_to_name(error));
    if (error != ESP_OK) {
        print_pinescan();
        return;
    }
    console_printf("generation=%lu\r\nfollowing=%u\r\ntransmitters=%u\r\nconfirmed=%u\r\n"
        "capacity=%u\r\nvalid_beacons=%lu\r\nrejected_frames=%lu\r\ncapacity_drops=%lu\r\n"
        "stale_frames=%lu\r\nreceived_24ghz=%lu\r\nreceived_5ghz=%lu\r\n",
        (unsigned long)analysis.generation, analysis.following ? 1U : 0U,
        (unsigned)analysis.transmitters, (unsigned)analysis.confirmed,
        (unsigned)analysis.capacity, (unsigned long)analysis.valid_beacons,
        (unsigned long)analysis.rejected_frames, (unsigned long)analysis.capacity_drops,
        (unsigned long)analysis.stale_frames, (unsigned long)analysis.received_24ghz,
        (unsigned long)analysis.received_5ghz);
    cerberus_s3_multissid_follow_t follow;
    error = cerberus_s3_multissid_follow_status(&follow);
    if (error == ESP_OK)
        console_printf("follow_valid=%u\r\nfollow_generation=%lu\r\nmatching_beacons=%lu\r\n"
            "last_received_age_ms=%lu\r\nsource_observed_us=%llu\r\n",
            follow.valid ? 1U : 0U, (unsigned long)follow.generation,
            (unsigned long)follow.matching_beacons, (unsigned long)follow.last_received_age_ms,
            (unsigned long long)follow.latest.observed_us);
    print_pinescan();
}

static void print_remote_id(void)
{
    cerberus_remote_id_diagnostics_t snapshot;
    cerberus_remote_id_diagnostics_get(&snapshot);
    console_printf("remote_id_scope=published_session_aggregates\r\n"
        "remote_id_open=%u\r\nremote_id_running=%u\r\nremote_id_stopping=%u\r\n"
        "remote_id_selected=%u\r\nremote_id_present=%u\r\n",
        snapshot.open ? 1U : 0U, snapshot.running ? 1U : 0U,
        snapshot.stopping ? 1U : 0U, snapshot.selected ? 1U : 0U,
        snapshot.present ? 1U : 0U);
    for (unsigned i = 0U; i < 2U; ++i) {
        const cerberus_remote_id_radio_diagnostics_t *radio = &snapshot.radios[i];
        const char *name = i ? "ble" : "wifi";
        console_printf("remote_id_%s_sweeps=%" PRIu32 "\r\n"
            "remote_id_%s_sources=%u\r\nremote_id_%s_dropped_reports=%u\r\n"
            "remote_id_%s_error=%" PRId32 "\r\nremote_id_%s_owned=%u\r\n",
            name, radio->completed_sweeps, name, (unsigned)radio->retained_sources,
            name, (unsigned)radio->dropped_reports, name, radio->error,
            name, radio->owned ? 1U : 0U);
    }
    console_printf("remote_id_wifi_priority=%u\r\nremote_id_wifi_profile_valid=%u\r\n"
        "remote_id_wifi_job_id=%" PRIu64 "\r\nremote_id_wifi_job_state=%u\r\n"
        "remote_id_wifi_channel=%u\r\nremote_id_wifi_window_elapsed_ms=%" PRIu32 "\r\n"
        "remote_id_wifi_lease_remaining_ms=%u\r\n",
        snapshot.wifi_priority ? 1U : 0U, snapshot.wifi_profile_valid ? 1U : 0U,
        snapshot.wifi_job_id, (unsigned)snapshot.wifi_job_state, (unsigned)snapshot.wifi_channel,
        snapshot.wifi_window_elapsed_ms, (unsigned)snapshot.wifi_lease_remaining_ms);
    console_printf("remote_id_follow_enabled=%u\r\nremote_id_wifi_follow_requested=%u\r\n"
        "remote_id_wifi_follow_bound=%u\r\n",
        (snapshot.wifi_follow_flags & CERBERUS_REMOTE_ID_DIAG_FOLLOW_ENABLED) ? 1U : 0U,
        (snapshot.wifi_follow_flags & CERBERUS_REMOTE_ID_DIAG_FOLLOW_REQUESTED) ? 1U : 0U,
        (snapshot.wifi_follow_flags & CERBERUS_REMOTE_ID_DIAG_FOLLOW_BOUND) ? 1U : 0U);
}

static void execute_command(char *line)
{
    char *start = line;
    while (*start != '\0' && isspace((unsigned char)*start)) {
        ++start;
    }

    char *end = start + strlen(start);
    while (end > start && isspace((unsigned char)end[-1])) {
        --end;
    }
    *end = '\0';
    for (char *cursor = start; *cursor != '\0'; ++cursor) {
        *cursor = (char)tolower((unsigned char)*cursor);
    }

    if (*start == '\0') {
        return;
    }
    if (strcmp(start, "help") == 0) {
        print_help();
    } else if (strcmp(start, "info") == 0) {
        print_info();
    } else if (strcmp(start, "health") == 0) {
        print_health();
    } else if (strcmp(start, "memory") == 0) {
        print_memory();
    } else if (strcmp(start, "c5") == 0) {
        print_c5();
    } else if (strcmp(start, "storage") == 0) {
        print_storage();
    } else if (strcmp(start, "resources") == 0) {
        print_resources();
    } else if (strcmp(start, "capture") == 0) {
        print_capture();
    } else if (strcmp(start, "usb") == 0) {
        print_usb();
    } else if (strcmp(start, "network") == 0) {
        print_network();
    } else if (strcmp(start, "wifi_seen") == 0) {
        print_wifi_seen();
    } else if (strcmp(start, "wifi_readout") == 0) {
        print_wifi_readout();
    } else if (strcmp(start, "wifi_dual") == 0) {
        print_wifi_dual();
    } else if (strcmp(start, "packet_count") == 0) {
        print_packet_count();
    } else if (strcmp(start, "recon_record") == 0) {
        print_recon_record();
    } else if (strcmp(start, "ble_archive") == 0) {
        print_ble_archive();
    } else if (strcmp(start, "remote_id") == 0) {
        print_remote_id();
    } else if (strcmp(start, "stations") == 0) {
        print_stations();
    } else if (strcmp(start, "reports") == 0) {
        print_reports();
    } else if (strcmp(start, "dual_ble") == 0) {
        print_dual_ble();
    } else if (strcmp(start, "ble_memory") == 0) {
        print_ble_memory();
    } else if (strcmp(start, "radio") == 0) {
        print_radio();
    } else if (strcmp(start, "gps") == 0) {
        print_gps();
    } else if (strcmp(start, "upload_profile") == 0) {
        print_upload_profile();
    } else if (strcmp(start, "upload_status") == 0) {
        print_upload_status();
    } else if (strcmp(start, "ward_logs") == 0) {
        print_wardrive_logs();
    } else if (strcmp(start, "battery") == 0) {
        print_battery();
    } else {
        console_write("ERR unknown command; type 'help'\r\n");
    }
}

static void usb_console_task(void *argument)
{
    (void)argument;
    char line[USB_CONSOLE_LINE_CAPACITY];
    size_t line_length = 0U;
    bool discarding_line = false;
    bool session_announced = false;
    bool previous_was_cr = false;

    while (!atomic_load(&console_stop_requested)) {
        if (!usb_serial_jtag_is_connected()) {
            session_announced = false;
            line_length = 0U;
            discarding_line = false;
            previous_was_cr = false;
        }

        uint8_t byte = 0U;
        int received = usb_serial_jtag_read_bytes(&byte, 1U, pdMS_TO_TICKS(50));
        if (received <= 0) {
            continue;
        }

        if (!session_announced) {
            print_banner();
            session_announced = true;
        }

        if (byte == '\r' || byte == '\n') {
            if (byte == '\n' && previous_was_cr) {
                previous_was_cr = false;
                continue;
            }
            previous_was_cr = byte == '\r';
            console_write("\r\n");
            if (discarding_line) {
                console_write("ERR line too long\r\n");
            } else {
                line[line_length] = '\0';
                execute_command(line);
            }
            line_length = 0U;
            discarding_line = false;
            console_write("cerberus> ");
            continue;
        }

        previous_was_cr = false;
        if (byte == '\b' || byte == 0x7fU) {
            if (!discarding_line && line_length > 0U) {
                --line_length;
                console_write("\b \b");
            }
            continue;
        }
        if (byte < 0x20U || byte > 0x7eU) {
            continue;
        }
        if (discarding_line) {
            continue;
        }
        if (line_length + 1U >= sizeof(line)) {
            discarding_line = true;
            continue;
        }
        line[line_length++] = (char)byte;
        char echo[2] = {(char)byte, '\0'};
        console_write(echo);
    }
    /* Publish quiescence only after the last command releases its locks and
     * finishes using the driver. Deleting a task inside a command can strand
     * a storage or driver mutex. No driver calls are allowed after this point. */
    atomic_store(&console_task_stopped, true);
    vTaskDelete(NULL);
}

esp_err_t cerberus_usb_console_start(void)
{
    if (console_task_handle != NULL && !atomic_load(&console_task_stopped)) {
        return ESP_OK;
    }
    console_task_handle = NULL;

    /* No internal-RAM or automatic-array fallback: insufficient PSRAM leaves
     * the optional diagnostic console unavailable without taxing radio memory.
     * A completed task after a prior stop timeout may still own this allocation;
     * its published quiescence makes reuse safe. */
    if (console_output == NULL) {
        console_output = heap_caps_malloc(USB_CONSOLE_OUTPUT_CAPACITY,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (console_output == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    /* TinyUSB leaves the internal PHY routed to OTG after uninstall. Own a
     * native PHY handle while the console is active, releasing it on stop so
     * subsequent USB SD sessions can acquire the same physical USB pads. */
    if (console_phy == NULL) {
        usb_phy_config_t phy_config = {
            .controller = USB_PHY_CTRL_SERIAL_JTAG,
            .target = USB_PHY_TARGET_INT,
        };
        esp_err_t err = usb_new_phy(&phy_config, &console_phy);
        if (err != ESP_OK) {
            heap_caps_free(console_output);
            console_output = NULL;
            return err;
        }
    }

    if (!usb_serial_jtag_is_driver_installed()) {
        usb_serial_jtag_driver_config_t driver_config = {
            .tx_buffer_size = 1024U,
            .rx_buffer_size = 256U,
        };
        esp_err_t err = usb_serial_jtag_driver_install(&driver_config);
        if (err != ESP_OK) {
            (void)usb_del_phy(console_phy);
            console_phy = NULL;
            heap_caps_free(console_output);
            console_output = NULL;
            return err;
        }
    }
    usb_serial_jtag_vfs_use_driver();
    atomic_store(&console_stop_requested, false);
    atomic_store(&console_task_stopped, false);

    BaseType_t created = xTaskCreate(
        usb_console_task,
        "usb_console",
        USB_CONSOLE_TASK_STACK_BYTES,
        NULL,
        USB_CONSOLE_TASK_PRIORITY,
        &console_task_handle);
    if (created != pdPASS) {
        console_task_handle = NULL;
        atomic_store(&console_task_stopped, true);
        (void)cerberus_usb_console_stop();
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "read-only native USB diagnostic console ready");
    return ESP_OK;
}

esp_err_t cerberus_usb_console_stop(void)
{
    if (console_task_handle != NULL) {
        atomic_store(&console_stop_requested, true);
        TickType_t started = xTaskGetTickCount();
        while (!atomic_load(&console_task_stopped)) {
            if ((TickType_t)(xTaskGetTickCount() - started) >= pdMS_TO_TICKS(2000)) {
                return ESP_ERR_TIMEOUT;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        console_task_handle = NULL;
    }

    /* A timed-out stop returns above, preserving the active writer's buffer.
     * Once quiescent, neither the task nor the USB driver borrows these bytes:
     * write_bytes copies them into the driver's TX ring before returning. */
    if (console_output != NULL) {
        heap_caps_free(console_output);
        console_output = NULL;
    }

    /* Secondary ESP-IDF logging also uses this VFS. Detach it before freeing
     * the driver: otherwise a log calls the removed driver, whose own error
     * log recurses through the same VFS until the main task crashes. The VFS
     * setter takes its write lock, allowing any in-flight log to finish. */
    usb_serial_jtag_vfs_use_nonblocking();
    esp_err_t err = ESP_OK;
    if (usb_serial_jtag_is_driver_installed()) {
        (void)usb_serial_jtag_wait_tx_done(pdMS_TO_TICKS(100));
        err = usb_serial_jtag_driver_uninstall();
    }
    if (err == ESP_OK && console_phy != NULL) {
        err = usb_del_phy(console_phy);
        if (err == ESP_OK) console_phy = NULL;
    }
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "native USB diagnostic console stopped");
    }
    return err;
}
