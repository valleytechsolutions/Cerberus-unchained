#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "wifi_channel_analyzer_scan.h"
#include "wifi_channel_analyzer_validate.h"

enum { INFO, START, STATUS, STOP, SAMPLE, PEEK, READ, OPERATIONS };
static unsigned calls[OPERATIONS];
static esp_err_t errors[OPERATIONS];
static cerberus_wifi_channel_analyzer_status_t remote;
static cerberus_wifi_channel_analyzer_hint_t beacon;
static uint32_t frames, duration, projection, age;
static bool bad_sample, bad_projection, bad_padding;
static wifi_channel_analyzer_scan_t scan;

static esp_err_t info(cerberus_wifi_channel_analyzer_status_t *out)
{
    ++calls[INFO]; *out = (cerberus_wifi_channel_analyzer_status_t){.session_id=100U};
    return errors[INFO];
}
static esp_err_t start(uint64_t session, uint8_t channel, cerberus_wifi_channel_analyzer_status_t *out)
{
    ++calls[START]; remote.session_id=session; remote.channel=channel;
    *out=remote; return errors[START];
}
static esp_err_t status(uint64_t session, cerberus_wifi_channel_analyzer_status_t *out)
{ ++calls[STATUS]; assert(session==101U); *out=remote; return errors[STATUS]; }
static esp_err_t stop(uint64_t session, cerberus_wifi_channel_analyzer_status_t *out)
{ ++calls[STOP]; assert(session==101U); *out=remote; return errors[STOP]; }
static esp_err_t sample(uint64_t session, uint32_t sequence, cerberus_wifi_channel_analyzer_sample_t *out)
{
    ++calls[SAMPLE]; assert(session==101U && sequence);
    *out=(cerberus_wifi_channel_analyzer_sample_t){.session_id=session,
        .sequence=sequence+(bad_sample ? 1U : 0U),.channel=remote.channel,
        .frames=frames,.window_us=duration,
        .flags=frames==UINT32_MAX ? CERBERUS_WIFI_CHANNEL_ANALYZER_SATURATED : 0U};
    return errors[SAMPLE];
}
static esp_err_t peek(uint64_t session, cerberus_wifi_channel_analyzer_hint_status_t *out)
{
    ++calls[PEEK]; *out=(cerberus_wifi_channel_analyzer_hint_status_t){.session_id=session,
        .projection=projection,.age_ms=age,.length=42,.flags=1};
    return errors[PEEK];
}
static esp_err_t read_hint(uint64_t session, uint32_t id, uint8_t part,
    cerberus_wifi_channel_analyzer_hint_part_t *out)
{
    ++calls[READ]; assert(part<5U);
    *out=(cerberus_wifi_channel_analyzer_hint_part_t){.session_id=session,
        .projection=id+(bad_projection ? 1U : 0U),.part=part};
    unsigned offset=part*9U, length=sizeof(beacon)-offset;
    if(length>9U) length=9U;
    memcpy(out->bytes, (uint8_t *)&beacon+offset, length);
    if(bad_padding && part==4U) out->bytes[8]=1U;
    return errors[READ];
}
static const wifi_channel_analyzer_transport_t transport={info,start,status,stop,sample,peek,read_hint};

static unsigned total_calls(void)
{ unsigned total=0U; for(unsigned i=0;i<OPERATIONS;++i) total+=calls[i]; return total; }
static bool tick(uint64_t now)
{
    unsigned before=total_calls();
    bool changed=wifi_channel_analyzer_scan_tick(&scan,now,&transport);
    assert(total_calls()-before<=1U); return changed;
}
static void reset(void)
{
    memset(&scan,0,sizeof(scan)); memset(calls,0,sizeof(calls)); memset(errors,0,sizeof(errors));
    remote=(cerberus_wifi_channel_analyzer_status_t){.session_id=101U,.target_ms=100U,
        .channel=1,.state=CERBERUS_WIFI_PACKET_MONITOR_RUNNING,
        .flags=CERBERUS_WIFI_CHANNEL_ANALYZER_BOUND|CERBERUS_WIFI_CHANNEL_ANALYZER_HELD};
    beacon=(cerberus_wifi_channel_analyzer_hint_t){.bssid={2,3,4,5,6,7},.ssid={'a','b','c'},
        .ssid_length=3,.rssi=-44,.channel=1,.flags=1};
    frames=10U; duration=100000U; projection=1U; age=10U;
    bad_sample=bad_projection=bad_padding=false;
}
static void begin(void)
{
    assert(wifi_channel_analyzer_scan_start(&scan,1,false));
    assert(tick(1000U) && scan.phase==WIFI_ANALYZER_START);
    assert(tick(1001U) && scan.phase==WIFI_ANALYZER_WATCH);
    assert(scan.session==101U && calls[START]==1U);
}
static void publish(uint32_t latest)
{
    remote.latest_sequence=latest;
    remote.stored_count=latest<64U ? (uint8_t)latest : 64U;
}
static void finish(uint64_t now)
{
    wifi_channel_analyzer_scan_stop(&scan);
    remote.flags=CERBERUS_WIFI_CHANNEL_ANALYZER_BOUND; remote.state=CERBERUS_WIFI_PACKET_MONITOR_IDLE;
    assert(tick(now));
    for(unsigned i=0;scan.phase!=WIFI_ANALYZER_OFF && i<65U;++i) assert(tick(now+i+1U));
    assert(scan.phase==WIFI_ANALYZER_OFF && !scan.session);
}

static void rates_gaps_and_retune(void)
{
    reset(); begin(); publish(2); assert(tick(1251));
    frames=10; duration=100000; assert(tick(1252)); assert(!scan.current_valid);
    frames=20; duration=300000; assert(tick(1253)); assert(scan.current_valid);
    wifi_channel_analyzer_summary_t sum; wifi_channel_analyzer_scan_summary(&scan,&sum);
    assert(sum.windows==2 && sum.frames==30 && sum.window_us==400000);
    assert(sum.current_rate==66 && sum.average_rate==75 && sum.peak_rate==100 && !sum.lower_bound);
    publish(70); assert(tick(1501)); assert(!scan.current_valid);
    assert(scan.lost_windows==4 && scan.fetched==6);
    wifi_channel_analyzer_scan_summary(&scan,&sum);
    assert(sum.gap_count==1 && sum.missing_windows==4 && sum.windows==2 && sum.average_rate==75);
    finish(1502);
    unsigned count=scan.history_count;
    assert(wifi_channel_analyzer_scan_start(&scan,36,true));
    assert(scan.history_count==count && !scan.current_valid && !scan.hint_valid);
    remote=(cerberus_wifi_channel_analyzer_status_t){.session_id=101,.channel=36,.target_ms=100,
        .flags=3,.state=CERBERUS_WIFI_PACKET_MONITOR_RUNNING};
    assert(tick(2000)); assert(tick(2001)); publish(1); assert(tick(2251)); assert(tick(2252));
    wifi_channel_analyzer_entry_t row;
    assert(wifi_channel_analyzer_scan_history(&scan,count,&row) && row.kind==WIFI_ANALYZER_GAP && !row.missing_windows);
    assert(wifi_channel_analyzer_scan_history(&scan,count+1,&row) && row.kind==WIFI_ANALYZER_CHANNEL && row.channel==36);
    assert(wifi_channel_analyzer_scan_history(&scan,count+2,&row) && row.kind==WIFI_ANALYZER_WINDOW && row.channel==36);
    finish(2253);
    assert(wifi_channel_analyzer_scan_start(&scan,36,false) && !scan.history_count);
    wifi_channel_analyzer_scan_stop(&scan); assert(scan.phase==WIFI_ANALYZER_OFF);
}

static void history_and_limits(void)
{
    reset(); begin(); uint64_t now=1100;
    for(uint32_t i=1;i<=400U;++i) {
        publish(i); scan.status_due_ms=now; assert(tick(now++)); assert(tick(now++));
    }
    assert(scan.history_count==WIFI_ANALYZER_HISTORY);
    wifi_channel_analyzer_entry_t row;
    assert(wifi_channel_analyzer_scan_history(&scan,0,&row) && row.sequence==81 && row.kind==WIFI_ANALYZER_WINDOW);
    assert(wifi_channel_analyzer_scan_history(&scan,319,&row) && row.sequence==400);
    assert(!wifi_channel_analyzer_scan_history(&scan,320,&row));
    wifi_channel_analyzer_summary_t sum; wifi_channel_analyzer_scan_summary(&scan,&sum);
    assert(sum.windows==320 && sum.frames==3200 && sum.average_rate==100);

    reset(); begin(); publish(2); assert(tick(1251)); frames=UINT32_MAX;
    duration=UINT32_MAX; assert(tick(1252)); assert(tick(1253));
    wifi_channel_analyzer_scan_summary(&scan,&sum);
    assert(sum.frames==2ULL*UINT32_MAX && sum.window_us==2ULL*UINT32_MAX);
    assert(sum.lower_bound && !sum.rate_limited && sum.average_rate==1000000);
    publish(3); assert(tick(1501)); duration=100000; assert(tick(1502));
    wifi_channel_analyzer_scan_summary(&scan,&sum);
    assert(sum.lower_bound && sum.rate_limited && sum.peak_rate==UINT32_MAX);

    reset(); begin(); scan.fetched=UINT32_MAX-1U; scan.status.latest_sequence=UINT32_MAX-1U;
    publish(UINT32_MAX); assert(tick(1251)); assert(tick(1252));
    assert(scan.fetched==UINT32_MAX); unsigned reads=calls[SAMPLE]; assert(tick(1253));
    assert(calls[SAMPLE]==reads); finish(1254);
}

static void lifecycle_and_failures(void)
{
    reset(); assert(!wifi_channel_analyzer_scan_start(&scan,0,false));
    assert(wifi_channel_analyzer_scan_start(&scan,1,false)); wifi_channel_analyzer_scan_stop(&scan);
    assert(!tick(1000) && !total_calls());
    assert(wifi_channel_analyzer_scan_start(&scan,1,false)); assert(tick(1001));
    wifi_channel_analyzer_scan_stop(&scan); assert(scan.phase==WIFI_ANALYZER_OFF && !calls[START]);

    reset(); errors[INFO]=ESP_ERR_NOT_SUPPORTED;
    assert(wifi_channel_analyzer_scan_start(&scan,1,false)); assert(tick(1000));
    assert(scan.phase==WIFI_ANALYZER_OFF && scan.error==ESP_ERR_NOT_SUPPORTED && !calls[START]);

    reset(); errors[START]=ESP_ERR_TIMEOUT; begin(); assert(scan.transport_error==ESP_ERR_TIMEOUT);
    assert(!tick(1002)); assert(tick(1501)); assert(calls[START]==1 && calls[STATUS]==1);
    wifi_channel_analyzer_scan_stop(&scan); assert(tick(1502));
    assert(scan.phase==WIFI_ANALYZER_STOP); unsigned stops=calls[STOP]; assert(!tick(1503));
    assert(tick(1752)); assert(calls[STOP]==stops+1);
    finish(2002);

    reset(); begin(); remote=(cerberus_wifi_channel_analyzer_status_t){.session_id=101,.error_code=ESP_ERR_NOT_FOUND};
    assert(tick(1251)); assert(scan.phase==WIFI_ANALYZER_OFF && scan.error==ESP_ERR_NOT_FOUND);
    assert(!calls[STOP]); /* Reboot/expired session cannot be adopted. */

    reset(); begin(); remote.session_id=999; assert(tick(1251));
    assert(scan.phase==WIFI_ANALYZER_STOP && scan.error==ESP_ERR_INVALID_RESPONSE);
    remote.session_id=101; finish(1252); /* STOP remains bound to our exact 101. */

    reset(); begin(); publish(1); assert(tick(1251)); bad_sample=true; assert(tick(1252));
    assert(scan.phase==WIFI_ANALYZER_STOP && scan.local_error==ESP_ERR_INVALID_RESPONSE);
    bad_sample=false; finish(1752);

    reset(); begin(); publish(1); assert(tick(1251)); errors[SAMPLE]=ESP_ERR_TIMEOUT;
    finish(1252); assert(scan.lost_windows==1 && !scan.history[0].frames);
    assert(scan.transport_error==ESP_ERR_TIMEOUT);

    reset(); begin(); publish(1); assert(tick(1251)); assert(tick(1252)); assert(scan.current_valid);
    errors[STATUS]=ESP_ERR_TIMEOUT; assert(tick(1501)); assert(!scan.current_valid);
    errors[STATUS]=ESP_OK; publish(2); assert(tick(2001)); assert(tick(2002)); assert(scan.current_valid);

    reset(); begin(); scan.hint_valid=true; assert(tick(999));
    assert(scan.phase==WIFI_ANALYZER_STOP && !scan.hint_valid && scan.local_error==ESP_ERR_INVALID_STATE);
    finish(1300);
}

static void hints_and_validation(void)
{
    reset(); begin(); assert(tick(1002)); assert(scan.reading_hint);
    for(unsigned i=0;i<5;++i) assert(tick(1003+i));
    assert(scan.hint_valid && !scan.reading_hint && !memcmp(&scan.hint,&beacon,sizeof(beacon)));
    assert(wifi_channel_analyzer_scan_hint_age(&scan,1102)==110);
    assert(wifi_channel_analyzer_scan_hint_age(&scan,1001)==UINT32_MAX);
    assert(wifi_channel_analyzer_scan_hint_age(&scan,UINT64_MAX)==UINT32_MAX);
    scan.hint_age_at_peek_ms=UINT32_MAX;
    assert(wifi_channel_analyzer_scan_hint_age(&scan,1102)==UINT32_MAX);

    scan.status_due_ms=4000; scan.hint_due_ms=1100; projection=2; beacon.ssid[0]='z';
    assert(tick(1100)); assert(tick(1101)); bad_projection=true; assert(tick(1102));
    assert(!scan.reading_hint && scan.hint.ssid[0]=='a' && scan.hint_valid);
    bad_projection=false; scan.next_action_ms=0; scan.hint_due_ms=1200;
    assert(tick(1200)); for(unsigned i=0;i<5;++i) assert(tick(1201+i));
    assert(scan.hint.ssid[0]=='z');
    scan.hint_due_ms=1300; assert(tick(1300)); bad_padding=true;
    for(unsigned i=0;i<5;++i) assert(tick(1301+i));
    assert(scan.transport_error==ESP_ERR_INVALID_RESPONSE && scan.hint.ssid[0]=='z');

    char text[132]; beacon.ssid_length=6; memcpy(beacon.ssid,"A\0\\\n\xFFZ",6);
    assert(wifi_channel_analyzer_scan_ssid_display(&beacon,text,sizeof(text))==18);
    assert(!strcmp(text,"A\\x00\\x5C\\x0A\\xFFZ"));
    char tiny[3]; assert(wifi_channel_analyzer_scan_ssid_display(&beacon,tiny,sizeof(tiny))==18);
    assert(tiny[2]=='\0'); beacon.ssid_length=0;
    assert(wifi_channel_analyzer_scan_ssid_display(&beacon,text,sizeof(text))==0 && !text[0]);

    reset(); assert(wifi_analyzer_hint_valid(&beacon)); beacon.bssid[0]=1; assert(!wifi_analyzer_hint_valid(&beacon));
    beacon.bssid[0]=2; beacon.ssid[31]=1; assert(!wifi_analyzer_hint_valid(&beacon));
    assert(wifi_analyzer_status_valid(&remote,false,101)); remote.stored_count=1;
    assert(!wifi_analyzer_status_valid(&remote,false,101)); remote.stored_count=0; remote.flags=1;
    assert(!wifi_analyzer_status_valid(&remote,false,101));
    cerberus_wifi_channel_analyzer_sample_t window={.session_id=1,.sequence=1,.channel=1,.window_us=99999};
    assert(!wifi_analyzer_sample_valid(&window,1,1)); window.window_us=100000;
    assert(wifi_analyzer_sample_valid(&window,1,1)); window.flags=1;
    assert(!wifi_analyzer_sample_valid(&window,1,1)); window.frames=UINT32_MAX;
    assert(wifi_analyzer_sample_valid(&window,1,1));
    window.flags=0; assert(!wifi_analyzer_sample_valid(&window,1,1));
}

int main(void)
{
    rates_gaps_and_retune(); history_and_limits(); lifecycle_and_failures(); hints_and_validation();
    puts("Channel Analyzer model: rates, gaps, retention, exact cleanup and pinned hints passed");
    return 0;
}
