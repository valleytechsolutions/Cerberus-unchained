/* Actual C5 RX/service/dispatcher -> actual S3 wire adapter/controller.
 * Only SDK, scheduler, mutex and SPI electrical transport are replaced. */
#define main native_service_suite
#include "probe_sniff_native_check.c"
#undef main
#include "c5_spi_link.h"
#include "probe_sniff_scan.h"
static int link_sentinel;
static void *link_mutex=&link_sentinel;
static bool link_locked,lose_start_reply,reboot_before_start;
static uint8_t sequence;
static uint32_t peer_firmware_version,firmware=71;
static unsigned exchanges,starts_sent,stops_sent,reads_sent,fault,fail_read_part=UINT32_MAX;
static bool wrong_final_count;
static cerberus_probe_sniff_scan_t client;
#define pdTRUE 1
static int take_link_mutex(unsigned timeout)
{CHECK(timeout==20U&&!link_locked);link_locked=true;return pdTRUE;}
static void give_link_mutex(void){CHECK(link_locked);link_locked=false;}
static esp_err_t get_status_v2_passive_locked(uint8_t token,uint32_t deadline,cerberus_c5_status_t*out)
{(void)token;(void)deadline;CHECK(link_locked);++exchanges;memset(out,0,sizeof(*out));out->firmware_version=firmware;return ESP_OK;}
static esp_err_t spi_request_response_passive(const void*data,void*discarded,void*out,size_t size,uint32_t deadline)
{
    (void)discarded;CHECK(link_locked&&size==sizeof(cerberus_spi_frame_t)&&deadline>=(uint64_t)now_us/1000U);
    const cerberus_spi_frame_t*q=data;cerberus_spi_frame_t*r=out;++exchanges;
    CHECK(cerberus_protocol_frame_is_valid(q));
    if(q->command==CERBERUS_PROBE_SNIFF_COMMAND_START){++starts_sent;
        if(reboot_before_start){CHECK(!probe_sniff_context);probe_sniff_epoch+=10000U;reboot_before_start=false;}}
    if(q->command==CERBERUS_PROBE_SNIFF_COMMAND_STOP)++stops_sent;
    if(q->command==CERBERUS_PROBE_SNIFF_COMMAND_READ){++reads_sent;cerberus_probe_sniff_read_t request;memcpy(&request,q->payload,sizeof(request));
        if(request.part==fail_read_part){fail_read_part=UINT32_MAX;return ESP_ERR_TIMEOUT;}}
    dispatch(q);CHECK(response_size==22U);
    if(wrong_final_count&&q->command==CERBERUS_PROBE_SNIFF_COMMAND_STATUS&&client.phase==CERBERUS_PROBE_SCAN_AFTER){
        cerberus_probe_sniff_control_t c;memcpy(&c,response_bytes,22);CHECK(c.count>0U);--c.count;
        memcpy(response_bytes,&c,22);wrong_final_count=false;}
    CHECK(cerberus_protocol_prepare_frame(r,(cerberus_spi_command_t)response_command,response_sequence,response_bytes,response_size));
    if(q->command==CERBERUS_PROBE_SNIFF_COMMAND_START&&lose_start_reply){lose_start_reply=false;return ESP_ERR_TIMEOUT;}
    if(fault){unsigned f=fault;fault=0;
        if(f==1)r->sequence++;else if(f==2)r->command++;else if(f==3)r->payload_length--;
        else if(f==4)r->payload[0]^=1U;
        else if(f==5&&q->command==CERBERUS_PROBE_SNIFF_COMMAND_READ){cerberus_probe_sniff_chunk_t c;memcpy(&c,r->payload,22);c.pin++;memcpy(r->payload,&c,22);}
        else if(f==6&&q->command==CERBERUS_PROBE_SNIFF_COMMAND_READ)r->payload[21]=1U;
        /* Except f4 (bad CRC), recalculate CRC to test semantic framing. */
        if(f!=4){uint8_t payload[22];memcpy(payload,r->payload,22);CHECK(cerberus_protocol_prepare_frame(r,(cerberus_spi_command_t)r->command,r->sequence,payload,r->payload_length));}}
    return ESP_OK;
}
#include "probe_bridge_frame.h"
#include "c5_probe_sniff_link_impl.h"
static esp_err_t control_bridge(void*ctx,uint8_t cmd,const cerberus_probe_sniff_start_t*q,cerberus_probe_sniff_control_t*r)
{(void)ctx;return c5_spi_link_probe_sniff_control(cmd,q,r);}
static esp_err_t read_bridge(void*ctx,const cerberus_probe_sniff_read_t*q,cerberus_probe_sniff_chunk_t*r)
{(void)ctx;return c5_spi_link_probe_sniff_read(q,r);}
static int64_t bridge_clock(void*ctx){(void)ctx;return now_us;}
static const cerberus_probe_sniff_transport_t bridge={control_bridge,read_bridge,bridge_clock,NULL};
static void bridge_step(unsigned ms)
{tick(ms);unsigned before=exchanges;cerberus_probe_sniff_scan_tick(&client,(uint64_t)now_us/1000U,&bridge);CHECK(exchanges-before<=4U);}
static void bridge_reset(void)
{reset();cerberus_probe_sniff_scan_init(&client);firmware=71;peer_firmware_version=0;exchanges=starts_sent=stops_sent=reads_sent=fault=0;
 fail_read_part=UINT32_MAX;lose_start_reply=reboot_before_start=wrong_final_count=false;}
static void bridge_start(void)
{CHECK(cerberus_probe_sniff_scan_begin(&client,CERBERUS_PROBE_SNIFF_FIXED,6)==ESP_OK);bridge_step(0);CHECK(probe_sniff_context);bridge_step(20);CHECK(sdk_receiving);}
static void bridge_publish(bool frozen)
{uint64_t previous=client.publication;for(unsigned i=0;i<2000U&&(client.publication==previous||client.published.frozen!=frozen);++i)bridge_step(20);
 CHECK(client.publication>previous&&client.published.frozen==frozen);}
static void bridge_close(void)
{cerberus_probe_sniff_scan_close(&client);for(unsigned i=0;i<2000U&&!cerberus_probe_sniff_scan_released(&client);++i)bridge_step(20);
 CHECK(cerberus_probe_sniff_scan_released(&client)&&!probe_sniff_context&&!sdk_receiving&&!heap_object);}
static void adapter_errors(void)
{
    bridge_reset();firmware=70;CHECK(cerberus_probe_sniff_scan_begin(&client,0,6)==ESP_OK);bridge_step(0);
    CHECK(client.error==ESP_ERR_NOT_SUPPORTED&&!starts_sent&&!probe_sniff_context&&!sdk_calls);
    bridge_reset();reboot_before_start=true;CHECK(cerberus_probe_sniff_scan_begin(&client,0,6)==ESP_OK);bridge_step(0);
    CHECK(!probe_sniff_context&&!client.request.session_id&&!stops_sent&&!sdk_calls);
    bridge_reset();lose_start_reply=true;CHECK(cerberus_probe_sniff_scan_begin(&client,0,6)==ESP_OK);bridge_step(0);
    CHECK(probe_sniff_context&&client.request.session_id&&client.error==ESP_ERR_TIMEOUT);
    bridge_step(700);CHECK(client.have_control&&starts_sent==1U);bridge_close();
    bridge_reset();bridge_start();uint64_t token=client.request.session_id;
    cerberus_probe_sniff_control_t control;cerberus_probe_sniff_start_t request=client.request;
    for(unsigned i=1;i<=4;++i){fault=i;memset(&control,0xa5,sizeof(control));CHECK(c5_spi_link_probe_sniff_control(CERBERUS_PROBE_SNIFF_COMMAND_STATUS,&request,&control)==ESP_ERR_INVALID_RESPONSE);
        CHECK(!memcmp(&control,&(cerberus_probe_sniff_control_t){0},sizeof(control)));CHECK(probe_sniff_session==token&&sdk_receiving);}
    request.session_id--;CHECK(c5_spi_link_probe_sniff_control(CERBERUS_PROBE_SNIFF_COMMAND_STOP,&request,&control)==ESP_OK);
    CHECK(!control.flags&&control.error_code&&probe_sniff_session==token&&sdk_receiving);
    cerberus_probe_sniff_read_t q={.session_id=token};cerberus_probe_sniff_chunk_t chunk;
    CHECK(c5_spi_link_probe_sniff_read(&q,&chunk)==ESP_OK);q.pin=chunk.pin;q.part=1;fault=5;
    CHECK(c5_spi_link_probe_sniff_read(&q,&chunk)==ESP_ERR_INVALID_RESPONSE&&!memcmp(&chunk,&(cerberus_probe_sniff_chunk_t){0},sizeof(chunk)));
    q.part=9;fault=6;CHECK(c5_spi_link_probe_sniff_read(&q,&chunk)==ESP_ERR_INVALID_RESPONSE);
    bridge_close();
}
static void complete_capture(void)
{
    bridge_reset();bridge_start();uint8_t binary[32];for(unsigned i=0;i<32;++i)binary[i]=(uint8_t)(i*8U);
    wifi_promiscuous_pkt_t p;
    for(unsigned i=0;i<32;++i){p=frame(binary,(uint8_t)(i?32:0),(uint8_t)(i+1));packet(&p);}
    p=frame(binary,32,33);packet(&p);CHECK(probe_sniff_context->table.summary.capacity_dropped_frames==1U);
    bridge_publish(false);CHECK(client.published.count==32U&&cerberus_probe_sniff_snapshot_valid(&client.published));
    uint64_t publication=client.publication;fail_read_part=4;
    for(unsigned i=0;i<200U&&fail_read_part!=UINT32_MAX;++i)bridge_step(20);
    CHECK(fail_read_part==UINT32_MAX&&client.publication==publication&&client.error==ESP_ERR_TIMEOUT);
    /* A long capture download receives exact STATUS keepalive without rearming. */
    for(unsigned i=0;i<30;++i)bridge_step(500);
    CHECK(sdk_receiving&&starts_sent==1U&&client.requested);
    cerberus_probe_sniff_scan_stop(&client);unsigned reads=reads_sent;disable_error=ESP_FAIL;bridge_step(0);
    CHECK(stops_sent&&reads_sent==reads&&client.request.session_id);bridge_step(700);
    CHECK(sdk_receiving&&client.request.session_id&&!client.published.frozen);
    disable_error=ESP_OK;wrong_final_count=true;publication=client.publication;
    for(unsigned i=0;i<2000U&&wrong_final_count;++i)bridge_step(20);
    CHECK(!wrong_final_count&&client.publication==publication&&client.error==ESP_ERR_INVALID_RESPONSE);
    bridge_publish(true);
    CHECK(client.published.control.error_code==ESP_FAIL&&client.error==ESP_FAIL);
    CHECK(client.published.count==32U&&client.published.requested_channel==6U&&!client.published.control.channel);
    CHECK(client.published.summary.retained_frames==32U&&client.published.summary.capacity_dropped_frames==1U);
    static char report[8193];size_t length;CHECK(cerberus_probe_sniff_report(&client.published,report,sizeof(report),&length));
    CHECK(strstr(report,"0008101820283038404850586068707880889098A0A8B0B8C0C8D0D8E0E8F0F8"));
    bridge_close();CHECK(client.published.frozen&&cerberus_probe_sniff_snapshot_valid(&client.published));
    /* Reusing the full old ID cannot Stop, Read or rearm a replacement. */
    uint64_t old=client.published.control.session_id;cerberus_probe_sniff_control_t newer=begin(0,6);tick(20);
    cerberus_probe_sniff_start_t q={.session_id=old,.mode=0,.channel=6};cerberus_probe_sniff_control_t r;
    CHECK(c5_spi_link_probe_sniff_control(CERBERUS_PROBE_SNIFF_COMMAND_STOP,&q,&r)==ESP_OK&&r.error_code&&!r.flags&&sdk_receiving);
    CHECK(c5_spi_link_probe_sniff_control(CERBERUS_PROBE_SNIFF_COMMAND_START,&q,&r)==ESP_OK&&r.error_code&&probe_sniff_session==newer.session_id);
    cerberus_probe_sniff_read_t read={.session_id=old};cerberus_probe_sniff_chunk_t chunk;
    CHECK(c5_spi_link_probe_sniff_read(&read,&chunk)!=ESP_OK&&sdk_receiving);dispose(newer.session_id);
}
static void no_capture_and_reset(void)
{
    bridge_reset();CHECK(cerberus_probe_sniff_scan_begin(&client,0,6)==ESP_OK);bridge_step(0);
    cerberus_probe_sniff_scan_stop(&client);unsigned before=exchanges;
    cerberus_probe_sniff_scan_tick(&client,(uint64_t)now_us/1000U,&bridge);CHECK(exchanges-before<=4U);
    bridge_step(700);CHECK(!sdk_receiving&&!client.publication&&!(client.remote.flags&CERBERUS_PROBE_SNIFF_DATA_READY));bridge_close();
    bridge_reset();bridge_start();uint64_t old=client.request.session_id;
    /* Reset retires the old receiver lifetime; an unrelated new job survives old cleanup. */
    dispose(old);probe_sniff_epoch+=10000U;cerberus_probe_sniff_control_t fresh=begin(0,6);tick(20);
    cerberus_probe_sniff_scan_close(&client);bridge_step(0);
    CHECK(cerberus_probe_sniff_scan_released(&client)&&probe_sniff_session==fresh.session_id&&sdk_receiving);dispose(fresh.session_id);
}
int main(void)
{adapter_errors();complete_capture();no_capture_and_reset();CHECK(allocations==frees);printf("Probe actual C5/S3 bridge: %u assertions; no hardware.\n",checks);return 0;}
