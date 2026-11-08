#include "ieee802154_scanner.h"
#include "radio_test_service.h"

#include "ble_scanner.h"
#include "wifi_scanner.h"

#include "esp_attr.h"
#include "esp_ieee802154.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define IEEE802154_SCAN_TASK_STACK 4096U
#define IEEE802154_SCAN_TASK_PRIORITY 4U
#define IEEE802154_ENERGY_DURATION_SYMBOLS 3125U
#define IEEE802154_ENERGY_TIMEOUT_MS 500U
#define IEEE802154_BEACON_DWELL_MS 1000U
#define IEEE802154_BEACON_QUEUE_LENGTH 8U
#define IEEE802154_ACTIVITY_DWELL_MS 1000U

static const char *TAG = "ieee802154_scan";
static SemaphoreHandle_t scanner_mutex;
static TaskHandle_t scanner_task_handle;
static QueueHandle_t beacon_queue;
static cerberus_ieee802154_energy_scan_status_payload_t scan_status;
static int8_t scan_power[CERBERUS_IEEE802154_CHANNEL_COUNT];
static cerberus_ieee802154_beacon_scan_status_payload_t beacon_status;
static cerberus_ieee802154_beacon_scan_result_payload_t
    beacon_results[CERBERUS_IEEE802154_BEACON_MAX_RESULTS];
static volatile bool beacon_scan_active;
static cerberus_ieee802154_activity_scan_status_payload_t activity_status;
static volatile uint8_t activity_counts[CERBERUS_IEEE802154_CHANNEL_COUNT]
                                       [CERBERUS_IEEE802154_ACTIVITY_FRAME_TYPE_COUNT];
static volatile uint32_t activity_total_frames;
static volatile bool activity_scan_active;
static volatile bool energy_done;
static volatile bool energy_failed;
static volatile int8_t energy_power;
static volatile uint16_t energy_failure_code;

/* Bulk queue/snapshot storage is optional; fixed pointer/token/lock scalars are
 * measured separately. The single existing
 * worker owns SDK operations and disposal. Lock order: scanner_mutex, mesh_mux.
 * ISR sees the pointer and copies its complete record under mesh_mux only. */
typedef struct {
    uint32_t sequence;
    cerberus_mesh_observation_record_t record;
} mesh_observation_slot_t;
typedef struct {
    mesh_observation_slot_t slots[CERBERUS_MESH_OBSERVATION_CAPACITY];
    cerberus_mesh_observation_summary_t summary;
    cerberus_mesh_observation_summary_t pinned_summary;
    uint64_t lease_us,retention_us,clock_high_us,dwell_started_us;
    uint32_t summary_pin,summary_pin_counter,last_acked;
    uint8_t head,count,rx_channel,phase,next_channel,state;
    bool cancelled,release_requested,retiring,sdk_attempted;
    esp_err_t primary_error,cleanup_error;
} mesh_observation_context_t;
static portMUX_TYPE mesh_mux=portMUX_INITIALIZER_UNLOCKED;
static mesh_observation_context_t *mesh_context;
static uint64_t mesh_epoch,mesh_session;
static cerberus_mesh_observation_start_t mesh_request;
enum { MESH_SETUP=0,MESH_TUNE,MESH_DWELL,MESH_CLEANUP,MESH_STOPPED };

#include "ieee802154_locate_impl.h"

static uint32_t IRAM_ATTR mesh_increment(uint32_t value,mesh_observation_context_t *c)
{
    if(value<UINT32_MAX)++value;
    if(value==UINT32_MAX)c->summary.flags|=CERBERUS_MESH_OBSERVATION_COUNTERS_SATURATED;
    return value;
}
static void IRAM_ATTR mesh_receive(uint8_t *frame,const esp_ieee802154_frame_info_t *info)
{
    portENTER_CRITICAL_ISR(&mesh_mux);
    mesh_observation_context_t *c=mesh_context;
    if(c&&c->rx_channel&&!c->cancelled){
        c->summary.callbacks=mesh_increment(c->summary.callbacks,c);
        if(!frame||!info||frame[0]<4U||frame[0]>127U||info->channel!=c->rx_channel){
            c->summary.invalid_callbacks=mesh_increment(c->summary.invalid_callbacks,c);
        }else if(c->count==CERBERUS_MESH_OBSERVATION_CAPACITY){
            c->summary.queue_drops=mesh_increment(c->summary.queue_drops,c);
        }else if(c->summary.last_sequence==CERBERUS_MESH_OBSERVATION_SEQUENCE_MAX){
            c->summary.flags|=CERBERUS_MESH_OBSERVATION_SEQUENCE_EXHAUSTED;
            c->cancelled=true;c->rx_channel=0U;
            if(c->primary_error==ESP_OK)c->primary_error=ESP_ERR_INVALID_STATE;
            c->summary.invalid_callbacks=mesh_increment(c->summary.invalid_callbacks,c);
        }else{
            c->summary.tuned_mask|=(uint16_t)(1U<<(info->channel-11U));
            c->summary.channel=info->channel;
            uint8_t index=(uint8_t)((c->head+c->count)%CERBERUS_MESH_OBSERVATION_CAPACITY);
            mesh_observation_slot_t *slot=&c->slots[index];
            memset(slot,0,sizeof(*slot));
            slot->sequence=++c->summary.last_sequence;
            slot->record.channel=info->channel;slot->record.rssi=info->rssi;slot->record.lqi=info->lqi;
            slot->record.mac_length=(uint8_t)(frame[0]-2U);
            slot->record.flags=CERBERUS_MESH_OBSERVATION_RSSI_VALID|CERBERUS_MESH_OBSERVATION_LQI_VALID;
            int64_t now=esp_timer_get_time();
            if(now>=0&&(uint64_t)now>=c->clock_high_us&&
                !(c->summary.flags&CERBERUS_MESH_OBSERVATION_SUMMARY_CLOCK_INVALID)){
                slot->record.callback_us=(uint64_t)now;
                slot->record.flags|=CERBERUS_MESH_OBSERVATION_TIME_VALID;
                c->clock_high_us=(uint64_t)now;
                /* A callback can occur inside receive() before it returns. Its
                 * observed source time is the first proven acquisition time. */
                if(!(c->summary.flags&CERBERUS_MESH_OBSERVATION_START_TIME_VALID)){
                    c->summary.started_us=(uint64_t)now;
                    c->summary.flags|=CERBERUS_MESH_OBSERVATION_START_TIME_VALID;
                }
            }else{
                c->summary.flags|=CERBERUS_MESH_OBSERVATION_SUMMARY_CLOCK_INVALID;
                c->cancelled=true;c->rx_channel=0U;
                if(c->primary_error==ESP_OK)c->primary_error=ESP_ERR_INVALID_STATE;
            }
            memcpy(slot->record.mac,frame+1U,slot->record.mac_length);
            ++c->count;c->summary.enqueued=mesh_increment(c->summary.enqueued,c);
            if(c->summary.last_sequence==CERBERUS_MESH_OBSERVATION_SEQUENCE_MAX){
                c->summary.flags|=CERBERUS_MESH_OBSERVATION_SEQUENCE_EXHAUSTED;
                c->cancelled=true;c->rx_channel=0U;
                if(c->primary_error==ESP_OK)c->primary_error=ESP_ERR_INVALID_STATE;
            }
        }
    }
    portEXIT_CRITICAL_ISR(&mesh_mux);
}

static bool IRAM_ATTR parse_zigbee_beacon(
    const uint8_t *data,
    size_t length,
    const esp_ieee802154_frame_info_t *frame_info,
    cerberus_ieee802154_beacon_scan_result_payload_t *result)
{
    if (data == NULL || frame_info == NULL || result == NULL || length < 7U) {
        return false;
    }

    uint16_t fcf = (uint16_t)data[0] | ((uint16_t)data[1] << 8U);
    uint8_t frame_type = (uint8_t)(fcf & 0x07U);
    bool security_enabled = (fcf & (1U << 3U)) != 0U;
    bool pan_compression = (fcf & (1U << 6U)) != 0U;
    bool sequence_suppressed = (fcf & (1U << 8U)) != 0U;
    bool header_ie_present = (fcf & (1U << 9U)) != 0U;
    uint8_t destination_mode = (uint8_t)((fcf >> 10U) & 0x03U);
    uint8_t frame_version = (uint8_t)((fcf >> 12U) & 0x03U);
    uint8_t source_mode = (uint8_t)((fcf >> 14U) & 0x03U);

    /* Decode only classic, unsecured beacon frames. Enhanced beacons use a
       different header/IE layout and are deliberately outside this survey. */
    if (frame_type != 0U || security_enabled || pan_compression ||
        sequence_suppressed || header_ie_present || destination_mode != 0U ||
        frame_version > 1U || (source_mode != 2U && source_mode != 3U)) {
        return false;
    }

    size_t offset = 3U; /* FCF and sequence number. */
    size_t source_address_length = source_mode == 2U ? 2U : 8U;
    if (offset + 2U + source_address_length > length) {
        return false;
    }
    uint16_t pan_id = (uint16_t)data[offset] | ((uint16_t)data[offset + 1U] << 8U);
    offset += 2U;
    uint16_t source_short_address = 0xFFFFU;
    if (source_mode == 2U) {
        source_short_address =
            (uint16_t)data[offset] | ((uint16_t)data[offset + 1U] << 8U);
    }
    offset += source_address_length;

    if (offset + 3U > length) {
        return false;
    }
    uint16_t superframe =
        (uint16_t)data[offset] | ((uint16_t)data[offset + 1U] << 8U);
    offset += 2U;
    uint8_t gts_count = data[offset] & 0x07U;
    offset += 1U;
    if (gts_count > 0U) {
        size_t gts_length = 1U + (size_t)gts_count * 3U;
        if (offset + gts_length > length) {
            return false;
        }
        offset += gts_length;
    }

    if (offset + 1U > length) {
        return false;
    }
    uint8_t pending_spec = data[offset++];
    size_t pending_length =
        (size_t)(pending_spec & 0x07U) * 2U +
        (size_t)((pending_spec >> 4U) & 0x07U) * 8U;
    if (offset + pending_length + 15U > length) {
        return false;
    }
    offset += pending_length;

    const uint8_t *zigbee = &data[offset];
    if (zigbee[0] != 0U) {
        return false;
    }
    bool all_zero = true;
    bool all_ff = true;
    for (uint8_t index = 0; index < 8U; ++index) {
        uint8_t value = zigbee[3U + index];
        all_zero = all_zero && value == 0U;
        all_ff = all_ff && value == 0xFFU;
    }
    if (all_zero || all_ff) {
        return false;
    }

    result->generation = 0U;
    result->index = 0U;
    result->channel = frame_info->channel;
    result->rssi = frame_info->rssi;
    result->lqi = frame_info->lqi;
    result->flags = 0U;
    if ((zigbee[2] & (1U << 2U)) != 0U) {
        result->flags |= CERBERUS_IEEE802154_BEACON_FLAG_ROUTER_CAPACITY;
    }
    if ((zigbee[2] & (1U << 7U)) != 0U) {
        result->flags |= CERBERUS_IEEE802154_BEACON_FLAG_END_DEVICE_CAPACITY;
    }
    if ((superframe & (1U << 15U)) != 0U) {
        result->flags |= CERBERUS_IEEE802154_BEACON_FLAG_ASSOCIATION_PERMIT;
    }
    if ((superframe & (1U << 14U)) != 0U) {
        result->flags |= CERBERUS_IEEE802154_BEACON_FLAG_PAN_COORDINATOR;
    }
    result->pan_id = pan_id;
    for (uint8_t index = 0; index < 8U; ++index) {
        result->extended_pan_id[index] = zigbee[3U + index];
    }
    result->source_short_address = source_short_address;
    result->stack_profile = zigbee[1] & 0x0FU;
    result->protocol_version = (zigbee[1] >> 4U) & 0x0FU;
    result->update_id = zigbee[14];
    result->reserved = 0U;
    return true;
}

void IRAM_ATTR esp_ieee802154_energy_detect_done(int8_t power)
{
    energy_power = power;
    energy_done = true;
}

void IRAM_ATTR esp_ieee802154_ed_failed(uint16_t error)
{
    energy_failure_code = error;
    energy_failed = true;
}

void IRAM_ATTR esp_ieee802154_receive_done(
    uint8_t *frame,
    esp_ieee802154_frame_info_t *frame_info)
{
    BaseType_t higher_priority_task_woken = pdFALSE;
    locate_receive(frame,frame_info);
    mesh_receive(frame,frame_info);
    if (activity_scan_active && frame != NULL && frame_info != NULL && frame[0] >= 4U &&
        frame_info->channel >= CERBERUS_IEEE802154_FIRST_CHANNEL &&
        frame_info->channel < CERBERUS_IEEE802154_FIRST_CHANNEL +
            CERBERUS_IEEE802154_CHANNEL_COUNT) {
        uint8_t channel_index = frame_info->channel - CERBERUS_IEEE802154_FIRST_CHANNEL;
        uint8_t mac_frame_type = frame[1] & 0x07U;
        uint8_t counter_index;
        switch (mac_frame_type) {
        case 0U:
            counter_index = CERBERUS_IEEE802154_ACTIVITY_FRAME_BEACON;
            break;
        case 1U:
            counter_index = CERBERUS_IEEE802154_ACTIVITY_FRAME_DATA;
            break;
        case 2U:
            counter_index = CERBERUS_IEEE802154_ACTIVITY_FRAME_ACK;
            break;
        case 3U:
            counter_index = CERBERUS_IEEE802154_ACTIVITY_FRAME_MAC_COMMAND;
            break;
        default:
            counter_index = CERBERUS_IEEE802154_ACTIVITY_FRAME_OTHER;
            break;
        }
        if (activity_counts[channel_index][counter_index] < UINT8_MAX) {
            ++activity_counts[channel_index][counter_index];
        }
        if (activity_total_frames < UINT32_MAX) {
            ++activity_total_frames;
        }
    }
    if (beacon_scan_active && beacon_queue != NULL && frame != NULL && frame[0] >= 2U) {
        cerberus_ieee802154_beacon_scan_result_payload_t observation;
        /* The receive length includes two footer bytes that contain RSSI/LQI,
           not MAC payload. Frame metadata supplies those values separately. */
        size_t mac_length = (size_t)frame[0] - 2U;
        if (parse_zigbee_beacon(frame + 1U, mac_length, frame_info, &observation)) {
            (void)xQueueSendFromISR(beacon_queue, &observation, &higher_priority_task_woken);
        }
    }
    (void)esp_ieee802154_receive_handle_done(frame);
    if (higher_priority_task_woken == pdTRUE) {
        portYIELD_FROM_ISR(higher_priority_task_woken);
    }
}

static bool another_radio_operation_is_active(void)
{
    cerberus_wifi_scan_status_payload_t wifi_status;
    cerberus_wifi_connection_status_payload_t wifi_connection;
    cerberus_ble_scan_status_payload_t ble_status;
    cerberus_ble_connection_status_payload_t connection_status;
    cerberus_wifi_scanner_get_status(&wifi_status);
    cerberus_wifi_connection_get_status(&wifi_connection);
    cerberus_ble_scanner_get_status(&ble_status);
    cerberus_ble_connection_get_status(&connection_status);

    bool connection_active = connection_status.state == CERBERUS_BLE_CONNECTION_CONNECTING ||
        connection_status.state == CERBERUS_BLE_CONNECTION_CONNECTED ||
        connection_status.state == CERBERUS_BLE_CONNECTION_DISCONNECTING;
    return wifi_status.state == CERBERUS_WIFI_SCAN_RUNNING ||
        wifi_connection.state == CERBERUS_WIFI_CONNECTION_STAGING ||
        wifi_connection.state == CERBERUS_WIFI_CONNECTION_CONNECTING ||
        wifi_connection.state == CERBERUS_WIFI_CONNECTION_CONNECTED ||
        wifi_connection.state == CERBERUS_WIFI_CONNECTION_DISCONNECTING ||
        cerberus_wifi_defense_survey_is_running() || cerberus_wifi_station_running() ||
        ble_status.state == CERBERUS_BLE_SCAN_RUNNING || connection_active;
}

static bool mesh_legacy_running_locked(void)
{
    return locate_context!=NULL||scan_status.state==CERBERUS_IEEE802154_ENERGY_SCAN_RUNNING||
        beacon_status.state==CERBERUS_IEEE802154_BEACON_SCAN_RUNNING||
        activity_status.state==CERBERUS_IEEE802154_ACTIVITY_SCAN_RUNNING;
}
static bool mesh_start_valid(const cerberus_mesh_observation_start_t *q)
{
    return q&&q->session_id&&q->channel_mask&&q->dwell_ms>=100U&&q->dwell_ms<=2000U&&
        (q->mode==CERBERUS_MESH_OBSERVATION_SWEEP||
        (q->mode==CERBERUS_MESH_OBSERVATION_FIXED&&
        (q->channel_mask&(uint16_t)(q->channel_mask-1U))==0U));
}
/* Both locks held. Clocks are sampled after serialization, never before waiting
 * for a mutex. Expiry is evaluated before any possible renewal. */
static bool mesh_now_locked(mesh_observation_context_t *c,uint64_t *now)
{
    int64_t source=esp_timer_get_time();
    if(source<0||(uint64_t)source<c->clock_high_us||
        (uint64_t)source>INT64_MAX-(uint64_t)CERBERUS_MESH_OBSERVATION_RETENTION_MS*1000U){
        c->summary.flags|=CERBERUS_MESH_OBSERVATION_SUMMARY_CLOCK_INVALID;
        c->cancelled=true;c->rx_channel=0U;
        if(c->primary_error==ESP_OK)c->primary_error=ESP_ERR_INVALID_STATE;
        if(c->phase==MESH_STOPPED)c->retiring=true;
        else c->state=CERBERUS_MESH_OBSERVATION_STOPPING;
        return false;
    }
    *now=(uint64_t)source;c->clock_high_us=*now;
    if(c->phase==MESH_STOPPED){
        if(*now>=c->retention_us)c->retiring=true;
    }else if(*now>=c->lease_us){
        c->cancelled=true;c->rx_channel=0U;c->state=CERBERUS_MESH_OBSERVATION_STOPPING;
        if(c->primary_error==ESP_OK)c->primary_error=ESP_ERR_TIMEOUT;
    }
    return true;
}
static void mesh_renew_retention_locked(mesh_observation_context_t *c,uint64_t now)
{
    if(c->phase==MESH_STOPPED&&!c->retiring&&!c->release_requested)
        c->retention_us=now+(uint64_t)CERBERUS_MESH_OBSERVATION_RETENTION_MS*1000U;
}
/* scanner_mutex held; helper acquires ISR lock. */
static void mesh_reply_locked(uint64_t token,bool info,esp_err_t error,
    cerberus_mesh_observation_control_t *out)
{
    memset(out,0,sizeof(*out));out->session_id=info?mesh_epoch:token;out->error_code=error;
    portENTER_CRITICAL(&mesh_mux);
    mesh_observation_context_t *c=mesh_context;
    if(c&&(info||token==mesh_session)){
        out->flags=CERBERUS_MESH_OBSERVATION_BOUND|CERBERUS_MESH_OBSERVATION_HELD;
        if(!c->retiring&&!c->release_requested)out->flags|=CERBERUS_MESH_OBSERVATION_DATA_READY;
        if(c->rx_channel&&!c->cancelled&&c->state==CERBERUS_MESH_OBSERVATION_RECEIVING)
            out->flags|=CERBERUS_MESH_OBSERVATION_RX_ACTIVE;
        if(c->summary.flags&CERBERUS_MESH_OBSERVATION_SUMMARY_CLOCK_INVALID)out->flags|=CERBERUS_MESH_OBSERVATION_CLOCK_INVALID;
        if(c->summary.flags&CERBERUS_MESH_OBSERVATION_COUNTERS_SATURATED)out->flags|=CERBERUS_MESH_OBSERVATION_SATURATED;
        if(c->summary.flags&CERBERUS_MESH_OBSERVATION_SEQUENCE_EXHAUSTED)out->flags|=CERBERUS_MESH_OBSERVATION_EXHAUSTED;
        out->sequence=c->summary.last_sequence;out->state=c->state;out->count=c->count;
        out->capacity=CERBERUS_MESH_OBSERVATION_CAPACITY;out->version=CERBERUS_MESH_OBSERVATION_VERSION;
        out->channel=c->rx_channel;
        if(error==ESP_OK)out->error_code=c->cleanup_error!=ESP_OK?c->cleanup_error:c->primary_error;
    }else if(info){
        out->capacity=CERBERUS_MESH_OBSERVATION_CAPACITY;out->version=CERBERUS_MESH_OBSERVATION_VERSION;
        if(mesh_legacy_running_locked()){
            out->flags=CERBERUS_MESH_OBSERVATION_HELD;out->state=CERBERUS_MESH_OBSERVATION_QUEUED;
        }
    }
    portEXIT_CRITICAL(&mesh_mux);
}
void cerberus_mesh_observation_info(cerberus_mesh_observation_control_t *out)
{
    if(!out)return;
    memset(out,0,sizeof(*out));
    if(!scanner_mutex){out->error_code=ESP_ERR_INVALID_STATE;return;}
    xSemaphoreTake(scanner_mutex,portMAX_DELAY);
    mesh_reply_locked(0U,true,ESP_OK,out);
    xSemaphoreGive(scanner_mutex);
}
esp_err_t cerberus_mesh_observation_start(const cerberus_mesh_observation_start_t *q,
    cerberus_mesh_observation_control_t *out)
{
    if(!q||!out)return ESP_ERR_INVALID_ARG;
    memset(out,0,sizeof(*out));out->session_id=q->session_id;
    if(!scanner_mutex||!scanner_task_handle){out->error_code=ESP_ERR_INVALID_STATE;return ESP_ERR_INVALID_STATE;}
    xSemaphoreTake(scanner_mutex,portMAX_DELAY);
    if(q->session_id&&q->session_id==mesh_session){
        esp_err_t error=memcmp(q,&mesh_request,sizeof(*q))?ESP_ERR_INVALID_ARG:
            mesh_context?ESP_OK:ESP_ERR_NOT_FOUND;
        mesh_reply_locked(q->session_id,false,error,out);xSemaphoreGive(scanner_mutex);return error;
    }
    esp_err_t error=!mesh_start_valid(q)?ESP_ERR_INVALID_ARG:
        mesh_context||mesh_legacy_running_locked()||mesh_epoch==UINT64_MAX||q->session_id!=mesh_epoch+1U?
        ESP_ERR_INVALID_STATE:ESP_OK;
    xSemaphoreGive(scanner_mutex);
    mesh_observation_context_t *candidate=NULL;
    if(error==ESP_OK){
        candidate=heap_caps_calloc(1U,sizeof(*candidate),MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
        if(!candidate)error=ESP_ERR_NO_MEM;
    }
    if(error==ESP_OK&&(cerberus_c5_test_running()||another_radio_operation_is_active()))error=ESP_ERR_INVALID_STATE;
    xSemaphoreTake(scanner_mutex,portMAX_DELAY);
    int64_t source=esp_timer_get_time();
    if(error==ESP_OK&&(mesh_context||mesh_legacy_running_locked()||mesh_epoch==UINT64_MAX||
        q->session_id!=mesh_epoch+1U||source<0||
        (uint64_t)source>INT64_MAX-(uint64_t)CERBERUS_MESH_OBSERVATION_RETENTION_MS*1000U))error=ESP_ERR_INVALID_STATE;
    if(error==ESP_OK){
        mesh_epoch=mesh_session=q->session_id;mesh_request=*q;
        candidate->clock_high_us=(uint64_t)source;
        candidate->lease_us=(uint64_t)source+(uint64_t)CERBERUS_MESH_OBSERVATION_LEASE_MS*1000U;
        candidate->state=CERBERUS_MESH_OBSERVATION_QUEUED;
        candidate->summary.version=CERBERUS_MESH_OBSERVATION_VERSION;
        candidate->summary.mode=q->mode;candidate->summary.requested_mask=q->channel_mask;
        candidate->summary.dwell_ms=q->dwell_ms;
        candidate->summary_pin_counter=CERBERUS_MESH_OBSERVATION_SUMMARY_BASE;
        portENTER_CRITICAL(&mesh_mux);mesh_context=candidate;portEXIT_CRITICAL(&mesh_mux);
        candidate=NULL;
    }
    mesh_reply_locked(q->session_id,false,error,out);xSemaphoreGive(scanner_mutex);
    if(candidate)heap_caps_free(candidate);
    if(error==ESP_OK)xTaskNotifyGive(scanner_task_handle);
    return error;
}
static esp_err_t mesh_control(uint64_t token,unsigned action,cerberus_mesh_observation_control_t *out)
{
    if(!out)return ESP_ERR_INVALID_ARG;
    memset(out,0,sizeof(*out));out->session_id=token;
    if(!scanner_mutex){out->error_code=ESP_ERR_INVALID_STATE;return ESP_ERR_INVALID_STATE;}
    xSemaphoreTake(scanner_mutex,portMAX_DELAY);
    esp_err_t error=token&&token==mesh_session&&(mesh_context||action==2U)?ESP_OK:ESP_ERR_NOT_FOUND;
    portENTER_CRITICAL(&mesh_mux);
    mesh_observation_context_t *c=error==ESP_OK?mesh_context:NULL;
    if(c){
        uint64_t now=0U;bool valid=mesh_now_locked(c,&now);
        if(action){
            c->cancelled=true;c->rx_channel=0U;
            if(c->phase!=MESH_STOPPED)c->state=CERBERUS_MESH_OBSERVATION_STOPPING;
            if(action==2U)c->release_requested=c->retiring=true;
        }else if(valid&&!c->retiring&&!c->release_requested){
            if(c->phase!=MESH_STOPPED&&!c->cancelled)
                c->lease_us=now+(uint64_t)CERBERUS_MESH_OBSERVATION_LEASE_MS*1000U;
            else mesh_renew_retention_locked(c,now);
        }
    }
    portEXIT_CRITICAL(&mesh_mux);
    mesh_reply_locked(token,false,error,out);xSemaphoreGive(scanner_mutex);
    if(c&&action)xTaskNotifyGive(scanner_task_handle);
    return error;
}
esp_err_t cerberus_mesh_observation_status(uint64_t token,cerberus_mesh_observation_control_t *out)
{return mesh_control(token,0U,out);}
esp_err_t cerberus_mesh_observation_stop(uint64_t token,cerberus_mesh_observation_control_t *out)
{return mesh_control(token,1U,out);}
esp_err_t cerberus_mesh_observation_release(uint64_t token,cerberus_mesh_observation_control_t *out)
{return mesh_control(token,2U,out);}

esp_err_t cerberus_mesh_observation_peek(const cerberus_mesh_observation_peek_t *q,
    cerberus_mesh_observation_control_t *out)
{
    if(!q||!out)return ESP_ERR_INVALID_ARG;
    memset(out,0,sizeof(*out));out->session_id=q->session_id;
    if(!scanner_mutex){out->error_code=ESP_ERR_INVALID_STATE;return ESP_ERR_INVALID_STATE;}
    xSemaphoreTake(scanner_mutex,portMAX_DELAY);
    esp_err_t error=q->kind>CERBERUS_MESH_OBSERVATION_SUMMARY?ESP_ERR_INVALID_ARG:
        !q->session_id||q->session_id!=mesh_session||!mesh_context?ESP_ERR_NOT_FOUND:ESP_OK;
    uint32_t sequence=0U;
    portENTER_CRITICAL(&mesh_mux);
    mesh_observation_context_t *c=error==ESP_OK?mesh_context:NULL;
    if(c){
        uint64_t now=0U;bool valid=mesh_now_locked(c,&now);
        if(c->retiring||c->release_requested)error=ESP_ERR_NOT_FOUND;
        else if(q->kind==CERBERUS_MESH_OBSERVATION_ROW){
            if(c->count)sequence=c->slots[c->head].sequence;
        }else if(c->summary_pin_counter==UINT32_MAX){
            c->summary.flags|=CERBERUS_MESH_OBSERVATION_SEQUENCE_EXHAUSTED;
            c->cancelled=true;c->rx_channel=0U;error=ESP_ERR_INVALID_STATE;
            if(c->phase!=MESH_STOPPED)c->state=CERBERUS_MESH_OBSERVATION_STOPPING;
        }else{
            c->pinned_summary=c->summary;
            c->pinned_summary.error_code=c->cleanup_error!=ESP_OK?c->cleanup_error:c->primary_error;
            c->pinned_summary.copied_us=0U;
            c->pinned_summary.flags&=(uint8_t)~CERBERUS_MESH_OBSERVATION_COPY_TIME_VALID;
            if(valid&&!(c->summary.flags&CERBERUS_MESH_OBSERVATION_SUMMARY_CLOCK_INVALID)){
                c->pinned_summary.copied_us=now;
                c->pinned_summary.flags|=CERBERUS_MESH_OBSERVATION_COPY_TIME_VALID;
            }
            sequence=c->summary_pin=++c->summary_pin_counter;
        }
        if(error==ESP_OK&&valid)mesh_renew_retention_locked(c,now);
    }
    portEXIT_CRITICAL(&mesh_mux);
    mesh_reply_locked(q->session_id,false,error,out);out->sequence=error==ESP_OK?sequence:0U;
    xSemaphoreGive(scanner_mutex);return error;
}
esp_err_t cerberus_mesh_observation_read(const cerberus_mesh_observation_read_t *q,
    cerberus_mesh_observation_chunk_t *out)
{
    if(!q||!out)return ESP_ERR_INVALID_ARG;
    memset(out,0,sizeof(*out));out->session_id=q->session_id;out->part=q->part;
    esp_err_t error=ESP_ERR_INVALID_STATE;
    if(scanner_mutex){
        xSemaphoreTake(scanner_mutex,portMAX_DELAY);
        size_t length=q->kind==CERBERUS_MESH_OBSERVATION_ROW?CERBERUS_MESH_OBSERVATION_RECORD_BYTES:
            CERBERUS_MESH_OBSERVATION_SUMMARY_BYTES;
        size_t offset=(size_t)q->part*CERBERUS_MESH_OBSERVATION_CHUNK_BYTES;
        error=q->kind>CERBERUS_MESH_OBSERVATION_SUMMARY||offset>=length||!q->sequence?
            ESP_ERR_INVALID_ARG:!q->session_id||q->session_id!=mesh_session||!mesh_context?ESP_ERR_NOT_FOUND:ESP_OK;
        portENTER_CRITICAL(&mesh_mux);
        mesh_observation_context_t *c=error==ESP_OK?mesh_context:NULL;
        const uint8_t *bytes=NULL;
        if(c){
            if(q->kind==CERBERUS_MESH_OBSERVATION_ROW&&q->sequence<=CERBERUS_MESH_OBSERVATION_SEQUENCE_MAX&&
                c->count&&c->slots[c->head].sequence==q->sequence)bytes=(const uint8_t *)&c->slots[c->head].record;
            else if(q->kind==CERBERUS_MESH_OBSERVATION_SUMMARY&&q->sequence>CERBERUS_MESH_OBSERVATION_SUMMARY_BASE&&
                c->summary_pin==q->sequence)bytes=(const uint8_t *)&c->pinned_summary;
            if(!bytes)error=ESP_ERR_NOT_FOUND;
            else{
                uint64_t now=0U;bool valid=mesh_now_locked(c,&now);
                if(c->retiring||c->release_requested)error=ESP_ERR_NOT_FOUND;
                else{
                    size_t count=length-offset;if(count>CERBERUS_MESH_OBSERVATION_CHUNK_BYTES)count=CERBERUS_MESH_OBSERVATION_CHUNK_BYTES;
                    memcpy(out->data,bytes+offset,count);out->sequence=q->sequence;
                    if(valid)mesh_renew_retention_locked(c,now);
                }
            }
        }
        portEXIT_CRITICAL(&mesh_mux);xSemaphoreGive(scanner_mutex);
    }
    if(error!=ESP_OK){
        uint32_t code=(uint32_t)error;
        for(unsigned i=0U;i<4U;++i)out->data[i]=(uint8_t)(code>>(i*8U));
    }
    return error;
}
esp_err_t cerberus_mesh_observation_ack(const cerberus_mesh_observation_ack_t *q,
    cerberus_mesh_observation_control_t *out)
{
    if(!q||!out)return ESP_ERR_INVALID_ARG;
    memset(out,0,sizeof(*out));out->session_id=q->session_id;
    if(!scanner_mutex){out->error_code=ESP_ERR_INVALID_STATE;return ESP_ERR_INVALID_STATE;}
    xSemaphoreTake(scanner_mutex,portMAX_DELAY);
    esp_err_t error=!q->sequence||q->sequence>CERBERUS_MESH_OBSERVATION_SEQUENCE_MAX?ESP_ERR_INVALID_ARG:
        !q->session_id||q->session_id!=mesh_session||!mesh_context?ESP_ERR_NOT_FOUND:ESP_OK;
    portENTER_CRITICAL(&mesh_mux);
    mesh_observation_context_t *c=error==ESP_OK?mesh_context:NULL;
    if(c){
        bool current=c->count&&q->sequence==c->slots[c->head].sequence;
        if(!current&&q->sequence!=c->last_acked)error=ESP_ERR_NOT_FOUND;
        else{
            uint64_t now=0U;bool valid=mesh_now_locked(c,&now);
            if(c->retiring||c->release_requested)error=ESP_ERR_NOT_FOUND;
            else{
                if(current){
                    c->last_acked=q->sequence;c->head=(uint8_t)((c->head+1U)%CERBERUS_MESH_OBSERVATION_CAPACITY);
                    --c->count;c->summary.acked=mesh_increment(c->summary.acked,c);
                }
                if(valid)mesh_renew_retention_locked(c,now);
            }
        }
    }
    portEXIT_CRITICAL(&mesh_mux);
    mesh_reply_locked(q->session_id,false,error,out);
    out->sequence=error==ESP_OK?q->sequence:0U;
    xSemaphoreGive(scanner_mutex);return error;
}

/* One worker, no SDK/heap/wait under either lock. Every SDK return is followed
 * by cancellation revalidation before reopening callback admission. */
static void run_mesh_observation(void)
{
    for(;;){
        xSemaphoreTake(scanner_mutex,portMAX_DELAY);
        portENTER_CRITICAL(&mesh_mux);
        mesh_observation_context_t *c=mesh_context;
        if(!c){portEXIT_CRITICAL(&mesh_mux);xSemaphoreGive(scanner_mutex);return;}
        uint64_t now=0U;bool clock_valid=mesh_now_locked(c,&now);
        if(c->cancelled&&c->phase<MESH_STOPPED){
            c->rx_channel=0U;c->state=CERBERUS_MESH_OBSERVATION_STOPPING;
            if(c->phase<MESH_CLEANUP)c->phase=MESH_CLEANUP;
        }
        uint8_t phase=c->phase;
        bool dispose=phase==MESH_STOPPED&&(c->release_requested||c->retiring);
        if(dispose)mesh_context=NULL;
        portEXIT_CRITICAL(&mesh_mux);xSemaphoreGive(scanner_mutex);
        if(dispose){heap_caps_free(c);return;}
        esp_err_t error=ESP_OK;
        if(phase==MESH_SETUP){
            c->sdk_attempted=true;error=esp_ieee802154_enable();
            if(error==ESP_OK)error=esp_ieee802154_set_coordinator(false);
            if(error==ESP_OK)error=esp_ieee802154_set_promiscuous(true);
            if(error==ESP_OK)error=esp_ieee802154_set_rx_when_idle(true);
            xSemaphoreTake(scanner_mutex,portMAX_DELAY);portENTER_CRITICAL(&mesh_mux);
            if(error!=ESP_OK){if(c->primary_error==ESP_OK)c->primary_error=error;c->cancelled=true;}
            c->phase=c->cancelled?MESH_CLEANUP:MESH_TUNE;
            if(c->cancelled)c->state=CERBERUS_MESH_OBSERVATION_STOPPING;
            portEXIT_CRITICAL(&mesh_mux);xSemaphoreGive(scanner_mutex);
            continue;
        }
        if(phase==MESH_TUNE){
            uint8_t index=c->next_channel;
            while(!(mesh_request.channel_mask&(uint16_t)(1U<<index)))index=(uint8_t)((index+1U)%16U);
            uint8_t channel=(uint8_t)(11U+index);
            error=esp_ieee802154_set_channel(channel);
            /* Publish callback admission before receive(), which may deliver a
             * frame synchronously. A queued control does not claim RX_ACTIVE
             * until the SDK accepts, but an actual delivered frame is retained. */
            xSemaphoreTake(scanner_mutex,portMAX_DELAY);portENTER_CRITICAL(&mesh_mux);
            uint64_t armed=0U;bool arm_valid=mesh_now_locked(c,&armed);
            bool call_receive=error==ESP_OK&&arm_valid&&!c->cancelled;
            if(call_receive){c->rx_channel=channel;c->state=CERBERUS_MESH_OBSERVATION_QUEUED;}
            portEXIT_CRITICAL(&mesh_mux);xSemaphoreGive(scanner_mutex);
            if(call_receive)error=esp_ieee802154_receive();
            xSemaphoreTake(scanner_mutex,portMAX_DELAY);portENTER_CRITICAL(&mesh_mux);
            uint64_t started=0U;bool valid=mesh_now_locked(c,&started);
            if(error!=ESP_OK){if(c->primary_error==ESP_OK)c->primary_error=error;c->cancelled=true;}
            if(!c->cancelled&&valid){
                c->summary.tuned_mask|=(uint16_t)(1U<<index);
                if(!(c->summary.flags&CERBERUS_MESH_OBSERVATION_START_TIME_VALID)){
                    c->summary.started_us=started;c->summary.flags|=CERBERUS_MESH_OBSERVATION_START_TIME_VALID;
                }
                c->dwell_started_us=started;c->rx_channel=channel;c->summary.channel=channel;
                c->next_channel=mesh_request.mode==CERBERUS_MESH_OBSERVATION_FIXED?index:(uint8_t)((index+1U)%16U);
                c->phase=MESH_DWELL;c->state=CERBERUS_MESH_OBSERVATION_RECEIVING;
            }else{c->rx_channel=0U;c->phase=MESH_CLEANUP;c->state=CERBERUS_MESH_OBSERVATION_STOPPING;}
            portEXIT_CRITICAL(&mesh_mux);xSemaphoreGive(scanner_mutex);
            continue;
        }
        if(phase==MESH_DWELL&&clock_valid&&now-c->dwell_started_us>=(uint64_t)mesh_request.dwell_ms*1000U){
            xSemaphoreTake(scanner_mutex,portMAX_DELAY);portENTER_CRITICAL(&mesh_mux);
            uint64_t ended=0U;bool valid=mesh_now_locked(c,&ended);
            /* Stop may have won between the outer decision and this lock. An
             * interrupted window receives no completed-dwell credit. */
            if(valid&&!c->cancelled&&c->phase==MESH_DWELL&&c->rx_channel){
                uint8_t index=(uint8_t)(c->rx_channel-11U);
                uint64_t elapsed=(ended-c->dwell_started_us)/1000U;
                uint32_t previous=c->summary.completed_dwell_ms[index];
                if(elapsed>=UINT32_MAX-previous){c->summary.completed_dwell_ms[index]=UINT32_MAX;c->summary.flags|=CERBERUS_MESH_OBSERVATION_COUNTERS_SATURATED;}
                else c->summary.completed_dwell_ms[index]=previous+(uint32_t)elapsed;
            }
            c->rx_channel=0U;
            portEXIT_CRITICAL(&mesh_mux);xSemaphoreGive(scanner_mutex);
            error=esp_ieee802154_sleep();
            xSemaphoreTake(scanner_mutex,portMAX_DELAY);portENTER_CRITICAL(&mesh_mux);
            if(error!=ESP_OK){if(c->primary_error==ESP_OK)c->primary_error=error;c->cancelled=true;}
            c->phase=c->cancelled?MESH_CLEANUP:MESH_TUNE;
            portEXIT_CRITICAL(&mesh_mux);xSemaphoreGive(scanner_mutex);
            continue;
        }
        if(phase==MESH_CLEANUP){
            esp_err_t idle_error=ESP_OK,sleep_error=ESP_OK;
            if(c->sdk_attempted){
                idle_error=esp_ieee802154_set_rx_when_idle(false);
                sleep_error=esp_ieee802154_sleep();
                error=esp_ieee802154_disable();
                if(error==ESP_OK){
                    esp_err_t restore=esp_ieee802154_set_promiscuous(false);
                    if(idle_error==ESP_OK)idle_error=restore;
                }
            }
            xSemaphoreTake(scanner_mutex,portMAX_DELAY);portENTER_CRITICAL(&mesh_mux);
            c->cleanup_error=error;
            if(error==ESP_OK){
                if(c->primary_error==ESP_OK)c->primary_error=idle_error!=ESP_OK?idle_error:sleep_error;
                uint64_t stopped=0U;bool valid=mesh_now_locked(c,&stopped);
                c->phase=MESH_STOPPED;c->state=c->primary_error==ESP_OK?CERBERUS_MESH_OBSERVATION_COMPLETE:CERBERUS_MESH_OBSERVATION_ERROR;
                c->rx_channel=0U;
                c->summary.channel=0U;c->summary.flags|=CERBERUS_MESH_OBSERVATION_STOPPED;
                c->summary.error_code=c->primary_error;
                if(valid&&!(c->summary.flags&CERBERUS_MESH_OBSERVATION_SUMMARY_CLOCK_INVALID)){
                    c->summary.stopped_us=stopped;c->summary.flags|=CERBERUS_MESH_OBSERVATION_STOP_TIME_VALID;
                }
                if(valid)c->retention_us=stopped+(uint64_t)CERBERUS_MESH_OBSERVATION_RETENTION_MS*1000U;
                else c->retiring=true;
            }
            portEXIT_CRITICAL(&mesh_mux);xSemaphoreGive(scanner_mutex);
            if(error==ESP_OK)continue;
        }
        /* Notifications wake Stop promptly; the finite wait also enforces lease
         * and orphan retention without any SPI caller. SDK duration is unbounded
         * by this scheduler interval and is not a claimed response-time bound. */
        TickType_t wait=pdMS_TO_TICKS(phase==MESH_CLEANUP?100U:20U);
        (void)ulTaskNotifyTake(pdTRUE,wait?wait:1U);
    }
}

static void finish_scan(esp_err_t error)
{
    xSemaphoreTake(scanner_mutex, portMAX_DELAY);
    scan_status.current_channel = 0;
    scan_status.error_code = error;
    scan_status.state = error == ESP_OK
        ? CERBERUS_IEEE802154_ENERGY_SCAN_COMPLETE
        : CERBERUS_IEEE802154_ENERGY_SCAN_ERROR;
    xSemaphoreGive(scanner_mutex);

    if (error == ESP_OK) {
        ESP_LOGI(TAG, "channel energy scan complete");
    } else {
        ESP_LOGW(TAG, "channel energy scan failed: %s", esp_err_to_name(error));
    }
}

static void finish_beacon_scan(esp_err_t error)
{
    xSemaphoreTake(scanner_mutex, portMAX_DELAY);
    beacon_status.current_channel = 0U;
    beacon_status.error_code = error;
    beacon_status.state = error == ESP_OK
        ? CERBERUS_IEEE802154_BEACON_SCAN_COMPLETE
        : CERBERUS_IEEE802154_BEACON_SCAN_ERROR;
    xSemaphoreGive(scanner_mutex);

    if (error == ESP_OK) {
        ESP_LOGI(TAG, "passive beacon survey complete: %u frames, %u networks",
            beacon_status.beacon_count, beacon_status.stored_count);
    } else {
        ESP_LOGW(TAG, "passive beacon survey failed: %s", esp_err_to_name(error));
    }
}

static void finish_activity_scan(esp_err_t error)
{
    xSemaphoreTake(scanner_mutex, portMAX_DELAY);
    activity_status.current_channel = 0U;
    activity_status.total_frames = activity_total_frames;
    activity_status.error_code = error;
    activity_status.state = error == ESP_OK
        ? CERBERUS_IEEE802154_ACTIVITY_SCAN_COMPLETE
        : CERBERUS_IEEE802154_ACTIVITY_SCAN_ERROR;
    xSemaphoreGive(scanner_mutex);

    if (error == ESP_OK) {
        ESP_LOGI(TAG, "passive activity survey complete: %lu frames",
            (unsigned long)activity_total_frames);
    } else {
        ESP_LOGW(TAG, "passive activity survey failed: %s", esp_err_to_name(error));
    }
}

static bool same_extended_pan_id(const uint8_t *left, const uint8_t *right)
{
    for (uint8_t index = 0; index < 8U; ++index) {
        if (left[index] != right[index]) {
            return false;
        }
    }
    return true;
}

static void store_beacon_observation(
    const cerberus_ieee802154_beacon_scan_result_payload_t *observation)
{
    xSemaphoreTake(scanner_mutex, portMAX_DELAY);
    if (beacon_status.beacon_count < UINT16_MAX) {
        ++beacon_status.beacon_count;
    }

    for (uint8_t index = 0; index < beacon_status.stored_count; ++index) {
        cerberus_ieee802154_beacon_scan_result_payload_t *stored = &beacon_results[index];
        if (stored->channel == observation->channel &&
            same_extended_pan_id(stored->extended_pan_id, observation->extended_pan_id)) {
            stored->flags |= observation->flags;
            stored->pan_id = observation->pan_id;
            stored->source_short_address = observation->source_short_address;
            stored->stack_profile = observation->stack_profile;
            stored->protocol_version = observation->protocol_version;
            stored->update_id = observation->update_id;
            if (observation->rssi > stored->rssi) {
                stored->rssi = observation->rssi;
                stored->lqi = observation->lqi;
            }
            xSemaphoreGive(scanner_mutex);
            return;
        }
    }

    if (beacon_status.stored_count < CERBERUS_IEEE802154_BEACON_MAX_RESULTS) {
        uint8_t index = beacon_status.stored_count++;
        beacon_results[index] = *observation;
        beacon_results[index].index = index;
    }
    xSemaphoreGive(scanner_mutex);
}

static esp_err_t scan_one_channel(uint8_t channel, int8_t *power)
{
    esp_err_t err = esp_ieee802154_set_channel(channel);
    if (err != ESP_OK) {
        return err;
    }

    energy_done = false;
    energy_failed = false;
    energy_failure_code = 0;
    err = esp_ieee802154_energy_detect(IEEE802154_ENERGY_DURATION_SYMBOLS);
    if (err != ESP_OK) {
        return err;
    }

    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(IEEE802154_ENERGY_TIMEOUT_MS);
    while ((int32_t)(deadline - xTaskGetTickCount()) > 0) {
        if (energy_failed) {
            ESP_LOGW(TAG, "energy detection failed on channel %u: radio error %u",
                channel, energy_failure_code);
            return ESP_FAIL;
        }
        if (energy_done) {
            *power = energy_power;
            return ESP_OK;
        }
        /* One scheduler tick is guaranteed to advance time. A nominal 1 ms
           delay can round down to zero when CONFIG_FREERTOS_HZ is 100. */
        vTaskDelay(1);
    }
    if (energy_done) {
        *power = energy_power;
        return ESP_OK;
    }
    if (energy_failed) {
        return ESP_FAIL;
    }
    return ESP_ERR_TIMEOUT;
}

static void run_energy_scan(void)
{
    esp_err_t err = esp_ieee802154_enable();
    for (uint8_t index = 0;
         err == ESP_OK && index < CERBERUS_IEEE802154_CHANNEL_COUNT;
         ++index) {
        uint8_t channel = CERBERUS_IEEE802154_FIRST_CHANNEL + index;
        xSemaphoreTake(scanner_mutex, portMAX_DELAY);
        scan_status.current_channel = channel;
        xSemaphoreGive(scanner_mutex);

        int8_t power = CERBERUS_IEEE802154_POWER_UNMEASURED;
        err = scan_one_channel(channel, &power);
        if (err == ESP_OK) {
            xSemaphoreTake(scanner_mutex, portMAX_DELAY);
            scan_power[index] = power;
            scan_status.completed_count = index + 1U;
            xSemaphoreGive(scanner_mutex);
        }
    }

    esp_err_t sleep_err = esp_ieee802154_sleep();
    if (err == ESP_OK && sleep_err != ESP_OK) {
        err = sleep_err;
    }
    esp_err_t disable_err = esp_ieee802154_disable();
    if (err == ESP_OK && disable_err != ESP_OK) {
        err = disable_err;
    }
    finish_scan(err);
}

static void run_beacon_scan(void)
{
    esp_err_t err = esp_ieee802154_enable();
    if (err == ESP_OK) {
        err = esp_ieee802154_set_coordinator(false);
    }
    if (err == ESP_OK) {
        err = esp_ieee802154_set_promiscuous(true);
    }
    if (err == ESP_OK) {
        /* IDF otherwise sleeps after the first received frame. Promiscuous
         * mode disables automatic ACKs; keep listening for this whole dwell. */
        err = esp_ieee802154_set_rx_when_idle(true);
    }
    beacon_scan_active = err == ESP_OK;

    for (uint8_t index = 0;
         err == ESP_OK && index < CERBERUS_IEEE802154_CHANNEL_COUNT;
         ++index) {
        uint8_t channel = CERBERUS_IEEE802154_FIRST_CHANNEL + index;
        xSemaphoreTake(scanner_mutex, portMAX_DELAY);
        beacon_status.current_channel = channel;
        xSemaphoreGive(scanner_mutex);

        err = esp_ieee802154_set_channel(channel);
        if (err == ESP_OK) {
            err = esp_ieee802154_receive();
        }

        TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(IEEE802154_BEACON_DWELL_MS);
        while (err == ESP_OK && (int32_t)(deadline - xTaskGetTickCount()) > 0) {
            cerberus_ieee802154_beacon_scan_result_payload_t observation;
            TickType_t remaining = deadline - xTaskGetTickCount();
            TickType_t wait_ticks = pdMS_TO_TICKS(50U);
            if (wait_ticks == 0U || wait_ticks > remaining) {
                wait_ticks = remaining;
            }
            if (xQueueReceive(beacon_queue, &observation, wait_ticks) == pdTRUE) {
                store_beacon_observation(&observation);
            }
        }

        esp_err_t sleep_err = esp_ieee802154_sleep();
        if (err == ESP_OK && sleep_err != ESP_OK) {
            err = sleep_err;
        }
        cerberus_ieee802154_beacon_scan_result_payload_t observation;
        while (xQueueReceive(beacon_queue, &observation, 0U) == pdTRUE) {
            store_beacon_observation(&observation);
        }
        if (err == ESP_OK) {
            xSemaphoreTake(scanner_mutex, portMAX_DELAY);
            beacon_status.completed_count = index + 1U;
            xSemaphoreGive(scanner_mutex);
        }
    }

    beacon_scan_active = false;
    /* Clear continued reception on every exit, including setup failure. */
    esp_err_t idle_err = esp_ieee802154_set_rx_when_idle(false);
    if (err == ESP_OK && idle_err != ESP_OK) {
        err = idle_err;
    }
    esp_err_t promiscuous_err = esp_ieee802154_set_promiscuous(false);
    if (err == ESP_OK && promiscuous_err != ESP_OK) {
        err = promiscuous_err;
    }
    esp_err_t disable_err = esp_ieee802154_disable();
    if (err == ESP_OK && disable_err != ESP_OK) {
        err = disable_err;
    }
    finish_beacon_scan(err);
}

static void run_activity_scan(void)
{
    esp_err_t err = esp_ieee802154_enable();
    if (err == ESP_OK) {
        err = esp_ieee802154_set_coordinator(false);
    }
    if (err == ESP_OK) {
        err = esp_ieee802154_set_promiscuous(true);
    }
    if (err == ESP_OK) {
        /* IDF otherwise sleeps after the first received frame. Promiscuous
         * mode disables automatic ACKs; keep listening for this whole dwell. */
        err = esp_ieee802154_set_rx_when_idle(true);
    }
    activity_scan_active = err == ESP_OK;

    for (uint8_t index = 0;
         err == ESP_OK && index < CERBERUS_IEEE802154_CHANNEL_COUNT;
         ++index) {
        uint8_t channel = CERBERUS_IEEE802154_FIRST_CHANNEL + index;
        xSemaphoreTake(scanner_mutex, portMAX_DELAY);
        activity_status.current_channel = channel;
        activity_status.total_frames = activity_total_frames;
        xSemaphoreGive(scanner_mutex);

        err = esp_ieee802154_set_channel(channel);
        if (err == ESP_OK) {
            err = esp_ieee802154_receive();
        }
        if (err == ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(IEEE802154_ACTIVITY_DWELL_MS));
        }
        esp_err_t sleep_err = esp_ieee802154_sleep();
        if (err == ESP_OK && sleep_err != ESP_OK) {
            err = sleep_err;
        }
        if (err == ESP_OK) {
            xSemaphoreTake(scanner_mutex, portMAX_DELAY);
            activity_status.completed_count = index + 1U;
            activity_status.total_frames = activity_total_frames;
            xSemaphoreGive(scanner_mutex);
        }
    }

    activity_scan_active = false;
    /* Clear continued reception on every exit, including setup failure. */
    esp_err_t idle_err = esp_ieee802154_set_rx_when_idle(false);
    if (err == ESP_OK && idle_err != ESP_OK) {
        err = idle_err;
    }
    esp_err_t promiscuous_err = esp_ieee802154_set_promiscuous(false);
    if (err == ESP_OK && promiscuous_err != ESP_OK) {
        err = promiscuous_err;
    }
    esp_err_t disable_err = esp_ieee802154_disable();
    if (err == ESP_OK && disable_err != ESP_OK) {
        err = disable_err;
    }
    finish_activity_scan(err);
}

/* All three modes already exclude each other. One resident worker keeps the
 * same stack headroom while avoiding two idle 4 KiB stacks and their TCBs. */
static void scanner_task(void *arg)
{
    (void)arg;
    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        xSemaphoreTake(scanner_mutex, portMAX_DELAY);
        bool energy = scan_status.state == CERBERUS_IEEE802154_ENERGY_SCAN_RUNNING;
        bool beacon = beacon_status.state == CERBERUS_IEEE802154_BEACON_SCAN_RUNNING;
        bool activity = activity_status.state == CERBERUS_IEEE802154_ACTIVITY_SCAN_RUNNING;
        bool mesh = mesh_context != NULL;
        bool locate = locate_context != NULL;
        xSemaphoreGive(scanner_mutex);
        if (locate) run_mesh_locate();
        else if (mesh) run_mesh_observation();
        else if (energy) run_energy_scan();
        else if (beacon) run_beacon_scan();
        else if (activity) run_activity_scan();
        /* A spurious/stale notification must not restart a completed sweep. */
    }
}

esp_err_t cerberus_ieee802154_scanner_init(void)
{
    /* Initialization is owned by app_main; repeated calls reuse the worker. */
    if (scanner_task_handle != NULL) return ESP_OK;
    scanner_mutex = xSemaphoreCreateMutex();
    if (scanner_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }

    memset(&scan_status, 0, sizeof(scan_status));
    memset(scan_power, CERBERUS_IEEE802154_POWER_UNMEASURED, sizeof(scan_power));
    memset(&beacon_status, 0, sizeof(beacon_status));
    memset(beacon_results, 0, sizeof(beacon_results));
    memset(&activity_status, 0, sizeof(activity_status));
    memset((void *)activity_counts, 0, sizeof(activity_counts));
    scan_status.state = CERBERUS_IEEE802154_ENERGY_SCAN_IDLE;
    beacon_status.state = CERBERUS_IEEE802154_BEACON_SCAN_IDLE;
    activity_status.state = CERBERUS_IEEE802154_ACTIVITY_SCAN_IDLE;
    beacon_queue = xQueueCreate(
        IEEE802154_BEACON_QUEUE_LENGTH,
        sizeof(cerberus_ieee802154_beacon_scan_result_payload_t));
    if (beacon_queue == NULL) {
        vSemaphoreDelete(scanner_mutex);
        scanner_mutex = NULL;
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(scanner_task, "ieee802154_scan", IEEE802154_SCAN_TASK_STACK,
            NULL, IEEE802154_SCAN_TASK_PRIORITY, &scanner_task_handle) != pdPASS) {
        scan_status.state = CERBERUS_IEEE802154_ENERGY_SCAN_ERROR;
        scan_status.error_code = ESP_ERR_NO_MEM;
        beacon_status.state = CERBERUS_IEEE802154_BEACON_SCAN_ERROR;
        beacon_status.error_code = ESP_ERR_NO_MEM;
        activity_status.state = CERBERUS_IEEE802154_ACTIVITY_SCAN_ERROR;
        activity_status.error_code = ESP_ERR_NO_MEM;
        vQueueDelete(beacon_queue);
        beacon_queue = NULL;
        vSemaphoreDelete(scanner_mutex);
        scanner_mutex = NULL;
        return ESP_ERR_NO_MEM;
    }

    /* Boot discriminator only; no eternal uniqueness across power cycles. */
    mesh_epoch=((uint64_t)esp_random()<<32U)|esp_random();
    if(!mesh_epoch||mesh_epoch==UINT64_MAX)mesh_epoch=UINT64_MAX/2U;
    ESP_LOGI(TAG, "energy, beacon, passive activity and mesh scanners ready");
    return ESP_OK;
}

esp_err_t cerberus_ieee802154_scanner_start(void)
{
    if(cerberus_c5_test_running())return ESP_ERR_INVALID_STATE;
    if (scanner_mutex == NULL || scanner_task_handle == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (another_radio_operation_is_active()) {
        xSemaphoreTake(scanner_mutex, portMAX_DELAY);
        ++scan_status.generation;
        scan_status.state = CERBERUS_IEEE802154_ENERGY_SCAN_ERROR;
        scan_status.completed_count = 0;
        scan_status.current_channel = 0;
        scan_status.error_code = ESP_ERR_INVALID_STATE;
        xSemaphoreGive(scanner_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(scanner_mutex, portMAX_DELAY);
    if (locate_context != NULL || mesh_context != NULL ||
        scan_status.state == CERBERUS_IEEE802154_ENERGY_SCAN_RUNNING ||
        beacon_status.state == CERBERUS_IEEE802154_BEACON_SCAN_RUNNING ||
        activity_status.state == CERBERUS_IEEE802154_ACTIVITY_SCAN_RUNNING) {
        xSemaphoreGive(scanner_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    ++scan_status.generation;
    scan_status.state = CERBERUS_IEEE802154_ENERGY_SCAN_RUNNING;
    scan_status.completed_count = 0;
    scan_status.current_channel = CERBERUS_IEEE802154_FIRST_CHANNEL;
    scan_status.error_code = ESP_OK;
    memset(scan_power, CERBERUS_IEEE802154_POWER_UNMEASURED, sizeof(scan_power));
    xSemaphoreGive(scanner_mutex);

    xTaskNotifyGive(scanner_task_handle);
    return ESP_OK;
}

bool cerberus_ieee802154_scanner_is_running(void)
{
    if (scanner_mutex == NULL) {
        return false;
    }
    xSemaphoreTake(scanner_mutex, portMAX_DELAY);
    bool running = locate_context != NULL || mesh_context != NULL ||
        scan_status.state == CERBERUS_IEEE802154_ENERGY_SCAN_RUNNING ||
        beacon_status.state == CERBERUS_IEEE802154_BEACON_SCAN_RUNNING ||
        activity_status.state == CERBERUS_IEEE802154_ACTIVITY_SCAN_RUNNING;
    xSemaphoreGive(scanner_mutex);
    return running;
}

void cerberus_ieee802154_scanner_get_status(
    cerberus_ieee802154_energy_scan_status_payload_t *status)
{
    if (status == NULL) {
        return;
    }
    memset(status, 0, sizeof(*status));
    if (scanner_mutex == NULL) {
        status->state = CERBERUS_IEEE802154_ENERGY_SCAN_ERROR;
        status->error_code = ESP_ERR_INVALID_STATE;
        return;
    }
    xSemaphoreTake(scanner_mutex, portMAX_DELAY);
    *status = scan_status;
    xSemaphoreGive(scanner_mutex);
}

esp_err_t cerberus_ieee802154_scanner_get_result(
    uint8_t generation,
    cerberus_ieee802154_energy_scan_result_payload_t *result)
{
    if (result == NULL || scanner_mutex == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(scanner_mutex, portMAX_DELAY);
    if (scan_status.state != CERBERUS_IEEE802154_ENERGY_SCAN_COMPLETE ||
        generation != scan_status.generation) {
        xSemaphoreGive(scanner_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    memset(result, 0, sizeof(*result));
    result->generation = generation;
    result->first_channel = CERBERUS_IEEE802154_FIRST_CHANNEL;
    result->channel_count = CERBERUS_IEEE802154_CHANNEL_COUNT;
    memcpy(result->power_dbm, scan_power, sizeof(result->power_dbm));
    xSemaphoreGive(scanner_mutex);
    return ESP_OK;
}

esp_err_t cerberus_ieee802154_beacon_scanner_start(void)
{
    if(cerberus_c5_test_running())return ESP_ERR_INVALID_STATE;
    if (scanner_mutex == NULL || scanner_task_handle == NULL || beacon_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (another_radio_operation_is_active()) {
        xSemaphoreTake(scanner_mutex, portMAX_DELAY);
        ++beacon_status.generation;
        beacon_status.state = CERBERUS_IEEE802154_BEACON_SCAN_ERROR;
        beacon_status.beacon_count = 0U;
        beacon_status.stored_count = 0U;
        beacon_status.completed_count = 0U;
        beacon_status.current_channel = 0U;
        beacon_status.error_code = ESP_ERR_INVALID_STATE;
        xSemaphoreGive(scanner_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(scanner_mutex, portMAX_DELAY);
    if (locate_context != NULL || mesh_context != NULL ||
        scan_status.state == CERBERUS_IEEE802154_ENERGY_SCAN_RUNNING ||
        beacon_status.state == CERBERUS_IEEE802154_BEACON_SCAN_RUNNING ||
        activity_status.state == CERBERUS_IEEE802154_ACTIVITY_SCAN_RUNNING) {
        xSemaphoreGive(scanner_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    ++beacon_status.generation;
    beacon_status.state = CERBERUS_IEEE802154_BEACON_SCAN_RUNNING;
    beacon_status.beacon_count = 0U;
    beacon_status.stored_count = 0U;
    beacon_status.completed_count = 0U;
    beacon_status.current_channel = CERBERUS_IEEE802154_FIRST_CHANNEL;
    beacon_status.error_code = ESP_OK;
    memset(beacon_results, 0, sizeof(beacon_results));
    xQueueReset(beacon_queue);
    xSemaphoreGive(scanner_mutex);

    xTaskNotifyGive(scanner_task_handle);
    return ESP_OK;
}

void cerberus_ieee802154_beacon_scanner_get_status(
    cerberus_ieee802154_beacon_scan_status_payload_t *status)
{
    if (status == NULL) {
        return;
    }
    memset(status, 0, sizeof(*status));
    if (scanner_mutex == NULL) {
        status->state = CERBERUS_IEEE802154_BEACON_SCAN_ERROR;
        status->error_code = ESP_ERR_INVALID_STATE;
        return;
    }
    xSemaphoreTake(scanner_mutex, portMAX_DELAY);
    *status = beacon_status;
    xSemaphoreGive(scanner_mutex);
}

esp_err_t cerberus_ieee802154_beacon_scanner_get_result(
    uint8_t generation,
    uint8_t index,
    cerberus_ieee802154_beacon_scan_result_payload_t *result)
{
    if (result == NULL || scanner_mutex == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(scanner_mutex, portMAX_DELAY);
    if (beacon_status.state != CERBERUS_IEEE802154_BEACON_SCAN_COMPLETE ||
        generation != beacon_status.generation || index >= beacon_status.stored_count) {
        xSemaphoreGive(scanner_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    *result = beacon_results[index];
    result->generation = generation;
    result->index = index;
    xSemaphoreGive(scanner_mutex);
    return ESP_OK;
}

esp_err_t cerberus_ieee802154_activity_scanner_start(void)
{
    if(cerberus_c5_test_running())return ESP_ERR_INVALID_STATE;
    if (scanner_mutex == NULL || scanner_task_handle == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (another_radio_operation_is_active()) {
        xSemaphoreTake(scanner_mutex, portMAX_DELAY);
        ++activity_status.generation;
        activity_status.state = CERBERUS_IEEE802154_ACTIVITY_SCAN_ERROR;
        activity_status.completed_count = 0U;
        activity_status.current_channel = 0U;
        activity_status.total_frames = 0U;
        activity_status.error_code = ESP_ERR_INVALID_STATE;
        xSemaphoreGive(scanner_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(scanner_mutex, portMAX_DELAY);
    if (locate_context != NULL || mesh_context != NULL ||
        scan_status.state == CERBERUS_IEEE802154_ENERGY_SCAN_RUNNING ||
        beacon_status.state == CERBERUS_IEEE802154_BEACON_SCAN_RUNNING ||
        activity_status.state == CERBERUS_IEEE802154_ACTIVITY_SCAN_RUNNING) {
        xSemaphoreGive(scanner_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    ++activity_status.generation;
    activity_status.state = CERBERUS_IEEE802154_ACTIVITY_SCAN_RUNNING;
    activity_status.completed_count = 0U;
    activity_status.current_channel = CERBERUS_IEEE802154_FIRST_CHANNEL;
    activity_status.total_frames = 0U;
    activity_status.error_code = ESP_OK;
    activity_total_frames = 0U;
    memset((void *)activity_counts, 0, sizeof(activity_counts));
    xSemaphoreGive(scanner_mutex);

    xTaskNotifyGive(scanner_task_handle);
    return ESP_OK;
}

void cerberus_ieee802154_activity_scanner_get_status(
    cerberus_ieee802154_activity_scan_status_payload_t *status)
{
    if (status == NULL) {
        return;
    }
    memset(status, 0, sizeof(*status));
    if (scanner_mutex == NULL) {
        status->state = CERBERUS_IEEE802154_ACTIVITY_SCAN_ERROR;
        status->error_code = ESP_ERR_INVALID_STATE;
        return;
    }
    xSemaphoreTake(scanner_mutex, portMAX_DELAY);
    *status = activity_status;
    if (status->state == CERBERUS_IEEE802154_ACTIVITY_SCAN_RUNNING) {
        status->total_frames = activity_total_frames;
    }
    xSemaphoreGive(scanner_mutex);
}

esp_err_t cerberus_ieee802154_activity_scanner_get_result(
    uint8_t generation,
    uint8_t result_index,
    cerberus_ieee802154_activity_scan_result_payload_t *result)
{
    if (result == NULL || scanner_mutex == NULL ||
        result_index >= CERBERUS_IEEE802154_ACTIVITY_RESULT_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(scanner_mutex, portMAX_DELAY);
    if (activity_status.state != CERBERUS_IEEE802154_ACTIVITY_SCAN_COMPLETE ||
        generation != activity_status.generation) {
        xSemaphoreGive(scanner_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    memset(result, 0, sizeof(*result));
    result->generation = generation;
    uint8_t first_index = result_index * CERBERUS_IEEE802154_ACTIVITY_CHANNELS_PER_RESULT;
    result->first_channel = CERBERUS_IEEE802154_FIRST_CHANNEL + first_index;
    for (uint8_t channel = 0;
         channel < CERBERUS_IEEE802154_ACTIVITY_CHANNELS_PER_RESULT;
         ++channel) {
        for (uint8_t type = 0;
             type < CERBERUS_IEEE802154_ACTIVITY_FRAME_TYPE_COUNT;
             ++type) {
            result->counts[channel][type] = activity_counts[first_index + channel][type];
        }
    }
    xSemaphoreGive(scanner_mutex);
    return ESP_OK;
}
