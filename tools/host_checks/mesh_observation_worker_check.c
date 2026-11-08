/* Actual C5 scanner, receive callback, shared task and extracted SPI dispatch.
 * SDK/task boundaries are deterministic models, not radio/latency evidence. */
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

static unsigned long checks;
#define CHECK(x) do{++checks;if(!(x)){fprintf(stderr,"mesh worker line %d: %s\n",__LINE__,#x);exit(1);}}while(0)
static jmp_buf yielded;
static void (*task_fn)(void *);
static unsigned notifications,locked,wait_budget,created,allocations,frees,allocation_calls;
static unsigned enables,disables,receives,returned;
static int64_t source_us=1000000;
static bool allocation_fail,foreign_wifi,foreign_ble,foreign_test,foreign_station;
static bool enabled,receiving,continuous,auto_ack_rx,auto_ack_tx,release_during_receive;
static bool callback_inside_receive;
static void (*locate_receive_hook)(void);
static unsigned stop_on_take;
static esp_err_t enable_error,receive_error,sleep_error,idle_error,disable_error;
static unsigned disable_failures;
static uint8_t channel;
static cerberus_spi_frame_t reply;
static unsigned replies;
static void offer(unsigned length,unsigned marker);

static void prepare_frame_response(cerberus_spi_command_t command,uint8_t sequence,
    const void *payload,size_t length)
{
    CHECK(length<=22U);memset(&reply,0,sizeof(reply));reply.command=command;
    reply.sequence=sequence;reply.payload_length=(uint8_t)length;
    memcpy(reply.payload,payload,length);++replies;
}
#include "mesh_dispatch.inc"

void *heap_caps_calloc(size_t n,size_t size,unsigned caps)
{
    CHECK(!locked&&!mesh_mux.depth&&caps==3U);++allocation_calls;
    if(allocation_fail)return NULL;
    void *p=calloc(n,size);CHECK(p!=NULL);++allocations;return p;
}
void heap_caps_free(void *p){CHECK(p&&!locked&&!mesh_mux.depth);++frees;free(p);}
uint32_t esp_random(void){return UINT32_C(0x56781234);}
int64_t esp_timer_get_time(void){return source_us;}
SemaphoreHandle_t xSemaphoreCreateMutex(void){return (void*)1;}
void vSemaphoreDelete(SemaphoreHandle_t h){CHECK(h!=NULL);}
int xSemaphoreTake(SemaphoreHandle_t h,unsigned wait)
{
    (void)wait;CHECK(h&&!locked);
    if(stop_on_take&&!--stop_on_take){
        cerberus_mesh_observation_control_t out;
        CHECK(cerberus_mesh_observation_stop(mesh_session,&out)==ESP_OK);
    }
    ++locked;return pdTRUE;
}
int xSemaphoreGive(SemaphoreHandle_t h){CHECK(h&&locked==1U);--locked;return pdTRUE;}
QueueHandle_t xQueueCreate(unsigned n,unsigned size){CHECK(n==8U&&size>0U);return (void*)2;}
void vQueueDelete(QueueHandle_t h){CHECK(h!=NULL);}
int xQueueReceive(QueueHandle_t h,void *out,unsigned wait){CHECK(h&&out);source_us+=(int64_t)wait*1000;return pdFALSE;}
int xQueueSendFromISR(QueueHandle_t h,const void *in,BaseType_t *wake){CHECK(h&&in&&wake);return pdTRUE;}
int xQueueReset(QueueHandle_t h){CHECK(h!=NULL);return pdTRUE;}
int xTaskCreate(void(*fn)(void*),const char *name,unsigned stack,void *arg,unsigned priority,TaskHandle_t *out)
{CHECK(name&&stack==4096U&&!arg&&priority==4U);task_fn=fn;*out=(void*)3;++created;return pdPASS;}
void xTaskNotifyGive(TaskHandle_t h){CHECK(h==(void*)3);++notifications;}
unsigned ulTaskNotifyTake(int clear,unsigned wait)
{
    CHECK(clear==pdTRUE&&!locked&&!mesh_mux.depth);
    if(wait==portMAX_DELAY){if(!notifications)longjmp(yielded,1);notifications=0U;return 1U;}
    CHECK(wait==20U||wait==100U);
    if(!wait_budget--)longjmp(yielded,1);
    if(notifications){notifications=0U;return 1U;}
    source_us+=(int64_t)wait*1000;return 0U;
}
TickType_t xTaskGetTickCount(void){return (TickType_t)(source_us/1000);}
void vTaskDelay(unsigned n){CHECK(!locked&&!mesh_mux.depth);source_us+=(int64_t)n*1000;}
esp_err_t esp_ieee802154_enable(void)
{CHECK(!locked&&!mesh_mux.depth);++enables;enabled=true;continuous=false;auto_ack_rx=auto_ack_tx=true;return enable_error;}
esp_err_t esp_ieee802154_disable(void)
{
    CHECK(!locked&&!mesh_mux.depth);++disables;enabled=receiving=false;
    if(disable_failures){--disable_failures;return ESP_FAIL;}
    return disable_error;
}
esp_err_t esp_ieee802154_sleep(void){CHECK(!locked&&!mesh_mux.depth);receiving=false;return sleep_error;}
esp_err_t esp_ieee802154_set_channel(uint8_t c){CHECK(!locked&&!mesh_mux.depth&&c>=11U&&c<=26U);channel=c;return ESP_OK;}
esp_err_t esp_ieee802154_set_coordinator(bool value){CHECK(!locked&&!mesh_mux.depth&&!value);return ESP_OK;}
esp_err_t esp_ieee802154_set_promiscuous(bool value){CHECK(!locked&&!mesh_mux.depth);auto_ack_rx=auto_ack_tx=!value;return ESP_OK;}
esp_err_t esp_ieee802154_set_rx_when_idle(bool value){CHECK(!locked&&!mesh_mux.depth);continuous=value;return value?idle_error:ESP_OK;}
esp_err_t esp_ieee802154_receive(void)
{
    CHECK(!locked&&!mesh_mux.depth&&!auto_ack_rx&&!auto_ack_tx&&continuous);++receives;
    receiving=receive_error==ESP_OK;
    if (locate_context) {
        cerberus_mesh_locate_control_t pending;
        CHECK(cerberus_mesh_locate_status(locate_session,&pending)==ESP_OK);
        CHECK(pending.state==CERBERUS_MESH_LOCATE_QUEUED&&!(pending.flags&CERBERUS_MESH_LOCATE_RX_ACTIVE));
        if (locate_receive_hook) locate_receive_hook();
        return receive_error;
    }
    cerberus_mesh_observation_control_t pending;
    CHECK(cerberus_mesh_observation_status(mesh_session,&pending)==ESP_OK);
    CHECK(pending.state==CERBERUS_MESH_OBSERVATION_QUEUED&&!(pending.flags&CERBERUS_MESH_OBSERVATION_RX_ACTIVE));
    if(callback_inside_receive){callback_inside_receive=false;offer(3U,0x40U);}
    if(release_during_receive){
        release_during_receive=false;cerberus_mesh_observation_control_t out;
        CHECK(cerberus_mesh_observation_release(mesh_session,&out)==ESP_OK);
        CHECK(out.flags&CERBERUS_MESH_OBSERVATION_HELD);
        unsigned retained=mesh_context->count;offer(3U,0x50U);
        CHECK(mesh_context->count==retained); /* Late callback after close is returned, not admitted. */
    }
    return receive_error;
}
esp_err_t esp_ieee802154_receive_handle_done(uint8_t *frame){(void)frame;++returned;return ESP_OK;}
esp_err_t esp_ieee802154_energy_detect(uint32_t duration){CHECK(duration==3125U);esp_ieee802154_energy_detect_done(-77);return ESP_OK;}
bool cerberus_c5_test_running(void){return foreign_test;}
void cerberus_wifi_scanner_get_status(cerberus_wifi_scan_status_payload_t *s){memset(s,0,sizeof(*s));s->state=foreign_wifi?CERBERUS_WIFI_SCAN_RUNNING:0U;}
void cerberus_wifi_connection_get_status(cerberus_wifi_connection_status_payload_t *s){memset(s,0,sizeof(*s));}
void cerberus_wifi_defense_survey_get_status(cerberus_wifi_defense_status_payload_t *s){memset(s,0,sizeof(*s));}
void cerberus_ble_scanner_get_status(cerberus_ble_scan_status_payload_t *s){memset(s,0,sizeof(*s));s->state=foreign_ble?CERBERUS_BLE_SCAN_RUNNING:0U;}
void cerberus_ble_connection_get_status(cerberus_ble_connection_status_payload_t *s){memset(s,0,sizeof(*s));}
bool cerberus_wifi_defense_survey_is_running(void){return false;}
bool cerberus_wifi_station_running(void){return foreign_station;}

static void pump(unsigned waits)
{
    wait_budget=waits;if((mesh_context||locate_context)&&!notifications)notifications=1U;
    if(!setjmp(yielded))task_fn(NULL);
    CHECK(!locked&&!mesh_mux.depth);
}
static cerberus_mesh_observation_control_t start(unsigned mask,unsigned mode)
{
    cerberus_mesh_observation_start_t q={mesh_epoch+1U,(uint16_t)mask,100U,(uint8_t)mode};
    cerberus_mesh_observation_control_t out;
    CHECK(cerberus_mesh_observation_start(&q,&out)==ESP_OK);
    CHECK(out.session_id==q.session_id&&out.state==CERBERUS_MESH_OBSERVATION_QUEUED);
    CHECK((out.flags&(CERBERUS_MESH_OBSERVATION_BOUND|CERBERUS_MESH_OBSERVATION_HELD))==3U);
    CHECK(current_radio_state()&CERBERUS_C5_RADIO_IEEE802154_SCAN);
    return out;
}
static void offer(unsigned length,unsigned marker)
{
    uint8_t frame[128]={0};frame[0]=(uint8_t)(length+2U);
    for(unsigned i=0U;i<length&&i<125U;++i)frame[i+1U]=(uint8_t)(marker+i);
    esp_ieee802154_frame_info_t info={.channel=channel,.rssi=-53,.lqi=142U};
    unsigned before=returned;esp_ieee802154_receive_done(frame,&info);CHECK(returned==before+1U);
}
static void release_current(void)
{
    uint64_t session=mesh_session;cerberus_mesh_observation_control_t out;
    CHECK(cerberus_mesh_observation_release(session,&out)==ESP_OK);
    pump(5U);CHECK(!mesh_context&&!cerberus_ieee802154_scanner_is_running());
    CHECK(!(current_radio_state()&CERBERUS_C5_RADIO_IEEE802154_SCAN));
    CHECK(cerberus_mesh_observation_release(session,&out)==ESP_OK);
    CHECK(out.session_id==session&&!out.flags&&!out.error_code&&!out.version);
    CHECK(cerberus_mesh_observation_start(&mesh_request,&out)==ESP_ERR_NOT_FOUND);
}
static void read_record(uint32_t sequence,cerberus_mesh_observation_record_t *record)
{
    memset(record,0xA5,sizeof(*record));
    for(unsigned part=0U;part<CERBERUS_MESH_OBSERVATION_RECORD_PARTS;++part){
        cerberus_mesh_observation_read_t q={mesh_session,sequence,CERBERUS_MESH_OBSERVATION_ROW,(uint8_t)part};
        cerberus_mesh_observation_chunk_t out;
        CHECK(cerberus_mesh_observation_read(&q,&out)==ESP_OK);
        CHECK(out.session_id==mesh_session&&out.sequence==sequence&&out.part==part);
        unsigned offset=part*9U,n=sizeof(*record)-offset;if(n>9U)n=9U;
        memcpy((uint8_t*)record+offset,out.data,n);
        for(unsigned j=n;j<9U;++j)CHECK(out.data[j]==0U);
    }
}
static cerberus_mesh_observation_summary_t read_summary(void)
{
    cerberus_mesh_observation_peek_t q={mesh_session,CERBERUS_MESH_OBSERVATION_SUMMARY};
    cerberus_mesh_observation_control_t control;CHECK(cerberus_mesh_observation_peek(&q,&control)==ESP_OK);
    CHECK(control.sequence>CERBERUS_MESH_OBSERVATION_SUMMARY_BASE);
    uint32_t pin=control.sequence;cerberus_mesh_observation_summary_t value;
    for(unsigned part=0;part<CERBERUS_MESH_OBSERVATION_SUMMARY_PARTS;++part){
        cerberus_mesh_observation_read_t read={mesh_session,pin,CERBERUS_MESH_OBSERVATION_SUMMARY,(uint8_t)part};
        cerberus_mesh_observation_chunk_t out;CHECK(cerberus_mesh_observation_read(&read,&out)==ESP_OK);
        CHECK(out.sequence==pin);unsigned offset=part*9U,n=sizeof(value)-offset;if(n>9U)n=9U;
        memcpy((uint8_t*)&value+offset,out.data,n);for(unsigned j=n;j<9U;++j)CHECK(out.data[j]==0U);
    }
    return value;
}
static void admission_and_wire(void)
{
    cerberus_mesh_observation_control_t out;cerberus_mesh_observation_info(&out);
    CHECK(out.session_id==mesh_epoch&&out.capacity==16U&&out.version==1U&&!out.flags);
    cerberus_mesh_observation_start_t q={mesh_epoch+1U,1U,100U,0U};
    allocation_fail=true;unsigned before=enables;
    CHECK(cerberus_mesh_observation_start(&q,&out)==ESP_ERR_NO_MEM);
    CHECK(enables==before&&!mesh_context);allocation_fail=false;
    bool *owners[]={&foreign_wifi,&foreign_ble,&foreign_test,&foreign_station};
    for(unsigned i=0;i<4U;++i){*owners[i]=true;CHECK(cerberus_mesh_observation_start(&q,&out)==ESP_ERR_INVALID_STATE);*owners[i]=false;CHECK(!mesh_context);}
    CHECK(cerberus_ieee802154_scanner_start()==ESP_OK);cerberus_mesh_observation_info(&out);
    CHECK(!(out.flags&CERBERUS_MESH_OBSERVATION_BOUND)&&(out.flags&CERBERUS_MESH_OBSERVATION_HELD));
    CHECK(cerberus_mesh_observation_start(&q,&out)==ESP_ERR_INVALID_STATE);pump(0U);
    CHECK(scan_status.state==CERBERUS_IEEE802154_ENERGY_SCAN_COMPLETE);
    q.channel_mask=3U;CHECK(cerberus_mesh_observation_start(&q,&out)==ESP_ERR_INVALID_ARG);q.channel_mask=1U;
    q.dwell_ms=99U;CHECK(cerberus_mesh_observation_start(&q,&out)==ESP_ERR_INVALID_ARG);
    start(1U,0U);unsigned allocated=allocation_calls;
    CHECK(cerberus_mesh_observation_start(&mesh_request,&out)==ESP_OK&&allocation_calls==allocated);
    CHECK(cerberus_ieee802154_scanner_start()==ESP_ERR_INVALID_STATE);
    CHECK(cerberus_ieee802154_beacon_scanner_start()==ESP_ERR_INVALID_STATE);
    CHECK(cerberus_ieee802154_activity_scanner_start()==ESP_ERR_INVALID_STATE);
    uint64_t lease=mesh_context->lease_us;source_us+=1000;
    CHECK(cerberus_mesh_observation_start(&mesh_request,&out)==ESP_OK&&mesh_context->lease_us==lease);
    CHECK(cerberus_mesh_observation_stop(mesh_session+1U,&out)==ESP_ERR_NOT_FOUND&&!mesh_context->cancelled);
    cerberus_spi_frame_t request={.command=CERBERUS_MESH_OBSERVATION_COMMAND_STATUS,.sequence=71U,.payload_length=8U};
    memcpy(request.payload,&mesh_session,8U);mesh_dispatch(&request);
    CHECK(reply.command==request.command&&reply.sequence==71U&&reply.payload_length==22U);
    memcpy(&out,reply.payload,22U);CHECK(out.session_id==mesh_session&&out.flags&CERBERUS_MESH_OBSERVATION_BOUND);
    unsigned sent=replies;request.payload_length=7U;mesh_dispatch(&request);CHECK(sent==replies);
    /* Cancel queued work: no SDK startup, no new task, no lost attachment. */
    before=enables;CHECK(cerberus_mesh_observation_stop(mesh_session,&out)==ESP_OK);pump(0U);
    CHECK(enables==before&&mesh_context->phase==MESH_STOPPED&&out.flags&CERBERUS_MESH_OBSERVATION_HELD);
    release_current();
}
static void queue_and_pins(void)
{
    start(1U,0U);pump(0U);CHECK(receiving&&channel==11U&&continuous&&!auto_ack_tx&&!auto_ack_rx);
    cerberus_mesh_observation_control_t control;cerberus_mesh_observation_peek_t peek={mesh_session,0U};
    CHECK(cerberus_mesh_observation_peek(&peek,&control)==ESP_OK&&!control.sequence);
    for(unsigned i=0;i<20U;++i)offer(i==0U?125U:3U,20U+i);
    offer(126U,0U);offer(1U,0U);
    CHECK(mesh_context->count==16U&&mesh_context->summary.queue_drops==4U&&mesh_context->summary.invalid_callbacks==2U);
    CHECK(cerberus_mesh_observation_peek(&peek,&control)==ESP_OK&&control.sequence==1U);
    cerberus_mesh_observation_record_t first,again;read_record(1U,&first);
    CHECK(first.mac_length==125U&&first.flags==7U&&first.rssi==-53&&first.lqi==142U&&first.channel==11U);
    for(unsigned i=0;i<125U;++i)CHECK(first.mac[i]==(uint8_t)(20U+i));
    source_us+=1000;offer(3U,88U);read_record(1U,&again);CHECK(!memcmp(&first,&again,sizeof(first)));
    for(unsigned part=16U;part<256U;++part){
        cerberus_mesh_observation_read_t q={mesh_session,1U,0U,(uint8_t)part};cerberus_mesh_observation_chunk_t out;
        CHECK(cerberus_mesh_observation_read(&q,&out)==ESP_ERR_INVALID_ARG&&!out.sequence);
        for(unsigned i=4U;i<9U;++i)CHECK(!out.data[i]);
    }
    cerberus_mesh_observation_summary_t summary=read_summary();uint32_t pin=mesh_context->summary_pin;
    CHECK(summary.callbacks==23U&&summary.enqueued==16U&&summary.queue_drops==5U&&summary.invalid_callbacks==2U);
    cerberus_mesh_observation_read_t wrong={mesh_session,pin,0U,0U};cerberus_mesh_observation_chunk_t bad;
    CHECK(cerberus_mesh_observation_read(&wrong,&bad)==ESP_ERR_NOT_FOUND);
    wrong.sequence=1U;wrong.kind=1U;CHECK(cerberus_mesh_observation_read(&wrong,&bad)==ESP_ERR_NOT_FOUND);
    CHECK(cerberus_mesh_observation_status(mesh_session,&control)==ESP_OK&&mesh_context->summary_pin==pin);
    cerberus_mesh_observation_ack_t ack={mesh_session,1U};CHECK(cerberus_mesh_observation_ack(&ack,&control)==ESP_OK&&control.sequence==1U);
    CHECK(cerberus_mesh_observation_ack(&ack,&control)==ESP_OK&&mesh_context->count==15U&&mesh_context->summary.acked==1U);
    offer(3U,55U);CHECK(mesh_context->count==16U&&mesh_context->summary.last_sequence==17U);
    ack.sequence=3U;CHECK(cerberus_mesh_observation_ack(&ack,&control)==ESP_ERR_NOT_FOUND&&mesh_context->count==16U&&!control.sequence);
    CHECK(cerberus_mesh_observation_stop(mesh_session,&control)==ESP_OK);offer(3U,99U);
    CHECK(mesh_context->summary.callbacks==24U);pump(0U);
    CHECK(!enabled&&!receiving&&mesh_context->phase==MESH_STOPPED&&cerberus_ieee802154_scanner_is_running());
    for(unsigned seq=2U;seq<=17U;++seq){ack.sequence=seq;CHECK(cerberus_mesh_observation_ack(&ack,&control)==ESP_OK);}
    summary=read_summary();CHECK(summary.enqueued==summary.acked&&summary.acked==summary.last_sequence);
    CHECK(summary.callbacks==summary.enqueued+summary.invalid_callbacks+summary.queue_drops);
    CHECK(summary.flags&CERBERUS_MESH_OBSERVATION_STOPPED);
    release_current();
}
static void cleanup_expiry_and_faults(void)
{
    cerberus_mesh_observation_control_t out;
    callback_inside_receive=true;start(1U,0U);pump(0U);
    CHECK(mesh_context->count==1U&&mesh_context->summary.callbacks==1U);
    CHECK(mesh_context->summary.started_us<=mesh_context->slots[0].record.callback_us);
    CHECK(mesh_context->summary.tuned_mask==1U);
    source_us=(int64_t)mesh_context->dwell_started_us+100000;
    stop_on_take=2U;pump(0U);
    CHECK(!stop_on_take&&mesh_context->phase==MESH_STOPPED&&!mesh_context->summary.completed_dwell_ms[0]);
    release_current();
    start(3U,1U);pump(7U);CHECK(mesh_context->summary.tuned_mask==3U);
    CHECK(mesh_context->summary.completed_dwell_ms[0]>=100U);offer(3U,1U);
    disable_failures=3U;CHECK(cerberus_mesh_observation_release(mesh_session,&out)==ESP_OK);
    unsigned before=frees;pump(0U);CHECK(mesh_context&&frees==before&&mesh_context->cleanup_error==ESP_FAIL);
    CHECK(cerberus_ieee802154_scanner_is_running()&&!mesh_context->rx_channel);
    pump(10U);CHECK(!mesh_context&&frees==before+1U);
    /* No SPI traffic is needed for lease expiry, RF cleanup or retained expiry. */
    start(1U,0U);pump(0U);source_us=(int64_t)mesh_context->lease_us;
    pump(0U);CHECK(mesh_context->phase==MESH_STOPPED&&!enabled);
    CHECK(mesh_context->primary_error==ESP_ERR_TIMEOUT);
    source_us=(int64_t)mesh_context->retention_us;pump(0U);CHECK(!mesh_context);
    start(1U,0U);release_during_receive=true;pump(2U);CHECK(!mesh_context&&!enabled);
    enable_error=ESP_FAIL;start(1U,0U);pump(0U);
    CHECK(mesh_context->phase==MESH_STOPPED&&mesh_context->primary_error==ESP_FAIL);
    CHECK(!(mesh_context->summary.flags&CERBERUS_MESH_OBSERVATION_START_TIME_VALID));
    enable_error=ESP_OK;release_current();
    receive_error=ESP_FAIL;start(1U,0U);pump(0U);CHECK(mesh_context->primary_error==ESP_FAIL);
    CHECK(cerberus_mesh_observation_status(mesh_session,&out)==ESP_OK);
    CHECK(out.state==CERBERUS_MESH_OBSERVATION_ERROR&&!out.channel&&!(out.flags&CERBERUS_MESH_OBSERVATION_RX_ACTIVE));
    receive_error=ESP_OK;release_current();
    start(1U,0U);pump(0U);offer(3U,0U);
    source_us-=1;offer(3U,0U);CHECK(mesh_context->cancelled&&mesh_context->count==2U);
    CHECK(!(mesh_context->slots[1].record.flags&CERBERUS_MESH_OBSERVATION_TIME_VALID)&&!mesh_context->slots[1].record.callback_us);
    source_us+=2;pump(0U);CHECK(mesh_context->phase==MESH_STOPPED);
    CHECK(mesh_context->summary.flags&CERBERUS_MESH_OBSERVATION_SUMMARY_CLOCK_INVALID);release_current();
    start(1U,0U);pump(0U);
    /* Boundary injection changes the counter, not the production callback route. */
    mesh_context->summary.last_sequence=CERBERUS_MESH_OBSERVATION_SEQUENCE_MAX-1U;
    mesh_context->summary.enqueued=mesh_context->summary.acked=CERBERUS_MESH_OBSERVATION_SEQUENCE_MAX-1U;
    mesh_context->summary.callbacks=CERBERUS_MESH_OBSERVATION_SEQUENCE_MAX-1U;
    offer(3U,0U);CHECK(mesh_context->cancelled&&mesh_context->summary.last_sequence==CERBERUS_MESH_OBSERVATION_SEQUENCE_MAX);
    cerberus_mesh_observation_ack_t ack={mesh_session,CERBERUS_MESH_OBSERVATION_SEQUENCE_MAX};
    CHECK(cerberus_mesh_observation_ack(&ack,&out)==ESP_OK);pump(0U);
    cerberus_mesh_observation_summary_t summary=read_summary();CHECK(summary.enqueued==summary.acked&&summary.callbacks==summary.enqueued);
    release_current();
    start(1U,0U);pump(0U);mesh_context->summary_pin_counter=UINT32_MAX;
    cerberus_mesh_observation_peek_t peek={mesh_session,1U};CHECK(cerberus_mesh_observation_peek(&peek,&out)==ESP_ERR_INVALID_STATE);
    CHECK(mesh_context->cancelled&&!mesh_context->rx_channel);release_current();
    uint64_t original=mesh_epoch;mesh_epoch=UINT64_MAX;
    cerberus_mesh_observation_start_t q={1U,1U,100U,0U};CHECK(cerberus_mesh_observation_start(&q,&out)==ESP_ERR_INVALID_STATE);
    mesh_epoch=original;CHECK(!mesh_context);
}
static void retention_and_summary_fences(void)
{
    cerberus_mesh_observation_control_t control;
    start(1U,0U);pump(0U);offer(3U,1U);
    uint64_t old_session=mesh_session;
    cerberus_mesh_observation_peek_t peek={mesh_session,1U};
    CHECK(cerberus_mesh_observation_peek(&peek,&control)==ESP_OK);
    uint32_t pin=control.sequence;
    cerberus_mesh_observation_summary_t frozen=mesh_context->pinned_summary;
    offer(3U,2U);
    CHECK(cerberus_mesh_observation_status(mesh_session,&control)==ESP_OK);
    CHECK(!memcmp(&frozen,&mesh_context->pinned_summary,sizeof(frozen))&&mesh_context->summary_pin==pin);
    for(unsigned part=15U;part<256U;++part){
        cerberus_mesh_observation_read_t q={mesh_session,pin,1U,(uint8_t)part};
        cerberus_mesh_observation_chunk_t out;CHECK(cerberus_mesh_observation_read(&q,&out)==ESP_ERR_INVALID_ARG);
        CHECK(!out.sequence);for(unsigned i=4U;i<9U;++i)CHECK(!out.data[i]);
    }
    CHECK(cerberus_mesh_observation_stop(mesh_session,&control)==ESP_OK);pump(0U);
    uint64_t deadline=mesh_context->retention_us;source_us=(int64_t)deadline-1000;
    cerberus_mesh_observation_info(&control);CHECK(mesh_context->retention_us==deadline);
    cerberus_mesh_observation_read_t bad={mesh_session,77U,0U,0U};cerberus_mesh_observation_chunk_t chunk;
    CHECK(cerberus_mesh_observation_read(&bad,&chunk)==ESP_ERR_NOT_FOUND&&mesh_context->retention_us==deadline);
    bad.sequence=1U;CHECK(cerberus_mesh_observation_read(&bad,&chunk)==ESP_OK&&mesh_context->retention_us>deadline);
    deadline=mesh_context->retention_us;source_us=(int64_t)deadline;
    CHECK(cerberus_mesh_observation_status(mesh_session,&control)==ESP_OK);
    CHECK(mesh_context->retiring&&mesh_context->retention_us==deadline&&!(control.flags&CERBERUS_MESH_OBSERVATION_DATA_READY));
    pump(0U);CHECK(!mesh_context);
    start(1U,0U);pump(0U);offer(3U,3U);
    bad.session_id=old_session;CHECK(cerberus_mesh_observation_read(&bad,&chunk)==ESP_ERR_NOT_FOUND);
    cerberus_mesh_observation_ack_t ack={old_session,1U};
    CHECK(cerberus_mesh_observation_ack(&ack,&control)==ESP_ERR_NOT_FOUND&&mesh_context->count==1U);
    CHECK(cerberus_mesh_observation_release(old_session,&control)==ESP_ERR_NOT_FOUND&&!mesh_context->cancelled);
    release_current();
    /* Counter saturation flags are reciprocal, distinct from row sequence limit. */
    start(1U,0U);pump(0U);
    mesh_context->summary.callbacks=mesh_context->summary.invalid_callbacks=UINT32_MAX-1U;
    offer(1U,0U);CHECK(mesh_context->summary.callbacks==UINT32_MAX&&mesh_context->summary.invalid_callbacks==UINT32_MAX);
    CHECK(cerberus_mesh_observation_stop(mesh_session,&control)==ESP_OK);pump(0U);
    cerberus_mesh_observation_summary_t summary=read_summary();
    CHECK(summary.flags&CERBERUS_MESH_OBSERVATION_COUNTERS_SATURATED);
    CHECK(!summary.enqueued&&!summary.acked&&!summary.last_sequence);
    release_current();
    start(1U,0U);pump(0U);
    mesh_context->primary_error=ESP_FAIL;source_us=(int64_t)mesh_context->lease_us;
    pump(0U);CHECK(mesh_context->primary_error==ESP_FAIL);release_current();
    int64_t clock_before=source_us;source_us=-1;
    cerberus_mesh_observation_start_t request={mesh_epoch+1U,1U,100U,0U};
    unsigned before=enables;CHECK(cerberus_mesh_observation_start(&request,&control)==ESP_ERR_INVALID_STATE&&!mesh_context&&enables==before);
    source_us=clock_before;
}
int main(void)
{
    CHECK(cerberus_ieee802154_scanner_init()==ESP_OK&&created==1U);
    admission_and_wire();queue_and_pins();cleanup_expiry_and_faults();retention_and_summary_fences();
    CHECK(allocations==frees&&created==1U&&!locked&&!mesh_mux.depth);
    printf("mesh actual worker/ISR/dispatch: %lu checks PASS; slot=%zu context=%zu bytes\n",checks,
        sizeof(mesh_observation_slot_t),sizeof(mesh_observation_context_t));
    return 0;
}
