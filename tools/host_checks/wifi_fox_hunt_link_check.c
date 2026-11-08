#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "esp_err.h"
#include "c5_spi_link.h"

#define pdTRUE 1
#define pdMS_TO_TICKS(n) (n)
static int sentinel;
static void *link_mutex=&sentinel;
static bool locked, fail_lock, rewind_clock, no_signal;
static uint8_t sequence;
static uint32_t peer_firmware_version, firmware=83;
static unsigned gate_calls, wire_calls, gate_delay, wire_delay;
static uint32_t gate_deadline, wire_deadline;
static int64_t clock_us;
static esp_err_t gate_error, wire_error, remote_error;
enum { GOOD, BAD_CRC, BAD_TOKEN, BAD_COMMAND, BAD_LENGTH, BAD_SESSION, BAD_RSSI,
    BAD_CHANNEL, BAD_FLAGS, SHORT_CONTROL, ZERO_ERROR, BAD_EMPTY };
static unsigned damage;

static int64_t esp_timer_get_time(void) { return clock_us; }
static int take_link_mutex(unsigned ticks)
{ assert(ticks==20&&!locked);if(fail_lock)return 0;locked=true;return pdTRUE; }
static void give_link_mutex(void) { assert(locked);locked=false; }
static esp_err_t get_status_v2_passive_locked(uint8_t token,uint32_t deadline,cerberus_c5_status_t *out)
{
    (void)token;assert(locked);++gate_calls;gate_deadline=deadline;clock_us+=gate_delay;
    memset(out,0,sizeof(*out));out->firmware_version=(uint8_t)firmware;return gate_error;
}
static esp_err_t spi_request_response_passive(const void *data,void *discarded,
    void *output,size_t size,uint32_t deadline)
{
    (void)discarded;assert(locked&&size==sizeof(cerberus_spi_frame_t));
    const cerberus_spi_frame_t *q=data;cerberus_spi_frame_t *r=output;
    assert(cerberus_protocol_frame_is_valid(q));++wire_calls;wire_deadline=deadline;
    uint8_t bytes[22]={0};size_t length=sizeof(bytes);uint64_t session=100;
    if(q->payload_length>=8)memcpy(&session,q->payload,sizeof(session));
    if(damage==BAD_SESSION)++session;
    if(q->command==CERBERUS_SPI_COMMAND_WIFI_FOX_HUNT_SIGNAL) {
        if(remote_error||damage==ZERO_ERROR) {
            cerberus_wifi_fox_hunt_signal_error_t failure={session,remote_error};
            memcpy(bytes,&failure,sizeof(failure));length=sizeof(failure);
        } else {
            cerberus_wifi_fox_hunt_signal_t signal={.session_id=session,.sequence=no_signal?0U:1U,
                .age_ms=no_signal?UINT32_MAX:10,.observed_ms=no_signal?0U:123U,
                .rssi=no_signal?INT8_MIN:-50,.channel=no_signal?0:36};
            if(damage==BAD_RSSI)signal.rssi=21;
            if(damage==BAD_CHANNEL)signal.channel=35;
            if(damage==BAD_EMPTY)signal.age_ms=0;
            memcpy(bytes,&signal,sizeof(signal));
        }
    } else {
        cerberus_wifi_fox_hunt_status_t status={.session_id=session};
        if(q->command!=CERBERUS_SPI_COMMAND_WIFI_FOX_HUNT_INFO) {
            status.current_channel=damage==BAD_CHANNEL?35:36;status.state=1;
            status.flags=damage==BAD_FLAGS?9:11;
        }
        memcpy(bytes,&status,sizeof(status));
    }
    if(damage==BAD_LENGTH)--length;
    if(damage==SHORT_CONTROL)length=12;
    assert(cerberus_protocol_prepare_frame(r,
        damage==BAD_COMMAND?CERBERUS_SPI_COMMAND_STATUS:(cerberus_spi_command_t)q->command,
        q->sequence+(damage==BAD_TOKEN?1U:0U),bytes,length));
    if(damage==BAD_CRC)r->crc16^=1;
    clock_us=rewind_clock?clock_us-1:clock_us+wire_delay;return wire_error;
}
#include "fox_gate_impl.h"
#include "wifi_fox_hunt_link.h"

static void reset(void)
{
    link_mutex=&sentinel;locked=fail_lock=rewind_clock=no_signal=false;sequence=0;
    peer_firmware_version=0;firmware=83;gate_calls=wire_calls=gate_delay=wire_delay=0;
    clock_us=1000000;gate_error=wire_error=remote_error=ESP_OK;damage=GOOD;
}
static void zeroed(const void *p,size_t size)
{ const uint8_t *bytes=p;for(size_t i=0;i<size;++i)assert(!bytes[i]); }

int main(void)
{
    cerberus_wifi_fox_hunt_start_t request={.session_id=101,.target={2,3,4,5,6,7},.initial_channel=36,.search_band=3};
    cerberus_wifi_fox_hunt_status_t control;cerberus_wifi_fox_hunt_signal_t signal;
    reset();firmware=82;
    assert(c5_spi_link_wifi_fox_hunt_info(&control)==ESP_ERR_NOT_SUPPORTED);
    assert(gate_calls==1&&!wire_calls);zeroed(&control,sizeof(control));
    assert(c5_spi_link_wifi_fox_hunt_start(&request,&control)==ESP_ERR_NOT_SUPPORTED);
    assert(gate_calls==2&&!wire_calls);
    assert(c5_spi_link_wifi_fox_hunt_signal(101,&signal)==ESP_ERR_NOT_SUPPORTED&&!wire_calls);

    reset();gate_delay=5000;wire_delay=14000;
    assert(c5_spi_link_wifi_fox_hunt_start(&request,&control)==ESP_OK);
    assert(control.session_id==101&&control.current_channel==36&&gate_calls==1&&wire_calls==1);
    assert(gate_deadline==wire_deadline&&!locked);
    assert(c5_spi_link_wifi_fox_hunt_status(101,&control)==ESP_OK&&gate_calls==1);
    assert(c5_spi_link_wifi_fox_hunt_stop(101,&control)==ESP_OK);
    assert(c5_spi_link_wifi_fox_hunt_signal(101,&signal)==ESP_OK&&signal.rssi==-50&&signal.observed_ms==123);
    no_signal=true;assert(c5_spi_link_wifi_fox_hunt_signal(101,&signal)==ESP_OK&&signal.rssi==INT8_MIN&&!signal.sequence);
    damage=BAD_EMPTY;assert(c5_spi_link_wifi_fox_hunt_signal(101,&signal)==ESP_ERR_INVALID_RESPONSE);
    zeroed(&signal,sizeof(signal));

    reset();gate_delay=10000;wire_delay=10000;
    assert(c5_spi_link_wifi_fox_hunt_start(&request,&control)==ESP_ERR_TIMEOUT&&wire_calls==1);
    zeroed(&control,sizeof(control));
    reset();rewind_clock=true;
    assert(c5_spi_link_wifi_fox_hunt_start(&request,&control)==ESP_ERR_TIMEOUT);zeroed(&control,sizeof(control));
    reset();fail_lock=true;memset(&control,0xff,sizeof(control));
    assert(c5_spi_link_wifi_fox_hunt_info(&control)==ESP_ERR_TIMEOUT&&!gate_calls&&!wire_calls);
    zeroed(&control,sizeof(control));
    reset();gate_error=ESP_ERR_TIMEOUT;
    assert(c5_spi_link_wifi_fox_hunt_start(&request,&control)==ESP_ERR_TIMEOUT&&!wire_calls);
    reset();wire_error=ESP_ERR_TIMEOUT;
    assert(c5_spi_link_wifi_fox_hunt_start(&request,&control)==ESP_ERR_TIMEOUT&&wire_calls==1);
    zeroed(&control,sizeof(control));

    const unsigned faults[]={BAD_CRC,BAD_TOKEN,BAD_COMMAND,BAD_LENGTH,BAD_SESSION,BAD_CHANNEL,BAD_FLAGS,SHORT_CONTROL};
    for(unsigned i=0;i<sizeof(faults)/sizeof(faults[0]);++i) {
        reset();damage=faults[i];
        assert(c5_spi_link_wifi_fox_hunt_start(&request,&control)==ESP_ERR_INVALID_RESPONSE);
        assert(wire_calls==1&&!locked);zeroed(&control,sizeof(control));
    }
    reset();damage=BAD_RSSI;
    for(unsigned i=0;i<3;++i) {
        assert(c5_spi_link_wifi_fox_hunt_signal(101,&signal)==ESP_ERR_INVALID_RESPONSE);
        zeroed(&signal,sizeof(signal));
    }
    reset();remote_error=ESP_ERR_NOT_FOUND;
    assert(c5_spi_link_wifi_fox_hunt_signal(101,&signal)==ESP_ERR_NOT_FOUND);zeroed(&signal,sizeof(signal));
    damage=BAD_SESSION;assert(c5_spi_link_wifi_fox_hunt_signal(101,&signal)==ESP_ERR_INVALID_RESPONSE);
    damage=ZERO_ERROR;remote_error=ESP_OK;
    assert(c5_spi_link_wifi_fox_hunt_signal(101,&signal)==ESP_ERR_INVALID_RESPONSE);

    reset();assert(c5_spi_link_wifi_fox_hunt_start(NULL,&control)==ESP_ERR_INVALID_ARG);
    assert(c5_spi_link_wifi_fox_hunt_status(0,&control)==ESP_ERR_INVALID_ARG);
    assert(c5_spi_link_wifi_fox_hunt_signal(0,&signal)==ESP_ERR_INVALID_ARG);
    request.search_band=1;assert(c5_spi_link_wifi_fox_hunt_start(&request,&control)==ESP_ERR_INVALID_ARG);
    assert(!gate_calls&&!wire_calls);
    puts("Wi-Fi Fox Hunt SPI: cap83 gate, transport budget, no replay and strict signal/error identity passed");
    return 0;
}
