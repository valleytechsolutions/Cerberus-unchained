#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gps_recorder.c"

static cerberus_wardrive_gps_uart_state_t receiver;
static void (*task)(void *), (*on_delay)(void), (*on_allocate)(void);
static void *task_arg;
static unsigned locks,allocations,frees,delays,attaches,detaches,writes,opens,closes;
static unsigned attach_wait,detach_wait,open_wait,append_wait,close_wait;
static bool fail_alloc,fail_task,small_heap,small_block,gps_owned,sd_owned;
static esp_err_t read_error,attach_error,append_error,finish_error,release_error;
static int64_t now_us;
static char output[16384];

void test_enter(void *lock){(void)lock;assert(!locks);++locks;}
void test_exit(void *lock){(void)lock;assert(locks==1);--locks;}
int64_t esp_timer_get_time(void){assert(!locks);return now_us;}
size_t heap_caps_get_free_size(unsigned caps){assert(!locks&&caps==3);return small_heap?8192:100000;}
size_t heap_caps_get_largest_free_block(unsigned caps){assert(!locks&&caps==3);return small_block?4096:80000;}
void *heap_caps_calloc(size_t n,size_t size,unsigned caps)
{assert(!locks&&caps==6);++allocations;if(on_allocate)on_allocate();return fail_alloc?NULL:calloc(n,size);}
void heap_caps_free(void *p){assert(!locks);if(p)++frees;free(p);}
int xTaskCreate(void (*fn)(void *),const char *name,unsigned stack,void *arg,unsigned priority,void *handle)
{assert(!locks&&strcmp(name,"gps_log")==0&&stack==4096&&priority==3&&!handle);if(fail_task)return 0;task=fn;task_arg=arg;return pdPASS;}
void vTaskDelete(void *p){assert(!p&&!locks&&!gps_owned&&!sd_owned);}
void vTaskDelay(unsigned ms)
{assert(!locks&&ms&&++delays<10000);now_us+=(int64_t)ms*1000;if(on_delay)on_delay();}
esp_err_t cerberus_wardrive_gps_uart_record(bool enable)
{
    assert(!locks);
    if(enable){++attaches;if(attach_wait){--attach_wait;return ESP_ERR_NOT_FINISHED;}gps_owned=true;return attach_error;}
    ++detaches;assert(gps_recording_status.busy);
    if(detach_wait){--detach_wait;return ESP_ERR_NOT_FINISHED;}
    if(release_error){esp_err_t e=release_error;release_error=ESP_OK;return e;}
    gps_owned=false;return ESP_OK;
}
esp_err_t cerberus_wardrive_gps_uart_get_state(cerberus_wardrive_gps_uart_state_t *out)
{assert(!locks);*out=receiver;return read_error;}
esp_err_t cerberus_storage_gps_begin(uint64_t job,cerberus_gpx_mode_t mode)
{
    assert(!locks&&gps_owned&&job==gps_recording_status.job&&!sd_owned);
    if(open_wait){--open_wait;return ESP_ERR_TIMEOUT;}
    ++opens;sd_owned=true;strcpy(output,cerberus_gpx_header(mode));return ESP_OK;
}
esp_err_t cerberus_storage_gps_append(uint64_t job,const char *row,size_t length)
{
    assert(!locks&&sd_owned&&job==gps_recording_status.job&&length==strlen(row));
    if(append_wait){--append_wait;return ESP_ERR_TIMEOUT;}
    if(append_error)return append_error;
    assert(strlen(output)+length<sizeof(output));strcat(output,row);++writes;return ESP_OK;
}
esp_err_t cerberus_storage_gps_finish(uint64_t job,bool keep,char name[16],uint64_t *bytes)
{
    assert(!locks&&sd_owned&&job==gps_recording_status.job&&gps_recording_status.busy);
    name[0]=0;*bytes=0;
    if(close_wait){--close_wait;return ESP_ERR_TIMEOUT;}
    ++closes;sd_owned=false;
    if(keep&&!finish_error){strcat(output,cerberus_gpx_footer(gps_recording_status.mode));strcpy(name,"GPS-T00001.GPX");}
    *bytes=strlen(output);return finish_error;
}
static void fresh(unsigned second)
{
    receiver=(cerberus_wardrive_gps_uart_state_t){.driver_active=true,.reader_running=true,.clock_valid=true,
        .gps={.state=CERBERUS_WARDRIVE_GPS_FRESH,.coordinates_valid=true,.utc_valid=true,.utc_from_rmc=true,
            .latitude=12.0+second*.0001,.longitude=34.0}};
    snprintf(receiver.gps.utc,sizeof(receiver.gps.utc),"2026-11-08T00:00:%02u.000Z",second);
}
static void reset(void)
{
    assert(!gps_recording&&!gps_recording_status.busy&&!locks&&!sd_owned&&!gps_owned);
    memset(&gps_recording_status,0,sizeof(gps_recording_status));memset(&receiver,0,sizeof(receiver));
    task=NULL;task_arg=NULL;on_delay=NULL;on_allocate=NULL;
    allocations=frees=delays=attaches=detaches=writes=opens=closes=0;
    attach_wait=detach_wait=open_wait=append_wait=close_wait=0;
    fail_alloc=fail_task=small_heap=small_block=false;
    read_error=attach_error=append_error=finish_error=release_error=ESP_OK;
    output[0]=0;now_us=1000000;
}
static void stop(void){assert(cerberus_gps_recorder_stop(gps_recording_status.job)==ESP_OK);}
static void run(void)
{assert(task);task(task_arg);assert(!gps_recording&&!gps_recording_status.busy&&!locks&&frees==allocations);}
static void waiting(void)
{
    assert(!opens&&!writes&&!gps_recording_status.fix_ready);
    if(now_us>=5000000)stop();
}
static void tracking(void)
{
    if(gps_recording_status.state==CERBERUS_GPS_LOG_STOPPING)return;
    unsigned elapsed=(unsigned)(now_us/1000000)-1;
    if(elapsed<2){assert(!opens);return;}
    if(elapsed==5)receiver.gps.state=CERBERUS_WARDRIVE_GPS_NO_FIX;
    else fresh(elapsed);
    if(elapsed>=8)stop();
}
static void stop_allocating(void){stop();}
static unsigned occurrences(const char *text,const char *word)
{unsigned n=0;while((text=strstr(text,word))!=NULL){++n;text+=strlen(word);}return n;}
int main(void)
{
    reset();uint64_t job;
    assert(cerberus_gps_recorder_start(CERBERUS_GPX_TRACK,NULL)==ESP_ERR_INVALID_ARG);
    for(unsigned fault=0;fault<4;++fault){
        reset();small_heap=fault==0;small_block=fault==1;fail_alloc=fault==2;fail_task=fault==3;
        assert(cerberus_gps_recorder_start(CERBERUS_GPX_TRACK,&job)==ESP_ERR_NO_MEM&&!job);
        assert(!gps_recording_status.busy&&!gps_recording&&!attaches);
        assert(frees==(fault==3?1U:0U));
    }
    reset();on_allocate=stop_allocating;
    assert(cerberus_gps_recorder_start(CERBERUS_GPX_TRACK,&job)==ESP_OK);run();
    assert(!attaches&&!opens&&gps_recording_status.state==CERBERUS_GPS_LOG_STOPPED);
    reset();attach_wait=2;on_delay=stop;
    assert(cerberus_gps_recorder_start(CERBERUS_GPX_TRACK,&job)==ESP_OK);run();
    assert(attaches==1&&!opens&&!gps_recording_status.error);
    reset();on_delay=waiting;
    assert(cerberus_gps_recorder_start(CERBERUS_GPX_TRACK,&job)==ESP_OK);
    uint64_t other;assert(cerberus_gps_recorder_start(CERBERUS_GPX_POINTS,&other)==ESP_ERR_INVALID_STATE&&!other);
    assert(cerberus_gps_recorder_stop(job+1)==ESP_ERR_INVALID_STATE);
    run();assert(!opens&&!writes&&!gps_recording_status.error);
    assert(cerberus_gps_recorder_stop(job)==ESP_OK);

    reset();on_delay=tracking;open_wait=2;append_wait=3;close_wait=2;detach_wait=3;
    assert(cerberus_gps_recorder_start(CERBERUS_GPX_TRACK,&job)==ESP_OK);run();
    assert(gps_recording_status.state==CERBERUS_GPS_LOG_STOPPED&&opens==1&&closes==1);
    assert(writes>=4&&writes<=6&&gps_recording_status.written==writes&&gps_recording_status.track.segments==2);
    assert(occurrences(output,"<trkseg>")==2&&occurrences(output,"<trkpt ")==writes);
    assert(gps_recording_status.file_name[0]&&gps_recording_status.bytes==strlen(output)&&!gps_recording_status.discarded);

    reset();fresh(1);
    assert(cerberus_gps_recorder_start(CERBERUS_GPX_POINTS,&job)==ESP_OK);
    assert(cerberus_gps_recorder_mark(job+1,"wrong")==ESP_ERR_INVALID_STATE);
    assert(cerberus_gps_recorder_mark(job,"bad\nlabel")==ESP_ERR_INVALID_ARG);
    for(unsigned i=0;i<4;++i){fresh(i);assert(cerberus_gps_recorder_mark(job,i==0?"Bridge & park":NULL)==ESP_OK);}
    assert(cerberus_gps_recorder_mark(job,NULL)==ESP_ERR_NO_MEM&&gps_recording_status.rejected==1);
    stop();assert(cerberus_gps_recorder_mark(job,NULL)==ESP_ERR_INVALID_STATE);
    receiver.gps.state=CERBERUS_WARDRIVE_GPS_NO_FIX;read_error=ESP_FAIL;run();
    assert(writes==4&&!gps_recording_status.discarded&&!gps_recording_status.error);
    assert(occurrences(output,"<wpt ")==4&&strstr(output,"Bridge &amp; park")&&strstr(output,"POI 4"));
    assert(strstr(output,"00:00:00.000Z")&&strstr(output,"00:00:03.000Z"));

    reset();fresh(0);assert(cerberus_gps_recorder_start(CERBERUS_GPX_POINTS,&job)==ESP_OK);
    assert(cerberus_gps_recorder_mark(job,NULL)==ESP_OK&&cerberus_gps_recorder_mark(job,NULL)==ESP_OK);
    append_error=finish_error=ESP_FAIL;run();
    assert(gps_recording_status.state==CERBERUS_GPS_LOG_ERROR&&gps_recording_status.discarded==2&&!writes&&!gps_recording_status.file_name[0]);
    reset();attach_error=ESP_FAIL;release_error=ESP_FAIL;
    assert(cerberus_gps_recorder_start(CERBERUS_GPX_TRACK,&job)==ESP_OK);run();
    assert(gps_recording_status.error==ESP_FAIL&&!opens&&detaches==2);
    reset();fresh(0);on_delay=stop;release_error=ESP_ERR_TIMEOUT;
    assert(cerberus_gps_recorder_start(CERBERUS_GPX_TRACK,&job)==ESP_OK);run();
    assert(gps_recording_status.error==ESP_ERR_TIMEOUT&&closes==1&&gps_recording_status.file_name[0]);
    reset();now_us=-1;assert(cerberus_gps_recorder_start(CERBERUS_GPX_TRACK,&job)==ESP_ERR_INVALID_STATE);
    printf("GPX worker: GPS wait/cancel, track gaps, bounded POI queue, original fixes, exact jobs, SD retries and retained cleanup pass; context %zu bytes.\n",sizeof(gps_recorder_t));
}
