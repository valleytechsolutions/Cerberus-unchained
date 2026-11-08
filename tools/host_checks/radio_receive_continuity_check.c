/* Receive continuity regression: includes the production worker without rewriting
 * its receive/dwell/control logic. Platform fake follows installed IDF6.0.1:
 * mac_init resets rx_when_idle=false; RX completion next_operation sleeps
 * unless rx_when_idle=true. No claim that a host fake validates RF reception. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fake_worker_platform.h"
#include "ieee802154_scanner.c"

static jmp_buf idle;
static void (*task_fn)(void *);
static TickType_t ticks;
static unsigned notifications, locked, created, enabled, disabled, receive_calls;
static unsigned offered, delivered, returned, emitted, current_channel;
static unsigned rx_continuous_enables, rx_continuous_disables;
static bool radio_enabled, rx_active, rx_when_idle, auto_ack_tx, auto_ack_rx;
static bool fail_enable, fail_receive, hardware_sleep;
static esp_err_t idle_enable_error, idle_disable_error, promiscuous_restore_error, disable_error;
static unsigned scenario; /* 1=beacons,2=activity; zero=energy/no synthetic frames. */
static const uint8_t *outstanding;
static cerberus_ieee802154_beacon_scan_result_payload_t queue[8];
static unsigned queue_head, queue_count;
void *heap_caps_calloc(size_t n,size_t size,unsigned caps){assert(caps==3U);return calloc(n,size);}
void heap_caps_free(void *p){free(p);}
uint32_t esp_random(void){return 0x23456789U;}
int64_t esp_timer_get_time(void){return (int64_t)ticks*1000;}

static void offer_one_frame(void)
{
    if (!scenario || emitted >= (scenario == 1U ? 3U : 5U)) return;
    unsigned event = emitted++;
    ++offered;
    if (!radio_enabled || !rx_active || hardware_sleep) return;
    assert(!auto_ack_tx && !auto_ack_rx); /* Passive sniffing never acknowledges. */
    uint8_t frame[29] = {0};
    if (scenario == 1U && event != 0U) {
        frame[0] = 28U; /* 26 MAC bytes plus two driver footer bytes. */
        uint8_t *mac = frame + 1U;
        mac[0] = 0U; mac[1] = 0x80U; mac[2] = (uint8_t)event;
        mac[3] = 0x34U; mac[4] = 0x12U; mac[5] = 0x78U; mac[6] = 0x56U;
        mac[7] = 0U; mac[8] = 0xC0U; /* Coordinator/association flags. */
        mac[9] = 0U; mac[10] = 0U; /* No GTS or pending addresses. */
        uint8_t *zigbee = mac + 11U;
        zigbee[0] = 0U; zigbee[1] = 0x22U; zigbee[2] = 0x84U;
        for (unsigned i = 0U; i < 8U; ++i) zigbee[3U + i] = (uint8_t)(current_channel + i);
        zigbee[14] = 7U;
    } else {
        static const uint8_t types[] = {0U,1U,2U,3U,7U};
        frame[0] = 6U;
        frame[1] = scenario == 1U ? 1U : types[event];
    }
    esp_ieee802154_frame_info_t info = {
        .channel = (uint8_t)current_channel, .rssi = event == 2U ? -42 : -70, .lqi = 100U};
    assert(!outstanding);
    outstanding = frame;
    ++delivered;
    esp_ieee802154_receive_done(frame, &info);
    assert(!outstanding); /* Every SDK-owned receive buffer is released once. */
    /* The real SDK ISR performs next_operation after this callback. */
    rx_active = radio_enabled && rx_when_idle;
    hardware_sleep = !rx_active;
}

SemaphoreHandle_t xSemaphoreCreateMutex(void){return (void *)1;}
void vSemaphoreDelete(SemaphoreHandle_t h){assert(h);}
int xSemaphoreTake(SemaphoreHandle_t h,unsigned wait){(void)wait;assert(h&&!locked);++locked;return pdTRUE;}
int xSemaphoreGive(SemaphoreHandle_t h){assert(h&&locked==1U);--locked;return pdTRUE;}
QueueHandle_t xQueueCreate(unsigned n,unsigned size){assert(n==8U&&size==sizeof(queue[0]));return (void *)2;}
void vQueueDelete(QueueHandle_t h){assert(h);}
int xQueueReceive(QueueHandle_t h,void *out,unsigned wait)
{
    assert(h&&out);
    if (!queue_count && wait) { ticks += wait; offer_one_frame(); }
    if (!queue_count) return pdFALSE;
    memcpy(out,&queue[queue_head],sizeof(queue[0]));
    queue_head=(queue_head+1U)%8U;--queue_count;return pdTRUE;
}
int xQueueSendFromISR(QueueHandle_t h,const void *in,BaseType_t *woken)
{
    assert(h&&in&&woken);
    if(queue_count==8U)return pdFALSE;
    memcpy(&queue[(queue_head+queue_count)%8U],in,sizeof(queue[0]));++queue_count;
    return pdTRUE;
}
int xQueueReset(QueueHandle_t h){assert(h);queue_head=queue_count=0U;return pdTRUE;}
int xTaskCreate(void(*fn)(void*),const char *name,unsigned stack,void *arg,unsigned priority,TaskHandle_t *out)
{assert(name&&arg==NULL&&priority==4U&&stack==4096U);++created;task_fn=fn;*out=(void*)3;return pdPASS;}
void xTaskNotifyGive(TaskHandle_t h){assert(h==(void*)3);++notifications;}
unsigned ulTaskNotifyTake(int clear,unsigned wait)
{assert(clear==pdTRUE&&wait==portMAX_DELAY);if(!notifications)longjmp(idle,1);unsigned n=notifications;notifications=0U;return n;}
TickType_t xTaskGetTickCount(void){return ticks;}
void vTaskDelay(unsigned n)
{
    ticks += n;
    if(scenario==2U) for(unsigned i=0U;i<5U;++i)offer_one_frame();
}
esp_err_t esp_ieee802154_enable(void)
{
    ++enabled;rx_when_idle=false;auto_ack_tx=auto_ack_rx=true;
    radio_enabled=!fail_enable;rx_active=false;hardware_sleep=true;
    return fail_enable?ESP_FAIL:ESP_OK;
}
esp_err_t esp_ieee802154_disable(void)
{/* Installed SDK disables RF before returning MAC-deinit errors. */
 ++disabled;radio_enabled=rx_active=false;hardware_sleep=true;return disable_error;}
esp_err_t esp_ieee802154_sleep(void)
{rx_active=false;hardware_sleep=true;return ESP_OK;}
esp_err_t esp_ieee802154_set_rx_when_idle(bool on)
{
    if(on)++rx_continuous_enables;else ++rx_continuous_disables;
    esp_err_t error=on?idle_enable_error:idle_disable_error;
    if(error!=ESP_OK)return error;
    rx_when_idle=on;return ESP_OK;
}
esp_err_t esp_ieee802154_set_channel(uint8_t channel)
{assert(channel>=11U&&channel<=26U&&!rx_active);current_channel=channel;emitted=0U;return ESP_OK;}
esp_err_t esp_ieee802154_set_coordinator(bool value){assert(!value);return ESP_OK;}
esp_err_t esp_ieee802154_set_promiscuous(bool value)
{
    if(!value){assert(!rx_active);if(promiscuous_restore_error)return promiscuous_restore_error;}
    auto_ack_tx=auto_ack_rx=!value;return ESP_OK;
}
esp_err_t esp_ieee802154_receive(void)
{++receive_calls;if(fail_receive)return ESP_FAIL;assert(radio_enabled);rx_active=true;hardware_sleep=false;return ESP_OK;}
esp_err_t esp_ieee802154_receive_handle_done(uint8_t *frame)
{assert(frame==outstanding);outstanding=NULL;++returned;return ESP_OK;}
esp_err_t esp_ieee802154_energy_detect(uint32_t duration)
{assert(duration==3125U);esp_ieee802154_energy_detect_done(-77);return ESP_OK;}
bool cerberus_c5_test_running(void){return false;}
void cerberus_wifi_scanner_get_status(cerberus_wifi_scan_status_payload_t *s){memset(s,0,sizeof(*s));}
void cerberus_wifi_connection_get_status(cerberus_wifi_connection_status_payload_t *s){memset(s,0,sizeof(*s));}
void cerberus_ble_scanner_get_status(cerberus_ble_scan_status_payload_t *s){memset(s,0,sizeof(*s));}
void cerberus_ble_connection_get_status(cerberus_ble_connection_status_payload_t *s){memset(s,0,sizeof(*s));}
bool cerberus_wifi_defense_survey_is_running(void){return false;}
bool cerberus_wifi_station_running(void){return false;}
static void run_worker(void){assert(task_fn);if(!setjmp(idle))task_fn(NULL);assert(!locked&&!outstanding);}
static void reset_observed(unsigned next_scenario)
{scenario=next_scenario;offered=delivered=returned=receive_calls=0U;}

static void receive_failure_case(bool beacon,esp_err_t expected)
{
    unsigned clear_before=rx_continuous_disables,disable_before=disabled;
    reset_observed(beacon?1U:2U);
    assert((beacon?cerberus_ieee802154_beacon_scanner_start():
        cerberus_ieee802154_activity_scanner_start())==ESP_OK);
    run_worker();
    assert((beacon?beacon_status.state:activity_status.state)==CERBERUS_IEEE802154_BEACON_SCAN_ERROR);
    assert((beacon?beacon_status.error_code:activity_status.error_code)==expected);
    assert(rx_continuous_disables==clear_before+1U&&disabled==disable_before+1U);
    assert(!radio_enabled&&!rx_active&&!beacon_scan_active&&!activity_scan_active);
    assert(!cerberus_ieee802154_scanner_is_running()&&returned==delivered);
}

static void setup_and_cleanup_failures(void)
{
    /* Distinct injected errors prove first-error precedence. The installed
     * setting APIs currently return OK; these failures exercise callers rather
     * than claim observed hardware failure modes. Disable mirrors actual SDK
     * RF shutdown before a possible resource-deinit error. */
    const esp_err_t setup_error=0x1201,clear_error=0x1202,restore_error=0x1203,teardown_error=0x1204;
    for(unsigned mode=0U;mode<2U;++mode){
        bool beacon=mode==0U;
        idle_enable_error=setup_error;idle_disable_error=clear_error;
        promiscuous_restore_error=restore_error;disable_error=teardown_error;
        receive_failure_case(beacon,setup_error);assert(!receive_calls&&!delivered);
        idle_enable_error=ESP_OK;fail_enable=true;
        receive_failure_case(beacon,ESP_FAIL);assert(!receive_calls&&!delivered);fail_enable=false;
        fail_receive=true;
        receive_failure_case(beacon,ESP_FAIL);assert(receive_calls==1U&&!delivered);fail_receive=false;
        receive_failure_case(beacon,clear_error);
        idle_disable_error=ESP_OK;
        receive_failure_case(beacon,restore_error);assert(!rx_when_idle);
        promiscuous_restore_error=ESP_OK;
        receive_failure_case(beacon,teardown_error);assert(!rx_when_idle);
        disable_error=ESP_OK;
    }
}

int main(int argc,char **argv)
{
    const bool expect_bug=argc==2&&strcmp(argv[1],"--expect-bug")==0;
    assert(argc==1||expect_bug);
    assert(cerberus_ieee802154_scanner_init()==ESP_OK&&created==1U);
    reset_observed(1U);
    assert(cerberus_ieee802154_beacon_scanner_start()==ESP_OK);
    assert(cerberus_ieee802154_scanner_is_running());
    run_worker();
    assert(offered==48U&&returned==delivered&&receive_calls==16U);
    assert(beacon_status.state==CERBERUS_IEEE802154_BEACON_SCAN_COMPLETE);
    if(expect_bug){assert(delivered==16U&&beacon_status.beacon_count==0U&&beacon_status.stored_count==0U);}
    else{
        assert(delivered==48U&&beacon_status.beacon_count==32U&&beacon_status.stored_count==16U);
        for(unsigned i=0U;i<16U;++i){assert(beacon_results[i].channel==11U+i);assert(beacon_results[i].rssi==-42);}
        assert(!rx_when_idle);
    }
    assert(!radio_enabled&&!rx_active&&!beacon_scan_active&&!cerberus_ieee802154_scanner_is_running());
    /* A late hardware opportunity cannot call production RX after disable. */
    unsigned before=delivered;emitted=0U;offer_one_frame();assert(delivered==before);
    reset_observed(2U);
    assert(cerberus_ieee802154_activity_scanner_start()==ESP_OK);run_worker();
    assert(offered==80U&&returned==delivered&&receive_calls==16U);
    assert(activity_status.state==CERBERUS_IEEE802154_ACTIVITY_SCAN_COMPLETE);
    assert(activity_status.total_frames==(expect_bug?16U:80U));
    for(unsigned channel=0U;channel<16U;++channel)
        for(unsigned type=0U;type<5U;++type)
            assert(activity_counts[channel][type]==(expect_bug&&type!=0U?0U:1U));
    assert(!radio_enabled&&!rx_active&&!activity_scan_active);
    if(!expect_bug)assert(!rx_when_idle);
    /* Normal completion/sleep is the existing cancellation boundary: there is
     * no public Stop command in this source. Do not model one into existence. */
    fail_receive=true;reset_observed(2U);
    assert(cerberus_ieee802154_activity_scanner_start()==ESP_OK);run_worker();
    assert(activity_status.state==CERBERUS_IEEE802154_ACTIVITY_SCAN_ERROR&&!delivered);
    assert(!radio_enabled&&!rx_active&&!activity_scan_active);fail_receive=false;
    if(!expect_bug)assert(!rx_when_idle);
    fail_enable=true;reset_observed(1U);
    assert(cerberus_ieee802154_beacon_scanner_start()==ESP_OK);run_worker();
    assert(beacon_status.state==CERBERUS_IEEE802154_BEACON_SCAN_ERROR&&!delivered);
    assert(!radio_enabled&&!rx_active&&!beacon_scan_active);fail_enable=false;
    if(!expect_bug)setup_and_cleanup_failures();
    reset_observed(0U);
    unsigned enables_before_energy=rx_continuous_enables,disables_before_energy=rx_continuous_disables;
    assert(cerberus_ieee802154_scanner_start()==ESP_OK);run_worker();
    assert(scan_status.state==CERBERUS_IEEE802154_ENERGY_SCAN_COMPLETE&&scan_status.completed_count==16U);
    assert(!receive_calls&&!delivered&&!rx_when_idle&&enabled==disabled&&created==1U);
    assert(rx_continuous_enables==enables_before_energy&&rx_continuous_disables==disables_before_energy);
    if(expect_bug)assert(!rx_continuous_enables&&!rx_continuous_disables);
    else assert(rx_continuous_enables>=3U&&rx_continuous_disables>=4U);
    puts(expect_bug?"REPRODUCED: first frame consumes each dwell; subsequent beacons/activity are missed.":
        "PASS: sustained passive receive, data-before-beacon, per-type activity, buffer return and cleanup.");
}
