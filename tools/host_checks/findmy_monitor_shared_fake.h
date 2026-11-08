/* Monitor presentation boundaries. The companion/discovery suites defer this
 * scaffold and compile their actual source owners; this suite injects commits. */
#include "ble_discovery_list.h"
#include "s3_ble_discovery_session.h"
#include "dual_ble_response_ui_fixture.h"
#define UI_BLE_MODE_C5 99U
typedef struct {
    cerberus_ble_discovery_session_t session;
    cerberus_s3_ble_discovery_inventory_t inventory;
    cerberus_s3_ble_discovery_entry_t painted[4], selected;
    cerberus_dual_ble_row_t painted_c5[4];
    cerberus_dual_ble_provenance_t painted_c5_provenance[4];
    cerberus_ble_discovery_list_ref_t order[64];
    cerberus_ble_discovery_list_source_t source_filter;
    uint8_t painted_source[4], painted_count,built_count,selected_source;
    uint8_t c5_family_bits[32],c5_response_bits[32];
    bool order_valid;
} ble_discovery_live_store_t;
static ble_discovery_live_store_t monitor_store,*ble_discovery_live;
static cerberus_dual_ble_row_t monitor_bank[32],monitor_selected_c5;
static cerberus_dual_ble_provenance_t monitor_proof[32],monitor_selected_proof;
static uint8_t monitor_bank_count;
static cerberus_dual_ble_view_t monitor_c5_view;
static bool monitor_c5_held,dual_ble_ui_response_requests;
static unsigned monitor_start_calls,monitor_cancel_calls,monitor_retry_calls,monitor_apply_calls;
static uint64_t monitor_cancel_clock;
static void ble_select_frozen_live(const cerberus_findmy_monitor_record_t *);
static void dual_ble_ui_get_view(cerberus_dual_ble_view_t *out){*out=monitor_c5_view;}
static bool dual_ble_ui_released(void){return !monitor_c5_held;}
static bool dual_ble_live_discovery_released(void)
{return !ble_discovery_live||cerberus_ble_discovery_session_released(&ble_discovery_live->session);}
static void dual_ble_ui_retry(void){++monitor_retry_calls;monitor_c5_view.error=ESP_OK;}
static void dual_ble_ui_cancel(void)
{++monitor_cancel_calls;monitor_cancel_clock=(uint64_t)clock_ms;monitor_c5_view.enabled=false;}
static void dual_ble_ui_sync(int mode,const uint8_t *address,uint8_t type,uint64_t now)
{assert(mode==-1&&!address&&!type);dual_ble_ui_cancel();monitor_cancel_clock=now;}
static esp_err_t ble_discovery_monitor_start(void)
{
    ++monitor_start_calls;if(fail_alloc)return ESP_ERR_NO_MEM;
    if(!ble_discovery_live){memset(&monitor_store,0,sizeof(monitor_store));ble_discovery_live=&monitor_store;
        cerberus_ble_discovery_session_init(&monitor_store.session,CERBERUS_S3_BLE_FILTER_FIND_MY);}
    cerberus_ble_discovery_session_select(&monitor_store.session,CERBERUS_S3_BLE_FILTER_FIND_MY);
    cerberus_ble_discovery_session_start(&monitor_store.session);
    monitor_store.selected_source=monitor_store.painted_count=0;
    return ESP_OK;
}
static uint8_t ble_discovery_combined_order(void)
{
    unsigned count=0;ble_discovery_live_store_t *s=ble_discovery_live;
    if(!s)return 0;
    if(s->source_filter!=CERBERUS_BLE_DISCOVERY_LIST_C5)
        for(unsigned i=0;i<s->inventory.count;++i)
            s->order[count++]=(cerberus_ble_discovery_list_ref_t){CERBERUS_BLE_DISCOVERY_LIST_S3,(uint8_t)i};
    for(unsigned i=0;i<monitor_bank_count;++i){
        cerberus_dual_ble_family_evidence_t adv,rsp;
        s->c5_family_bits[i]=cerberus_dual_ble_family_classify(&monitor_bank[i],&monitor_proof[i],&adv)?adv.categories:0;
        s->c5_response_bits[i]=cerberus_dual_ble_response_classify(&monitor_bank[i],&monitor_proof[i],&rsp)?rsp.categories:0;
        if(s->source_filter!=CERBERUS_BLE_DISCOVERY_LIST_S3&&
           ((s->c5_family_bits[i]|s->c5_response_bits[i])&CERBERUS_DUAL_BLE_FAMILY_FIND_MY))
            s->order[count++]=(cerberus_ble_discovery_list_ref_t){CERBERUS_BLE_DISCOVERY_LIST_C5,(uint8_t)i};
    }
    return (uint8_t)count;
}
static bool ble_discovery_build_unified_row(unsigned row,size_t pos,uint64_t now)
{
    (void)now;ble_discovery_live_store_t *s=ble_discovery_live;
    if(!s||row>=4||pos>=64)return false;
    cerberus_ble_discovery_list_ref_t ref=s->order[pos];s->painted_source[row]=ref.source;
    if(ref.source==CERBERUS_BLE_DISCOVERY_LIST_S3)s->painted[row]=s->inventory.entries[ref.index];
    else{s->painted_c5[row]=monitor_bank[ref.index];s->painted_c5_provenance[row]=monitor_proof[ref.index];}
    ++s->built_count;return true;
}
static bool ble_discovery_monitor_freeze_selection(unsigned row)
{
    ble_discovery_live_store_t *s=ble_discovery_live;if(!s||row>=s->painted_count)return false;
    s->selected_source=s->painted_source[row];
    if(s->selected_source==CERBERUS_BLE_DISCOVERY_LIST_S3)s->selected=s->painted[row];
    else{monitor_selected_c5=s->painted_c5[row];monitor_selected_proof=s->painted_c5_provenance[row];}
    s->painted_count=0;cerberus_ble_discovery_session_stop(&s->session);return true;
}
static bool ble_discovery_monitor_apply_selection(void)
{
    ble_discovery_live_store_t *s=ble_discovery_live;
    if(!s||!dual_ble_live_discovery_released()||!dual_ble_ui_released())return false;
    ++monitor_apply_calls;
    if(s->selected_source==CERBERUS_BLE_DISCOVERY_LIST_C5)ble_mode=UI_BLE_MODE_C5;
    else if(s->selected_source==CERBERUS_BLE_DISCOVERY_LIST_S3){
        cerberus_s3_ble_discovery_record_t *r=&s->selected.record;
        cerberus_findmy_monitor_record_t proof={.peer=r->peer,.advertisement=r->advertisement,.focus=r->focus,
            .generation=r->generation,.index=r->index,.observed_ms=r->observed_ms};
        ble_select_frozen_live(&proof);ble_selected_from_monitor=true;
        ble_selected_original_time64_valid=true;ble_selected_scan_epoch=s->selected.epoch;
        ble_selected_original_ms64=s->selected.original_source_ms;
        ble_selected_source_ceiling_ms64=s->selected.source_now_ms;
    }else return false;
    s->selected_source=0;return true;
}
