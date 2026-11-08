#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "esp_err.h"
#include "c5_spi_link.h"
#include "wifi_capture_plan.h"
#define pdTRUE 1
#define pdMS_TO_TICKS(n) (n)
static int sentinel;
static void *link_mutex=&sentinel;
static bool locked,fail_lock,rewind_clock;
static uint8_t sequence;
static uint32_t peer_firmware_version,firmware;
static unsigned gate_calls,wire_calls,gate_delay,wire_delay;
static uint32_t gate_deadline,wire_deadline;
static int64_t clock_us;
static esp_err_t gate_error,wire_error,remote_error;
enum {GOOD,BAD_CRC,BAD_TOKEN,BAD_COMMAND,BAD_LENGTH,BAD_GENERATION,SHORT_REPLY,ZERO_ERROR};
static unsigned damage;
static cerberus_wifi_capture_plan_status_payload_t plan_reply;
static cerberus_wifi_capture_hop_status_payload_t hop_reply;
static cerberus_spi_frame_t last_request;
static int64_t esp_timer_get_time(void){return clock_us;}
static int take_link_mutex(unsigned ticks)
{assert(ticks==20&&!locked);if(fail_lock)return 0;locked=true;return 1;}
static void give_link_mutex(void){assert(locked);locked=false;}
static esp_err_t get_status_v2_passive_locked(uint8_t token,uint32_t deadline,cerberus_c5_status_t *out)
{
    assert(locked&&token);++gate_calls;gate_deadline=deadline;
    memset(out,0,sizeof(*out));out->firmware_version=firmware;
    clock_us+=gate_delay;return gate_error;
}
static esp_err_t spi_request_response_passive(const void *data,void *discarded,
    void *output,size_t size,uint32_t deadline)
{
    (void)discarded;assert(locked&&size==sizeof(cerberus_spi_frame_t));
    const cerberus_spi_frame_t *q=data;cerberus_spi_frame_t *r=output;
    assert(cerberus_protocol_frame_is_valid(q));last_request=*q;++wire_calls;wire_deadline=deadline;
    uint8_t bytes[22];size_t length=sizeof(bytes);
    if(q->command==CERBERUS_SPI_COMMAND_WIFI_CAPTURE_HOP_STATUS){
        assert(q->payload_length==4);
        if(remote_error||damage==ZERO_ERROR){
            cerberus_wifi_capture_hop_error_payload_t failure={hop_reply.generation,remote_error};
            memcpy(bytes,&failure,sizeof(failure));length=sizeof(failure);
        }else memcpy(bytes,&hop_reply,sizeof(hop_reply));
    }else{
        assert((q->command==CERBERUS_SPI_COMMAND_WIFI_CAPTURE_PLAN&&q->payload_length==16)||
            (q->command==CERBERUS_SPI_COMMAND_WIFI_CAPTURE_TARGET&&q->payload_length==11));
        cerberus_wifi_capture_plan_status_payload_t p=plan_reply;p.error_code=remote_error;
        memcpy(bytes,&p,sizeof(p));
    }
    if(damage==BAD_GENERATION)++bytes[0];
    if(damage==BAD_LENGTH)--length;
    if(damage==SHORT_REPLY)length=8;
    assert(cerberus_protocol_prepare_frame(r,damage==BAD_COMMAND?CERBERUS_SPI_COMMAND_STATUS:(cerberus_spi_command_t)q->command,
        q->sequence+(damage==BAD_TOKEN),bytes,length));
    if(damage==BAD_CRC)r->crc16^=1;
    clock_us=rewind_clock?clock_us-1:clock_us+wire_delay;return wire_error;
}
#include "capture_plan_gate_impl.h"
#include "c5_capture_plan_link_impl.h"
static void reset(void)
{
    assert(!locked);link_mutex=&sentinel;fail_lock=rewind_clock=false;sequence=0;
    peer_firmware_version=0;firmware=86;gate_calls=wire_calls=gate_delay=wire_delay=0;
    clock_us=1000000;gate_error=wire_error=remote_error=ESP_OK;damage=GOOD;
    plan_reply=(cerberus_wifi_capture_plan_status_payload_t){.generation=100,
        .channel_mask=cerberus_wifi_capture_plan_channel_bit(1)|cerberus_wifi_capture_plan_channel_bit(177),
        .state=CERBERUS_WIFI_CAPTURE_PLAN_READY};
    hop_reply=(cerberus_wifi_capture_hop_status_payload_t){.generation=100,
        .attempted_mask=plan_reply.channel_mask,.applied_mask=cerberus_wifi_capture_plan_channel_bit(1),.completed_sweeps=3};
}
static void zeroed(const void *p,size_t size)
{const uint8_t *bytes=p;for(size_t i=0;i<size;++i)assert(!bytes[i]);}
int main(void)
{
    reset();cerberus_wifi_capture_plan_request_t plan={.generation=100,.channel_mask=plan_reply.channel_mask,.dwell_ms=250};
    cerberus_wifi_capture_target_request_t target={.generation=100,.index=1,.bssid={2,3,4,5,6,7}};
    cerberus_wifi_capture_plan_status_payload_t status;
    cerberus_wifi_capture_hop_status_payload_t hop;
    firmware=85;
    assert(c5_spi_link_wifi_capture_plan_stage(&plan,&status)==ESP_ERR_NOT_SUPPORTED);
    assert(c5_spi_link_wifi_capture_plan_status(&status)==ESP_ERR_NOT_SUPPORTED);
    assert(c5_spi_link_wifi_capture_plan_target(&target,&status)==ESP_ERR_NOT_SUPPORTED);
    assert(c5_spi_link_wifi_capture_hop_status(100,&hop)==ESP_ERR_NOT_SUPPORTED);
    assert(gate_calls==4&&!wire_calls);zeroed(&status,sizeof(status));zeroed(&hop,sizeof(hop));
    reset();plan.dwell_ms=2000U;
    assert(c5_spi_link_wifi_capture_plan_stage(&plan,&status)==ESP_ERR_NOT_SUPPORTED);
    assert(gate_calls==1&&!wire_calls);zeroed(&status,sizeof(status));
    firmware=87U;
    assert(c5_spi_link_wifi_capture_plan_stage(&plan,&status)==ESP_OK);
    assert(gate_calls==2&&wire_calls==1&&!memcmp(last_request.payload,&plan,sizeof(plan)));
    /* A cached newer capability cannot authorize a downgraded peer. */
    firmware=86U;
    assert(c5_spi_link_wifi_capture_plan_stage(&plan,&status)==ESP_ERR_NOT_SUPPORTED);
    assert(gate_calls==3&&wire_calls==1);zeroed(&status,sizeof(status));
    plan.dwell_ms=1999U;
    assert(c5_spi_link_wifi_capture_plan_stage(&plan,&status)==ESP_ERR_INVALID_ARG&&gate_calls==3);
    reset();plan.dwell_ms=100U;firmware=87U;
    assert(c5_spi_link_wifi_capture_plan_stage(&plan,&status)==ESP_ERR_NOT_SUPPORTED);
    assert(gate_calls==1&&!wire_calls);zeroed(&status,sizeof(status));
    firmware=88U;
    assert(c5_spi_link_wifi_capture_plan_stage(&plan,&status)==ESP_OK);
    assert(gate_calls==2&&wire_calls==1&&!memcmp(last_request.payload,&plan,sizeof(plan)));
    firmware=87U;
    assert(c5_spi_link_wifi_capture_plan_stage(&plan,&status)==ESP_ERR_NOT_SUPPORTED);
    assert(gate_calls==3&&wire_calls==1);zeroed(&status,sizeof(status));
    plan.dwell_ms=250U;
    reset();gate_delay=5000;wire_delay=14000;
    assert(c5_spi_link_wifi_capture_plan_stage(&plan,&status)==ESP_OK);
    assert(!memcmp(last_request.payload,&plan,sizeof(plan))&&gate_deadline==wire_deadline&&!locked);
    assert(c5_spi_link_wifi_capture_plan_status(&status)==ESP_OK);
    zeroed(last_request.payload,16);
    assert(c5_spi_link_wifi_capture_hop_status(100,&hop)==ESP_OK&&hop.completed_sweeps==3);
    plan_reply.target_count=2;plan_reply.target_present_mask=3;
    assert(c5_spi_link_wifi_capture_plan_target(&target,&status)==ESP_OK);
    assert(!memcmp(last_request.payload,&target,sizeof(target))&&gate_calls==4&&wire_calls==4);
    /* Every malformed wire reply is rejected without returning prior fields. */
    for(unsigned fault=BAD_CRC;fault<=SHORT_REPLY;++fault){
        reset();damage=fault;memset(&status,0xa5,sizeof(status));
        assert(c5_spi_link_wifi_capture_plan_stage(&plan,&status)==ESP_ERR_INVALID_RESPONSE);
        zeroed(&status,sizeof(status));assert(wire_calls==1&&!locked);
    }
    for(unsigned fault=0;fault<7;++fault){
        reset();
        if(fault==0)plan_reply.state=3;if(fault==1)plan_reply.channel_mask=UINT64_C(1)<<42;
        if(fault==2)plan_reply.target_count=33;if(fault==3)plan_reply.generation=0;
        if(fault==4)plan_reply.target_present_mask=1;
        if(fault==5)plan_reply.state=CERBERUS_WIFI_CAPTURE_PLAN_STAGING;
        if(fault==6)plan_reply.channel_mask=cerberus_wifi_capture_plan_channel_bit(6);
        assert(c5_spi_link_wifi_capture_plan_stage(&plan,&status)==ESP_ERR_INVALID_RESPONSE);
        zeroed(&status,sizeof(status));
    }
    reset();memset(&plan_reply,0,sizeof(plan_reply));
    assert(c5_spi_link_wifi_capture_plan_status(&status)==ESP_OK&&!status.generation);
    for(unsigned count=1;count<=32;++count){
        reset();plan.target_count=count;plan_reply.target_count=count;plan_reply.state=CERBERUS_WIFI_CAPTURE_PLAN_STAGING;
        assert(c5_spi_link_wifi_capture_plan_stage(&plan,&status)==ESP_OK);
        plan_reply.target_present_mask=count==32?UINT32_MAX:(UINT32_C(1)<<count)-1;
        plan_reply.state=CERBERUS_WIFI_CAPTURE_PLAN_READY;
        assert(c5_spi_link_wifi_capture_plan_stage(&plan,&status)==ESP_OK);
    }
    plan.target_count=0;
    reset();remote_error=ESP_ERR_INVALID_STATE;plan_reply.generation=99;
    assert(c5_spi_link_wifi_capture_plan_stage(&plan,&status)==ESP_ERR_INVALID_STATE);zeroed(&status,sizeof(status));
    reset();remote_error=ESP_ERR_NOT_FOUND;
    assert(c5_spi_link_wifi_capture_hop_status(100,&hop)==ESP_ERR_NOT_FOUND);zeroed(&hop,sizeof(hop));
    damage=BAD_GENERATION;assert(c5_spi_link_wifi_capture_hop_status(100,&hop)==ESP_ERR_INVALID_RESPONSE);
    reset();damage=ZERO_ERROR;assert(c5_spi_link_wifi_capture_hop_status(100,&hop)==ESP_ERR_INVALID_RESPONSE);
    reset();hop_reply.applied_mask=cerberus_wifi_capture_plan_channel_bit(6);
    assert(c5_spi_link_wifi_capture_hop_status(100,&hop)==ESP_ERR_INVALID_RESPONSE);
    reset();hop_reply.attempted_mask=UINT64_C(1)<<42;
    assert(c5_spi_link_wifi_capture_hop_status(100,&hop)==ESP_ERR_INVALID_RESPONSE);
    reset();gate_delay=10000;wire_delay=10000;
    assert(c5_spi_link_wifi_capture_plan_stage(&plan,&status)==ESP_ERR_TIMEOUT&&!locked);zeroed(&status,sizeof(status));
    reset();rewind_clock=true;assert(c5_spi_link_wifi_capture_plan_stage(&plan,&status)==ESP_ERR_TIMEOUT);
    reset();fail_lock=true;assert(c5_spi_link_wifi_capture_plan_stage(&plan,&status)==ESP_ERR_TIMEOUT&&!wire_calls&&!gate_calls);
    reset();gate_error=ESP_ERR_TIMEOUT;assert(c5_spi_link_wifi_capture_plan_stage(&plan,&status)==ESP_ERR_TIMEOUT&&!wire_calls);
    reset();wire_error=ESP_ERR_TIMEOUT;assert(c5_spi_link_wifi_capture_plan_stage(&plan,&status)==ESP_ERR_TIMEOUT&&wire_calls==1);
    reset();clock_us=-1;assert(c5_spi_link_wifi_capture_plan_stage(&plan,&status)==ESP_ERR_INVALID_STATE&&!gate_calls);
    reset();plan.reserved=1;assert(c5_spi_link_wifi_capture_plan_stage(&plan,&status)==ESP_ERR_INVALID_ARG&&!gate_calls);plan.reserved=0;
    target.index=32;assert(c5_spi_link_wifi_capture_plan_target(&target,&status)==ESP_ERR_INVALID_ARG&&!gate_calls);
    assert(c5_spi_link_wifi_capture_hop_status(0,&hop)==ESP_ERR_INVALID_ARG&&!gate_calls);
    puts("S3 planned capture link: fresh capability86/87/88 dwell gate, exact full frames/identity/readiness,32target masks, structured hop errors,20ms budget, no replay and cleared failures passed.");
    return 0;
}
