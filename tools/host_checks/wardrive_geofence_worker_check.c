/* Use the production writer with scripted radio replies and GPS snapshots. */
#define main original_writer_checks
#include "wardrive_stream_worker_check.c"
#undef main

enum { CROSS_AREA=1, STOP_INSIDE, START_INSIDE, LOST_START, LOST_CONTROL,
       LOST_FIX, LOST_UTC, REENTER, PAUSE_STUCK, RADIO_FAIL, LONG_INITIAL_HOLD };
static unsigned test_case, origin, pause_losses, resume_losses, exclusion_writes;
static bool confirmed_pause, saw_resuming;
typedef struct {
    uint64_t session;
    uint32_t produced, ack;
    unsigned starts, pauses, resumes, phase_ticks;
    uint8_t state;
    bool released;
} scripted_radio_t;
static scripted_radio_t script[2];

static bool inside_now(void)
{
    unsigned elapsed=clock_value-origin;
    if(test_case==LONG_INITIAL_HOLD)return elapsed<12000U;
    if(test_case==START_INSIDE)return true;
    return elapsed>=500U&&(elapsed<1500U||test_case==STOP_INSIDE||
        (test_case==REENTER&&elapsed>=2000U&&elapsed<2500U)||
        test_case==PAUSE_STUCK||test_case==RADIO_FAIL);
}
static void gps_tick(void)
{
    gps_fixture=(cerberus_wardrive_gps_snapshot_t){.state=CERBERUS_WARDRIVE_GPS_FRESH,
        .coordinates_valid=true,.utc_valid=true,.utc_from_rmc=true,
        .latitude=inside_now()?0:1};
    unsigned elapsed=clock_value-origin;
    if((test_case==LOST_FIX||test_case==LOST_UTC)&&elapsed>=500U&&elapsed<1500U){
        gps_fixture.latitude=1;
        if(test_case==LOST_FIX)gps_fixture.state=CERBERUS_WARDRIVE_GPS_STALE;
        else gps_fixture.utc_valid=false;
    }
    if(wardrive_status.geofence_phase==CERBERUS_WARDRIVE_AREA_PAUSED){
        confirmed_pause=true;
        for(unsigned i=0;i<2;++i)CHECK(!script[i].session||script[i].state==CERBERUS_WD_PAUSED);
    }
    if(wardrive_status.geofence_phase==CERBERUS_WARDRIVE_AREA_RESUMING)saw_resuming=true;
    unsigned end=test_case==LONG_INITIAL_HOLD?15000U:
        (test_case==STOP_INSIDE||test_case==START_INSIDE)?1000U:3500U;
    if(elapsed>=end)stop_requested=true;
    if(test_case==PAUSE_STUCK||test_case==RADIO_FAIL)stop_requested=false; /* The real error path must stop. */
}
static void append_check(void)
{
    CHECK(!inside_now()&&gps_fixture.state==CERBERUS_WARDRIVE_GPS_FRESH&&gps_fixture.utc_valid);
    CHECK(wardrive_status.geofence_phase!=CERBERUS_WARDRIVE_AREA_PAUSING&&
        wardrive_status.geofence_phase!=CERBERUS_WARDRIVE_AREA_PAUSED);
    ++exclusion_writes;
}
static esp_err_t radio_exchange(unsigned chip,uint8_t command,const cerberus_wd_request_t *q,void *out)
{
    CHECK(!depth);scripted_radio_t *r=&script[chip];memset(out,0,54);
    cerberus_wd_status_t *s=out;s->session=q->session;
    if(command==CERBERUS_WD_START){
        if(!r->session){r->session=q->session;r->state=CERBERUS_WD_RUNNING;++r->starts;}
        if(test_case==LOST_START&&chip&&clock_value-origin<1000U){memset(out,0,54);return ESP_ERR_TIMEOUT;}
        if(test_case==LONG_INITIAL_HOLD&&chip&&clock_value-origin<12500U){memset(out,0,54);return ESP_ERR_TIMEOUT;}
    }
    if(!r->session||r->released){s->error=ESP_ERR_INVALID_STATE;return chip?ESP_OK:ESP_ERR_INVALID_STATE;}
    CHECK(q->session==r->session&&q->ack>=r->ack&&q->ack<=r->produced);r->ack=q->ack;
    if(command==CERBERUS_WD_STATUS&&r->state==CERBERUS_WD_RUNNING)++r->produced;
    if(command==CERBERUS_WD_PAUSE){
        if(r->state==CERBERUS_WD_RUNNING){r->state=CERBERUS_WD_PAUSING;r->phase_ticks=0;++r->pauses;r->produced+=3;}
        if(r->state==CERBERUS_WD_PAUSING&&++r->phase_ticks>=3U&&test_case!=PAUSE_STUCK)r->state=CERBERUS_WD_PAUSED;
        if(test_case==RADIO_FAIL&&chip){s->error=ESP_FAIL;return ESP_OK;}
        if(test_case==LOST_CONTROL&&chip&&pause_losses){--pause_losses;memset(out,0,54);return ESP_ERR_TIMEOUT;}
    }
    if(command==CERBERUS_WD_RESUME){
        /* No receiver may restart before every paused source queue is empty. */
        if(r->state==CERBERUS_WD_PAUSED){
            for(unsigned i=0;i<2;++i)CHECK(script[i].ack==script[i].produced);
            r->state=CERBERUS_WD_STARTING;r->phase_ticks=0;++r->resumes;
        }
        if(r->state==CERBERUS_WD_STARTING&&++r->phase_ticks>=3U)r->state=CERBERUS_WD_RUNNING;
        if(test_case==LOST_CONTROL&&chip&&resume_losses){--resume_losses;memset(out,0,54);return ESP_ERR_TIMEOUT;}
    }
    if(command==CERBERUS_WD_STOP)r->state=CERBERUS_WD_STOPPED;
    s->state=r->state;s->held=r->state!=CERBERUS_WD_STOPPED;s->pending=(uint16_t)(r->produced-r->ack);
    s->phase=r->state==CERBERUS_WD_RUNNING?CERBERUS_WD_PHASE_WIFI:0;
    s->channel=s->phase?(chip?36:6):0;
    if(command==CERBERUS_WD_READ){
        memset(out,0,54);
        if(r->ack<r->produced){
            cerberus_wd_record_t *row=out;row->sequence=r->ack+1U;row->age_ms=1;row->kind=1;
            row->channel=chip?36:6;row->address[0]=2;row->rssi=-45;row->label_length=1;row->label[0]='X';
        }
    }
    if(command==CERBERUS_WD_RELEASE){
        CHECK(r->state==CERBERUS_WD_STOPPED&&(q->discard||r->ack==r->produced));r->released=true;
    }
    return ESP_OK;
}
static void run_area(unsigned selected)
{
    test_case=selected;origin=clock_value;pause_losses=resume_losses=2;
    memset(script,0,sizeof(script));confirmed_pause=saw_resuming=false;exclusion_writes=0;
    wardrive_geofences=cerberus_geofence_defaults();
    wardrive_geofences.areas[0].enabled=1;strcpy(wardrive_geofences.areas[0].name,"test area");
    wardrive_status=(cerberus_wardrive_status_t){.mode=CERBERUS_WD_WIFI,.gps_requested=true,.geofences_enabled=true};
    writes=open_file_rows=wigle_file_rows=sd_fail_at=lost_start=lost_reads=gps_checks=lose_fix_at=0;
    stop_after=UINT32_MAX;stop_requested=storage_closed=wardrive_file_open=wigle_file_open=false;
    worker_active=true;started_ms=clock_value;tick_hook=gps_tick;exchange_hook=radio_exchange;append_hook=append_check;
    gps_tick();wardrive_stream_worker(NULL);
    CHECK(!worker_active&&!recent_worker_session&&!stream_recording&&storage_closed);
    for(unsigned i=0;i<2;++i)CHECK(!script[i].session||script[i].released);
    if(selected==PAUSE_STUCK)CHECK(wardrive_status.state==CERBERUS_WARDRIVE_ERROR&&wardrive_status.error_code==ESP_ERR_TIMEOUT);
    else if(selected==RADIO_FAIL)CHECK(wardrive_status.state==CERBERUS_WARDRIVE_ERROR&&wardrive_status.error_code==ESP_FAIL);
    else {
        CHECK(wardrive_status.state==CERBERUS_WARDRIVE_STOPPED&&confirmed_pause);
        if(selected==START_INSIDE)CHECK(!writes&&!script[0].starts&&!script[1].starts&&!wardrive_status.geofence_skipped);
        else if(selected==LONG_INITIAL_HOLD)CHECK(writes>0&&script[0].starts==1&&script[1].starts==1&&saw_resuming);
        else {
            CHECK(writes==exclusion_writes&&writes>0&&wardrive_status.geofence_skipped>0);
            CHECK(script[0].starts==1&&script[1].starts==1);
            if(selected==STOP_INSIDE)CHECK(!saw_resuming&&!script[0].resumes&&!script[1].resumes);
            else CHECK(saw_resuming&&script[0].resumes&&script[1].resumes);
        }
    }
    if(selected==LOST_CONTROL)CHECK(!pause_losses&&!resume_losses);
    if(selected==REENTER)CHECK(script[0].pauses==2&&script[1].pauses==2);
    tick_hook=NULL;exchange_hook=NULL;append_hook=NULL;
}
int main(void)
{
    original_writer_checks();
    for(unsigned scenario=CROSS_AREA;scenario<=LONG_INITIAL_HOLD;++scenario)run_area(scenario);
    puts("Geofence writer: enter/exit, initial hold, Stop, GPS/UTC loss, lost replies, reentry and radio faults passed; no boundary row was appended.");
    return 0;
}
