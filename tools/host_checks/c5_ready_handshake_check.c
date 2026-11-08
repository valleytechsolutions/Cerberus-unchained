#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include "c5_spi_link.h"
#include "c5_ready_wait.h"

#define DRAM_ATTR
#define IRAM_ATTR
#define SPI3_HOST 3
#define CERBERUS_ONBOARD_C5_READY_GPIO 44
#define CERBERUS_ONBOARD_C5_SPI_MISO_GPIO 13
#define GPIO_MODE_INPUT 1
#define GPIO_PULLUP_DISABLE 0
#define GPIO_PULLDOWN_ENABLE 1
#define GPIO_INTR_DISABLE 0
#define GPIO_INTR_NEGEDGE 2
#define ESP_INTR_FLAG_IRAM 4
#define pdTRUE 1
#define pdFALSE 0
#define pdMS_TO_TICKS(ms) (ms)
#define portMAX_DELAY UINT32_MAX
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(lock) ((void)(lock))
#define portEXIT_CRITICAL(lock) ((void)(lock))
#define ESP_LOGW(tag,...) ((void)(tag))
#define ESP_LOGE(tag,...) ((void)(tag))
#define ESP_LOGI(tag,...) ((void)(tag))
#define ESP_RETURN_ON_ERROR(expression,tag,...) do { \
    esp_err_t checked = (expression); (void)(tag); if (checked != ESP_OK) return checked; \
} while (0)
typedef int BaseType_t;
typedef unsigned TickType_t;
typedef void *SemaphoreHandle_t;
typedef void *spi_device_handle_t;
typedef int portMUX_TYPE;
typedef struct {unsigned length; const void *tx_buffer; void *rx_buffer;} spi_transaction_t;
typedef struct {uint64_t pin_bit_mask; int mode,pull_up_en,pull_down_en,intr_type;} gpio_config_t;
static int64_t esp_timer_get_time(void);
static void esp_rom_delay_us(uint32_t);
static int gpio_get_level(int);
static esp_err_t spi_device_polling_transmit(spi_device_handle_t,spi_transaction_t *);
static esp_err_t spi_device_polling_start(spi_device_handle_t,spi_transaction_t *,TickType_t);
static esp_err_t spi_device_polling_end(spi_device_handle_t,TickType_t);
static BaseType_t xSemaphoreTake(SemaphoreHandle_t,TickType_t);
static BaseType_t xSemaphoreGive(SemaphoreHandle_t);
static esp_err_t radio_bus_init(void);
static spi_device_handle_t radio_bus_onboard(void);
static SemaphoreHandle_t radio_bus_mutex(void);
static esp_err_t spi_device_get_actual_freq(spi_device_handle_t,int *);
static esp_err_t gpio_pullup_en(int);
static esp_err_t gpio_config(const gpio_config_t *);
static esp_err_t gpio_install_isr_service(int);
static esp_err_t gpio_set_intr_type(int,int);
static esp_err_t gpio_isr_handler_add(int,void(*)(void *),void *);
static esp_err_t gpio_intr_disable(int);
#include "c5_spi_transport_impl.h"
#include "c5_watch_link.h"
#include "c5_wifi_relation_link_impl.h"
#include "c5_wifi_scan_link_impl.h"

static int64_t clock_us, low_at, high_at;
static bool high_level, scheduled, irq_enabled, fail_mutex, held;
static unsigned transfers, requests, releases, install_calls, handler_calls;
static unsigned corrupt_crc, wrong_sequence, wrong_command, wrong_length;
static uint32_t mutex_wait_us;
static unsigned watch_status_calls, corrupt_watch_crc;
static bool watch_unsupported, watch_status_changes, watch_invalid_status;
static bool watch_invalid_chunk, watch_invalid_record, activity_invalid;
static esp_err_t service_error, handler_error, spi_error;
static void (*irq_handler)(void *);
static cerberus_spi_frame_t reply;
static bool passive_model;
static uint8_t passive_ssid_length;
static unsigned fail_spi_at, rollback_transfer;
static uint32_t transfer_cost_us, delayed_low_us, delayed_high_us;
static TickType_t last_mutex_ticks;
static unsigned polling_starts, polling_ends, fail_start_at;
static spi_transaction_t *submitted_transaction;
enum pulse_kind {EARLY, DELAYED, NONE, NO_ISR, STUCK_LOW, REQUEST_ONLY, LEGACY};
static enum pulse_kind pulse;
static bool separate_pulses;
static enum pulse_kind request_pulse, response_pulse;

static void falling(void)
{
    high_level = false;
    if (irq_enabled && irq_handler) irq_handler(NULL);
}
static int64_t esp_timer_get_time(void) { return clock_us; }
static void esp_rom_delay_us(uint32_t us)
{
    clock_us += us;
    if (scheduled && (int32_t)(clock_us - low_at) >= 0 && high_level) falling();
    if (scheduled && (int32_t)(clock_us - high_at) >= 0) {
        high_level = true;
        scheduled = false;
    }
}
static int gpio_get_level(int pin) {assert(pin==44);return high_level;}
static BaseType_t xSemaphoreTake(SemaphoreHandle_t mutex,TickType_t ticks)
{
    assert(mutex==(void *)1&&!held);
    last_mutex_ticks = ticks;
    if (fail_mutex) {clock_us += ticks*1000U;return pdFALSE;}
    clock_us += mutex_wait_us;held=true;return pdTRUE;
}
static BaseType_t xSemaphoreGive(SemaphoreHandle_t mutex)
{assert(mutex==(void *)1&&held);held=false;++releases;return pdTRUE;}
static esp_err_t radio_bus_init(void) {return ESP_OK;}
static spi_device_handle_t radio_bus_onboard(void) {return (void *)2;}
static SemaphoreHandle_t radio_bus_mutex(void) {return (void *)1;}
static esp_err_t spi_device_get_actual_freq(spi_device_handle_t device,int *khz)
{assert(device==(void *)2);*khz=40000;return ESP_OK;}
static esp_err_t gpio_pullup_en(int pin) {assert(pin==13);return ESP_OK;}
static esp_err_t gpio_config(const gpio_config_t *config)
{assert(config->intr_type==GPIO_INTR_DISABLE);return ESP_OK;}
static esp_err_t gpio_install_isr_service(int flags)
{assert(flags==ESP_INTR_FLAG_IRAM);++install_calls;return service_error;}
static esp_err_t gpio_set_intr_type(int pin,int type)
{assert(pin==44&&type==GPIO_INTR_NEGEDGE);return ESP_OK;}
static esp_err_t gpio_isr_handler_add(int pin,void(*handler)(void *),void *arg)
{
    assert(pin==44&&arg==NULL);++handler_calls;
    if (handler_error!=ESP_OK) return handler_error;
    irq_handler=handler;irq_enabled=true;return ESP_OK;
}
static esp_err_t gpio_intr_disable(int pin)
{assert(pin==44);irq_enabled=false;return ESP_OK;}

static size_t model_reply_payload(const cerberus_spi_frame_t *frame, uint8_t *payload)
{
    if (frame->command == CERBERUS_SPI_COMMAND_STATUS) {
        cerberus_status_payload_t value = {
            .ready = 1U, .firmware_version = passive_model ? 60U : CERBERUS_WATCH_MIN_FIRMWARE,
            .capabilities = watch_unsupported ? 0U :
                CERBERUS_C5_CAP_WATCH_OBSERVATIONS | CERBERUS_C5_CAP_BLE_ACTIVITY |
                    (passive_model ? CERBERUS_C5_CAP_WIFI_2_4_GHZ | CERBERUS_C5_CAP_WIFI_5_GHZ : 0U),
        };
        memcpy(payload, &value, sizeof(value)); return sizeof(value);
    }
    if (frame->command == CERBERUS_SPI_COMMAND_WIFI_SCAN_RESULT) {
        cerberus_wifi_scan_result_request_t query;
        assert(passive_model && frame->payload_length == sizeof(query));
        memcpy(&query, frame->payload, sizeof(query));
        assert(query.generation == 7U && query.index == 0U);
        cerberus_wifi_scan_result_payload_t value = {.generation = 7U, .index = 0U,
            .rssi = -44, .channel = 36U, .security = CERBERUS_WIFI_SECURITY_WPA2,
            .ssid_length = passive_ssid_length, .bssid = {2, 3, 4, 5, 6, 7},
            .observation_age_ms = 17U, .observation_source_ms = 91U,
            .observation_flags = CERBERUS_WIFI_OBSERVATION_VALID | CERBERUS_WIFI_OBSERVATION_BEACON,
            .observation_channel = 36U};
        memcpy(payload, &value, sizeof(value)); return sizeof(value);
    }
    if (frame->command == CERBERUS_SPI_COMMAND_WIFI_SCAN_SSID) {
        cerberus_wifi_scan_ssid_request_t query;
        assert(passive_model && frame->payload_length == sizeof(query));
        memcpy(&query, frame->payload, sizeof(query));
        const unsigned offset = query.chunk * CERBERUS_WIFI_SSID_CHUNK_SIZE;
        assert(query.generation == 7U && query.index == 0U && offset < passive_ssid_length);
        unsigned count = passive_ssid_length - offset;
        if (count > CERBERUS_WIFI_SSID_CHUNK_SIZE) count = CERBERUS_WIFI_SSID_CHUNK_SIZE;
        cerberus_wifi_scan_ssid_payload_t value = {7U, 0U, query.chunk, (uint8_t)count, {0}};
        for (unsigned i = 0U; i < count; ++i) value.bytes[i] = (uint8_t)('A' + (offset + i) % 26U);
        memcpy(payload, &value, sizeof(value)); return sizeof(value);
    }
    if (frame->command == CERBERUS_SPI_COMMAND_WIFI_RELATION_STATUS) {
        assert(passive_model && frame->payload_length == 1U && frame->payload[0] == 7U);
        cerberus_wifi_relation_status_t value = {.epoch = UINT64_C(0x100000007), .generation = 7U,
            .state = CERBERUS_WIFI_RELATION_COMPLETE, .flags = CERBERUS_WIFI_RELATION_AVAILABLE};
        memcpy(payload, &value, sizeof(value)); return sizeof(value);
    }
    if (frame->command == CERBERUS_SPI_COMMAND_WIFI_RELATION_CLOCK) {
        cerberus_wifi_relation_clock_query_t query;
        assert(passive_model && frame->payload_length == sizeof(query));
        memcpy(&query, frame->payload, sizeof(query));
        assert(query.epoch == UINT64_C(0x100000007) && query.generation == 7U);
        cerberus_wifi_relation_clock_t value = {.epoch = query.epoch, .source_now_us = 500000U,
            .generation = 7U};
        memcpy(payload, &value, sizeof(value)); return sizeof(value);
    }
    if (frame->command == CERBERUS_SPI_COMMAND_WATCH_SCAN_STATUS) {
        cerberus_watch_scan_request_t query;
        assert(frame->payload_length == sizeof(query));
        memcpy(&query, frame->payload, sizeof(query));
        cerberus_watch_scan_status_payload_t value = {
            .radio = query.radio, .generation = query.generation,
            .state = CERBERUS_WATCH_COMPLETE, .stored_count = 1U,
            .reports = 10U, .duration_ms = 1000U,
        };
        if (watch_invalid_status) ++value.generation;
        if (++watch_status_calls > 1U && watch_status_changes) ++value.reports;
        memcpy(payload, &value, sizeof(value)); return sizeof(value);
    }
    if (frame->command == CERBERUS_SPI_COMMAND_WATCH_SCAN_DATA) {
        cerberus_watch_scan_data_request_t query;
        assert(frame->payload_length == sizeof(query));
        memcpy(&query, frame->payload, sizeof(query));
        cerberus_watch_observation_t record = {
            .address = {2U, 0U, 0U, 0U, 0U, 1U}, .rssi = -60,
            .channel = 8U, .kind = SENTINEL_WATCH_PROTOCOL_PWNAGOTCHI,
            .transport = SENTINEL_WATCH_TRANSPORT_WIFI_BEACON,
            .flags = SENTINEL_WATCH_PROTOCOL_HAS_ID, .id = "node",
        };
        if (watch_invalid_record) record.flags = 0x80U;
        size_t offset = query.chunk * CERBERUS_WATCH_DATA_CHUNK_SIZE;
        assert(offset < sizeof(record));
        size_t count = sizeof(record) - offset;
        if (count > CERBERUS_WATCH_DATA_CHUNK_SIZE) count = CERBERUS_WATCH_DATA_CHUNK_SIZE;
        cerberus_watch_scan_data_payload_t value = {
            .radio = query.radio, .generation = query.generation,
            .index = query.index, .chunk = query.chunk, .byte_count = (uint8_t)count,
        };
        if (watch_invalid_chunk) ++value.generation;
        memcpy(value.bytes, (const uint8_t *)&record + offset, count);
        memcpy(payload, &value, sizeof(value)); return sizeof(value);
    }
    if (frame->command == CERBERUS_SPI_COMMAND_BLE_ACTIVITY) {
        cerberus_watch_scan_request_t query;
        assert(frame->payload_length == sizeof(query));
        memcpy(&query, frame->payload, sizeof(query));
        cerberus_ble_activity_payload_t value = {
            .generation = query.generation, .state = CERBERUS_WATCH_COMPLETE,
            .duration_ms = 1000U, .reports = 12U, .valid_reports = 10U,
            .identities = 3U, .peak_reports_per_second = 5U,
        };
        if (activity_invalid) value.valid_reports = 13U;
        memcpy(payload, &value, sizeof(value)); return sizeof(value);
    }
    cerberus_wifi_scan_status_payload_t value = {.generation = 7,
        .state = passive_model ? CERBERUS_WIFI_SCAN_COMPLETE : CERBERUS_WIFI_SCAN_RUNNING};
    memcpy(payload, &value, sizeof(value)); return sizeof(value);
}

static esp_err_t spi_device_polling_transmit(spi_device_handle_t device,spi_transaction_t *tx)
{
    assert(device==(void *)2&&tx->length==CERBERUS_SPI_TRANSFER_SIZE*8U&&held);
    ++transfers;clock_us += transfer_cost_us;
    if (transfers == rollback_transfer) clock_us -= 100000;
    bool request=tx->tx_buffer!=NULL;
    if (request) {
        ++requests;
        const cerberus_spi_frame_t *frame=tx->tx_buffer;
        assert(cerberus_protocol_frame_is_valid(frame));
        uint8_t value[CERBERUS_SPI_FRAME_PAYLOAD_SIZE] = {0};
        size_t value_size = model_reply_payload(frame, value);
        assert(cerberus_protocol_prepare_frame(&reply,frame->command,frame->sequence,value,value_size));
        if (wrong_sequence) {--wrong_sequence;
            assert(cerberus_protocol_prepare_frame(&reply,frame->command,(uint8_t)(frame->sequence-1U),value,value_size));}
        if (wrong_command) {--wrong_command;
            assert(cerberus_protocol_prepare_frame(&reply,(uint8_t)(frame->command+1U),frame->sequence,value,value_size));}
        if (wrong_length) {--wrong_length;
            assert(cerberus_protocol_prepare_frame(&reply,frame->command,frame->sequence,value,value_size-1U));}
        if (corrupt_crc) {--corrupt_crc;reply.payload[0]^=1U;}
        if (frame->command == CERBERUS_SPI_COMMAND_WATCH_SCAN_STATUS && corrupt_watch_crc) {
            --corrupt_watch_crc; reply.payload[0] ^= 1U;
        }
    } else memcpy(tx->rx_buffer,&reply,sizeof(reply));
    if (transfers == fail_spi_at) return ESP_FAIL;
    if (spi_error!=ESP_OK) return spi_error;
    enum pulse_kind kind = separate_pulses ? (request ? request_pulse : response_pulse) : pulse;
    if (kind==EARLY || (kind==REQUEST_ONLY&&request)) {falling();high_level=true;}
    else if (kind==DELAYED) {scheduled=true;low_at=clock_us+delayed_low_us;high_at=clock_us+delayed_high_us;}
    else if (kind==NO_ISR) high_level=false;
    else if (kind==STUCK_LOW) falling();
    return ESP_OK;
}

static esp_err_t spi_device_polling_start(spi_device_handle_t device,
    spi_transaction_t *transaction, TickType_t wait)
{
    assert(device == (void *)2 && held && !submitted_transaction && wait == portMAX_DELAY);
    ++polling_starts;
    if (polling_starts == fail_start_at) return ESP_FAIL; /* No wire clocks. */
    submitted_transaction = transaction;
    return ESP_OK;
}
static esp_err_t spi_device_polling_end(spi_device_handle_t device, TickType_t wait)
{
    assert(submitted_transaction && held && wait == portMAX_DELAY);
    ++polling_ends;
    esp_err_t error = spi_device_polling_transmit(device, submitted_transaction);
    submitted_transaction = NULL;
    return error;
}

static void reset_case(enum pulse_kind kind)
{
    memset(&transport_diagnostics,0,sizeof(transport_diagnostics));
    atomic_store(&ready_fall_epoch,0U);sequence=0U;mutex_acquired_us=0U;
    clock_us=low_at=high_at=0U;high_level=kind!=LEGACY;scheduled=false;
    fail_mutex=held=false;transfers=requests=releases=0U;mutex_wait_us=0U;
    corrupt_crc=wrong_sequence=wrong_command=wrong_length=0U;spi_error=ESP_OK;pulse=kind;
    watch_status_calls = corrupt_watch_crc = 0U;
    watch_unsupported = watch_status_changes = watch_invalid_status = false;
    watch_invalid_chunk = watch_invalid_record = activity_invalid = false;
    passive_ready_pending = false; passive_ready_epoch = 0U;
    passive_model = separate_pulses = false; passive_ssid_length = 32U;
    peer_firmware_version = 60U; fail_spi_at = rollback_transfer = 0U;
    transfer_cost_us = 3U; delayed_low_us = 40U; delayed_high_us = 80U;
    last_mutex_ticks = 0U;
    assert(!submitted_transaction);
    polling_starts = polling_ends = fail_start_at = 0U;
}
static void scan_ok(void)
{
    cerberus_wifi_scan_status_payload_t status={0};
    assert(c5_spi_link_get_wifi_scan_status(&status)==ESP_OK);
    assert(status.generation==7&&status.state==CERBERUS_WIFI_SCAN_RUNNING&&!held);
}

static bool bytes_zero(const void *value, size_t length)
{
    const uint8_t *bytes = value;
    for (size_t i = 0U; i < length; ++i) if (bytes[i]) return false;
    return true;
}

static void watch_lock_checks(void)
{
    cerberus_watch_scan_status_payload_t status;
    cerberus_watch_observation_t record;
    cerberus_ble_activity_payload_t activity;

    for (unsigned route = 0U; route < 3U; ++route) {
        reset_case(EARLY); fail_mutex = true;
        memset(&status, 0xa5, sizeof(status));
        memset(&record, 0xa5, sizeof(record));
        memset(&activity, 0xa5, sizeof(activity));
        esp_err_t error = route == 0U ? c5_spi_link_get_watch_status(CERBERUS_WATCH_RADIO_WIFI, 7U, &status) :
            route == 1U ? c5_spi_link_get_watch_result(CERBERUS_WATCH_RADIO_WIFI, 7U, 0U, &record) :
            c5_spi_link_get_ble_activity(7U, &activity);
        assert(error == ESP_ERR_TIMEOUT && !held && releases == 0U && transfers == 0U);
        assert(transport_diagnostics.mutex_timeouts == 1U && transport_diagnostics.max_mutex_wait_us == 250000U);
        assert(route == 0U ? bytes_zero(&status, sizeof(status)) :
            route == 1U ? bytes_zero(&record, sizeof(record)) : bytes_zero(&activity, sizeof(activity)));
    }

    reset_case(EARLY); mutex_wait_us = 70000U;
    assert(c5_spi_link_get_watch_status(CERBERUS_WATCH_RADIO_WIFI, 7U, &status) == ESP_OK);
    assert(status.generation == 7U && status.reports == 10U && status.stored_count == 1U);
    assert(requests == 2U && releases == 1U && !held && transport_diagnostics.max_mutex_wait_us == 70000U);
    assert(transport_diagnostics.max_mutex_hold_us == 12U && transport_diagnostics.exchanges == 2U);

    reset_case(EARLY); mutex_wait_us = 80000U;
    assert(c5_spi_link_get_watch_result(CERBERUS_WATCH_RADIO_WIFI, 7U, 0U, &record) == ESP_OK);
    assert(record.channel == 8U && record.rssi == -60 && strcmp(record.id, "node") == 0);
    unsigned result_exchanges = 3U + (sizeof(record) + CERBERUS_WATCH_DATA_CHUNK_SIZE - 1U) / CERBERUS_WATCH_DATA_CHUNK_SIZE;
    assert(requests == result_exchanges && releases == 1U && !held);
    assert(transport_diagnostics.max_mutex_wait_us == 80000U &&
        transport_diagnostics.max_mutex_hold_us == result_exchanges * 6U);

    reset_case(EARLY); mutex_wait_us = 90000U;
    assert(c5_spi_link_get_ble_activity(7U, &activity) == ESP_OK);
    assert(activity.generation == 7U && activity.reports == 12U && activity.valid_reports == 10U);
    assert(releases == 1U && !held && transport_diagnostics.max_mutex_wait_us == 90000U);
    assert(transport_diagnostics.max_mutex_hold_us == 12U);

    for (unsigned route = 0U; route < 3U; ++route) {
        reset_case(EARLY); watch_unsupported = true;
        esp_err_t error = route == 0U ? c5_spi_link_get_watch_status(CERBERUS_WATCH_RADIO_WIFI, 7U, &status) :
            route == 1U ? c5_spi_link_get_watch_result(CERBERUS_WATCH_RADIO_WIFI, 7U, 0U, &record) :
            c5_spi_link_get_ble_activity(7U, &activity);
        assert(error == ESP_ERR_NOT_SUPPORTED && releases == 1U && !held && requests == 1U);
        assert(transport_diagnostics.max_mutex_hold_us == 6U);
        assert(route == 0U ? bytes_zero(&status, sizeof(status)) :
            route == 1U ? bytes_zero(&record, sizeof(record)) : bytes_zero(&activity, sizeof(activity)));
    }

    reset_case(EARLY); watch_invalid_status = true;
    assert(c5_spi_link_get_watch_status(CERBERUS_WATCH_RADIO_WIFI, 7U, &status) == ESP_ERR_INVALID_RESPONSE);
    assert(bytes_zero(&status, sizeof(status)) && !held && releases == 1U);
    reset_case(EARLY); corrupt_watch_crc = 3U;
    assert(c5_spi_link_get_watch_status(CERBERUS_WATCH_RADIO_WIFI, 7U, &status) == ESP_ERR_INVALID_RESPONSE);
    assert(bytes_zero(&status, sizeof(status)) && !held && releases == 1U && requests == 4U);
    for (unsigned failure = 0U; failure < 3U; ++failure) {
        reset_case(EARLY);
        watch_invalid_chunk = failure == 0U;
        watch_status_changes = failure == 1U;
        watch_invalid_record = failure == 2U;
        assert(c5_spi_link_get_watch_result(CERBERUS_WATCH_RADIO_WIFI, 7U, 0U, &record) == ESP_ERR_INVALID_RESPONSE);
        assert(bytes_zero(&record, sizeof(record)) && !held && releases == 1U);
    }
    reset_case(EARLY); activity_invalid = true;
    assert(c5_spi_link_get_ble_activity(7U, &activity) == ESP_ERR_INVALID_RESPONSE);
    assert(bytes_zero(&activity, sizeof(activity)) && !held && releases == 1U);
    reset_case(EARLY);
    assert(c5_spi_link_get_watch_result(CERBERUS_WATCH_RADIO_WIFI, 7U, 1U, &record) == ESP_ERR_INVALID_ARG);
    assert(bytes_zero(&record, sizeof(record)) && !held && releases == 1U && requests == 2U);
    reset_case(EARLY);
    assert(c5_spi_link_get_watch_status(0U, 7U, &status) == ESP_ERR_INVALID_ARG);
    assert(c5_spi_link_get_watch_result(0U, 7U, 0U, &record) == ESP_ERR_INVALID_ARG);
    assert(c5_spi_link_get_ble_activity(7U, NULL) == ESP_ERR_INVALID_ARG);
    assert(!transfers && !releases && !held && !transport_diagnostics.mutex_timeouts);
}

/* Unlike the protocol-only readers, these cases execute production mutex,
 * framing, bounded READY waits and row/fence adapters. Only the SDK boundary,
 * GPIO levels/ISR pulses and monotonic clock are simulated. */
static uint32_t passive_deadline(unsigned ahead_ms)
{ return (uint32_t)((uint64_t)clock_us / 1000U) + ahead_ms; }

static void passive_status_faults(void)
{
    const enum pulse_kind kinds[] = {EARLY, DELAYED, NONE, NO_ISR, STUCK_LOW, REQUEST_ONLY, LEGACY};
    const unsigned expected_transfers[] = {2U, 2U, 1U, 1U, 1U, 2U, 0U};
    for (unsigned i = 0U; i < sizeof(kinds) / sizeof(kinds[0]); ++i) {
        reset_case(kinds[i]); passive_model = true;
        cerberus_wifi_scan_status_payload_t status; memset(&status, 0xa5, sizeof(status));
        const esp_err_t error = c5_spi_link_get_wifi_scan_status_once(1000U, &status);
        assert(transfers == expected_transfers[i] && releases == 1U && !held);
        assert(transport_diagnostics.exchanges == 1U && !transport_diagnostics.legacy_fallbacks);
        assert(last_mutex_ticks == 20U && transport_diagnostics.max_mutex_hold_us == (uint32_t)clock_us);
        if (i < 2U) {
            assert(error == ESP_OK && status.generation == 7U && status.state == CERBERUS_WIFI_SCAN_COMPLETE);
            assert(clock_us == (i ? 166 : 6) && !passive_ready_pending);
        } else {
            assert(error == ESP_ERR_TIMEOUT && bytes_zero(&status, sizeof(status)));
            assert(clock_us == C5_PASSIVE_READY_BUDGET_US && passive_ready_pending == (i != 6U));
            assert(transport_diagnostics.max_exchange_us == C5_PASSIVE_READY_BUDGET_US);
        }
    }
    for (unsigned fault = 0U; fault < 4U; ++fault) {
        reset_case(EARLY); passive_model = true;
        if (!fault) corrupt_crc = 1U;
        if (fault == 1U) wrong_sequence = 1U;
        if (fault == 2U) wrong_command = 1U;
        if (fault == 3U) wrong_length = 1U;
        cerberus_wifi_scan_status_payload_t status; memset(&status, 0xa5, sizeof(status));
        assert(c5_spi_link_get_wifi_scan_status_once(1000U, &status) == ESP_ERR_INVALID_RESPONSE);
        assert(requests == 1U && transfers == 2U && releases == 1U && !held && !passive_ready_pending);
        assert(bytes_zero(&status, sizeof(status)) && !transport_diagnostics.legacy_fallbacks);
        /* A malformed frame is terminal for that call; a fresh call recovers
         * after the completed edge cycle, without a hidden internal retry. */
        assert(c5_spi_link_get_wifi_scan_status_once(1000U, &status) == ESP_OK && requests == 2U);
    }
    for (unsigned after = 0U; after < 2U; ++after) {
        reset_case(EARLY); passive_model = true; fail_spi_at = after + 1U;
        cerberus_wifi_scan_status_payload_t status; memset(&status, 0xa5, sizeof(status));
        assert(c5_spi_link_get_wifi_scan_status_once(1000U, &status) == ESP_FAIL);
        assert(bytes_zero(&status, sizeof(status)) && !held && releases == 1U &&
            transfers == after + 1U && passive_ready_pending && transport_diagnostics.spi_errors == 1U);
    }
    reset_case(REQUEST_ONLY); passive_model = true;
    cerberus_spi_frame_t request, discarded, response;
    assert(cerberus_protocol_prepare_frame(&request, CERBERUS_SPI_COMMAND_WIFI_SCAN_STATUS, 93U, NULL, 0U));
    memset(&discarded, 0xa5, sizeof(discarded)); memset(&response, 0xa5, sizeof(response));
    assert(take_link_mutex(20U) == pdTRUE);
    assert(spi_request_response_passive(&request, &discarded, &response, sizeof(request), 1000U) == ESP_ERR_TIMEOUT);
    give_link_mutex();
    assert(bytes_zero(&discarded, sizeof(discarded)) && bytes_zero(&response, sizeof(response)) &&
        transfers == 2U && passive_ready_pending && !held);
}

static void passive_time_and_lock(void)
{
    cerberus_wifi_scan_status_payload_t status;
    reset_case(EARLY); passive_model = true; c5_spi = NULL;
    memset(&status, 0xa5, sizeof(status));
    assert(c5_spi_link_get_wifi_scan_status_once(1000U, &status) == ESP_ERR_INVALID_STATE);
    assert(bytes_zero(&status, sizeof(status)) && !held && !transfers && releases == 1U &&
        !passive_ready_pending && !polling_starts && !polling_ends);
    c5_spi = (void *)2;
    assert(c5_spi_link_get_wifi_scan_status_once(1000U, &status) == ESP_OK && transfers == 2U && !held);
    reset_case(EARLY); passive_model = true; fail_mutex = true;
    memset(&status, 0xa5, sizeof(status));
    assert(c5_spi_link_get_wifi_scan_status_once(10U, &status) == ESP_ERR_TIMEOUT);
    assert(bytes_zero(&status, sizeof(status)) && !held && !transfers && !releases &&
        last_mutex_ticks == 10U && clock_us == 10000 && transport_diagnostics.mutex_timeouts == 1U);
    reset_case(EARLY); passive_model = true; mutex_wait_us = 10000U;
    assert(c5_spi_link_get_wifi_scan_status_once(10U, &status) == ESP_ERR_TIMEOUT);
    assert(!transfers && releases == 1U && !held && !passive_ready_pending);
    reset_case(EARLY); passive_model = true; mutex_wait_us = 7000U;
    assert(c5_spi_link_get_wifi_scan_status_once(1000U, &status) == ESP_OK);
    assert(clock_us == 7006 && transport_diagnostics.max_mutex_wait_us == 7000U &&
        transport_diagnostics.max_exchange_us == 6U && transport_diagnostics.max_mutex_hold_us == 6U);

    reset_case(LEGACY); passive_model = true; clock_us = 999;
    assert(c5_spi_link_get_wifi_scan_status_once(1U, &status) == ESP_ERR_TIMEOUT);
    assert(clock_us == 1000 && !transfers && !held && releases == 1U && bytes_zero(&status, sizeof(status)));
    reset_case(EARLY); passive_model = true; clock_us = 999;
    assert(c5_spi_link_get_wifi_scan_status_once(1U, &status) == ESP_ERR_TIMEOUT);
    assert(clock_us == 1002 && transfers == 1U && passive_ready_pending && !held);
    reset_case(EARLY); passive_model = true; clock_us = 1000;
    assert(c5_spi_link_get_wifi_scan_status_once(1U, &status) == ESP_ERR_TIMEOUT);
    assert(!transfers && !releases && bytes_zero(&status, sizeof(status)));

    reset_case(DELAYED); passive_model = true; delayed_low_us = 1000; delayed_high_us = 2600;
    assert(c5_spi_link_get_wifi_scan_status_once(1000U, &status) == ESP_ERR_TIMEOUT);
    assert(clock_us == 5000 && transfers == 2U && passive_ready_pending && bytes_zero(&status, sizeof(status)));
    reset_case(DELAYED); passive_model = true; delayed_low_us = 500; delayed_high_us = 1000;
    high_level = false; scheduled = true; low_at = 0; high_at = 1500;
    assert(c5_spi_link_get_wifi_scan_status_once(3U, &status) == ESP_ERR_TIMEOUT);
    assert(clock_us == 3000 && transfers == 2U && passive_ready_pending);
    reset_case(EARLY); passive_model = true; transfer_cost_us = 7000;
    assert(c5_spi_link_get_wifi_scan_status_once(1000U, &status) == ESP_ERR_TIMEOUT);
    assert(clock_us == 7000 && transfers == 1U && passive_ready_pending);
    /* An in-flight SDK call can exceed the software budget. No subsequent
     * transfer is admitted, and no hard physical latency bound is claimed. */
    assert(transport_diagnostics.max_exchange_us == 7000U && !held && bytes_zero(&status, sizeof(status)));

    reset_case(DELAYED); passive_model = true; clock_us = (int64_t)UINT32_MAX - 50;
    atomic_store(&ready_fall_epoch, UINT32_MAX);
    const int64_t start = clock_us;
    assert(c5_spi_link_get_wifi_scan_status_once(passive_deadline(20U), &status) == ESP_OK);
    assert(clock_us - start == 166 && ready_epoch() == 1U && !passive_ready_pending);
    reset_case(DELAYED); passive_model = true;
    clock_us = ((int64_t)UINT32_MAX - 1) * 1000 + 950;
    assert(c5_spi_link_get_wifi_scan_status_once(2U, &status) == ESP_OK && !held);
    reset_case(EARLY); passive_model = true; clock_us = -1;
    assert(c5_spi_link_get_wifi_scan_status_once(10U, &status) == ESP_ERR_INVALID_STATE);
    assert(!transfers && !releases && bytes_zero(&status, sizeof(status)));
    reset_case(EARLY); passive_model = true; clock_us = 1000000; rollback_transfer = 1U;
    assert(c5_spi_link_get_wifi_scan_status_once(2000U, &status) == ESP_ERR_INVALID_STATE);
    assert(transfers == 1U && passive_ready_pending && !held && bytes_zero(&status, sizeof(status)));
}

static void passive_recovery(void)
{
    for (unsigned failed_start = 1U; failed_start <= 2U; ++failed_start) {
        for (unsigned legacy = 0U; legacy < 2U; ++legacy) {
            reset_case(EARLY); passive_model = true; fail_start_at = failed_start;
            cerberus_wifi_scan_status_payload_t status;
            assert(c5_spi_link_get_wifi_scan_status_once(1000U, &status) == ESP_FAIL);
            assert(polling_starts == failed_start && polling_ends == failed_start - 1U &&
                transfers == failed_start - 1U && !passive_ready_pending && !held &&
                bytes_zero(&status, sizeof(status)));
            const unsigned prior_transfers = transfers;
            fail_start_at = 0U;
            assert((legacy ? c5_spi_link_get_wifi_scan_status(&status) :
                c5_spi_link_get_wifi_scan_status_once(1000U, &status)) == ESP_OK);
            assert(transfers == prior_transfers + 2U && !held && !passive_ready_pending &&
                status.generation == 7U && !transport_diagnostics.ready_fall_timeouts);
        }
    }
    for (unsigned failed_end = 1U; failed_end <= 2U; ++failed_end) {
        reset_case(EARLY); passive_model = true; fail_spi_at = failed_end;
        cerberus_wifi_scan_status_payload_t status;
        assert(c5_spi_link_get_wifi_scan_status_once(1000U, &status) == ESP_FAIL);
        assert(polling_starts == failed_end && polling_ends == failed_end && passive_ready_pending);
        const unsigned before = transfers;
        fail_spi_at = 0U;
        assert(c5_spi_link_get_wifi_scan_status_once(1000U, &status) == ESP_ERR_TIMEOUT && transfers == before);
        falling(); high_level = true;
        assert(c5_spi_link_get_wifi_scan_status_once(1000U, &status) == ESP_OK && transfers == before + 2U);
        assert(!held && !passive_ready_pending);
    }
    for (unsigned legacy = 0U; legacy < 2U; ++legacy) {
        reset_case(NONE); passive_model = true;
        atomic_store(&ready_fall_epoch, UINT32_MAX); /* Recovery must survive epoch wrap. */
        cerberus_wifi_scan_status_payload_t status;
        assert(c5_spi_link_get_wifi_scan_status_once(1000U, &status) == ESP_ERR_TIMEOUT);
        assert(requests == 1U && transfers == 1U && passive_ready_pending && high_level);
        unsigned old_requests = requests, old_transfers = transfers;
        if (legacy) assert(c5_spi_link_get_wifi_scan_status(&status) == ESP_ERR_INVALID_RESPONSE);
        else assert(c5_spi_link_get_wifi_scan_status_once(1000U, &status) == ESP_ERR_TIMEOUT);
        /* Stale high cannot authorize a request; no new clocks or legacy
         * fallback are allowed while the exact previous edge is absent. */
        assert(requests == old_requests && transfers == old_transfers && !held && passive_ready_pending &&
            !transport_diagnostics.legacy_fallbacks && bytes_zero(&status, sizeof(status)));
        falling(); high_level = true; pulse = EARLY;
        assert((legacy ? c5_spi_link_get_wifi_scan_status(&status) :
            c5_spi_link_get_wifi_scan_status_once(1000U, &status)) == ESP_OK);
        assert(!passive_ready_pending && !held && requests == old_requests + 1U && transfers == old_transfers + 2U);
    }
    reset_case(REQUEST_ONLY); passive_model = true;
    cerberus_wifi_scan_status_payload_t status;
    assert(c5_spi_link_get_wifi_scan_status_once(1000U, &status) == ESP_ERR_TIMEOUT);
    assert(transfers == 2U && passive_ready_pending && ready_epoch() == 1U);
    assert(c5_spi_link_get_wifi_scan_status_once(1000U, &status) == ESP_ERR_TIMEOUT && transfers == 2U);
    falling(); high_level = true; pulse = EARLY;
    assert(c5_spi_link_get_wifi_scan_status_once(1000U, &status) == ESP_OK && transfers == 4U);
    assert(!passive_ready_pending && ready_epoch() == 4U);
    /* Recovery itself shares the next call's budget with its request/response. */
    reset_case(NONE); passive_model = true;
    assert(c5_spi_link_get_wifi_scan_status_once(1000U, &status) == ESP_ERR_TIMEOUT);
    const int64_t began = clock_us;
    scheduled = true; low_at = clock_us + 3000; high_at = clock_us + 4000;
    pulse = DELAYED; delayed_low_us = 500; delayed_high_us = 1000;
    assert(c5_spi_link_get_wifi_scan_status_once(1000U, &status) == ESP_ERR_TIMEOUT);
    assert(clock_us - began == 5000 && transfers == 2U && requests == 2U && passive_ready_pending && !held);
}

static void passive_reader_routes(void)
{
    for (unsigned length = 0U; length <= 32U; ++length) {
        reset_case(EARLY); passive_model = true; passive_ssid_length = (uint8_t)length;
        cerberus_c5_wifi_scan_reader_t reader;
        cerberus_c5_wifi_result_t row;
        cerberus_s3_wifi_observation_t observation;
        bool complete = false;
        assert(c5_spi_link_wifi_scan_result_begin(&reader, 7U, 0U, 1000U) == ESP_OK);
        while (!complete) {
            const unsigned before = requests;
            memset(&row, 0xa5, sizeof(row)); memset(&observation, 0xa5, sizeof(observation));
            assert(c5_spi_link_wifi_scan_result_step(&reader, &row, &observation, &complete) == ESP_OK);
            assert(requests == before + 1U && !held && !passive_ready_pending);
            if (!complete) assert(bytes_zero(&row, sizeof(row)) && bytes_zero(&observation, sizeof(observation)));
        }
        assert(requests == 1U + (length + 17U) / 18U && releases == requests && row.ssid[length] == 0 &&
            observation.valid && observation.request_started_us == 0U && observation.source_age_ms == 17U);
        for (unsigned i = 0U; i < length; ++i) assert(row.ssid[i] == (char)('A' + i % 26U));
    }
    for (unsigned fault_step = 1U; fault_step <= 3U; ++fault_step) {
        reset_case(EARLY); passive_model = true;
        cerberus_c5_wifi_scan_reader_t reader;
        cerberus_c5_wifi_result_t row;
        cerberus_s3_wifi_observation_t observation;
        bool complete = false;
        assert(c5_spi_link_wifi_scan_result_begin(&reader, 7U, 0U, 1000U) == ESP_OK);
        for (unsigned step = 1U; step <= fault_step; ++step) {
            if (step == fault_step) pulse = NONE;
            const esp_err_t error = c5_spi_link_wifi_scan_result_step(&reader, &row, &observation, &complete);
            assert(error == (step == fault_step ? ESP_ERR_TIMEOUT : ESP_OK));
        }
        assert(requests == fault_step && !complete && !held && bytes_zero(&row, sizeof(row)) &&
            bytes_zero(&observation, sizeof(observation)) && bytes_zero(&reader.pending, sizeof(reader.pending)));
        assert(c5_spi_link_wifi_scan_result_step(&reader, &row, &observation, &complete) == ESP_ERR_TIMEOUT &&
            requests == fault_step);
        falling(); high_level = true; pulse = EARLY;
        assert(c5_spi_link_wifi_scan_result_begin(&reader, 7U, 0U, 1000U) == ESP_OK);
        while (!complete)
            assert(c5_spi_link_wifi_scan_result_step(&reader, &row, &observation, &complete) == ESP_OK);
        assert(requests == fault_step + 3U && !held && !passive_ready_pending && row.ssid[32] == 0 && observation.valid);
    }
    for (unsigned fault_step = 0U; fault_step <= 6U; ++fault_step) {
        reset_case(EARLY); passive_model = true;
        cerberus_c5_wifi_relation_reader_t reader;
        cerberus_c5_wifi_relation_bank_t proof;
        bool complete = false;
        assert(c5_spi_link_wifi_relation_begin_check(&reader, 7U, 1000U) == ESP_OK && !reader.legacy_exchange);
        unsigned step = 0U;
        while (!complete) {
            ++step; assert(step <= 6U);
            if (step == fault_step) pulse = NONE;
            memset(&proof, 0xa5, sizeof(proof));
            const esp_err_t error = c5_spi_link_wifi_relation_step(&reader, &proof, &complete, NULL, NULL);
            assert(requests == step && !held && releases == requests);
            if (step == fault_step) {
                assert(error == ESP_ERR_TIMEOUT && !complete && bytes_zero(&proof, sizeof(proof)) &&
                    bytes_zero(&reader.pending, sizeof(reader.pending)));
                assert(c5_spi_link_wifi_relation_step(&reader, &proof, &complete, NULL, NULL) == error && requests == step);
                break;
            }
            assert(error == ESP_OK);
            if (!complete) assert(bytes_zero(&proof, sizeof(proof)));
        }
        if (!fault_step) {
            assert(complete && step == 6U && proof.status.epoch == UINT64_C(0x100000007) &&
                proof.clock_valid && proof.request_started_us == 0U);
            const cerberus_c5_wifi_relation_bank_t original = proof;
            clock_us += 20000;
            assert(c5_spi_link_wifi_relation_begin_check_expected(&reader, &original, 1000U) == ESP_OK);
            complete = false;
            while (!complete) assert(c5_spi_link_wifi_relation_step(&reader, &proof, &complete, NULL, NULL) == ESP_OK);
            assert(requests == 12U && proof.request_started_us == original.request_started_us &&
                proof.source_now_us == original.source_now_us && !held && !passive_ready_pending);
        }
    }
    /* Blocking worker wrappers deliberately retain legacy timing/compatibility;
     * the six exchanges succeed here even with no READY edges. */
    for (unsigned copy = 0U; copy < 2U; ++copy) {
        reset_case(NONE); passive_model = true;
        cerberus_c5_wifi_relation_bank_t proof;
        const esp_err_t error = copy ? c5_spi_link_copy_wifi_relations(7U, 2000U, &proof, NULL, 0U, NULL, NULL) :
            c5_spi_link_get_wifi_relation_bank(7U, 2000U, &proof);
        assert(error == ESP_OK && requests == 6U && !held && !passive_ready_pending &&
            !polling_starts && !polling_ends && transport_diagnostics.legacy_fallbacks == 6U);
        assert(proof.status.epoch == UINT64_C(0x100000007) && clock_us == 6 * 110006);
    }
}

int main(void)
{
    service_error=ESP_ERR_NO_MEM;
    assert(c5_spi_link_init()==ESP_ERR_NO_MEM&&!link_mutex&&handler_calls==0U);
    service_error=ESP_ERR_INVALID_STATE;handler_error=ESP_FAIL;
    assert(c5_spi_link_init()==ESP_FAIL&&!link_mutex&&!irq_enabled);
    handler_error=ESP_OK;
    assert(c5_spi_link_init()==ESP_OK&&link_mutex&&irq_enabled);
    unsigned installs=install_calls;assert(c5_spi_link_init()==ESP_OK&&install_calls==installs);

    reset_case(EARLY);scan_ok();scan_ok();
    assert(transfers==4U&&requests==2U&&clock_us==12U);
    assert(transport_diagnostics.ready_latched_pulses==4U);
    assert(!transport_diagnostics.ready_fall_timeouts&&!transport_diagnostics.legacy_fallbacks);
    assert(transport_diagnostics.max_exchange_us==6U&&transport_diagnostics.max_mutex_hold_us==6U);

    reset_case(DELAYED);scan_ok();
    assert(clock_us==166U&&!transport_diagnostics.ready_latched_pulses);
    assert(!transport_diagnostics.ready_fall_timeouts);
    reset_case(REQUEST_ONLY);scan_ok();
    assert(transport_diagnostics.ready_fall_timeouts==1U&&clock_us==100006U);
    /* A request edge cannot satisfy the response transfer's new baseline. */
    assert(transport_diagnostics.ready_latched_pulses==1U);

    reset_case(NONE);scan_ok();
    assert(clock_us==110006U&&transport_diagnostics.legacy_fallbacks==1U);
    assert(transport_diagnostics.ready_fall_timeouts==1U);
    reset_case(NO_ISR);scan_ok();
    assert(transport_diagnostics.ready_fall_timeouts==1U);
    /* Merely low, with its ISR still pending, must never count as consumed. */
    reset_case(STUCK_LOW);scan_ok();
    assert(transport_diagnostics.ready_high_timeouts==1U&&clock_us==110006U);
    reset_case(LEGACY);scan_ok();
    assert(clock_us==110006U&&transport_diagnostics.ready_initial_timeouts==1U);

    reset_case(EARLY);wrong_sequence=1U;scan_ok();assert(requests==2U&&sequence==2U);
    reset_case(EARLY);corrupt_crc=1U;scan_ok();assert(requests==2U&&sequence==2U);
    reset_case(EARLY);wrong_command=1U;scan_ok();assert(requests==2U&&sequence==2U);
    reset_case(EARLY);wrong_length=1U;scan_ok();assert(requests==2U&&sequence==2U);
    reset_case(EARLY);wrong_sequence=3U;
    cerberus_wifi_scan_status_payload_t status={0};
    assert(c5_spi_link_get_wifi_scan_status(&status)==ESP_ERR_INVALID_RESPONSE);
    assert(requests==3U&&sequence==3U&&!held&&releases==1U);
    reset_case(EARLY);spi_error=ESP_FAIL;
    assert(c5_spi_link_get_wifi_scan_status(&status)==ESP_ERR_INVALID_RESPONSE);
    assert(transport_diagnostics.spi_errors==6U&&!held);

    reset_case(EARLY);fail_mutex=true;
    assert(c5_spi_link_get_wifi_scan_status(&status)==ESP_ERR_TIMEOUT);
    assert(!transfers&&transport_diagnostics.mutex_timeouts==1U);
    assert(transport_diagnostics.max_mutex_wait_us==100000U);
    reset_case(EARLY);mutex_wait_us=70000U;scan_ok();
    assert(transport_diagnostics.max_mutex_wait_us==70000U);
    assert(transport_diagnostics.max_mutex_hold_us==6U);

    reset_case(DELAYED);clock_us=UINT32_MAX-50U;
    atomic_store(&ready_fall_epoch,UINT32_MAX);scan_ok();
    assert(ready_epoch()==1U&&transport_diagnostics.max_exchange_us==166U);
    cerberus_c5_transport_diagnostics_t snapshot={0};
    unsigned before=transfers;c5_spi_link_get_transport_diagnostics(&snapshot);
    assert(transfers==before&&snapshot.ready_fall_edges==1U&&snapshot.exchanges==1U);
    watch_lock_checks();
    passive_status_faults();
    passive_time_and_lock();
    passive_recovery();
    passive_reader_routes();
    puts("C5 READY: pulses/faults/framing/metrics/init/wrap; Watch status/chunks/activity lock metrics, release paths and immutable validated snapshots passed.");
    puts("Passive AP READY: actual row/status/fence routes, shared 5ms readiness budget, absolute deadlines, pending-edge recovery, no stale output/retry and common-path isolation passed; SDK calls are not preemptible.");
    return 0;
}
