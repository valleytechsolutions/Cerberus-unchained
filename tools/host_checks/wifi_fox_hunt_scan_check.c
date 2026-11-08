#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "wifi_fox_hunt_scan.h"
#include "wifi_fox_hunt_validate.h"

enum { INFO, START, STATUS, STOP, SIGNAL, OPS };
static unsigned calls[OPS];
static esp_err_t errors[OPS];
static cerberus_wifi_fox_hunt_status_t control;
static cerberus_wifi_fox_hunt_signal_t signal;
static wifi_fox_hunt_scan_t scan;
static const uint8_t target[6]={2,3,4,5,6,7};

static esp_err_t get_info(cerberus_wifi_fox_hunt_status_t *out)
{ ++calls[INFO]; *out=(cerberus_wifi_fox_hunt_status_t){.session_id=100}; return errors[INFO]; }
static esp_err_t start(const cerberus_wifi_fox_hunt_start_t *request,cerberus_wifi_fox_hunt_status_t *out)
{
    ++calls[START]; assert(request->session_id==101 && !memcmp(request->target,target,6));
    assert(request->initial_channel==1 && request->search_band==3); *out=control; return errors[START];
}
static esp_err_t status(uint64_t session,cerberus_wifi_fox_hunt_status_t *out)
{ ++calls[STATUS]; assert(session==101); *out=control; return errors[STATUS]; }
static esp_err_t stop(uint64_t session,cerberus_wifi_fox_hunt_status_t *out)
{ ++calls[STOP]; assert(session==101); *out=control; return errors[STOP]; }
static esp_err_t get_signal(uint64_t session,cerberus_wifi_fox_hunt_signal_t *out)
{ ++calls[SIGNAL]; assert(session==101); *out=signal; return errors[SIGNAL]; }
static const wifi_fox_hunt_transport_t transport={get_info,start,status,stop,get_signal};
static unsigned total_calls(void)
{ unsigned total=0;for(unsigned i=0;i<OPS;++i)total+=calls[i];return total; }
static bool tick(uint64_t now)
{ unsigned before=total_calls();bool changed=wifi_fox_hunt_scan_tick(&scan,now,&transport);assert(total_calls()-before<=1);return changed; }
static void reset(void)
{
    memset(&scan,0,sizeof(scan));memset(calls,0,sizeof(calls));memset(errors,0,sizeof(errors));
    control=(cerberus_wifi_fox_hunt_status_t){.session_id=101,.state=1,.current_channel=1,.flags=11};
    signal=(cerberus_wifi_fox_hunt_signal_t){.session_id=101,.rssi=INT8_MIN,.age_ms=UINT32_MAX};
}
static void begin(void)
{
    assert(wifi_fox_hunt_scan_start(&scan,target,1,3,false));
    assert(tick(1000)&&scan.phase==WIFI_FOX_START);
    assert(tick(1001)&&scan.phase==WIFI_FOX_WATCH);
}
static void observation(uint32_t sequence,uint8_t channel,int8_t rssi,uint32_t age)
{
    signal=(cerberus_wifi_fox_hunt_signal_t){.session_id=101,.sequence=sequence,
        .rssi=rssi,.age_ms=age,.observed_ms=sequence*100U,.channel=channel};
    control.latest_sequence=sequence;control.last_target_channel=channel;
}
static wifi_fox_hunt_view_t view(uint64_t now)
{ wifi_fox_hunt_view_t result;wifi_fox_hunt_scan_view(&scan,now,&result);return result; }
static void finish(uint64_t now)
{
    wifi_fox_hunt_scan_stop(&scan);control.flags=1;control.current_channel=0;control.state=0;control.error_code=0;
    assert(tick(now));if(scan.phase==WIFI_FOX_FINAL)assert(tick(now+1));
    assert(scan.phase==WIFI_FOX_OFF&&!scan.session&&!view(now+1).current);
}

static void signal_history_and_search(void)
{
    reset();begin();assert(tick(1002));assert(view(1002).state==WIFI_FOX_WAITING&&!scan.history_count);
    observation(1,1,-64,0);assert(tick(1102));
    wifi_fox_hunt_view_t v=view(1102);assert(v.current&&v.state==WIFI_FOX_TRACKING&&v.rssi==-64&&v.age_ms==20);
    assert(scan.history_count==1&&v.observed_channel==1);
    signal.age_ms=100;assert(tick(1202));assert(scan.history_count==1); /* Duplicate poll is not a new callback. */
    assert(view(1302).age_ms==220);
    observation(7,1,-45,0);assert(tick(1302));assert(scan.history_count==2); /* Latest-only coalescing. */
    assert(view(2782).state==WIFI_FOX_STALE&&!view(2782).current);
    control.flags=27;control.current_channel=36;control.sweeps=1;assert(tick(2782));
    assert(view(2782).state==WIFI_FOX_SEARCHING&&view(2782).observed_channel==1);
    assert(scan.history_count==3&&scan.history[2].kind==WIFI_FOX_GAP);
    observation(8,44,-52,0);assert(tick(2783));assert(!view(2783).current);
    control.flags=11;control.current_channel=44;assert(tick(2784));
    assert(view(2784).current&&view(2784).observed_channel==44&&view(2784).listening_channel==44);
    unsigned rows=scan.history_count;finish(2800);assert(scan.history_count==rows+1);
    assert(wifi_fox_hunt_scan_start(&scan,target,1,3,true));
    assert(!scan.have_signal&&scan.signal.rssi==INT8_MIN&&view(2900).state==WIFI_FOX_WAITING);
    assert(scan.history_count==rows+1&&scan.history[rows].kind==WIFI_FOX_GAP);
    wifi_fox_hunt_scan_stop(&scan);assert(scan.phase==WIFI_FOX_OFF);
}

static void lifecycle_and_errors(void)
{
    reset();assert(!wifi_fox_hunt_scan_start(&scan,(uint8_t[6]){0},1,3,false));
    assert(!wifi_fox_hunt_scan_start(&scan,(uint8_t[6]){1,2,3,4,5,6},1,3,false));
    assert(!wifi_fox_hunt_scan_start(&scan,target,1,2,false));
    assert(wifi_fox_hunt_scan_start(&scan,target,1,3,false));wifi_fox_hunt_scan_stop(&scan);
    assert(!tick(1000)&&!total_calls());
    assert(wifi_fox_hunt_scan_start(&scan,target,1,3,false));assert(tick(1000));
    wifi_fox_hunt_scan_stop(&scan);assert(scan.phase==WIFI_FOX_OFF&&!calls[START]);

    reset();errors[INFO]=ESP_ERR_NOT_SUPPORTED;
    assert(wifi_fox_hunt_scan_start(&scan,target,1,3,false));assert(tick(1000));
    assert(scan.phase==WIFI_FOX_OFF&&scan.error==ESP_ERR_NOT_SUPPORTED&&!calls[START]);

    reset();errors[START]=ESP_ERR_TIMEOUT;begin();assert(!tick(1002));assert(tick(1251));
    assert(calls[START]==1&&calls[STATUS]==1&&scan.session==101);
    wifi_fox_hunt_scan_stop(&scan);assert(tick(1252));unsigned stops=calls[STOP];
    assert(scan.phase==WIFI_FOX_STOP&&!tick(1253));assert(tick(1502)&&calls[STOP]==stops+1);
    finish(1752);

    reset();begin();control=(cerberus_wifi_fox_hunt_status_t){.session_id=101,.error_code=ESP_ERR_NOT_FOUND};
    assert(tick(1501));assert(scan.phase==WIFI_FOX_OFF&&scan.error==ESP_ERR_NOT_FOUND&&!calls[STOP]);
    reset();begin();control.session_id=999;assert(tick(1501));
    assert(scan.phase==WIFI_FOX_STOP&&scan.local_error==ESP_ERR_INVALID_RESPONSE);
    control.session_id=101;finish(1502);

    reset();begin();observation(1,1,-50,0);assert(tick(1002));assert(view(1002).current);
    errors[STATUS]=ESP_ERR_TIMEOUT;assert(tick(1501));assert(!view(1501).current&&view(1501).state==WIFI_FOX_FAULT);
    errors[STATUS]=ESP_OK;assert(tick(1751));assert(!view(1751).current);
    observation(2,1,-51,0);assert(tick(1752));assert(view(1752).current);

    reset();begin();control.flags|=32;control.error_code=ESP_ERR_INVALID_ARG;assert(tick(1501));
    assert(!scan.error&&view(1501).warning==ESP_ERR_INVALID_ARG&&view(1501).channels_skipped);
    control.flags|=64;control.sweeps=UINT16_MAX;assert(tick(2001));assert(view(2001).sweeps_saturated);
    finish(2002);

    reset();begin();observation(1,1,-51,0);assert(tick(1002));
    assert(tick(999));assert(scan.phase==WIFI_FOX_STOP&&view(999).age_ms==UINT32_MAX&&!view(999).current);
    finish(1300);

    reset();begin();observation(1,1,-51,0);control.flags=1;control.current_channel=0;control.state=0;
    assert(tick(1501)&&scan.phase==WIFI_FOX_FINAL);errors[SIGNAL]=ESP_ERR_TIMEOUT;assert(tick(1502));
    assert(scan.phase==WIFI_FOX_OFF&&scan.transport_error==ESP_ERR_TIMEOUT);
}

static void strict_provenance_and_capacity(void)
{
    reset();begin();observation(1,1,-51,0);assert(tick(1002));
    signal.observed_ms++;assert(tick(1102));assert(scan.local_error==ESP_ERR_INVALID_RESPONSE&&scan.phase==WIFI_FOX_STOP);
    reset();begin();observation(1,1,-51,10);assert(tick(1002));
    signal.age_ms=9;assert(tick(1102));assert(scan.local_error==ESP_ERR_INVALID_RESPONSE);
    reset();begin();observation(1,1,-51,UINT32_MAX);assert(tick(1002));
    assert(scan.have_signal&&!view(1002).current&&view(1002).age_ms==UINT32_MAX);
    signal.age_ms=100;assert(tick(1102));assert(view(1102).age_ms==UINT32_MAX);

    reset();begin();uint64_t now=1002;
    for(uint32_t n=1;n<=200;++n) {
        observation(n,1,-60,0);scan.status_due_ms=UINT64_MAX;scan.signal_due_ms=now;
        assert(tick(now++));
    }
    assert(scan.history_count==WIFI_FOX_HISTORY);
    wifi_fox_hunt_entry_t entry;assert(wifi_fox_hunt_scan_history(&scan,0,&entry)&&entry.sequence==41);
    assert(wifi_fox_hunt_scan_history(&scan,159,&entry)&&entry.sequence==200);
    assert(!wifi_fox_hunt_scan_history(&scan,160,&entry));
    observation(UINT32_MAX,1,-60,0);scan.signal_due_ms=now;assert(tick(now++));
    assert(scan.signal.sequence==UINT32_MAX);
    control.state=2;control.flags=1;control.current_channel=0;control.error_code=ESP_ERR_INVALID_STATE;
    scan.status_due_ms=now;assert(tick(now));assert(scan.phase==WIFI_FOX_OFF&&scan.error==ESP_ERR_INVALID_STATE);
}

static void keepalive_and_repeated_bad_signal(void)
{
    reset();begin();
    for(uint64_t now=1002;now<=7002;now+=20)(void)tick(now);
    assert(calls[STATUS]==12&&calls[SIGNAL]>=50&&calls[SIGNAL]<=61&&calls[START]==1);
    assert(scan.phase==WIFI_FOX_WATCH&&view(7002).state==WIFI_FOX_WAITING);
    reset();begin();observation(1,1,-50,0);assert(tick(1002));assert(view(1002).current);
    errors[SIGNAL]=ESP_ERR_INVALID_RESPONSE;
    for(uint64_t now=1102;now<=2102;now+=20)(void)tick(now);
    assert(!view(2102).current&&scan.phase==WIFI_FOX_WATCH&&calls[START]==1&&calls[SIGNAL]>=4);
    finish(2200);
}

static void malformed_values(void)
{
    reset();assert(wifi_fox_status_valid(&control,false,101));
    control.current_channel=0;assert(!wifi_fox_status_valid(&control,false,101));
    control.current_channel=1;control.flags=1;assert(!wifi_fox_status_valid(&control,false,101));
    reset();assert(wifi_fox_signal_valid(&signal,101));signal.rssi=-127;assert(!wifi_fox_signal_valid(&signal,101));
    observation(1,177,20,0);assert(wifi_fox_signal_valid(&signal,101));
    signal.rssi=21;assert(!wifi_fox_signal_valid(&signal,101));signal.rssi=INT8_MIN;
    assert(!wifi_fox_signal_valid(&signal,101));signal.rssi=-127;signal.channel=35;
    assert(!wifi_fox_signal_valid(&signal,101));
}

int main(void)
{
    signal_history_and_search();lifecycle_and_errors();strict_provenance_and_capacity();
    keepalive_and_repeated_bad_signal();malformed_values();
    puts("Wi-Fi Fox Hunt model: exact cleanup, source age, stale/search gaps, history and signal validation passed");
    return 0;
}
