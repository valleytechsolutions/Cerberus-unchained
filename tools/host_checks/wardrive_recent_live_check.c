#include "recent_live_adapters.h"
#include "wardrive_service.h"
#include <inttypes.h>
#include <stdarg.h>
#define MALLOC_CAP_SPIRAM 1U
#define MALLOC_CAP_8BIT 2U
#define ESP_ERROR_CHECK(x) CHECK((x)==ESP_OK)
enum { UI_PAGE_WARDRIVE=1, UI_PAGE_SCAN=2 };
static unsigned current_page=UI_PAGE_WARDRIVE;
static bool wardrive_start_pending, allocation_fails;
static uint32_t wardrive_last_draw_ms=999;
static unsigned allocations, releases, copies, status_reads, stop_requests, summary_draws;
static size_t allocation_bytes;
static int64_t clock_us=5000000;
static cerberus_wardrive_recent_snapshot_t runtime;
static cerberus_wardrive_recent_status_t compact;
static void *heap_caps_calloc(size_t count,size_t size,unsigned caps)
{ CHECK(caps==(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT)); ++allocations;allocation_bytes=count*size;return allocation_fails?NULL:calloc(count,size); }
static void heap_caps_free(void *p){if(p){++releases;free(p);}}
static int64_t esp_timer_get_time(void){return clock_us;}
void cerberus_wardrive_copy_recent(cerberus_wardrive_recent_snapshot_t *out){++copies;*out=runtime;}
void cerberus_wardrive_get_recent_status(cerberus_wardrive_recent_status_t *out){++status_reads;*out=compact;}
void cerberus_wardrive_request_stop(void){++stop_requests;}
static esp_err_t draw_detail_page(void);
#include "wardrive_recent_live_ui.h"
static esp_err_t draw_detail_page(void)
{
    calls=fail_after=0;drawn_size=0;drawn[0]=0;
    if(wardrive_recent_is_open())return wardrive_recent_draw();
    ++summary_draws;return ESP_OK;
}
/* GPS rendering/observer lifetime has its own actual-production suite. Only
 * that viewer's visibility is a boundary for this shared repaint guard. */
static bool gps_view_opened;
static bool wardrive_logs_is_open(void){return false;}
static bool wardrive_areas_is_open(void){return false;}
static bool wardrive_gps_view_is_open(void)
{ return gps_view_opened && current_page == UI_PAGE_WARDRIVE; }
#include "recent_repaint_impl.h"
static char diagnostic[4096];
static void console_printf(const char *format,...)
{
    size_t used=strlen(diagnostic);va_list args;va_start(args,format);
    int n=vsnprintf(diagnostic+used,sizeof(diagnostic)-used,format,args);va_end(args);
    CHECK(n>=0&&(size_t)n<sizeof(diagnostic)-used);
}
#include "recent_console_impl.h"
static void state(cerberus_wardrive_state_t value)
{runtime.state=compact.state=value;runtime.worker_active=compact.worker_active=wardrive_recent_active(value);}
static void seed(void)
{
    cerberus_wardrive_recent_t model;cerberus_wardrive_recent_init(&model);
    CHECK(cerberus_wardrive_recent_begin(&model,7));
    cerberus_wardrive_observation_t row={.type=CERBERUS_WARDRIVE_WIFI,.radio=CERBERUS_WARDRIVE_RADIO_C5,
        .address={2,0,0,0,0,1},.channel=36,.rssi_dbm=-62,.label_length=4,.label={'T','e','s','t'}};
    cerberus_wardrive_recent_ticket_t ticket;
    CHECK(cerberus_wardrive_recent_observe(&model,7,1,&row,1000,true,&ticket));
    CHECK(cerberus_wardrive_recent_set_outcome(&model,ticket,CERBERUS_WARDRIVE_RECENT_APPENDED_UNLOCATED));
    runtime=(cerberus_wardrive_recent_snapshot_t){.session_id=7,.revision=model.revision,.available=true};
    runtime.count=(uint8_t)cerberus_wardrive_recent_copy_recent(&model,runtime.rows,10);
    compact=(cerberus_wardrive_recent_status_t){.session_id=7,.revision=model.revision,.available=true,.count=runtime.count};
    state(CERBERUS_WARDRIVE_RUNNING);
}
static void test_routes(void)
{
    seed();wardrive_recent_open();CHECK(wardrive_recent_is_open()&&allocations==1&&copies==1);
    CHECK(allocation_bytes==sizeof(wardrive_recent_ui_store_t)&&!wardrive_last_draw_ms);
    CHECK(strstr(drawn,"Test")&&strstr(drawn,"LIVE"));
    unsigned before=allocations;wardrive_recent_open();CHECK(allocations==before&&copies==2);
    wardrive_recent_handle_tap(8,150);CHECK(wardrive_recent_view->selected_valid&&strstr(drawn,"FROZEN ROW"));
    cerberus_wardrive_recent_row_t selected=wardrive_recent_view->selected;
    runtime.session_id=8;runtime.rows[0].observation.label[0]='X';state(CERBERUS_WARDRIVE_ERROR);
    CHECK(draw_detail_page()==ESP_OK);
    CHECK(!memcmp(&selected,&wardrive_recent_view->selected,sizeof(selected))&&strstr(drawn,"SESSION 7"));
    clock_us=-1;CHECK(draw_detail_page()==ESP_OK);CHECK(strstr(drawn,"PROCESSING AGE UNAVAILABLE"));clock_us=5000000;
    wardrive_recent_handle_tap(8,426);CHECK(wardrive_recent_view->view==WARDRIVE_RECENT_UI_LIST&&!wardrive_recent_view->selected_valid);
    state(CERBERUS_WARDRIVE_RUNNING);CHECK(draw_detail_page()==ESP_OK);
    unsigned stops=stop_requests;wardrive_recent_handle_tap(164,426);
    CHECK(stop_requests==stops+1&&wardrive_recent_is_open());
    wardrive_recent_handle_tap(8,426);CHECK(!wardrive_recent_is_open()&&!wardrive_recent_view&&releases==1&&summary_draws==1);
    CHECK(stop_requests==stops+1); /* Back to summary does not stop the session. */
    state(CERBERUS_WARDRIVE_STOPPED);wardrive_start_pending=true;wardrive_recent_open();
    CHECK(strstr(drawn,"STOP"));wardrive_recent_handle_tap(164,426);
    CHECK(!wardrive_start_pending&&stop_requests==stops+1&&wardrive_recent_is_open());
    CHECK(strstr(drawn,"INACTIVE"));
    current_page=UI_PAGE_SCAN;wardrive_recent_release_if_left();CHECK(!wardrive_recent_view&&!wardrive_recent_opened&&releases==2);
    before=allocations;wardrive_recent_open();CHECK(allocations==before&&!wardrive_recent_opened);
    wardrive_recent_release_if_left();CHECK(releases==2);current_page=UI_PAGE_WARDRIVE;
}
static void test_unavailable(void)
{
    state(CERBERUS_WARDRIVE_RUNNING);allocation_fails=true;
    unsigned reads=status_reads,stops=stop_requests;wardrive_recent_open();
    CHECK(wardrive_recent_is_open()&&!wardrive_recent_view&&status_reads==reads+1);
    CHECK(strstr(drawn,"ESP_ERR_NO_MEM")&&strstr(drawn,"STOP"));
    wardrive_recent_handle_tap(164,426);CHECK(stop_requests==stops+1);
    wardrive_recent_handle_tap(8,426);CHECK(!wardrive_recent_opened);
    state(CERBERUS_WARDRIVE_IDLE);wardrive_start_pending=true;wardrive_recent_open();
    wardrive_recent_handle_tap(164,426);CHECK(!wardrive_start_pending&&stop_requests==stops+1);
    CHECK(strstr(drawn,"INACTIVE"));wardrive_recent_close();allocation_fails=false;
    runtime.available=false;runtime.view_error=ESP_ERR_NO_MEM;wardrive_recent_open();
    CHECK(wardrive_recent_view&&strstr(drawn,"UNAVAILABLE"));
    unsigned gets=copies;wardrive_recent_handle_tap(8,150);CHECK(copies==gets&&!wardrive_recent_view->selected_valid);
    wardrive_recent_close();
}
static void test_console(void)
{
    compact=(cerberus_wardrive_recent_status_t){.session_id=UINT64_MAX,.revision=UINT64_MAX-1,.events=UINT64_MAX-2,
        .clock_faults=UINT32_MAX,.count=10,.retained_events=24,.available=true,.worker_active=true,
        .state=CERBERUS_WARDRIVE_STOPPING,.error=ESP_FAIL,.view_error=ESP_ERR_NO_MEM};
    unsigned gets=status_reads,banks=copies,allocs=allocations,stops=stop_requests;
    print_wardrive_recent();CHECK(status_reads==gets+1&&copies==banks&&allocations==allocs&&stop_requests==stops);
    CHECK(strstr(diagnostic,"processor=WARDRIVE_RECENT\r\n"));
    CHECK(strstr(diagnostic,"session_id=18446744073709551615\r\nrevision=18446744073709551614\r\nevents=18446744073709551613\r\n"));
    CHECK(strstr(diagnostic,"rows=10\r\nretained_events=24\r\nclock_faults=4294967295\r\n"));
    CHECK(strstr(diagnostic,"receipt_scope=S3_processing_uptime_not_RF_time")&&strstr(diagnostic,"append_scope=API_accepted_not_SD_durability"));
}
static void test_actual_repaint_cadence(void)
{
    seed();wardrive_recent_open();wardrive_last_draw_ms=1000U;
    unsigned copied=copies,draws=summary_draws,stops=stop_requests,allocated=allocations;
    actual_wardrive_repaint(1499U,false);CHECK(copies==copied&&wardrive_last_draw_ms==1000U);
    actual_wardrive_repaint(1500U,false);CHECK(copies==copied+1U&&wardrive_last_draw_ms==1500U);
    actual_wardrive_repaint(2500U,true);CHECK(copies==copied+1U&&wardrive_last_draw_ms==1500U);
    CHECK(summary_draws==draws&&stop_requests==stops&&allocations==allocated);
    wardrive_recent_close();wardrive_last_draw_ms=3000U;
    actual_wardrive_repaint(3999U,false);CHECK(summary_draws==draws&&wardrive_last_draw_ms==3000U);
    actual_wardrive_repaint(4000U,false);CHECK(summary_draws==draws+1U&&wardrive_last_draw_ms==4000U);
    gps_view_opened=true;wardrive_last_draw_ms=5000U;
    actual_wardrive_repaint(5499U,false);CHECK(summary_draws==draws+1U);
    actual_wardrive_repaint(5500U,false);CHECK(summary_draws==draws+2U&&wardrive_last_draw_ms==5500U);
    wardrive_last_draw_ms=0U;actual_wardrive_repaint(6000U,true);CHECK(summary_draws==draws+2U);
    actual_wardrive_repaint(6000U,false);CHECK(summary_draws==draws+3U&&wardrive_last_draw_ms==6000U);
    CHECK(stop_requests==stops&&allocations==allocated);gps_view_opened=false;
}
int main(void)
{
    test_routes();test_unavailable();test_console();test_actual_repaint_cadence();
    CHECK(releases+2==allocations); /* Two failed optional UI allocations. */
    printf("Wardriving recent live adapter/compact console: %u checks; frozen selection, Stop/Back, pending cancellation, allocation failure and cleanup pass. Host UI store=%zu bytes.\n",checks,sizeof(wardrive_recent_ui_store_t));
}
