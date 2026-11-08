"""Exercise the production S3 BLE scanner with a deterministic NimBLE event queue."""
from pathlib import Path
import os
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
out = root / "artifacts/s3-ble-event-checks"
out.mkdir(parents=True, exist_ok=True)
(out / "fake_s3_ble_platform.h").write_text(r'''
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#define CONFIG_BT_NIMBLE_ENABLED 1
#define MYNEWT_VAL(x) FAKE_EXT_SCAN
#define CONFIG_BT_NIMBLE_TRANSPORT_EVT_SIZE 257
#define MALLOC_CAP_INTERNAL 1U
#define MALLOC_CAP_SPIRAM 2U
#define MALLOC_CAP_8BIT 4U
#define portMAX_DELAY 0xffffffffU
#define pdMS_TO_TICKS(x) (x)
#define BLE_NPL_OK 0
#define BLE_HS_FOREVER 0x7fffffff
#define BLE_HS_EALREADY 8
#define BLE_HS_EOS 9
#define BLE_GAP_EVENT_DISC 1
#define BLE_GAP_EVENT_DISC_COMPLETE 2
#define BLE_GAP_EVENT_EXT_DISC 3
#define BLE_HCI_ADV_RPT_EVTYPE_ADV_IND 0
#define BLE_HCI_ADV_RPT_EVTYPE_DIR_IND 1
#define BLE_HCI_ADV_RPT_EVTYPE_SCAN_IND 2
#define BLE_HCI_ADV_RPT_EVTYPE_SCAN_RSP 4
#define BLE_HCI_ADV_RPT_EVTYPE_NONCONN_IND 3
#define BLE_HCI_ADV_LEGACY_MASK 1
#define BLE_HCI_ADV_CONN_MASK 2
#define BLE_HCI_ADV_SCAN_RSP_MASK 4
#define BLE_GAP_EXT_ADV_DATA_STATUS_COMPLETE 0
typedef void *SemaphoreHandle_t;
SemaphoreHandle_t xSemaphoreCreateMutex(void);
int xSemaphoreTake(SemaphoreHandle_t,unsigned);
int xSemaphoreGive(SemaphoreHandle_t);
int64_t esp_timer_get_time(void);
size_t heap_caps_get_free_size(unsigned);
void *heap_caps_calloc(size_t,size_t,unsigned);
void heap_caps_free(void *);
const char *esp_err_to_name(esp_err_t);
struct ble_npl_event {void(*fn)(struct ble_npl_event *);void *arg;bool queued;};
struct ble_npl_eventq {unsigned unused;};
struct ble_npl_callout {struct ble_npl_event event;bool active;};
void ble_npl_event_init(struct ble_npl_event *,void(*)(struct ble_npl_event *),void *);
void ble_npl_event_set_arg(struct ble_npl_event *,void *);
void *ble_npl_event_get_arg(struct ble_npl_event *);
void ble_npl_eventq_put(struct ble_npl_eventq *,struct ble_npl_event *);
void ble_npl_callout_init(struct ble_npl_callout *,struct ble_npl_eventq *,void(*)(struct ble_npl_event *),void *);
int ble_npl_callout_reset(struct ble_npl_callout *,uint32_t);
void ble_npl_callout_stop(struct ble_npl_callout *);
void ble_npl_callout_set_arg(struct ble_npl_callout *,void *);
uint32_t ble_npl_time_ms_to_ticks32(uint32_t);
struct ble_gap_disc_params {int itvl,window,filter_policy,limited,passive,filter_duplicates;};
struct ble_gap_ext_disc_params {int itvl,window,passive;};
struct ble_gap_disc_desc {struct {uint8_t val[6],type;} addr;int8_t rssi;uint8_t event_type;const uint8_t *data;uint8_t length_data;};
struct ble_gap_ext_disc_desc {struct {uint8_t val[6],type;} addr;int8_t rssi;uint8_t props,data_status,legacy_event_type,sid;const uint8_t *data;uint8_t length_data;};
struct ble_gap_event {int type;struct ble_gap_disc_desc disc;struct ble_gap_ext_disc_desc ext_disc;struct {int reason;} disc_complete;};
int ble_hs_id_infer_auto(int,uint8_t *);
int ble_gap_disc(uint8_t,int32_t,const struct ble_gap_disc_params *,int(*)(struct ble_gap_event *,void *),void *);
int ble_gap_ext_disc(uint8_t,uint16_t,uint16_t,uint8_t,uint8_t,uint8_t,
    const struct ble_gap_ext_disc_params *,const struct ble_gap_ext_disc_params *,
    int(*)(struct ble_gap_event *,void *),void *);
int ble_gap_disc_cancel(void);
int ble_gap_disc_active(void);
struct ble_npl_eventq *nimble_port_get_dflt_eventq(void);
''')
for name in ("sdkconfig.h", "esp_heap_caps.h", "esp_timer.h", "freertos/FreeRTOS.h",
             "freertos/semphr.h", "host/ble_hs.h", "host/util/util.h",
             "nimble/nimble_npl.h", "nimble/nimble_port.h"):
    path = out / name
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text('#include "fake_s3_ble_platform.h"\n')
(out / "esp_log.h").write_text('''#include "fake_s3_ble_platform.h"
#define ESP_LOGW(tag,fmt,...) do { (void)(tag); } while (0)
#define ESP_LOGE(tag,fmt,...) do { (void)(tag); } while (0)
''')
env = os.environ.copy()
env["ZIG_GLOBAL_CACHE_DIR"] = str(out / "zig-cache")
env["ZIG_LOCAL_CACHE_DIR"] = str(out / "zig-local")
for extended in (0, 1):
    exe = out / f"s3-ble-events-{extended}-check.exe"
    subprocess.run([
        sys.executable, "-m", "ziglang", "cc", "-std=c11", "-O1", "-Wall", "-Wextra", "-Werror",
        f"-DFAKE_EXT_SCAN={extended}",
        "-I", str(out), "-I", str(root / "tools/host_checks/stubs"),
        "-I", str(root / "shared"), "-I", str(root / "s3_main/main"),
        str(root / "tools/host_checks/s3_ble_events_check.c"),
        str(root / "s3_main/main/s3_ble_snapshot.c"),
        str(root / "s3_main/main/s3_ble_discovery_inventory.c"),
        str(root / "shared/cerberus_watch_capture.c"),
        str(root / "shared/mac_observation.c"),
        str(root / "shared/sentinel_watch_protocol.c"),
        str(root / "s3_main/main/sentinel_watch_detect.c"),
        str(root / "s3_main/main/ble_analyzer.c"), "-o", str(exe),
    ], check=True, env=env)
    subprocess.run([str(exe)], check=True, env=env)

# Additive inventory diagnostics remain canonical and RF-free in disabled builds.
disabled = out / "disabled"
disabled.mkdir(exist_ok=True)
(disabled / "sdkconfig.h").write_text("/* NimBLE disabled. */\n")
(disabled / "check.c").write_text(r'''
#include "s3_ble_scanner.h"
#include <stdio.h>
#include <string.h>
int main(void)
{
    cerberus_ble_scan_status_payload_t status, zero = {0};
    uint16_t replacements = 99;
    memset(&status, 0xa5, sizeof(status));
    if (cerberus_s3_ble_scanner_get_inventory_status(1, &status, &replacements) != ESP_ERR_NOT_SUPPORTED ||
        replacements || memcmp(&status, &zero, sizeof(status))) return 1;
    replacements = 99;
    if (cerberus_s3_ble_scanner_get_inventory_status(1, NULL, &replacements) != ESP_ERR_INVALID_ARG ||
        replacements) return 2;
    memset(&status, 0xa5, sizeof(status));
    if (cerberus_s3_ble_scanner_get_inventory_status(1, &status, NULL) != ESP_ERR_INVALID_ARG ||
        memcmp(&status, &zero, sizeof(status))) return 3;
    cerberus_s3_ble_mac_monitor_t context={0};cerberus_mac_monitor_ops_t ops;
    cerberus_mac_binding_t binding;cerberus_mac_status_t diagnostic;cerberus_mac_counters_t counters;
    cerberus_s3_ble_scanner_mac_ops(&context,&ops);
    if(!ops.reserve || !ops.start || !ops.poll || !ops.stop || !ops.renew || !ops.read_step || !ops.ack || !ops.now_us) return 4;
    memset(&binding,0xa5,sizeof(binding));
    if(ops.reserve(ops.context,1,&binding)!=ESP_ERR_NOT_SUPPORTED || binding.boot_id || binding.job_id || binding.epoch) return 5;
    cerberus_s3_ble_scanner_mac_diagnostics(&diagnostic,&counters);
    if(diagnostic.version!=1 || diagnostic.flags || diagnostic.state || counters.version!=1 || counters.epoch || counters.observations) return 6;
    cerberus_s3_ble_discovery_meta_t discovery, empty_discovery = {0};
    cerberus_s3_ble_discovery_bank_t bank, empty_bank = {0};
    memset(&discovery, 0xa5, sizeof(discovery));
    if(cerberus_s3_ble_scanner_start_discovery(CERBERUS_S3_BLE_FILTER_FIND_MY,0,&discovery)!=ESP_ERR_NOT_SUPPORTED ||
        memcmp(&discovery,&empty_discovery,sizeof(discovery))) return 7;
    memset(&bank, 0xa5, sizeof(bank));
    if(cerberus_s3_ble_scanner_copy_discovery(1,&bank)!=ESP_ERR_NOT_SUPPORTED || memcmp(&bank,&empty_bank,sizeof(bank))) return 8;
    memset(&discovery, 0xa5, sizeof(discovery));
    if(cerberus_s3_ble_scanner_discovery_status(1,&discovery)!=ESP_ERR_NOT_SUPPORTED ||
        memcmp(&discovery,&empty_discovery,sizeof(discovery))) return 9;
    if(cerberus_s3_ble_scanner_stop_discovery(1,&discovery)!=ESP_ERR_NOT_SUPPORTED ||
        cerberus_s3_ble_scanner_release_discovery(1)!=ESP_ERR_NOT_SUPPORTED) return 10;
    memset(&discovery, 0xa5, sizeof(discovery));
    if(cerberus_s3_ble_scanner_discovery_diagnostics(&discovery)!=ESP_OK ||
        memcmp(&discovery,&empty_discovery,sizeof(discovery))) return 11;
    cerberus_s3_ble_discovery_record_t reacquire[4], empty_reacquire[4] = {0};
    memset(&discovery,0xa5,sizeof(discovery));
    if(cerberus_s3_ble_scanner_start_reacquire((uint8_t[6]){1},0,&discovery)!=ESP_ERR_NOT_SUPPORTED ||
        memcmp(&discovery,&empty_discovery,sizeof(discovery))) return 30;
    memset(reacquire,0xa5,sizeof(reacquire));
    if(cerberus_s3_ble_scanner_copy_reacquire(1,reacquire,&discovery)!=ESP_ERR_NOT_SUPPORTED ||
        memcmp(reacquire,empty_reacquire,sizeof(reacquire)) || memcmp(&discovery,&empty_discovery,sizeof(discovery))) return 31;
    cerberus_s3_ble_analyzer_state_t analyzer, empty_analyzer={0};
    memset(&analyzer,0xa5,sizeof(analyzer));
    if(cerberus_s3_ble_scanner_analyzer_start(&analyzer)!=ESP_ERR_NOT_SUPPORTED ||
        memcmp(&analyzer,&empty_analyzer,sizeof(analyzer))) return 12;
    memset(&analyzer,0xa5,sizeof(analyzer));
    if(cerberus_s3_ble_scanner_analyzer_get_state(&analyzer)!=ESP_ERR_NOT_SUPPORTED ||
        memcmp(&analyzer,&empty_analyzer,sizeof(analyzer))) return 13;
    memset(&analyzer,0xa5,sizeof(analyzer));
    if(cerberus_s3_ble_scanner_analyzer_stop(&analyzer)!=ESP_ERR_NOT_SUPPORTED ||
        memcmp(&analyzer,&empty_analyzer,sizeof(analyzer))) return 14;
    if(cerberus_s3_ble_scanner_analyzer_start(NULL)!=ESP_ERR_INVALID_ARG ||
        cerberus_s3_ble_scanner_analyzer_get_state(NULL)!=ESP_ERR_INVALID_ARG ||
        cerberus_s3_ble_scanner_analyzer_stop(NULL)!=ESP_ERR_INVALID_ARG ||
        cerberus_s3_ble_scanner_analyzer_release()!=ESP_ERR_NOT_SUPPORTED) return 15;
    cerberus_s3_ble_track_state_t track, empty_track={0};
    memset(&track,0xa5,sizeof(track));
    if(cerberus_s3_ble_scanner_track_start(&track)!=ESP_ERR_NOT_SUPPORTED ||
        memcmp(&track,&empty_track,sizeof(track))) return 16;
    memset(&track,0xa5,sizeof(track));
    if(cerberus_s3_ble_scanner_track_get_state(&track)!=ESP_ERR_NOT_SUPPORTED ||
        memcmp(&track,&empty_track,sizeof(track))) return 17;
    memset(&track,0xa5,sizeof(track));
    if(cerberus_s3_ble_scanner_track_stop(&track)!=ESP_ERR_NOT_SUPPORTED ||
        memcmp(&track,&empty_track,sizeof(track))) return 18;
    if(cerberus_s3_ble_scanner_track_start(NULL)!=ESP_ERR_INVALID_ARG ||
        cerberus_s3_ble_scanner_track_get_state(NULL)!=ESP_ERR_INVALID_ARG ||
        cerberus_s3_ble_scanner_track_stop(NULL)!=ESP_ERR_INVALID_ARG ||
        cerberus_s3_ble_scanner_track_release()!=ESP_ERR_NOT_SUPPORTED) return 19;
    cerberus_s3_ble_passive_state_t passive,empty_passive={0};
    cerberus_s3_ble_discovery_record_t row,empty_row={0};
    cerberus_watch_scan_status_payload_t watch,empty_watch={0};
    cerberus_watch_observation_t result,empty_result={0};
    cerberus_watch_receive_timing_t timing,motion_timing,empty_timing={0};
    sentinel_watch_motion_t motion,empty_motion={0};
    cerberus_ble_activity_payload_t activity,empty_activity={0};
    memset(&passive,0xa5,sizeof(passive));
    if(cerberus_s3_ble_scanner_passive_start(0,&passive)!=ESP_ERR_NOT_SUPPORTED ||
        memcmp(&passive,&empty_passive,sizeof(passive)))return 20;
    memset(&passive,0xa5,sizeof(passive));
    if(cerberus_s3_ble_scanner_passive_get_state(1,&passive)!=ESP_ERR_NOT_SUPPORTED ||
        memcmp(&passive,&empty_passive,sizeof(passive)))return 21;
    memset(&passive,0xa5,sizeof(passive));
    if(cerberus_s3_ble_scanner_passive_stop(1,&passive)!=ESP_ERR_NOT_SUPPORTED ||
        memcmp(&passive,&empty_passive,sizeof(passive)))return 22;
    if(cerberus_s3_ble_scanner_passive_start(0,NULL)!=ESP_ERR_INVALID_ARG ||
        cerberus_s3_ble_scanner_passive_get_state(0,&passive)!=ESP_ERR_INVALID_ARG ||
        cerberus_s3_ble_scanner_passive_stop(1,NULL)!=ESP_ERR_INVALID_ARG ||
        cerberus_s3_ble_scanner_passive_release(1)!=ESP_ERR_NOT_SUPPORTED)return 23;
    memset(&passive,0xa5,sizeof(passive));memset(&row,0xa5,sizeof(row));
    if(cerberus_s3_ble_scanner_passive_copy_row(1,0,&row,&passive)!=ESP_ERR_NOT_SUPPORTED ||
        memcmp(&row,&empty_row,sizeof(row)) || memcmp(&passive,&empty_passive,sizeof(passive)))return 24;
    memset(&watch,0xa5,sizeof(watch));memset(&result,0xa5,sizeof(result));
    if(cerberus_s3_ble_scanner_passive_get_watch_status(1,&watch)!=ESP_ERR_NOT_SUPPORTED ||
        memcmp(&watch,&empty_watch,sizeof(watch)) ||
        cerberus_s3_ble_scanner_passive_get_watch_result(1,0,&result)!=ESP_ERR_NOT_SUPPORTED ||
        memcmp(&result,&empty_result,sizeof(result)))return 25;
    memset(&result,0xa5,sizeof(result));memset(&timing,0xa5,sizeof(timing));
    memset(&motion,0xa5,sizeof(motion));memset(&motion_timing,0xa5,sizeof(motion_timing));
    if(cerberus_s3_ble_scanner_passive_get_watch_detailed_result(1,0,&result,&timing,&motion,&motion_timing)!=ESP_ERR_NOT_SUPPORTED ||
        memcmp(&result,&empty_result,sizeof(result)) || memcmp(&timing,&empty_timing,sizeof(timing)) ||
        memcmp(&motion,&empty_motion,sizeof(motion)) || memcmp(&motion_timing,&empty_timing,sizeof(motion_timing)))return 26;
    memset(&activity,0xa5,sizeof(activity));
    if(cerberus_s3_ble_scanner_passive_get_activity(1,&activity)!=ESP_ERR_NOT_SUPPORTED ||
        memcmp(&activity,&empty_activity,sizeof(activity)))return 27;
    cerberus_c5_ble_result_t wardrive_rows[33];
    cerberus_s3_ble_wardrive_evidence_t wardrive_evidence[33];
    memset(wardrive_rows,0xa5,sizeof(wardrive_rows));
    memset(wardrive_evidence,0xa5,sizeof(wardrive_evidence));
    memset(&passive,0xa5,sizeof(passive));
    if(cerberus_s3_ble_scanner_passive_copy_wardrive(1,32,wardrive_rows,wardrive_evidence,32,&passive)!=ESP_ERR_NOT_SUPPORTED ||
        memcmp(&passive,&empty_passive,sizeof(passive)))return 28;
    for(size_t i=0;i<32*sizeof(*wardrive_rows);++i)if(((uint8_t *)wardrive_rows)[i])return 29;
    for(size_t i=0;i<32*sizeof(*wardrive_evidence);++i)if(((uint8_t *)wardrive_evidence)[i])return 30;
    if(((uint8_t *)&wardrive_rows[32])[0]!=0xa5 || ((uint8_t *)&wardrive_evidence[32])[0]!=0xa5)return 31;
    if(cerberus_s3_ble_scanner_passive_copy_wardrive(1,0,NULL,NULL,0,&passive)!=ESP_ERR_NOT_SUPPORTED ||
        cerberus_s3_ble_scanner_passive_copy_wardrive(0,0,NULL,NULL,0,&passive)!=ESP_ERR_INVALID_ARG ||
        cerberus_s3_ble_scanner_passive_copy_wardrive(1,0,NULL,NULL,0,NULL)!=ESP_ERR_INVALID_ARG ||
        cerberus_s3_ble_scanner_passive_copy_wardrive(1,32,wardrive_rows,wardrive_evidence,33,&passive)!=ESP_ERR_INVALID_ARG)return 32;
    cerberus_watch_profile_evidence_t profile,empty_profile={0};
    for(unsigned missing=0;missing<=5U;++missing){
        memset(&result,0xa5,sizeof(result));memset(&timing,0xa5,sizeof(timing));
        memset(&motion,0xa5,sizeof(motion));memset(&motion_timing,0xa5,sizeof(motion_timing));
        memset(&profile,0xa5,sizeof(profile));
        if(cerberus_s3_ble_scanner_get_watch_profiled_result(255,0,UINT64_MAX,
            missing==0U?NULL:&result,missing==1U?NULL:&timing,missing==2U?NULL:&motion,
            missing==3U?NULL:&motion_timing,missing==4U?NULL:&profile)!=ESP_ERR_NOT_SUPPORTED)return 33;
        if((missing!=0U&&memcmp(&result,&empty_result,sizeof(result)))||
           (missing!=1U&&memcmp(&timing,&empty_timing,sizeof(timing)))||
           (missing!=2U&&memcmp(&motion,&empty_motion,sizeof(motion)))||
           (missing!=3U&&memcmp(&motion_timing,&empty_timing,sizeof(motion_timing)))||
           (missing!=4U&&memcmp(&profile,&empty_profile,sizeof(profile))))return 34;
    }
    puts("Disabled scanner diagnostics, passive leases and Remote ID profiles: canonical empty output, explicit unsupported, no host dependencies passed.");
    return 0;
}
''')
exe = disabled / "s3-ble-inventory-disabled-check.exe"
subprocess.run([
    sys.executable, "-m", "ziglang", "cc", "-std=c11", "-O1", "-Wall", "-Wextra", "-Werror",
    "-I", str(disabled), "-I", str(root / "tools/host_checks/stubs"),
    "-I", str(root / "shared"), "-I", str(root / "s3_main/main"),
    str(disabled / "check.c"), str(root / "s3_main/main/s3_ble_scanner.c"),
    "-o", str(exe),
], check=True, env=env)
subprocess.run([str(exe)], check=True, env=env)
