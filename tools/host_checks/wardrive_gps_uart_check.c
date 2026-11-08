#undef NDEBUG
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gps_sdk_stub.h"
#include "wardrive_gps_uart.h"
static cerberus_gps_config_t test_config;
static uint32_t configured_baud;
cerberus_gps_config_t cerberus_settings_gps_get(void){return test_config;}
static unsigned checks;
#define CHECK(x) do { ++checks; assert(x); } while (0)
static bool locked, mutex_fail, create_mutex_fail, alloc_fail, task_fail, driver_installed;
static unsigned installs, deletes, reads, creates, task_exits, allocations, frees, delays;
static unsigned release_after_delay;
static unsigned timed_takes, fail_timed_take;
static esp_err_t install_error, config_error, pin_error, flush_error, delete_error;
static int read_error;
static int64_t now_us;
static void (*reader_task)(void *), (*sdk_hook)(void), (*allocation_hook)(void);
static const uint8_t *input;
static size_t input_size, input_offset, input_chunk;
static jmp_buf yield_reader;

SemaphoreHandle_t xSemaphoreCreateMutex(void) { return create_mutex_fail ? NULL : (void *)1; }
int xSemaphoreTake(SemaphoreHandle_t lock, unsigned wait)
{
    CHECK(lock != NULL && !locked);
    if (wait != portMAX_DELAY && ++timed_takes==fail_timed_take) return 0;
    if (wait != portMAX_DELAY && mutex_fail) { mutex_fail=false; return 0; }
    locked=true; return pdTRUE;
}
void xSemaphoreGive(SemaphoreHandle_t lock) { CHECK(lock && locked); locked=false; }
int64_t esp_timer_get_time(void) { return now_us; }
esp_err_t uart_driver_install(int port,int rx,int tx,int queue,void *handle,int flags)
{
    CHECK(!locked && port==2 && rx==4096 && !tx && !queue && !handle && !flags);
    ++installs; CHECK(!driver_installed);
    if (install_error) return install_error;
    driver_installed=true; return ESP_OK;
}
esp_err_t uart_param_config(int port,const uart_config_t *config)
{
    CHECK(!locked && port==2 && driver_installed);
    CHECK((uint32_t)config->baud_rate==(test_config.baud?test_config.baud:115200U) && config->data_bits==8 && !config->parity && config->stop_bits==1);
    configured_baud=(uint32_t)config->baud_rate;
    if (sdk_hook) {void (*hook)(void)=sdk_hook;sdk_hook=NULL;hook();}
    return config_error;
}
esp_err_t uart_set_pin(int port,int tx,int rx,int rts,int cts)
{ CHECK(!locked && port==2 && tx==-1 && rx==test_config.rx_gpio && rts==-1 && cts==-1);return pin_error; }
esp_err_t uart_set_baudrate(int port,uint32_t baud)
{CHECK(!locked&&port==2&&cerberus_gps_baud_valid(baud)&&baud);configured_baud=baud;return config_error;}
esp_err_t uart_flush_input(int port) {CHECK(!locked && port==2);return flush_error;}
esp_err_t uart_driver_delete(int port)
{
    CHECK(!locked && port==2 && driver_installed); ++deletes;
    if (delete_error) return delete_error;
    driver_installed=false; return ESP_OK;
}
int uart_read_bytes(int port,void *bytes,unsigned capacity,unsigned wait)
{
    CHECK(!locked && port==2 && capacity==256 && wait==20 && driver_installed);++reads;
    if (read_error) return read_error;
    if (input_offset==input_size) longjmp(yield_reader,1);
    size_t count=input_size-input_offset;
    if (count>input_chunk) count=input_chunk;
    if (count>capacity) count=capacity;
    memcpy(bytes,input+input_offset,count);input_offset+=count;return (int)count;
}
int xTaskCreate(void (*task)(void *),const char *name,unsigned stack,void *arg,unsigned priority,void *handle)
{
    CHECK(!locked && !strcmp(name,"wardrive_gps") && stack==3584 && !arg && priority==3 && !handle);
    ++creates; if(task_fail)return 0; reader_task=task;return pdPASS;
}
void vTaskDelete(void *arg) {CHECK(!locked && !arg);++task_exits;}
void vTaskDelay(unsigned ticks)
{
    CHECK(!locked && ticks==20);++delays;now_us+=(int64_t)ticks*1000;
    if (release_after_delay && delays>=release_after_delay) {release_after_delay=0;reader_task(NULL);}
}
void *heap_caps_calloc(size_t count,size_t size,unsigned caps)
{
    CHECK(!locked && count==1 && caps==(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));++allocations;
    if(allocation_hook){void(*hook)(void)=allocation_hook;allocation_hook=NULL;hook();}
    return alloc_fail?NULL:calloc(count,size);
}
void heap_caps_free(void *ptr) {CHECK(!locked && ptr);++frees;free(ptr);}

/* Whole production owner, not a lifecycle reimplementation. */
#include "../../s3_main/main/wardrive_gps_uart.c"

static void reset(void)
{
    CHECK(!locked);if(trace_store)free(trace_store);
    gps_lock=NULL;memset(&gps_core,0,sizeof(gps_core));
    uart_active=task_running=stop_requested=start_in_progress=false;
    last_uart_error=0;receiver_epoch=clock_highwater=0;clock_seen=receipt_clock_valid=false;
    trace_store=NULL;trace_token=trace_token_counter=trace_sequence=0;
    installs=deletes=reads=creates=task_exits=allocations=frees=delays=release_after_delay=0;
    timed_takes=fail_timed_take=0;
    mutex_fail=create_mutex_fail=alloc_fail=task_fail=driver_installed=false;
    install_error=config_error=pin_error=flush_error=delete_error=read_error=0;
    reader_task=NULL;sdk_hook=allocation_hook=NULL;now_us=1000000;
    test_config=cerberus_gps_config_default();cerberus_gps_config_profile(&test_config,CERBERUS_GPS_ATGM336H);
    gps_consumers=0;atomic_flag_clear(&gps_control);configured_baud=0;
}
static void feed(const void *bytes,size_t count,size_t chunk)
{
    input=bytes;input_size=count;input_offset=0;input_chunk=chunk;
    if(!setjmp(yield_reader))reader_task(NULL);
    CHECK(input_offset==input_size && !locked);
}
static size_t sentence(char *out,const char *body,const char *ending)
{
    uint8_t sum=0;for(const char *p=body;*p;++p)sum^=(uint8_t)*p;
    int length=snprintf(out,180,"$%s*%02X%s",body,sum,ending);CHECK(length>0 && length<180);return(size_t)length;
}
static void stop_clean(void)
{
    release_after_delay=delays+1;
    CHECK(cerberus_wardrive_gps_uart_stop()==ESP_OK);
    CHECK(!driver_installed && !uart_active && !task_running);
}
static void off_and_trace(void)
{
    reset();cerberus_wardrive_gps_uart_state_t state;
    cerberus_wardrive_gps_trace_snapshot_t snapshot;uint64_t token=42;
    CHECK(cerberus_wardrive_gps_uart_get_state(&state)==ESP_OK);
    CHECK(!state.driver_active && !state.reader_running && state.gps.utc_age_ms==UINT32_MAX && state.gps.gga_age_ms==UINT32_MAX);
    CHECK(cerberus_wardrive_gps_uart_trace_begin(&token)==ESP_ERR_NOT_FINISHED && !token);
    CHECK(cerberus_wardrive_gps_uart_trace_read(1,false,&snapshot)==ESP_ERR_NOT_FOUND && !snapshot.count);
    CHECK(cerberus_wardrive_gps_uart_trace_end(1)==ESP_ERR_NOT_FOUND);
    CHECK(!installs && !reads && !creates && !allocations && !gps_lock);
    CHECK(cerberus_wardrive_gps_uart_start()==ESP_OK);
    CHECK(cerberus_wardrive_gps_uart_start()==ESP_ERR_INVALID_STATE && installs==1);
    alloc_fail=true;CHECK(cerberus_wardrive_gps_uart_trace_begin(&token)==ESP_ERR_NO_MEM && !token);
    alloc_fail=false;CHECK(cerberus_wardrive_gps_uart_trace_begin(&token)==ESP_OK && token);
    unsigned allocation_count=allocations;uint64_t other;
    CHECK(cerberus_wardrive_gps_uart_trace_begin(&other)==ESP_ERR_INVALID_STATE && !other && allocations==allocation_count);
    char text[180];size_t size=sentence(text,"GPRMC,120000.000,A,4807.038,N,01131.000,E,0,0,010124","\r\n");
    feed(text,size-1,1);
    CHECK(cerberus_wardrive_gps_uart_trace_read(token,false,&snapshot)==ESP_OK && snapshot.count==0);
    feed(text+size-1,1,1);
    CHECK(cerberus_wardrive_gps_uart_trace_read(token,false,&snapshot)==ESP_OK && snapshot.count==1);
    CHECK(snapshot.rows[0].result==CERBERUS_WARDRIVE_GPS_LINE_RMC && snapshot.rows[0].termination==CERBERUS_WARDRIVE_GPS_TERMINATION_CRLF);
    CHECK(snapshot.rows[0].retained_length==size-2 && !memcmp(snapshot.rows[0].payload,text,size-2));
    CHECK(snapshot.rows[0].received_ms==1000 && snapshot.rows[0].receiver_epoch==1 && snapshot.rows[0].sequence==1);
    CHECK(cerberus_wardrive_gps_uart_get_state(&state)==ESP_OK && state.gps.coordinates_valid);
    CHECK(cerberus_wardrive_gps_uart_trace_read(token,true,&snapshot)==ESP_OK);
    CHECK(snapshot.rows[0].flags&CERBERUS_WARDRIVE_GPS_TRACE_MASKED);
    CHECK(!snapshot.rows[0].retained_length);
    for(size_t i=0;i<sizeof(snapshot.rows[0].payload);++i)CHECK(!snapshot.rows[0].payload[i]);
    const uint8_t bad[]={ '$','X',0,'Y','\n' };feed(bad,sizeof(bad),2);
    uint8_t overflow[141];memset(overflow,'x',sizeof(overflow));overflow[140]='\n';feed(overflow,sizeof(overflow),7);
    size=sentence(text,"GPTXT,01,01,02,hello","\n");text[size-3]='0';text[size-2]='0';feed(text,size,3);
    CHECK(cerberus_wardrive_gps_uart_trace_read(token,true,&snapshot)==ESP_OK && snapshot.count==4);
    for(unsigned i=0;i<snapshot.count;++i){
        CHECK(!snapshot.rows[i].retained_length && (snapshot.rows[i].flags&CERBERUS_WARDRIVE_GPS_TRACE_MASKED));
        for(size_t j=0;j<sizeof(snapshot.rows[i].payload);++j)CHECK(!snapshot.rows[i].payload[j]);
    }
    CHECK(cerberus_wardrive_gps_uart_trace_read(token,false,&snapshot)==ESP_OK);
    CHECK(snapshot.rows[1].result==CERBERUS_WARDRIVE_GPS_LINE_CONTROL && snapshot.rows[1].retained_length==4 &&
        !memcmp(snapshot.rows[1].payload,bad,4) && (snapshot.rows[1].flags&CERBERUS_WARDRIVE_GPS_TRACE_PAYLOAD_COMPLETE));
    CHECK(snapshot.rows[2].result==CERBERUS_WARDRIVE_GPS_LINE_OVERFLOW && snapshot.rows[2].retained_length==127 &&
        snapshot.rows[2].total_length==140 && !(snapshot.rows[2].flags&CERBERUS_WARDRIVE_GPS_TRACE_PAYLOAD_COMPLETE));
    CHECK(snapshot.rows[3].result==CERBERUS_WARDRIVE_GPS_LINE_BAD_CHECKSUM && snapshot.rows[3].retained_length);
    for(unsigned i=0;i<8;++i){size=sentence(text,"GPTXT,01,01,02,hello","\n");feed(text,size,4);}
    CHECK(cerberus_wardrive_gps_uart_trace_read(token,false,&snapshot)==ESP_OK);
    CHECK(snapshot.count==8 && snapshot.observed_lines==12 && snapshot.evicted_lines==4 && snapshot.overflow_lines==1 && snapshot.discarded_lines==2);
    for(unsigned i=0;i<8;++i)CHECK(snapshot.rows[i].sequence==i+5 && snapshot.rows[i].result==CERBERUS_WARDRIVE_GPS_LINE_OTHER);
    CHECK(allocations==allocation_count && !frees);
    mutex_fail=true;memset(&snapshot,0xaa,sizeof(snapshot));
    CHECK(cerberus_wardrive_gps_uart_trace_read(token,false,&snapshot)==ESP_ERR_TIMEOUT && !snapshot.observer_token && !snapshot.count);
    stop_clean();CHECK(cerberus_wardrive_gps_uart_trace_read(token,false,&snapshot)==ESP_OK && !snapshot.capturing && snapshot.count==8);
    CHECK(cerberus_wardrive_gps_uart_get_state(&state)==ESP_OK && !state.gps.coordinates_valid);
    CHECK(cerberus_wardrive_gps_uart_start()==ESP_OK);
    CHECK(cerberus_wardrive_gps_uart_trace_read(token,false,&snapshot)==ESP_OK && snapshot.receiver_epoch==2 && !snapshot.count);
    size=sentence(text,"GPTXT,01,01,02,new","\n");feed(text,size,20);
    CHECK(cerberus_wardrive_gps_uart_trace_read(token,false,&snapshot)==ESP_OK && snapshot.rows[0].sequence==13 && snapshot.rows[0].receiver_epoch==2);
    CHECK(cerberus_wardrive_gps_uart_trace_end(token+1)==ESP_ERR_NOT_FOUND && !frees);
    CHECK(cerberus_wardrive_gps_uart_trace_end(token)==ESP_OK && frees==1 && task_running);
    CHECK(cerberus_wardrive_gps_uart_trace_begin(&other)==ESP_OK && other>token);
    CHECK(cerberus_wardrive_gps_uart_trace_end(token)==ESP_ERR_NOT_FOUND && trace_token==other);
    CHECK(cerberus_wardrive_gps_uart_trace_end(other)==ESP_OK);stop_clean();
}
static void setup_and_cleanup(void)
{
    reset();create_mutex_fail=true;
    CHECK(cerberus_wardrive_gps_uart_start()==ESP_ERR_NO_MEM && !installs && !creates && !gps_lock);
    for(unsigned stage=0;stage<5;++stage){
        reset();if(stage==0)install_error=77;else if(stage==1)config_error=77;
        else if(stage==2)pin_error=77;else if(stage==3)flush_error=77;else task_fail=true;
        if(stage)delete_error=88;
        CHECK(cerberus_wardrive_gps_uart_start()!=ESP_OK);
        CHECK(!start_in_progress && !task_running && uart_active==(stage!=0) && driver_installed==(stage!=0));
        unsigned before=installs;
        if(stage){CHECK(cerberus_wardrive_gps_uart_start()==88 && installs==before);delete_error=0;CHECK(cerberus_wardrive_gps_uart_stop()==ESP_OK);}
        cerberus_wardrive_gps_uart_state_t state;CHECK(cerberus_wardrive_gps_uart_get_state(&state)==ESP_OK);
        CHECK(!state.driver_active && state.error==ESP_OK && state.historical_error!=ESP_OK);
        CHECK(cerberus_wardrive_gps_uart_stop()==ESP_OK);
    }
    reset();CHECK(cerberus_wardrive_gps_uart_start()==ESP_OK);
    CHECK(cerberus_wardrive_gps_uart_stop()==ESP_ERR_TIMEOUT && task_running && uart_active && !deletes);
    uint64_t token;CHECK(cerberus_wardrive_gps_uart_trace_begin(&token)==ESP_ERR_NOT_FINISHED && !allocations);
    delete_error=88;release_after_delay=delays+1;
    CHECK(cerberus_wardrive_gps_uart_stop()==88 && !task_running && uart_active);
    delete_error=0;CHECK(cerberus_wardrive_gps_uart_stop()==ESP_OK && !uart_active);
    CHECK(cerberus_wardrive_gps_uart_start()==ESP_OK);read_error=-1;
    reader_task(NULL);CHECK(!task_running && uart_active && stop_requested);
    cerberus_wardrive_gps_uart_state_t state;CHECK(cerberus_wardrive_gps_uart_get_state(&state)==ESP_OK);
    CHECK(state.error==ESP_FAIL && !state.gps.coordinates_valid && state.gps.fix_age_ms==UINT32_MAX);
    CHECK(cerberus_wardrive_gps_uart_stop()==ESP_OK);
}
static void cancel_setup(void)
{ CHECK(cerberus_wardrive_gps_uart_start()==ESP_ERR_NOT_FINISHED);CHECK(cerberus_wardrive_gps_uart_stop()==ESP_ERR_NOT_FINISHED); }
static void cancel_attach(void) { stop_clean(); }
static void clocks_and_identity(void)
{
    reset();sdk_hook=cancel_setup;CHECK(cerberus_wardrive_gps_uart_start()==ESP_OK);
    CHECK(driver_installed && creates==1 && installs==1 && deletes==0);stop_clean();
    CHECK(cerberus_wardrive_gps_uart_start()==ESP_OK);
    uint64_t token;allocation_hook=cancel_attach;
    CHECK(cerberus_wardrive_gps_uart_trace_begin(&token)==ESP_ERR_NOT_FINISHED && !token && frees==1);
    CHECK(cerberus_wardrive_gps_uart_start()==ESP_OK);
    CHECK(cerberus_wardrive_gps_uart_trace_begin(&token)==ESP_OK);
    char text[180];size_t size=sentence(text,"GPRMC,120000.000,A,4807.038,N,01131.000,E,0,0,010124","\n");
    feed(text,size,100);now_us=-1;feed(text,size,100);
    cerberus_wardrive_gps_uart_state_t state;cerberus_wardrive_gps_trace_snapshot_t snapshot;
    CHECK(cerberus_wardrive_gps_uart_get_state(&state)==ESP_OK && !state.clock_valid && !state.gps.coordinates_valid);
    CHECK(cerberus_wardrive_gps_uart_trace_read(token,false,&snapshot)==ESP_OK);
    CHECK(!(snapshot.rows[1].flags&CERBERUS_WARDRIVE_GPS_TRACE_RECEIPT_VALID) && !snapshot.rows[1].received_ms);
    now_us=1500000;CHECK(cerberus_wardrive_gps_uart_get_state(&state)==ESP_OK && !state.gps.coordinates_valid);
    feed(text,size,100);CHECK(cerberus_wardrive_gps_uart_get_state(&state)==ESP_OK && state.gps.coordinates_valid);
    now_us=1000000;CHECK(cerberus_wardrive_gps_uart_get_state(&state)==ESP_OK && !state.clock_valid && !state.gps.coordinates_valid);
    now_us=2000000;trace_sequence=UINT64_MAX-1;feed(text,size,100);feed(text,size,100);
    CHECK(cerberus_wardrive_gps_uart_trace_read(token,false,&snapshot)==ESP_OK && snapshot.sequence_exhausted && !snapshot.capturing);
    CHECK(snapshot.rows[snapshot.count-1].sequence==UINT64_MAX);
    trace_store->observed_lines=trace_store->evicted_lines=UINT32_MAX;feed(text,size,100);
    CHECK(trace_store->observed_lines==UINT32_MAX && trace_store->evicted_lines==UINT32_MAX);
    CHECK(cerberus_wardrive_gps_uart_trace_end(token)==ESP_OK);trace_token_counter=UINT64_MAX;
    unsigned before=allocations;CHECK(cerberus_wardrive_gps_uart_trace_begin(&token)==ESP_ERR_INVALID_STATE && !token && allocations==before);
    stop_clean();receiver_epoch=UINT64_MAX;before=installs;
    CHECK(cerberus_wardrive_gps_uart_start()==ESP_ERR_INVALID_STATE && installs==before);
}
static void observer_lock_timeouts(void)
{
    reset();CHECK(cerberus_wardrive_gps_uart_start()==ESP_OK);uint64_t token=99;
    fail_timed_take=timed_takes+1;
    CHECK(cerberus_wardrive_gps_uart_trace_begin(&token)==ESP_ERR_TIMEOUT && !token && !allocations);
    fail_timed_take=timed_takes+2;
    CHECK(cerberus_wardrive_gps_uart_trace_begin(&token)==ESP_ERR_TIMEOUT && !token && allocations==1 && frees==1);
    CHECK(!trace_store && !trace_token && task_running && uart_active);
    fail_timed_take=0;CHECK(cerberus_wardrive_gps_uart_trace_begin(&token)==ESP_OK);
    fail_timed_take=timed_takes+1;
    CHECK(cerberus_wardrive_gps_uart_trace_end(token)==ESP_ERR_TIMEOUT && trace_store && trace_token==token && frees==1);
    fail_timed_take=0;CHECK(cerberus_wardrive_gps_uart_trace_end(token)==ESP_OK && !trace_store && !trace_token && frees==2);
    CHECK(task_running && uart_active);stop_clean();
}
static void presets_preview_and_detection(void)
{
    reset();test_config=cerberus_gps_config_default();
    CHECK(cerberus_gps_config_valid(&test_config));
    CHECK(cerberus_wardrive_gps_uart_preview(true)==ESP_OK&&configured_baud==115200U);
    CHECK(cerberus_wardrive_gps_uart_start()==ESP_OK&&installs==1U);
    CHECK(cerberus_wardrive_gps_uart_record(true)==ESP_OK&&gps_consumers==7U&&installs==1U);
    CHECK(cerberus_wardrive_gps_uart_preview(false)==ESP_OK&&driver_installed);
    CHECK(cerberus_wardrive_gps_uart_stop()==ESP_OK&&driver_installed&&gps_consumers==4U);
    CHECK(cerberus_wardrive_gps_uart_start()==ESP_OK&&gps_consumers==5U&&installs==1U);
    CHECK(cerberus_wardrive_gps_uart_record(false)==ESP_OK&&driver_installed&&gps_consumers==1U);
    char text[180];size_t size=sentence(text,"GNRMC,120000.000,A,4807.038,N,01131.000,E,0,0,010124","\r\n");
    now_us+=2100000;feed(text,1,1);
    CHECK(configured_baud==9600U&&baud_changes==1U&&detecting_baud);
    feed(text,size,13);feed(text,size,7);
    CHECK(!detecting_baud&&gps_core.valid_sentences==2U);
    uint64_t epoch=receiver_epoch;
    CHECK(cerberus_wardrive_gps_uart_preview(true)==ESP_OK);
    CHECK(cerberus_wardrive_gps_uart_stop()==ESP_OK&&driver_installed&&receiver_epoch==epoch);
    CHECK(cerberus_wardrive_gps_uart_start()==ESP_OK&&installs==1U);
    CHECK(cerberus_wardrive_gps_uart_preview(false)==ESP_OK);
    stop_clean();
    reset();cerberus_gps_config_profile(&test_config,CERBERUS_GPS_LC29H_AA);
    CHECK(cerberus_wardrive_gps_uart_start()==ESP_OK&&configured_baud==115200U&&!detecting_baud);stop_clean();
    for(int pin=0;pin<49;++pin){cerberus_gps_config_t c=cerberus_gps_config_default();c.rx_gpio=(uint8_t)pin;
        CHECK(cerberus_gps_config_valid(&c)==(cerberus_gps_input_pin_valid(pin)&&pin!=2&&pin!=46));}
    reset();test_config.rx_gpio=44;CHECK(cerberus_wardrive_gps_uart_start()==ESP_ERR_INVALID_ARG&&!installs);
}
static void recent_time_after_stop(void)
{
    reset();cerberus_wardrive_gps_time_t sample;
    memset(&sample,0x5a,sizeof(sample));
    CHECK(cerberus_wardrive_gps_uart_recent_time(NULL)==ESP_ERR_INVALID_ARG);
    CHECK(cerberus_wardrive_gps_uart_recent_time(&sample)==ESP_ERR_NOT_FOUND&&!sample.utc[0]&&!sample.received_ms);
    CHECK(!installs&&!allocations&&!gps_lock);
    CHECK(cerberus_wardrive_gps_uart_start()==ESP_OK);
    char text[180];size_t size=sentence(text,"GNRMC,120000.123,A,4807.038,N,01131.000,E,0,0,010126","\r\n");
    feed(text,size,13);
    CHECK(cerberus_wardrive_gps_uart_recent_time(&sample)==ESP_OK);
    CHECK(!strcmp(sample.utc,"2026-01-01T12:00:00.123Z")&&sample.received_ms==gps_core.last_utc_ms);
    uint64_t received=sample.received_ms;now_us+=100000;
    size=sentence(text,"GNGGA,120000.123,4807.038,N,01131.000,E,1,10,0.9,100.0,M,0.0,M,,","\r\n");
    feed(text,size,13);
    CHECK(gps_core.last_fix_ms>gps_core.last_utc_ms);
    CHECK(cerberus_wardrive_gps_uart_recent_time(&sample)==ESP_OK&&sample.received_ms==received);
    stop_clean();
    unsigned old_installs=installs,old_reads=reads,old_allocations=allocations;
    now_us=(int64_t)received*1000+900000000;
    CHECK(cerberus_wardrive_gps_uart_recent_time(&sample)==ESP_OK&&sample.received_ms==received);
    cerberus_wardrive_gps_uart_state_t state;
    CHECK(cerberus_wardrive_gps_uart_get_state(&state)==ESP_OK&&!state.gps.utc_valid&&!state.gps.coordinates_valid);
    ++now_us;now_us+=999;
    CHECK(cerberus_wardrive_gps_uart_recent_time(&sample)==ESP_ERR_NOT_FOUND&&!sample.utc[0]);
    CHECK(installs==old_installs&&reads==old_reads&&allocations==old_allocations&&!driver_installed);
    now_us=(int64_t)received*1000;
    CHECK(cerberus_wardrive_gps_uart_recent_time(&sample)==ESP_ERR_NOT_FOUND); /* Clock reversal. */

    reset();CHECK(cerberus_wardrive_gps_uart_start()==ESP_OK);
    size=sentence(text,"GNRMC,120000.123,A,4807.038,N,01131.000,E,0,0,010126","\r\n");feed(text,size,13);
    fail_timed_take=timed_takes+1;
    CHECK(cerberus_wardrive_gps_uart_recent_time(&sample)==ESP_ERR_TIMEOUT&&!sample.utc[0]);fail_timed_take=0;
    size=sentence(text,"GNRMC,120001.000,V,,,,,,,010126","\r\n");feed(text,size,13);
    CHECK(cerberus_wardrive_gps_uart_recent_time(&sample)==ESP_ERR_NOT_FOUND&&!sample.utc[0]);
    stop_clean();
}
static void fix_class_start_gate(void)
{
    const char *bad[]={
        "GNRMC,120000.000,A,4807.038,N,01131.000,E,0,0,081126,,,E,V",
        "GNRMC,120000.000,V,,,,,,,081126,,,N,V",
        "GNGGA,120000.000,4807.038,N,01131.000,E,6,08,0.9,20,M,,,"};
    const char *good_rmc="GNRMC,120000.000,A,4807.038,N,01131.000,E,0,0,081126,,,A,V";
    const char *good_gga="GNGGA,120000.000,4807.038,N,01131.000,E,1,08,0.9,20,M,,,";
    for(unsigned i=0;i<3;++i)for(unsigned reverse=0;reverse<2;++reverse){
        reset();CHECK(cerberus_wardrive_gps_uart_start()==ESP_OK);
        const char *good=i==2?good_rmc:good_gga;
        char text[180];size_t size=sentence(text,reverse?good:bad[i],"\r\n");feed(text,size,13);
        size=sentence(text,reverse?bad[i]:good,"\r\n");feed(text,size,13);
        cerberus_wardrive_gps_uart_state_t state;cerberus_wardrive_gps_time_t sample;
        CHECK(cerberus_wardrive_gps_uart_get_state(&state)==ESP_OK);
        CHECK(!cerberus_wardrive_gps_start_ready(true,&state));
        CHECK(cerberus_wardrive_gps_uart_recent_time(&sample)==ESP_ERR_NOT_FOUND&&!sample.utc[0]);
        size=sentence(text,"GNRMC,120001.000,A,4807.038,N,01131.000,E,0,0,081126,,,A,V","\r\n");feed(text,size,13);
        CHECK(cerberus_wardrive_gps_uart_get_state(&state)==ESP_OK);
        CHECK(cerberus_wardrive_gps_start_ready(true,&state));
        CHECK(cerberus_wardrive_gps_uart_recent_time(&sample)==ESP_OK);
        stop_clean();
    }
}
int main(void)
{
    off_and_trace();setup_and_cleanup();clocks_and_identity();observer_lock_timeouts();presets_preview_and_detection();recent_time_after_stop();fix_class_start_gate();reset();
    printf("GPS UART/trace: %u checks; actual reader/parser/lifecycle, optional PSRAM, exact observer identity, privacy and cleanup passed. row=%zu store=%zu snapshot=%zu\n",
        checks,sizeof(cerberus_wardrive_gps_trace_row_t),sizeof(gps_trace_store_t),sizeof(cerberus_wardrive_gps_trace_snapshot_t));
    return 0;
}
