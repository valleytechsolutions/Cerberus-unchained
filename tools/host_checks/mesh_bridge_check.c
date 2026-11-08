/* Both production ends are compiled. Only SDK, task scheduling and the physical
 * SPI transaction boundary are modeled. No fixture makes controller decisions. */
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
#include "mesh_scan.h"
#include "c5_spi_link.h"

static unsigned long checks;
#define CHECK(x) do{++checks;if(!(x)){fprintf(stderr,"mesh bridge line %d: %s\n",__LINE__,#x);exit(1);}}while(0)
static mesh_scan_t controller;
static jmp_buf idle;
static void (*worker)(void *);
static unsigned notifications,c5_locked,created,allocated,freed,enables,disables,rx_calls,returned;
static int64_t source_clock=1000000,s3_clock=9000000;
static uint64_t tick_ms;
static bool rf_enabled,rx_active,rx_continuous,auto_ack_rx,auto_ack_tx;
static bool foreign_wifi,allocation_failure;
static uint8_t rf_channel;
static esp_err_t receive_failure;
static unsigned held_disable;
static cerberus_spi_frame_t dispatched;
static unsigned dispatch_replies;
static uint8_t drop_response[256];
static unsigned commands[256],refreshes,wire_calls;
static uint8_t first_command,last_command;
static unsigned sequence,peer_firmware_version;
static uint8_t firmware=CERBERUS_MESH_OBSERVATION_MIN_FIRMWARE;
static int link_mutex=1,link_locked;

void *heap_caps_calloc(size_t n,size_t size,unsigned caps)
{CHECK(!c5_locked&&!mesh_mux.depth&&caps==3U);if(allocation_failure)return NULL;void *p=calloc(n,size);CHECK(p);++allocated;return p;}
void heap_caps_free(void *p){CHECK(p&&!c5_locked&&!mesh_mux.depth);++freed;free(p);}
uint32_t esp_random(void){return UINT32_C(0x5a1c9432);}
int64_t esp_timer_get_time(void){return source_clock;}
SemaphoreHandle_t xSemaphoreCreateMutex(void){return (void*)1;}
void vSemaphoreDelete(SemaphoreHandle_t h){CHECK(h);}
int xSemaphoreTake(SemaphoreHandle_t h,unsigned wait){(void)wait;CHECK(h&&!c5_locked);++c5_locked;return pdTRUE;}
int xSemaphoreGive(SemaphoreHandle_t h){CHECK(h&&c5_locked==1U);--c5_locked;return pdTRUE;}
QueueHandle_t xQueueCreate(unsigned n,unsigned size){CHECK(n==8U&&size);return (void*)2;}
void vQueueDelete(QueueHandle_t h){CHECK(h);}
int xQueueReceive(QueueHandle_t h,void *out,unsigned wait){CHECK(h&&out);source_clock+=(int64_t)wait*1000;return pdFALSE;}
int xQueueSendFromISR(QueueHandle_t h,const void *in,BaseType_t *wake){CHECK(h&&in&&wake);return pdTRUE;}
int xQueueReset(QueueHandle_t h){CHECK(h);return pdTRUE;}
int xTaskCreate(void(*fn)(void*),const char *name,unsigned stack,void *arg,unsigned priority,TaskHandle_t *out)
{CHECK(name&&stack==4096U&&!arg&&priority==4U);worker=fn;*out=(void*)3;++created;return pdPASS;}
void xTaskNotifyGive(TaskHandle_t h){CHECK(h==(void*)3);++notifications;}
unsigned ulTaskNotifyTake(int clear,unsigned wait)
{
    CHECK(clear==pdTRUE&&!c5_locked&&!mesh_mux.depth);
    if(wait==portMAX_DELAY&&notifications){notifications=0U;return 1U;}
    CHECK(wait==portMAX_DELAY||wait==20U||wait==100U);longjmp(idle,1);
}
TickType_t xTaskGetTickCount(void){return (TickType_t)(source_clock/1000);}
void vTaskDelay(unsigned n){CHECK(!c5_locked&&!mesh_mux.depth);source_clock+=(int64_t)n*1000;}
esp_err_t esp_ieee802154_enable(void)
{CHECK(!c5_locked&&!mesh_mux.depth);++enables;rf_enabled=true;rx_continuous=false;auto_ack_rx=auto_ack_tx=true;return ESP_OK;}
esp_err_t esp_ieee802154_disable(void)
{CHECK(!c5_locked&&!mesh_mux.depth);++disables;rf_enabled=rx_active=false;if(held_disable){--held_disable;return ESP_FAIL;}return ESP_OK;}
esp_err_t esp_ieee802154_sleep(void){CHECK(!c5_locked&&!mesh_mux.depth);rx_active=false;return ESP_OK;}
esp_err_t esp_ieee802154_set_channel(uint8_t c){CHECK(!c5_locked&&!mesh_mux.depth&&c>=11U&&c<=26U);rf_channel=c;return ESP_OK;}
esp_err_t esp_ieee802154_set_coordinator(bool value){CHECK(!c5_locked&&!mesh_mux.depth&&!value);return ESP_OK;}
esp_err_t esp_ieee802154_set_promiscuous(bool value){CHECK(!c5_locked&&!mesh_mux.depth);auto_ack_rx=auto_ack_tx=!value;return ESP_OK;}
esp_err_t esp_ieee802154_set_rx_when_idle(bool value){CHECK(!c5_locked&&!mesh_mux.depth);rx_continuous=value;return ESP_OK;}
esp_err_t esp_ieee802154_receive(void)
{CHECK(!c5_locked&&!mesh_mux.depth&&rx_continuous&&!auto_ack_rx&&!auto_ack_tx);++rx_calls;rx_active=receive_failure==ESP_OK;return receive_failure;}
esp_err_t esp_ieee802154_receive_handle_done(uint8_t *frame){(void)frame;++returned;return ESP_OK;}
esp_err_t esp_ieee802154_energy_detect(uint32_t duration){CHECK(duration==3125U);esp_ieee802154_energy_detect_done(-77);return ESP_OK;}
bool cerberus_c5_test_running(void){return false;}
void cerberus_wifi_scanner_get_status(cerberus_wifi_scan_status_payload_t *s){memset(s,0,sizeof(*s));s->state=foreign_wifi?CERBERUS_WIFI_SCAN_RUNNING:0U;}
void cerberus_wifi_connection_get_status(cerberus_wifi_connection_status_payload_t *s){memset(s,0,sizeof(*s));}
void cerberus_wifi_defense_survey_get_status(cerberus_wifi_defense_status_payload_t *s){memset(s,0,sizeof(*s));}
void cerberus_ble_scanner_get_status(cerberus_ble_scan_status_payload_t *s){memset(s,0,sizeof(*s));}
void cerberus_ble_connection_get_status(cerberus_ble_connection_status_payload_t *s){memset(s,0,sizeof(*s));}
bool cerberus_wifi_defense_survey_is_running(void){return false;}
bool cerberus_wifi_station_running(void){return false;}
static void prepare_frame_response(cerberus_spi_command_t cmd,uint8_t seq,const void *p,size_t n)
{CHECK(cerberus_protocol_prepare_frame(&dispatched,cmd,seq,p,n));++dispatch_replies;}
#include "mesh_bridge_dispatch.inc"

static int64_t bridge_s3_time(void){return s3_clock;}
static int take_link_mutex(unsigned wait){CHECK(wait==20U&&!link_locked);link_locked=1;return pdTRUE;}
static void give_link_mutex(void){CHECK(link_locked);link_locked=0;}
static esp_err_t get_status_v2_passive_locked(unsigned serial,uint32_t deadline,cerberus_c5_status_t *out)
{
    CHECK(serial&&deadline&&link_locked);++refreshes;++wire_calls;s3_clock+=100;
    memset(out,0,sizeof(*out));out->firmware_version=firmware;return ESP_OK;
}
static esp_err_t spi_request_response_passive(const void *tx,void *discarded,void *rx,size_t size,uint32_t deadline)
{
    CHECK(tx&&discarded&&rx&&size==sizeof(cerberus_spi_frame_t)&&deadline&&link_locked);
    const cerberus_spi_frame_t *request=tx;CHECK(cerberus_protocol_frame_is_valid(request));
    ++commands[request->command];++wire_calls;last_command=request->command;
    if(!first_command)first_command=request->command;
    unsigned before=dispatch_replies;mesh_dispatch(request);CHECK(dispatch_replies==before+1U);
    CHECK(cerberus_protocol_frame_is_valid(&dispatched));s3_clock+=100;
    if(drop_response[request->command]){--drop_response[request->command];return ESP_ERR_TIMEOUT;}
    memcpy(rx,&dispatched,sizeof(dispatched));return ESP_OK;
}
/* The real shared framed exchange validates CRC, command, sequence and size. */
#include "mesh_bridge_frame.inc"
#define esp_timer_get_time bridge_s3_time
#include "c5_mesh_link_impl.h"
#undef esp_timer_get_time

static esp_err_t exchange(void *context,uint8_t command,const void *request,size_t n,void *out,size_t size)
{CHECK(context==&controller);return c5_spi_link_mesh_exchange(command,request,n,out,size);}
static int64_t controller_clock(void *context){CHECK(context==&controller);return s3_clock;}
static const mesh_scan_transport_t transport={exchange,controller_clock,&controller};
static void pump(void)
{
    if(mesh_context&&!notifications)notifications=1U;
    if(!setjmp(idle))worker(NULL);
    CHECK(!c5_locked&&!mesh_mux.depth);
}
static void step(unsigned ms)
{
    tick_ms+=ms;s3_clock+=(int64_t)ms*1000;source_clock+=(int64_t)ms*1000;
    pump();unsigned before=wire_calls;first_command=0U;
    mesh_scan_tick(&controller,tick_ms,&transport);
    CHECK(wire_calls-before<=MESH_SCAN_TICK_CALLS);CHECK(!link_locked);pump();
}
static void begin(void)
{
    CHECK(!mesh_context&&allocated==freed&&!rf_enabled);
    mesh_scan_init(&controller);memset(commands,0,sizeof(commands));memset(drop_response,0,sizeof(drop_response));
    firmware=CERBERUS_MESH_OBSERVATION_MIN_FIRMWARE;peer_firmware_version=0U;
    receive_failure=ESP_OK;foreign_wifi=allocation_failure=false;held_disable=0U;
    CHECK(mesh_scan_begin(&controller,CERBERUS_MESH_OBSERVATION_FIXED,1U,100U)==ESP_OK);
    step(1U);CHECK(controller.accepted&&mesh_context&&rx_active);
    CHECK(commands[CERBERUS_MESH_OBSERVATION_COMMAND_START]==1U);
    CHECK(current_radio_state()&CERBERUS_C5_RADIO_IEEE802154_SCAN);
}
static void emit(unsigned kind,unsigned serial)
{
    uint8_t raw[125]={0};size_t length;
    if(kind==0U){
        const uint8_t short_frame[]={0x41,0x88,0x21,0x34,0x12,0x78,0x56,0xbc,0x9a};
        memcpy(raw,short_frame,sizeof(short_frame));length=sizeof(short_frame);raw[2]=(uint8_t)serial;
    }else if(kind==1U){
        const uint8_t extended[]={0x01,0xec,0x33,0x34,0x12,
            0xef,0xcd,0xab,0x89,0x67,0x45,0x23,0x01,
            0x10,0x32,0x54,0x76,0x98,0xba,0xdc,0xfe};
        memcpy(raw,extended,sizeof(extended));length=sizeof(extended);
    }else if(kind==2U){raw[0]=0x41;raw[1]=0x88;length=2U;}
    else if(kind==3U){raw[0]=4U;length=3U;}
    else {raw[0]=0x41;raw[1]=0x88;length=125U;}
    uint8_t frame[128]={0};frame[0]=(uint8_t)(length+2U);memcpy(frame+1U,raw,length);
    esp_ieee802154_frame_info_t metadata={.channel=rf_channel,.rssi=-49,.lqi=201U};
    unsigned before=returned;esp_ieee802154_receive_done(frame,&metadata);CHECK(returned==before+1U);
}
static void finish(bool expect_frozen)
{
    for(unsigned i=0U;i<4000U&&!mesh_scan_released(&controller)&&
        (!controller.faulted||controller.close_requested);++i)step(25U);
    if(!mesh_scan_released(&controller)||mesh_context){
        fprintf(stderr,"finish phase=%u error=%d fault=%d requested=%d seq=%lu ack=%lu records=%lu remote=%u/%u native=%p nativephase=%u count=%u last=%lu acked=%lu\n",
            controller.phase,controller.error,controller.faulted,controller.requested,
            (unsigned long)controller.sequence,(unsigned long)controller.acked_sequence,
            (unsigned long)controller.model.counters.records,controller.remote.state,controller.remote.flags,
            (void*)mesh_context,mesh_context?mesh_context->phase:0U,mesh_context?mesh_context->count:0U,
            (unsigned long)(mesh_context?mesh_context->summary.last_sequence:0U),
            (unsigned long)(mesh_context?mesh_context->summary.acked:0U));
    }
    CHECK(mesh_scan_released(&controller)&&!mesh_context&&allocated==freed&&!rf_enabled);
    CHECK(controller.frozen==expect_frozen);CHECK(!(current_radio_state()&CERBERUS_C5_RADIO_IEEE802154_SCAN));
}
static void complete_bank(void)
{
    begin();unsigned before=returned;
    for(unsigned i=0U;i<20U;++i)emit(i<4U?i:0U,i);
    CHECK(returned==before+20U&&mesh_context->count==16U&&mesh_context->summary.queue_drops==4U);
    /* Stop takes precedence over an in-progress multipart record. */
    for(unsigned i=0U;i<100U&&controller.phase!=MESH_SCAN_READ_ROW;++i)step(25U);
    CHECK(controller.phase==MESH_SCAN_READ_ROW&&controller.part<16U);
    mesh_scan_stop(&controller);step(25U);CHECK(first_command==CERBERUS_MESH_OBSERVATION_COMMAND_STOP);
    finish(true);
    CHECK(controller.model.counters.records==16U&&controller.model.counters.malformed==1U&&controller.model.counters.unsupported==1U);
    CHECK(controller.final_summary.callbacks==20U&&controller.final_summary.queue_drops==4U);
    CHECK(controller.final_summary.enqueued==16U&&controller.final_summary.acked==16U&&controller.final_summary.last_sequence==16U);
    CHECK(controller.model.counters.replayed==0U&&controller.model.counters.delivery_sequence_gaps==0U);
    bool short_found=false,extended_found=false;
    for(unsigned i=0U;i<controller.model.address_count;++i){
        const mesh_observation_address_t *a=&controller.model.addresses[i];
        if(a->key.mode==MESH_MAC_ADDRESS_SHORT16&&a->key.value==0x9abcU){
            CHECK(a->key.pan.known&&a->key.pan.value==0x1234U&&a->source_rssi==-49);short_found=true;
        }
        if(a->key.mode==MESH_MAC_ADDRESS_EXT64&&a->key.value==UINT64_C(0xfedcba9876543210)){
            CHECK(!a->key.pan.known&&a->source_frames==1U);extended_found=true;
        }
    }
    CHECK(short_found&&extended_found&&controller.model.edge_count==2U);
    CHECK(controller.model.witness_count==16U&&controller.model.witnesses[0].input.mac[0]==0x41U);
}
static void response_losses(void)
{
    begin();emit(0U,1U);emit(1U,2U);
    drop_response[CERBERUS_MESH_OBSERVATION_COMMAND_READ]=1U;
    drop_response[CERBERUS_MESH_OBSERVATION_COMMAND_ACK]=1U;
    drop_response[CERBERUS_MESH_OBSERVATION_COMMAND_RELEASE]=1U;
    mesh_scan_stop(&controller);finish(true);
    CHECK(controller.model.counters.records==2U&&controller.model.counters.replayed==0U);
    CHECK(commands[CERBERUS_MESH_OBSERVATION_COMMAND_ACK]==3U);
    CHECK(commands[CERBERUS_MESH_OBSERVATION_COMMAND_READ]>=2U*16U+15U+1U);
    CHECK(commands[CERBERUS_MESH_OBSERVATION_COMMAND_RELEASE]>=2U);
    CHECK(controller.final_summary.enqueued==2U&&controller.final_summary.acked==2U);
    CHECK(!drop_response[151]&&!drop_response[152]&&!drop_response[153]);
}
static void ambiguity_errors_and_close(void)
{
    CHECK(!mesh_context);mesh_scan_init(&controller);memset(commands,0,sizeof(commands));
    CHECK(mesh_scan_begin(&controller,0U,1U,100U)==ESP_OK);
    drop_response[CERBERUS_MESH_OBSERVATION_COMMAND_START]=1U;step(1U);
    CHECK(mesh_context&&!controller.accepted&&controller.start_attempted);
    for(unsigned i=0U;i<100U&&!controller.accepted;++i)step(25U);
    CHECK(controller.accepted&&commands[147]==1U&&commands[148]>0U);
    emit(0U,1U);mesh_scan_stop(&controller);finish(true);
    begin();emit(0U,1U);
    /* Real SDK failure on a later fixed-channel rearm preserves prior evidence. */
    receive_failure=ESP_FAIL;source_clock=(int64_t)mesh_context->dwell_started_us+100000;pump();receive_failure=ESP_OK;
    CHECK(mesh_context->phase==MESH_STOPPED&&mesh_context->primary_error==ESP_FAIL);
    finish(true);CHECK(controller.final_summary.error_code==ESP_FAIL&&controller.model.counters.records==1U);
    begin();emit(0U,1U);
    cerberus_mesh_observation_ack_t wrong={mesh_session,2U};cerberus_mesh_observation_control_t error;
    CHECK(c5_spi_link_mesh_exchange(152,&wrong,sizeof(wrong),&error,sizeof(error))==ESP_OK);
    CHECK(error.error_code==ESP_ERR_NOT_FOUND&&!error.sequence&&mesh_context->count==1U);
    held_disable=3U;mesh_scan_close(&controller);step(25U);
    CHECK(mesh_context&&cerberus_ieee802154_scanner_is_running()&&!mesh_context->rx_channel);
    finish(false);CHECK(controller.model.counters.records==0U);
    CHECK(!mesh_context);mesh_scan_init(&controller);memset(commands,0,sizeof(commands));
    firmware=71U;unsigned prior=enables,allocs=allocated;
    CHECK(mesh_scan_begin(&controller,0U,1U,100U)==ESP_OK);step(1U);
    CHECK(mesh_scan_released(&controller)&&controller.error==ESP_ERR_NOT_SUPPORTED);
    CHECK(!commands[146]&&!commands[147]&&enables==prior&&allocated==allocs);
    firmware=72U;
    mesh_scan_init(&controller);CHECK(mesh_scan_begin(&controller,0U,1U,100U)==ESP_OK);
    mesh_scan_stop(&controller);prior=wire_calls;step(1U);CHECK(wire_calls==prior&&mesh_scan_released(&controller));
}
int main(void)
{
    CHECK(cerberus_ieee802154_scanner_init()==ESP_OK&&created==1U);
    complete_bank();response_losses();ambiguity_errors_and_close();
    CHECK(allocated==freed&&!mesh_context&&!c5_locked&&!link_locked&&!mesh_mux.depth&&created==1U);
    printf("Actual C5 ISR/worker/dispatch to S3 framed adapter/controller/model: %lu checks PASS; scan=%zu context=%zu\n",
        checks,sizeof(controller),sizeof(mesh_observation_context_t));
    return 0;
}
