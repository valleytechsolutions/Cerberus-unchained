#undef NDEBUG
#include <assert.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "recon_recorder.h"

static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr,"Recorder check %u: %s\n",__LINE__,#x); abort(); } } while(0)
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
static bool locked;
#define portENTER_CRITICAL(p) do { (void)(p); CHECK(!locked); locked=true; } while(0)
#define portEXIT_CRITICAL(p) do { (void)(p); CHECK(locked); locked=false; } while(0)
#define MALLOC_CAP_INTERNAL 1U
#define MALLOC_CAP_8BIT 2U
#define MALLOC_CAP_SPIRAM 4U
#define pdPASS 1
#define pdMS_TO_TICKS(x) (x)
static int64_t clock_us=1000;
static size_t free_internal=100000,largest_internal=50000;
static unsigned allocations,frees,tasks,deletes,delays,outstanding;
static bool fail_alloc,fail_task,stop_during_alloc,stop_during_begin,immediate_task;
static int64_t clock_script[8];
static unsigned clock_script_count,clock_script_index;
static void (*pending_task)(void *);
static void *pending_argument;
static void (*delay_hook)(void);
static int64_t esp_timer_get_time(void){CHECK(!locked);return clock_script_index<clock_script_count?clock_script[clock_script_index++]:clock_us;}
static size_t heap_caps_get_free_size(unsigned caps){CHECK(!locked&&caps==(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT));return free_internal;}
static size_t heap_caps_get_largest_free_block(unsigned caps){CHECK(!locked&&caps==(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT));return largest_internal;}
static void *heap_caps_calloc(size_t count,size_t size,unsigned caps)
{CHECK(!locked&&count==1&&caps==(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));++allocations;void *p=fail_alloc?NULL:calloc(count,size);if(p)++outstanding;if(stop_during_alloc)cerberus_recon_recorder_stop();return p;}
static void heap_caps_free(void *p){CHECK(!locked);if(p){CHECK(outstanding>0);--outstanding;++frees;}free(p);}
static int xTaskCreate(void (*fn)(void *),const char *name,unsigned stack,void *arg,unsigned priority,void *handle)
{CHECK(!locked&&!pending_task&&!strcmp(name,"recon_sd")&&stack==6144&&priority==2&&handle==NULL);++tasks;if(fail_task)return 0;if(immediate_task)fn(arg);else{pending_task=fn;pending_argument=arg;}return pdPASS;}
static void vTaskDelete(void *handle){CHECK(!locked&&handle==NULL);++deletes;}
static void vTaskDelay(unsigned ticks){CHECK(!locked&&(ticks==1||ticks==20)&&++delays<500);clock_us+=(int64_t)ticks*1000;if(delay_hook)delay_hook();}

static unsigned begins,appends,syncs,finishes;
static esp_err_t begin_error,append_error,sync_error,finish_error;
static bool begin_partial,lease,finish_success;
static unsigned fail_append_at;
static char final_manifest[2048];
static size_t final_length;
static uint64_t offered_during_append;
static cerberus_recon_record_t record(uint64_t when)
{
    cerberus_recon_record_t row={0};row.consumed_us=when;row.radio=CERBERUS_RECON_RADIO_BLE;
    row.rssi=-42;row.rssi_valid=true;row.address[0]=0xA1;row.name[0]='X';row.name_length=1;
    return row;
}
static esp_err_t cerberus_storage_recon_begin(const char *header,size_t length,char *name,size_t cap)
{
    CHECK(!locked&&!lease&&header&&length&&cap==7);++begins;
    cerberus_recon_recorder_status_t s;cerberus_recon_recorder_get_status(&s);CHECK(s.worker_active);
    if(begin_error==ESP_OK||begin_partial)memcpy(name,"R00001",7);
    if(begin_error==ESP_OK)lease=true;
    if(stop_during_begin)cerberus_recon_recorder_stop();
    return begin_error;
}
static esp_err_t cerberus_storage_recon_append(const char *row,size_t length)
{
    CHECK(!locked&&lease&&row&&length&&length<768);++appends;
    if(offered_during_append){cerberus_recon_record_t next=record((uint64_t)clock_us);offered_during_append=0;CHECK(cerberus_recon_recorder_offer(&next));}
    return fail_append_at==appends?append_error:ESP_OK;
}
static esp_err_t cerberus_storage_recon_sync(void){CHECK(!locked&&lease);++syncs;return sync_error;}
static esp_err_t cerberus_storage_recon_finish(const char *manifest,size_t length,bool success)
{
    CHECK(!locked&&lease&&length<sizeof(final_manifest));++finishes;
    cerberus_recon_recorder_status_t s;cerberus_recon_recorder_get_status(&s);
    CHECK(s.worker_active&&s.state==CERBERUS_RECON_RECORDER_STOPPING);
    CHECK(cerberus_recon_recorder_start()==ESP_ERR_INVALID_STATE);
    if(length)memcpy(final_manifest,manifest,length);final_manifest[length]=0;final_length=length;
    finish_success=success;lease=false;return finish_error;
}
#include "recorder_impl.h"

static char console_text[2048];static size_t console_length;
static void console_printf(const char *format,...)
{va_list args;va_start(args,format);int n=vsnprintf(console_text+console_length,sizeof(console_text)-console_length,format,args);va_end(args);CHECK(n>=0&&(size_t)n<sizeof(console_text)-console_length);console_length+=(size_t)n;}
#include "recorder_console_impl.h"

static cerberus_recon_recorder_status_t snapshot(void)
{cerberus_recon_recorder_status_t s;cerberus_recon_recorder_get_status(&s);return s;}
static void reset(void)
{
    CHECK(!locked&&!pending_task&&!lease&&!recorder_context&&!recorder_status.worker_active&&!outstanding);
    clock_us+=100000;free_internal=100000;largest_internal=50000;fail_alloc=fail_task=false;
    begin_error=append_error=sync_error=finish_error=ESP_OK;begin_partial=false;fail_append_at=0;
    begins=appends=syncs=finishes=delays=0;finish_success=false;final_length=0;final_manifest[0]=0;
    offered_during_append=0;delay_hook=NULL;console_length=0;console_text[0]=0;
    stop_during_alloc=stop_during_begin=immediate_task=false;clock_script_count=clock_script_index=0;
}
static void run_worker(void)
{
    CHECK(pending_task!=NULL);void (*fn)(void *)=pending_task;void *arg=pending_argument;
    pending_task=NULL;pending_argument=NULL;unsigned old_deletes=deletes;fn(arg);
    CHECK(deletes==old_deletes+1&&!locked&&!lease&&!recorder_context&&!snapshot().worker_active&&!outstanding);
}
static void stop_hook(void){cerberus_recon_recorder_stop();delay_hook=NULL;}
static void add(unsigned count)
{for(unsigned i=0;i<count;++i){cerberus_recon_record_t row=record((uint64_t)clock_us);
    if(i%3U==0U)row.radio=CERBERUS_RECON_RADIO_WIFI;else row.ibeacon=i%3U==2U;
    CHECK(cerberus_recon_recorder_offer(&row));}}
static void typed_counts(cerberus_recon_counters_t c,uint32_t aw,uint32_t ab,uint32_t ai,
    uint32_t ww,uint32_t wb,uint32_t wi)
{
    CHECK(c.accepted_wifi_rows==aw&&c.accepted_ble_rows==ab&&c.accepted_ibeacon_rows==ai);
    CHECK(c.written_wifi_rows==ww&&c.written_ble_rows==wb&&c.written_ibeacon_rows==wi);
    CHECK(c.accepted==aw+ab&&c.written==ww+wb&&ai<=ab&&wi<=wb);
}
static void console_typed_counts(cerberus_recon_counters_t c)
{
    const char *names[]={"accepted_wifi_rows","accepted_ble_rows","accepted_ibeacon_rows",
        "written_wifi_rows","written_ble_rows","written_ibeacon_rows"};
    uint32_t values[]={c.accepted_wifi_rows,c.accepted_ble_rows,c.accepted_ibeacon_rows,
        c.written_wifi_rows,c.written_ble_rows,c.written_ibeacon_rows};
    for(unsigned i=0;i<6;++i){char field[96];
        CHECK(snprintf(field,sizeof(field),"recon_record_%s=%" PRIu32 "\r\n",names[i],values[i])>0);
        CHECK(strstr(console_text,field));}
}
static void successful_lifecycle(const char *directory)
{
    reset();CHECK(cerberus_recon_recorder_start()==ESP_OK);CHECK(begins==0&&appends==0&&snapshot().state==CERBERUS_RECON_RECORDER_STARTING);
    uint64_t first=snapshot().session_id;CHECK(cerberus_recon_recorder_start()==ESP_ERR_INVALID_STATE);
    add(3);delay_hook=stop_hook;offered_during_append=1;run_worker();
    cerberus_recon_recorder_status_t s=snapshot();CHECK(s.state==CERBERUS_RECON_RECORDER_COMPLETE&&s.error==ESP_OK);
    CHECK(s.counters.accepted==4&&s.counters.written==4&&!s.counters.abandoned&&!s.queued);
    typed_counts(s.counters,1,3,1,1,3,1);
    CHECK(begins==1&&appends==4&&finishes==1&&finish_success&&final_length&&!strcmp(s.session_name,"R00001"));
    char path[1024];CHECK(snprintf(path,sizeof(path),"%s/complete-session.json",directory)>0);
    FILE *out=fopen(path,"wb");CHECK(out&&fwrite(final_manifest,1,final_length,out)==final_length&&fclose(out)==0);
    unsigned io=begins+appends+syncs+finishes;print_recon_record();CHECK(io==begins+appends+syncs+finishes);
    CHECK(strstr(console_text,"recon_record_state=COMPLETE\r\n")&&strstr(console_text,"recon_record_active=0\r\n")&&strstr(console_text,"recon_record_written=4\r\n"));
    console_typed_counts(s.counters);
    cerberus_recon_recorder_stop();CHECK(snapshot().state==CERBERUS_RECON_RECORDER_COMPLETE);
    reset();CHECK(cerberus_recon_recorder_start()==ESP_OK&&snapshot().session_id>first);cerberus_recon_recorder_stop();run_worker();
    CHECK(snapshot().state==CERBERUS_RECON_RECORDER_CANCELED&&!begins&&!finishes&&!snapshot().session_name[0]);
    typed_counts(snapshot().counters,0,0,0,0,0,0);
}
static void failure_paths(void)
{
    for(unsigned kind=0;kind<4;++kind){
        reset();if(kind==0)free_internal=24000;if(kind==1)largest_internal=8191;if(kind==2)fail_alloc=true;if(kind==3)fail_task=true;
        CHECK(cerberus_recon_recorder_start()==ESP_ERR_NO_MEM);CHECK(snapshot().state==CERBERUS_RECON_RECORDER_ERROR&&!snapshot().worker_active&&!recorder_context&&!begins&&!pending_task);
    }
    for(unsigned partial=0;partial<2;++partial){
        reset();begin_error=ESP_ERR_INVALID_STATE;begin_partial=partial!=0;CHECK(cerberus_recon_recorder_start()==ESP_OK);add(3);run_worker();
        CHECK(snapshot().state==CERBERUS_RECON_RECORDER_ERROR&&snapshot().error==begin_error&&snapshot().counters.abandoned==3&&!snapshot().queued&&!finishes);
        typed_counts(snapshot().counters,1,2,1,0,0,0);
        CHECK((snapshot().session_name[0]!=0)==(partial!=0));
    }
    for(unsigned nth=1;nth<=4;++nth){
        reset();append_error=ESP_FAIL;fail_append_at=nth;CHECK(cerberus_recon_recorder_start()==ESP_OK);add(4);cerberus_recon_recorder_stop();run_worker();
        CHECK(appends==nth&&finishes==1&&!finish_success&&snapshot().state==CERBERUS_RECON_RECORDER_ERROR);
        CHECK(snapshot().counters.written==nth-1&&snapshot().counters.abandoned==5-nth&&!snapshot().queued);
        typed_counts(snapshot().counters,2,2,1,nth>1?1:0,nth>2?nth-2:0,nth>3?1:0);
        CHECK(final_length&&strstr(final_manifest,"\"state\":\"error\""));
        print_recon_record();console_typed_counts(snapshot().counters);
    }
    reset();finish_error=ESP_FAIL;CHECK(cerberus_recon_recorder_start()==ESP_OK);add(1);cerberus_recon_recorder_stop();run_worker();
    CHECK(snapshot().state==CERBERUS_RECON_RECORDER_ERROR&&snapshot().error==ESP_FAIL&&snapshot().counters.written==1&&finishes==1);
}
static void sync_failure_hook(void){clock_us+=3000000;delay_hook=NULL;}
static void invalid_clock_hook(void){clock_us=-1;delay_hook=NULL;}
static void startup_interleavings(void)
{
    reset();stop_during_alloc=true;CHECK(cerberus_recon_recorder_start()==ESP_OK);run_worker();
    CHECK(snapshot().state==CERBERUS_RECON_RECORDER_CANCELED&&!begins&&!outstanding);
    reset();stop_during_begin=true;CHECK(cerberus_recon_recorder_start()==ESP_OK);run_worker();
    CHECK(snapshot().state==CERBERUS_RECON_RECORDER_COMPLETE&&finishes==1&&finish_success&&!outstanding);
    reset();immediate_task=true;delay_hook=stop_hook;CHECK(cerberus_recon_recorder_start()==ESP_OK);
    CHECK(snapshot().state==CERBERUS_RECON_RECORDER_COMPLETE&&finishes==1&&!pending_task&&!outstanding);
}
static void clock_interleavings(void)
{
    reset();CHECK(cerberus_recon_recorder_start()==ESP_OK);add(3);
    clock_script[0]=-1;clock_script_count=1;run_worker();
    CHECK(snapshot().state==CERBERUS_RECON_RECORDER_ERROR&&snapshot().counters.abandoned==3&&!appends&&finishes==1&&!finish_success);
    reset();CHECK(cerberus_recon_recorder_start()==ESP_OK);add(4);cerberus_recon_recorder_stop();
    clock_script[0]=clock_us+100;clock_script[1]=clock_us+200;clock_script[2]=clock_us+150;clock_script_count=3;
    run_worker();CHECK(snapshot().state==CERBERUS_RECON_RECORDER_ERROR&&appends==2&&snapshot().counters.abandoned==2&&!finish_success);
    reset();CHECK(cerberus_recon_recorder_start()==ESP_OK);add(2);cerberus_recon_recorder_stop();
    clock_script[0]=clock_us+100;clock_script[1]=clock_us+200;clock_script[2]=clock_us+300;clock_script[3]=clock_us+250;clock_script_count=4;
    run_worker();CHECK(snapshot().state==CERBERUS_RECON_RECORDER_ERROR&&!snapshot().ended_us&&!final_length&&finishes==1&&!finish_success);
}
static void bounds_and_timing(void)
{
    reset();CHECK(cerberus_recon_recorder_start()==ESP_OK);add(64);cerberus_recon_record_t row=record((uint64_t)clock_us);
    CHECK(!cerberus_recon_recorder_offer(&row)&&snapshot().counters.dropped==1&&snapshot().queued==64);
    row.consumed_us=(uint64_t)clock_us+1;CHECK(!cerberus_recon_recorder_offer(&row)&&snapshot().counters.invalid==1);
    CHECK(!cerberus_recon_recorder_offer(NULL)&&snapshot().counters.invalid==2);cerberus_recon_recorder_stop();
    typed_counts(snapshot().counters,22,42,21,0,0,0);
    row=record((uint64_t)clock_us);CHECK(!cerberus_recon_recorder_offer(&row)&&snapshot().queued==64);run_worker();
    CHECK(snapshot().state==CERBERUS_RECON_RECORDER_COMPLETE&&appends==64&&snapshot().counters.dropped==1);
    typed_counts(snapshot().counters,22,42,21,22,42,21);
    reset();sync_error=ESP_FAIL;CHECK(cerberus_recon_recorder_start()==ESP_OK);delay_hook=sync_failure_hook;run_worker();
    CHECK(syncs==1&&finishes==1&&!finish_success&&snapshot().state==CERBERUS_RECON_RECORDER_ERROR);
    reset();CHECK(cerberus_recon_recorder_start()==ESP_OK);delay_hook=invalid_clock_hook;run_worker();
    CHECK(snapshot().state==CERBERUS_RECON_RECORDER_ERROR&&snapshot().error==ESP_ERR_INVALID_STATE&&!snapshot().ended_us&&finishes==1&&!finish_success);
    clock_us=100000;reset();CHECK(cerberus_recon_recorder_start()==ESP_OK);recorder_next_sequence=UINT64_MAX;
    row=record((uint64_t)clock_us);CHECK(!cerberus_recon_recorder_offer(&row));run_worker();CHECK(snapshot().state==CERBERUS_RECON_RECORDER_ERROR);
    reset();recorder_last_session=UINT64_MAX;CHECK(cerberus_recon_recorder_start()==ESP_ERR_INVALID_STATE&&!pending_task&&!begins);
    CHECK(!strcmp(cerberus_recon_recorder_state_name((cerberus_recon_recorder_state_t)999),"UNKNOWN"));
}
int main(int argc,char **argv)
{
    CHECK(argc==2);cerberus_recon_recorder_get_status(NULL);CHECK(snapshot().state==CERBERUS_RECON_RECORDER_IDLE);
    successful_lifecycle(argv[1]);failure_paths();startup_interleavings();clock_interleavings();bounds_and_timing();
    CHECK(!locked&&!lease&&!pending_task&&!recorder_context&&!outstanding);printf("Async Recon recorder: %u checks, bounded queue, no UI IO, concurrent offers, Stop/failure cleanup and diagnostic state passed\n",checks);
    return 0;
}
