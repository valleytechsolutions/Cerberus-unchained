#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "esp_err.h"
#include "wifi_channel_analyzer.h"
#include "c5_spi_link.h"

#define pdTRUE 1
#define pdMS_TO_TICKS(n) (n)
static int sentinel;
static void *link_mutex=&sentinel;
static bool locked, fail_lock, rewind_clock;
static uint8_t sequence;
static uint32_t peer_firmware_version, firmware=82;
static unsigned gate_calls, wire_calls, gate_delay, wire_delay;
static uint32_t gate_deadline, wire_deadline;
static int64_t clock_us;
static esp_err_t gate_error, wire_error, remote_error;
enum { GOOD, BAD_CRC, BAD_TOKEN, BAD_COMMAND, BAD_LENGTH, BAD_SESSION, BAD_KEY,
    BAD_TARGET, BAD_COUNT, BAD_WINDOW, BAD_FLAGS, BAD_PART, BAD_PADDING, SHORT_CONTROL, ZERO_ERROR };
static unsigned damage;

static int64_t esp_timer_get_time(void) { return clock_us; }
static int take_link_mutex(unsigned ticks)
{ assert(ticks==20 && !locked); if(fail_lock) return 0; locked=true; return pdTRUE; }
static void give_link_mutex(void) { assert(locked); locked=false; }
static esp_err_t get_status_v2_passive_locked(uint8_t token, uint32_t deadline, cerberus_c5_status_t *out)
{
    (void)token; assert(locked); ++gate_calls; gate_deadline=deadline; clock_us+=gate_delay;
    memset(out,0,sizeof(*out)); out->firmware_version=(uint8_t)firmware;
    return gate_error;
}
static esp_err_t spi_request_response_passive(const void *data, void *discarded,
    void *output, size_t size, uint32_t deadline)
{
    (void)discarded; assert(locked && size==sizeof(cerberus_spi_frame_t));
    const cerberus_spi_frame_t *q=data; cerberus_spi_frame_t *r=output;
    assert(cerberus_protocol_frame_is_valid(q)); ++wire_calls; wire_deadline=deadline;
    uint8_t bytes[22]={0}; size_t length=sizeof(bytes); uint64_t session=100;
    if(q->payload_length>=8) memcpy(&session,q->payload,sizeof(session));
    if(damage==BAD_SESSION) ++session;
    if(q->command==CERBERUS_SPI_COMMAND_WIFI_CHANNEL_ANALYZER_SAMPLE ||
        q->command==CERBERUS_SPI_COMMAND_WIFI_CHANNEL_ANALYZER_HINT_READ) {
        uint32_t key; memcpy(&key,q->payload+8,sizeof(key));
        if(damage==BAD_KEY) ++key;
        if(remote_error || damage==ZERO_ERROR) {
            cerberus_wifi_channel_analyzer_read_error_t failure={session,key,remote_error};
            memcpy(bytes,&failure,sizeof(failure)); length=sizeof(failure);
        } else if(q->command==CERBERUS_SPI_COMMAND_WIFI_CHANNEL_ANALYZER_SAMPLE) {
            cerberus_wifi_channel_analyzer_sample_t sample={.session_id=session,.sequence=key,
                .frames=20,.window_us=damage==BAD_WINDOW ? 99999U : 100000U,
                .channel=36,.flags=damage==BAD_FLAGS ? 1U : 0U};
            memcpy(bytes,&sample,sizeof(sample));
        } else {
            cerberus_wifi_channel_analyzer_hint_part_t part={.session_id=session,.projection=key,
                .part=q->payload[12]+(damage==BAD_PART ? 1U : 0U)};
            if(damage==BAD_PADDING) part.bytes[8]=1;
            memcpy(bytes,&part,sizeof(part));
        }
    } else if(q->command==CERBERUS_SPI_COMMAND_WIFI_CHANNEL_ANALYZER_HINT_PEEK) {
        cerberus_wifi_channel_analyzer_hint_status_t hint={.session_id=session,.projection=1,
            .age_ms=10,.length=42,.flags=1}; memcpy(bytes,&hint,sizeof(hint));
    } else {
        cerberus_wifi_channel_analyzer_status_t control={.session_id=session};
        if(q->command!=CERBERUS_SPI_COMMAND_WIFI_CHANNEL_ANALYZER_INFO) {
            control.channel=q->command==CERBERUS_SPI_COMMAND_WIFI_CHANNEL_ANALYZER_START ? q->payload[8] : 36;
            control.target_ms=damage==BAD_TARGET ? 200U : 100U;
            control.state=CERBERUS_WIFI_PACKET_MONITOR_RUNNING; control.flags=3;
            control.stored_count=damage==BAD_COUNT ? 1 : 0;
        }
        memcpy(bytes,&control,sizeof(control));
    }
    if(damage==BAD_LENGTH) --length;
    if(damage==SHORT_CONTROL) length=16;
    assert(cerberus_protocol_prepare_frame(r,
        damage==BAD_COMMAND ? CERBERUS_SPI_COMMAND_STATUS : (cerberus_spi_command_t)q->command,
        q->sequence+(damage==BAD_TOKEN ? 1U : 0U),bytes,length));
    if(damage==BAD_CRC) r->crc16^=1;
    clock_us=rewind_clock ? clock_us-1 : clock_us+wire_delay;
    return wire_error;
}
#include "analyzer_gate_impl.h"
#include "wifi_channel_analyzer_link.h"

static void reset(void)
{
    link_mutex=&sentinel; locked=fail_lock=rewind_clock=false; sequence=0;
    peer_firmware_version=0; firmware=82; gate_calls=wire_calls=gate_delay=wire_delay=0;
    clock_us=1000000; gate_error=wire_error=remote_error=ESP_OK; damage=GOOD;
}
static void zeroed(const void *p,size_t size)
{ const uint8_t *bytes=p; for(size_t i=0;i<size;++i) assert(!bytes[i]); }

int main(void)
{
    cerberus_wifi_channel_analyzer_status_t control;
    cerberus_wifi_channel_analyzer_sample_t sample;
    cerberus_wifi_channel_analyzer_hint_status_t hint;
    cerberus_wifi_channel_analyzer_hint_part_t part;
    reset(); firmware=81;
    assert(c5_spi_link_wifi_channel_analyzer_info(&control)==ESP_ERR_NOT_SUPPORTED);
    assert(gate_calls==1 && !wire_calls); zeroed(&control,sizeof(control));
    assert(c5_spi_link_wifi_channel_analyzer_start(101,36,&control)==ESP_ERR_NOT_SUPPORTED);
    assert(gate_calls==2 && !wire_calls);
    assert(c5_spi_link_wifi_channel_analyzer_sample(101,1,&sample)==ESP_ERR_NOT_SUPPORTED && !wire_calls);

    reset(); gate_delay=5000; wire_delay=14000;
    assert(c5_spi_link_wifi_channel_analyzer_start(101,36,&control)==ESP_OK);
    assert(control.session_id==101 && control.channel==36 && gate_calls==1 && wire_calls==1);
    assert(gate_deadline==wire_deadline && !locked);
    assert(c5_spi_link_wifi_channel_analyzer_status(101,&control)==ESP_OK && gate_calls==1);
    assert(c5_spi_link_wifi_channel_analyzer_stop(101,&control)==ESP_OK);
    assert(c5_spi_link_wifi_channel_analyzer_sample(101,UINT32_MAX,&sample)==ESP_OK);
    assert(sample.sequence==UINT32_MAX && sample.frames==20 && sample.window_us==100000);
    assert(c5_spi_link_wifi_channel_analyzer_hint_peek(101,&hint)==ESP_OK && hint.projection==1);
    assert(c5_spi_link_wifi_channel_analyzer_hint_read(101,3,4,&part)==ESP_OK && part.projection==3);

    reset(); gate_delay=10000; wire_delay=10000;
    assert(c5_spi_link_wifi_channel_analyzer_start(101,36,&control)==ESP_ERR_TIMEOUT);
    assert(wire_calls==1); zeroed(&control,sizeof(control)); /* No replay. */
    reset(); rewind_clock=true;
    assert(c5_spi_link_wifi_channel_analyzer_start(101,36,&control)==ESP_ERR_TIMEOUT);
    zeroed(&control,sizeof(control));
    reset(); fail_lock=true; memset(&control,0xff,sizeof(control));
    assert(c5_spi_link_wifi_channel_analyzer_info(&control)==ESP_ERR_TIMEOUT);
    assert(!gate_calls && !wire_calls); zeroed(&control,sizeof(control));
    reset(); gate_error=ESP_ERR_TIMEOUT;
    assert(c5_spi_link_wifi_channel_analyzer_start(101,36,&control)==ESP_ERR_TIMEOUT && !wire_calls);
    reset(); wire_error=ESP_ERR_TIMEOUT;
    assert(c5_spi_link_wifi_channel_analyzer_start(101,36,&control)==ESP_ERR_TIMEOUT && wire_calls==1);
    zeroed(&control,sizeof(control));

    const unsigned frame_damage[]={BAD_CRC,BAD_TOKEN,BAD_COMMAND,BAD_LENGTH,BAD_SESSION,BAD_TARGET,BAD_COUNT,SHORT_CONTROL};
    for(unsigned i=0;i<sizeof(frame_damage)/sizeof(frame_damage[0]);++i) {
        reset(); damage=frame_damage[i];
        assert(c5_spi_link_wifi_channel_analyzer_start(101,36,&control)==ESP_ERR_INVALID_RESPONSE);
        assert(wire_calls==1 && !locked); zeroed(&control,sizeof(control));
    }
    const unsigned sample_damage[]={BAD_SESSION,BAD_KEY,BAD_WINDOW,BAD_FLAGS};
    for(unsigned i=0;i<sizeof(sample_damage)/sizeof(sample_damage[0]);++i) {
        reset(); damage=sample_damage[i];
        assert(c5_spi_link_wifi_channel_analyzer_sample(101,1,&sample)==ESP_ERR_INVALID_RESPONSE);
        zeroed(&sample,sizeof(sample));
    }
    reset(); remote_error=ESP_ERR_NOT_FOUND;
    assert(c5_spi_link_wifi_channel_analyzer_sample(101,1,&sample)==ESP_ERR_NOT_FOUND);
    zeroed(&sample,sizeof(sample));
    assert(c5_spi_link_wifi_channel_analyzer_hint_read(101,1,4,&part)==ESP_ERR_NOT_FOUND);
    zeroed(&part,sizeof(part));
    damage=BAD_KEY;
    assert(c5_spi_link_wifi_channel_analyzer_sample(101,1,&sample)==ESP_ERR_INVALID_RESPONSE);
    damage=BAD_SESSION;
    assert(c5_spi_link_wifi_channel_analyzer_hint_read(101,1,4,&part)==ESP_ERR_INVALID_RESPONSE);
    damage=ZERO_ERROR; remote_error=ESP_OK;
    assert(c5_spi_link_wifi_channel_analyzer_sample(101,1,&sample)==ESP_ERR_INVALID_RESPONSE);
    reset(); damage=BAD_PART;
    assert(c5_spi_link_wifi_channel_analyzer_hint_read(101,1,4,&part)==ESP_ERR_INVALID_RESPONSE);
    zeroed(&part,sizeof(part)); damage=BAD_PADDING;
    assert(c5_spi_link_wifi_channel_analyzer_hint_read(101,1,4,&part)==ESP_ERR_INVALID_RESPONSE);
    zeroed(&part,sizeof(part));

    reset(); assert(c5_spi_link_wifi_channel_analyzer_start(0,36,&control)==ESP_ERR_INVALID_ARG);
    assert(c5_spi_link_wifi_channel_analyzer_start(101,0,&control)==ESP_ERR_INVALID_ARG);
    assert(c5_spi_link_wifi_channel_analyzer_sample(101,0,&sample)==ESP_ERR_INVALID_ARG);
    assert(c5_spi_link_wifi_channel_analyzer_hint_read(101,0,0,&part)==ESP_ERR_INVALID_ARG);
    assert(c5_spi_link_wifi_channel_analyzer_hint_read(101,1,5,&part)==ESP_ERR_INVALID_ARG);
    assert(!gate_calls && !wire_calls);
    puts("Channel Analyzer SPI: cap82 gate, bounded operations, exact identity and malformed replies passed");
    return 0;
}
