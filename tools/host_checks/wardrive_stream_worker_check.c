#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef int esp_err_t;
enum {ESP_OK=0,ESP_FAIL=-1,ESP_ERR_NO_MEM=257,ESP_ERR_INVALID_ARG,ESP_ERR_INVALID_STATE,
    ESP_ERR_TIMEOUT,ESP_ERR_NOT_SUPPORTED,ESP_ERR_NOT_FINISHED,ESP_ERR_INVALID_RESPONSE};
#include "wardrive_service.h"
#include "wardrive_radio.h"
static unsigned checks,clock_value=1000,depth,writes,stop_after,sd_fail_at,lost_reads,lost_start;
static bool stop_requested,worker_active,recent_worker_session,stream_recording,storage_closed;
static uint64_t stream_observed_ms,started_ms;
static cerberus_wardrive_status_t wardrive_status;
static cerberus_geofence_config_t wardrive_geofences;
static cerberus_wardrive_gps_snapshot_t gps_fixture;
static void (*tick_hook)(void);
static esp_err_t (*exchange_hook)(unsigned,uint8_t,const cerberus_wd_request_t *,void *);
static void (*append_hook)(void);
static cerberus_wardrive_recent_t model,*wardrive_recent=&model;
static unsigned wardrive_lock,open_file_rows,wigle_file_rows;
static bool wardrive_file_open,wigle_file_open;
#define CHECK(x) do{++checks;if(!(x)){fprintf(stderr,"line %u: %s\n",__LINE__,#x);exit(1);}}while(0)
#define portENTER_CRITICAL(p) do{(void)(p);CHECK(!depth);++depth;}while(0)
#define portEXIT_CRITICAL(p) do{(void)(p);CHECK(depth==1);--depth;}while(0)
#ifndef FIXTURE_TICK_MS
#define FIXTURE_TICK_MS 1U
#endif
#define pdMS_TO_TICKS(x) ((x)/FIXTURE_TICK_MS)
#define WARDRIVE_EXPANSION_POLL_MS 1000U
#define WARDRIVE_SYNC_MS 2000U
static uint32_t now_ms(void){return clock_value;}
static uint64_t now_ms64(void){return clock_value;}
static uint32_t esp_random(void){static uint32_t n=100;return ++n;}
static void vTaskDelay(unsigned ticks){CHECK(!depth&&ticks);clock_value+=ticks*FIXTURE_TICK_MS;CHECK(clock_value<100000);if(tick_hook)tick_hook();}
static void vTaskDelete(void *p){CHECK(!depth&&!p&&!worker_active&&storage_closed);}
static bool wants_stop(void){return stop_requested;}
static void recent_begin_session(void){memset(&model,0,sizeof(model));recent_worker_session=true;}
static void begin_rssi_filter(void){CHECK(!depth&&stream_recording);}
typedef struct {cerberus_wardrive_recent_ticket_t ticket;} wardrive_recent_attempt_t;
static void recent_preview(const cerberus_wardrive_observation_t *o,wardrive_recent_attempt_t *a){(void)o;(void)a;}
static void recent_outcome(cerberus_wardrive_recent_ticket_t t,cerberus_wardrive_recent_outcome_t o){(void)t;(void)o;}
static esp_err_t start_optional_gps(bool enable){CHECK(enable);return ESP_OK;}
static uint32_t plus_one(uint32_t n){return n==UINT32_MAX?n:n+1U;}
static void snapshot_optional_gps(bool enabled,cerberus_wardrive_gps_snapshot_t *out,esp_err_t *error)
{CHECK(enabled);*out=gps_fixture;*error=ESP_OK;}
static unsigned gps_checks, lose_fix_at;
esp_err_t cerberus_wardrive_start_check(bool enable)
{CHECK(enable);return lose_fix_at&&++gps_checks>=lose_fix_at?ESP_ERR_NOT_FINISHED:ESP_OK;}
static esp_err_t stop_optional_gps(bool enable){CHECK(enable&&storage_closed);return ESP_OK;}
static void publish_error(esp_err_t error,char source){(void)source;if(!wardrive_status.error_code)wardrive_status.error_code=error;}
static esp_err_t append_observation_impl(const cerberus_wardrive_observation_t *o,cerberus_wardrive_recent_outcome_t *out)
{
    CHECK(!depth&&cerberus_wardrive_observation_valid(o)&&stream_recording&&stream_observed_ms>0);
    if(append_hook)append_hook();
    if(sd_fail_at&&writes+1==sd_fail_at)return ESP_FAIL;
    ++writes;++open_file_rows;wardrive_file_open=true;*out=CERBERUS_WARDRIVE_RECENT_APPENDED_UNLOCATED;
    if(writes==stop_after)stop_requested=true;return ESP_OK;
}
static esp_err_t capture_expansions(bool final){(void)final;return ESP_OK;}
static void stop_expansions_impl(bool keep){(void)keep;}
static esp_err_t cerberus_storage_wardrive_sync(void){return ESP_OK;}
static esp_err_t cerberus_storage_wardrive_wigle_sync(void){return ESP_OK;}
static esp_err_t save_open_file(bool keep){CHECK(keep&&writes);wardrive_file_open=false;return ESP_OK;}
static esp_err_t save_log_stream(bool located,bool keep){(void)located;(void)keep;wigle_file_open=false;return ESP_OK;}
static void finish_storage_session(void){CHECK(!wardrive_file_open&&!wigle_file_open);storage_closed=true;}
static esp_err_t field_survey_set_idle_probe_enabled(bool enabled){CHECK(enabled&&storage_closed);return ESP_OK;}
static void release_workspace(void){CHECK(storage_closed);}
typedef struct {uint64_t session;uint32_t ack;unsigned starts,stops;bool held,released;} radio_t;
static radio_t radios_fixture[2];
static unsigned delivered[2][5];
static esp_err_t exchange(unsigned chip,uint8_t command,const cerberus_wd_request_t *q,void *out)
{
    if(exchange_hook)return exchange_hook(chip,command,q,out);
    CHECK(!depth);radio_t *r=&radios_fixture[chip];memset(out,0,54);
    cerberus_wd_status_t *status=out;status->session=q->session;
    if(command==CERBERUS_WD_START){
        if(!r->session){r->session=q->session;r->held=true;++r->starts;}
        CHECK(q->session==r->session);
        if(chip&&lost_start){--lost_start;return ESP_ERR_TIMEOUT;}
    }
    if(r->released||r->session!=q->session){status->error=ESP_ERR_INVALID_STATE;return chip?ESP_OK:ESP_ERR_INVALID_STATE;}
    CHECK(q->ack==r->ack||q->ack==r->ack+1);r->ack=q->ack;
    status->state=r->held?CERBERUS_WD_RUNNING:CERBERUS_WD_STOPPED;status->held=r->held;status->pending=4-r->ack;
    if(command==CERBERUS_WD_STOP){r->held=false;++r->stops;status->state=CERBERUS_WD_STOPPED;status->held=0;}
    if(command==CERBERUS_WD_READ){
        if(chip&&lost_reads){--lost_reads;return ESP_ERR_TIMEOUT;}
        memset(out,0,54);if(r->ack<4){
            unsigned next=r->ack+1;CHECK(!delivered[chip][next]);++delivered[chip][next];
            cerberus_wd_record_t *row=out;row->sequence=next;row->age_ms=1;row->kind=1;row->channel=chip?36:6;
            row->address[0]=2;row->rssi=-45;row->label_length=1;row->label[0]='X';
        }
    }
    if(command==CERBERUS_WD_RELEASE){
        if(r->held||(r->ack<4&&!q->discard)){status->error=ESP_ERR_INVALID_STATE;return chip?ESP_OK:ESP_ERR_INVALID_STATE;}
        r->released=true;
    }
    return ESP_OK;
}
esp_err_t cerberus_wardrive_radio_exchange(uint8_t cmd,const cerberus_wd_request_t *q,void *out){return exchange(0,cmd,q,out);}
esp_err_t c5_spi_link_wardrive(uint8_t cmd,const cerberus_wd_request_t *q,void *out){return exchange(1,cmd,q,out);}
#include "wardrive_stream_worker.h"
static void run(unsigned stop,unsigned fail,unsigned start_loss,unsigned read_loss)
{
    memset(radios_fixture,0,sizeof(radios_fixture));memset(delivered,0,sizeof(delivered));
    wardrive_status=(cerberus_wardrive_status_t){.mode=CERBERUS_WD_WIFI,.gps_requested=true};
    writes=open_file_rows=wigle_file_rows=0;stop_after=stop;sd_fail_at=fail;lost_start=start_loss;lost_reads=read_loss;
    stop_requested=storage_closed=wardrive_file_open=wigle_file_open=false;worker_active=true;started_ms=clock_value;
    wardrive_stream_worker(NULL);
    CHECK(!worker_active&&!recent_worker_session&&!stream_recording&&model.all_observations);
    CHECK(radios_fixture[0].released&&(!radios_fixture[1].session||radios_fixture[1].released));
    CHECK(radios_fixture[0].starts==1&&radios_fixture[1].starts<=1);
    if(fail)CHECK(wardrive_status.state==CERBERUS_WARDRIVE_ERROR&&writes==fail-1);
    else CHECK(wardrive_status.state==CERBERUS_WARDRIVE_STOPPED&&
        writes==4U+(radios_fixture[1].session?4U:0U));
}
static void check_gps_loss(unsigned at)
{
    memset(radios_fixture,0,sizeof(radios_fixture));memset(delivered,0,sizeof(delivered));
    wardrive_status=(cerberus_wardrive_status_t){.mode=CERBERUS_WD_WIFI,.gps_requested=true};
    writes=open_file_rows=wigle_file_rows=sd_fail_at=lost_start=lost_reads=0;stop_after=999;
    stop_requested=storage_closed=wardrive_file_open=wigle_file_open=false;worker_active=true;
    gps_checks=0;lose_fix_at=at;wardrive_stream_worker(NULL);
    CHECK(wardrive_status.state==CERBERUS_WARDRIVE_ERROR&&!worker_active&&storage_closed);
    CHECK(radios_fixture[1].starts==0);
    if(at<3)CHECK(radios_fixture[0].starts==0&&writes==0);
    else CHECK(radios_fixture[0].starts==1&&radios_fixture[0].released);
    lose_fix_at=0;
}
int main(void)
{
    check_gps_loss(1);check_gps_loss(2);check_gps_loss(3);
    run(3,0,0,0); /* Stop before C5 START: drain only the started S3. */
    run(7,0,0,3); /* Lost reads/ACK progress must never duplicate an append. */
    run(7,0,2,0); /* START accepted but its response was lost. */
    run(7,3,0,0); /* Ambiguous failed append is never replayed during cleanup. */
    printf("Wardrive writer: %u checks; lost START/read responses, exact ACK, Stop drain and SD failure passed\n",checks);
    return 0;
}
