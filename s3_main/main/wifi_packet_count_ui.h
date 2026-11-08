#pragma once
/* Passive copied-target basket. No legacy wifi_targets selection or AP-index
 * rebind grants matching authority here. One optional UI-task PSRAM workspace. */
#include "wifi_packet_count_scan.h"
#include "wifi_packet_count_record.h"
#include "wifi_packet_count_diagnostics.h"
#include "s3_wifi_packet_count.h"
#include "esp_heap_caps.h"

typedef struct {
    wifi_packet_count_selection_t selection;
    wifi_packet_count_row_totals_t total;
    uint64_t source_session[2], age_us[2];
    int8_t rssi[2];
    uint8_t index;
    bool saturated, counts_valid, signal[2];
} wifi_count_painted_t;
typedef struct {
    wifi_packet_count_scan_t scan;
    wifi_count_painted_t painted[4], selected;
    wifi_packet_count_record_status_t recording;
    uint64_t job, last_draw_ms;
    esp_err_t error;
    uint8_t page, painted_count, mode, channel_index, origin;
    bool detail, closing, return_origin, start_pending, record_enabled, picking, job_retired;
} wifi_packet_count_ui_store_t;
static wifi_packet_count_ui_store_t *wifi_packet_count_ui;
static esp_err_t wifi_packet_count_ui_error;
static bool wifi_count_diagnostics_initialized;

static esp_err_t wifi_count_exchange(void *context, unsigned source, uint8_t command,
    const void *request, size_t request_size, void *reply, size_t reply_size)
{
    (void)context;
    return source == WIFI_PACKET_COUNT_SOURCE_S3 ?
        cerberus_s3_wifi_packet_count_exchange(command, request, request_size, reply, reply_size) :
        c5_spi_link_wifi_packet_count_exchange(command, request, request_size, reply, reply_size);
}
static int64_t wifi_count_clock(void *context)
{ (void)context; return esp_timer_get_time(); }
static esp_err_t wifi_count_record_bind(void *context, unsigned source, uint64_t session)
{
    (void)context;
    return wifi_packet_count_ui && wifi_packet_count_ui->job ?
        wifi_packet_count_record_bind(wifi_packet_count_ui->job, source, session) : ESP_ERR_INVALID_STATE;
}
static bool wifi_count_record_drained(void *context, unsigned source, uint64_t session)
{
    (void)context;
    return wifi_packet_count_ui && wifi_packet_count_ui->job &&
        (wifi_packet_count_ui->job_retired || wifi_packet_count_record_drained(wifi_packet_count_ui->job, source, session));
}
/* The worker copies only exchange/context. Neither points into disposable UI
 * memory. The controller alone invokes the bind/drain hooks on the UI task. */
static const wifi_packet_count_transport_t wifi_count_transport = {
    .exchange=wifi_count_exchange, .clock_us=wifi_count_clock,
    .record_bind=wifi_count_record_bind, .record_drained=wifi_count_record_drained};

static bool wifi_packet_count_ui_busy(void)
{
    /* Before begin(), old owner polling must remain enabled so the explicit
     * admission barrier can drain. INFO onward is covered by !released. */
    return wifi_packet_count_ui && (!wifi_packet_count_scan_released(&wifi_packet_count_ui->scan) ||
        (wifi_packet_count_ui->job && wifi_packet_count_ui->recording.busy));
}
static bool wifi_count_acquire(void)
{
    if (!wifi_packet_count_ui) {
        wifi_packet_count_ui=heap_caps_calloc(1U,sizeof(*wifi_packet_count_ui),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
        wifi_packet_count_ui_error=wifi_packet_count_ui?ESP_OK:ESP_ERR_NO_MEM;
        if (wifi_packet_count_ui) wifi_packet_count_ui->mode=WIFI_PACKET_COUNT_SELECTED;
    }
    return wifi_packet_count_ui!=NULL;
}
static esp_err_t wifi_packet_count_open_selection(const wifi_packet_count_selection_t *selection, uint8_t origin)
{
    /* Allocation/addition precedes any change to the old discovery intent. */
    if (!wifi_count_acquire()) return wifi_packet_count_ui_error;
    wifi_packet_count_ui_store_t *s=wifi_packet_count_ui;
    if (s->closing || wifi_packet_count_ui_busy()) { s->error=ESP_ERR_INVALID_STATE; return s->error; }
    s->error=wifi_packet_count_scan_add(&s->scan,selection);
    if (s->error!=ESP_OK) return s->error;
    s->origin=origin; s->picking=s->detail=false; s->painted_count=0;
    int channel=cerberus_wifi_packet_count_channel_index(selection->target.channel_hint);
    if (channel>=0 && s->scan.count==1U) s->channel_index=(uint8_t)channel;
    dual_wifi_live_cancel_handoff(); dual_wifi_ui_leave(); wifi_relation_cancel();
    sentinel_scan_session_select(&live_session,SENTINEL_SCAN_NONE);
    sentinel_scan_session_select(&live_ble_session,SENTINEL_SCAN_NONE);
    current_page=UI_PAGE_WIFI; wifi_mode=UI_WIFI_MODE_PACKET_COUNT;
    return ESP_OK;
}
static void wifi_count_close(bool back)
{
    wifi_packet_count_ui_store_t *s=wifi_packet_count_ui;
    if (!s) { if (back) wifi_mode=UI_WIFI_MODE_DUAL; return; }
    s->closing=true; s->return_origin|=back; s->start_pending=false;
    s->painted_count=0; wifi_packet_count_scan_close(&s->scan);
}
static void wifi_packet_count_back(void)
{
    wifi_packet_count_ui_store_t *s=wifi_packet_count_ui;
    if (s && s->detail && !s->closing) { s->detail=false; s->painted_count=0; }
    else wifi_count_close(true);
}
static bool wifi_count_previous_released(void)
{
    cerberus_dual_wifi_status_t status;
    cerberus_dual_wifi_scan_get_status(&status);
    return dual_wifi_live_released() && !dual_wifi_ui_busy(&status);
}
static void wifi_count_start_stop(void)
{
    wifi_packet_count_ui_store_t *s=wifi_packet_count_ui;
    if (!s || s->closing) return;
    if (s->start_pending || !wifi_packet_count_scan_released(&s->scan)) {
        s->start_pending=false;
        bool fault=s->scan.sources[0].faulted||s->scan.sources[1].faulted;
        if (fault) wifi_packet_count_scan_close(&s->scan);
        else wifi_packet_count_scan_stop(&s->scan);
        return;
    }
    if (s->job || !s->scan.count) return;
    if (!wifi_packet_count_scan_plan(&s->scan,s->mode,cerberus_wifi_packet_count_channel(s->channel_index))) {
        s->error=ESP_ERR_INVALID_ARG; return;
    }
    if (c5_status.firmware_version<CERBERUS_PACKET_COUNT_MIN_FIRMWARE) {
        s->error=ESP_ERR_NOT_SUPPORTED; return;
    }
    s->error=ESP_OK; s->start_pending=true; s->detail=false; s->painted_count=0;
}
static void wifi_packet_count_tick(uint64_t now, bool visible, bool picker_visible)
{
    wifi_packet_count_ui_store_t *s=wifi_packet_count_ui;
    if (!s) {
        if (!wifi_count_diagnostics_initialized) {
            wifi_packet_count_diagnostics_clear(now); wifi_count_diagnostics_initialized=true;
        }
        return;
    }
    if (!visible && !(s->picking && picker_visible) && !s->closing) wifi_count_close(false);
    if (s->job && !s->job_retired) {
        wifi_packet_count_record_status_t status;
        if (!wifi_packet_count_record_status(s->job,&status)) {
            /* The facade retires its exact job only after freeing worker
             * context and clearing busy. A mismatched job cannot retain our
             * old source references; never finish/release the replacement. */
            s->job_retired=true; s->recording.busy=false;
            s->recording.error=s->error=ESP_ERR_NOT_FOUND; s->start_pending=false;
            s->recording.source[0].loss_unknown=s->recording.source[1].loss_unknown=true;
            wifi_packet_count_scan_close(&s->scan);
        } else {
            s->recording=status;
            if (status.error!=ESP_OK) { s->error=status.error; s->start_pending=false; }
        }
    }
    if (s->start_pending && visible && !s->closing && wifi_count_previous_released()) {
        if (s->record_enabled && !s->job) {
            s->error=wifi_packet_count_record_start(&wifi_count_transport,&s->job);
            if (s->error!=ESP_OK) s->start_pending=false;
            else { s->job_retired=false; memset(&s->recording,0,sizeof(s->recording)); }
        } else if (!s->record_enabled || (s->recording.ready && s->recording.error==ESP_OK)) {
            uint64_t mask=wifi_packet_count_scan_plan(&s->scan,s->mode,
                cerberus_wifi_packet_count_channel(s->channel_index));
            s->error=wifi_packet_count_scan_begin(&s->scan,mask,100U,s->record_enabled);
            s->start_pending=false;
        }
    }
    wifi_packet_count_scan_tick(&s->scan,now,&wifi_count_transport);
    bool released=wifi_packet_count_scan_released(&s->scan);
    if (s->job && s->job_retired && released) s->job=0;
    if (s->job && released && !s->start_pending) {
        (void)wifi_packet_count_record_finish(s->job);
        if (wifi_packet_count_record_status(s->job,&s->recording) && !s->recording.busy) {
            esp_err_t error=wifi_packet_count_record_release(s->job);
            if (error==ESP_OK) s->job=0;
            else s->error=error;
        }
    }
    if (s->closing && released && !s->job) {
        bool back=s->return_origin; uint8_t origin=s->origin;
        heap_caps_free(s); wifi_packet_count_ui=NULL;
        wifi_packet_count_diagnostics_clear(now);
        wifi_count_diagnostics_initialized=true;
        if (back && visible) {
            if (origin==UI_WIFI_MODE_OBSERVED_CLIENTS) wifi_mode=origin;
            else dual_wifi_live_open(false);
            ESP_ERROR_CHECK(draw_detail_page());
        }
        return;
    }
    wifi_packet_count_diagnostics_publish(&s->scan,&s->recording,now,s->job,
        s->start_pending,s->closing,s->error);
    wifi_count_diagnostics_initialized=true;
    if (visible && now-s->last_draw_ms>=500U) {
        s->last_draw_ms=now; ESP_ERROR_CHECK(draw_detail_page());
    }
}
static esp_err_t wifi_count_button(unsigned x,unsigned y,unsigned w,const char *label,bool enabled)
{ return cerberus_ui_widget_draw_button((cerberus_ui_rect_t){x,y,w,36},label,1U,enabled,UI_WIDGET_THEME); }
static void wifi_count_short(uint64_t value,char *out,size_t size)
{
    if (value<UINT64_C(1000000000)) snprintf(out,size,"%llu",(unsigned long long)value);
    else if (value<UINT64_C(1000000000000)) snprintf(out,size,"%lluM+",(unsigned long long)(value/1000000U));
    else if (value<UINT64_C(1000000000000000)) snprintf(out,size,"%lluG+",(unsigned long long)(value/1000000000U));
    else snprintf(out,size,"%lluT+",(unsigned long long)(value/UINT64_C(1000000000000)));
}
static void wifi_count_small32(uint32_t value,char *out,size_t size)
{
    if(value<100000U)snprintf(out,size,"%lu",(unsigned long)value);
    else if(value<100000000U)snprintf(out,size,"%luK+",(unsigned long)(value/1000U));
    else snprintf(out,size,"%luM+",(unsigned long)(value/1000000U));
}
static const char *wifi_count_state(const wifi_packet_count_source_t *v)
{
    if (v->faulted) return "CLOSE REQUIRED";
    if (v->stop && v->request.session_id) return "STOPPING";
    if (v->error!=ESP_OK) return "ERROR";
    if (v->published.frozen) return "FROZEN";
    if (v->remote.flags&CERBERUS_PACKET_COUNT_RX_ACTIVE) return "RECEIVING";
    if (v->phase==WIFI_COUNT_INFO) return "CHECKING RADIO";
    if (v->phase==WIFI_COUNT_BEGIN) return "RESERVING";
    if (v->phase==WIFI_COUNT_TARGET) return "SETTING TARGETS";
    if (v->phase==WIFI_COUNT_START) return "STARTING";
    if (v->remote.state==CERBERUS_PACKET_COUNT_QUEUED) return "QUEUED";
    if (v->phase==WIFI_COUNT_PEEK || v->phase==WIFI_COUNT_READ ||
        v->phase==WIFI_COUNT_FINAL_POLL) return "READING COUNTERS";
    return v->phase==WIFI_COUNT_IDLE?"IDLE":"WAITING";
}
static bool wifi_count_analysis_only(const wifi_packet_count_ui_store_t *s,unsigned source)
{
    const wifi_packet_count_source_t *v=&s->scan.sources[source];
    return source==WIFI_PACKET_COUNT_SOURCE_S3 && !v->request.channel_mask &&
        s->scan.sources[WIFI_PACKET_COUNT_SOURCE_C5].request.channel_mask &&
        !v->faulted && !v->error;
}
/* Progress describes copied wire parts, not elapsed RF coverage or packets. */
static bool wifi_count_readout(const wifi_packet_count_source_t *v,char *out,size_t size)
{
    if (v->phase==WIFI_COUNT_TARGET) {
        snprintf(out,size,"TARGET %u/%u",(unsigned)v->upload_index+1U,v->request.target_count);
        return true;
    }
    if (v->phase==WIFI_COUNT_PEEK || v->phase==WIFI_COUNT_READ) {
        const char *kind=v->kind==CERBERUS_PACKET_COUNT_READ_SUMMARY?"SUMMARY":
            v->kind==CERBERUS_PACKET_COUNT_READ_ROW?"TARGET":"CHANNEL";
        unsigned parts=v->kind==CERBERUS_PACKET_COUNT_READ_SUMMARY?16U:
            v->kind==CERBERUS_PACKET_COUNT_READ_ROW?12U:2U;
        unsigned done=v->phase==WIFI_COUNT_READ?v->part:0U;
        snprintf(out,size,"READ %.7s %u/%u",kind,done,parts);
        return true;
    }
    if (v->phase==WIFI_COUNT_FINAL_POLL) {
        snprintf(out,size,"VERIFYING READOUT"); return true;
    }
    return false;
}
static void wifi_count_paint_value(wifi_packet_count_ui_store_t *s,unsigned index,wifi_count_painted_t *out)
{
    memset(out,0,sizeof(*out)); out->index=(uint8_t)index; out->selection=s->scan.selected[index];
    (void)wifi_packet_count_scan_row(&s->scan,index,&out->total,&out->saturated);
    for (unsigned radio=0;radio<2U;++radio) {
        out->counts_valid|=s->scan.sources[radio].published.valid || s->scan.sources[radio].completed.segments;
        out->source_session[radio]=s->scan.sources[radio].published.session;
        out->signal[radio]=wifi_packet_count_scan_signal_age(&s->scan,radio,index,esp_timer_get_time(),&out->age_us[radio]);
        out->rssi[radio]=s->scan.sources[radio].published.rows[index].rssi;
    }
}
static esp_err_t wifi_packet_count_draw(void)
{
    wifi_packet_count_ui_store_t *s=wifi_packet_count_ui;
    if (s) s->painted_count=0;
    ESP_RETURN_ON_ERROR(cerberus_display_fill_rect(0,0,320,480,UI_BACKGROUND),TAG,"count clear");
    ESP_RETURN_ON_ERROR(draw_header("PACKET COUNT"),TAG,"count header");
    if (!s) {
        ESP_RETURN_ON_ERROR(draw_centered(152,"VIEW UNAVAILABLE / PSRAM",UI_BAD,1),TAG,"count oom");
        return wifi_count_button(8,434,148,"BACK",true);
    }
    char line[53],mac[24],number[24];
    bool idle=wifi_packet_count_scan_released(&s->scan)&&!s->job&&!s->start_pending&&!s->closing;
    ESP_RETURN_ON_ERROR(draw_centered(42,"SDK CALLBACK TOTALS / NOT UNIQUE PACKETS",UI_MUTED,1),TAG,"count meaning");
    if (s->detail) {
        const wifi_count_painted_t *v=&s->selected;
        cerberus_privacy_format_wifi_identifier(v->selection.target.mac,privacy_addresses_masked(),mac,sizeof(mac));
        snprintf(line,sizeof(line),"%s %s",v->selection.target.role==CERBERUS_WIFI_PACKET_COUNT_AP?"AP":"STA",mac);
        ESP_RETURN_ON_ERROR(draw_centered(68,line,UI_TEXT,1),TAG,"count identity");
        snprintf(line,sizeof(line),"FROZEN DETAIL / DISCOVERY CH %u",v->selection.target.channel_hint);
        ESP_RETURN_ON_ERROR(draw_centered(88,line,UI_MUTED,1),TAG,"count frozen");
        const uint64_t values[]={v->total.observations,v->total.transmitted,v->total.addressed,
            v->total.bssid_context,v->total.management,v->total.data,v->total.retry};
        static const char *const labels[]={"OBSERVED","TRANSMITTER","RECEIVER","BSSID ROLE (SEPARATE)","MANAGEMENT","DATA","RETRY FLAG"};
        for (unsigned i=0;i<7U;++i) {
            if (v->counts_valid) snprintf(line,sizeof(line),"%s: %llu%s",labels[i],(unsigned long long)values[i],v->saturated?"+":"");
            else snprintf(line,sizeof(line),"%s: NOT SAMPLED",labels[i]);
            ESP_RETURN_ON_ERROR(draw_centered(114U+i*23U,line,UI_TEXT,1),TAG,"count detail total");
        }
        if (v->counts_valid) snprintf(line,sizeof(line),"PROTECTED FLAG: %llu",(unsigned long long)v->total.protected_frames);
        else snprintf(line,sizeof(line),"PROTECTED FLAG: NOT SAMPLED");
        ESP_RETURN_ON_ERROR(draw_centered(274,line,UI_TEXT,1),TAG,"count protected");
        if (v->counts_valid) snprintf(line,sizeof(line),"FRAGMENT FLAG: %llu",(unsigned long long)v->total.fragmented);
        else snprintf(line,sizeof(line),"FRAGMENT FLAG: NOT SAMPLED");
        ESP_RETURN_ON_ERROR(draw_centered(290,line,UI_TEXT,1),TAG,"count fragment");
        for (unsigned radio=0;radio<2U;++radio) {
            if (v->signal[radio]) snprintf(line,sizeof(line),"%s TX %d dBm / AGE@COPY <= %llus",radio?"C5":"S3",v->rssi[radio],
                (unsigned long long)(v->age_us[radio]/1000000U+(v->age_us[radio]%1000000U!=0)));
            else snprintf(line,sizeof(line),"%s TX SIGNAL / AGE UNKNOWN",radio?"C5":"S3");
            ESP_RETURN_ON_ERROR(draw_centered(314U+radio*20U,line,UI_MUTED,1),TAG,"count tx age");
        }
        ESP_RETURN_ON_ERROR(draw_centered(354,"ADDRESS ROLES DO NOT PROVE ASSOCIATION",UI_MUTED,1),TAG,"count role");
        ESP_RETURN_ON_ERROR(wifi_count_button(8,374,304,"REMOVE FROM BASKET",idle&&!s->scan.segments),TAG,"count remove");
    } else {
        for (unsigned radio=0;radio<2U;++radio) {
            const wifi_packet_count_source_t *v=&s->scan.sources[radio];
            bool analysis=wifi_count_analysis_only(s,radio);
            const char *state=wifi_count_state(v);
            /* Only a validated bank can distinguish rejected metadata from an
             * empty channel. Setup, cleanup and errors retain their labels. */
            bool metadata_rejected=!analysis && v->published.valid &&
                v->published.summary.invalid_metadata && !v->published.summary.valid_frames &&
                (!strcmp(state,"RECEIVING") || !strcmp(state,"FROZEN"));
            unsigned x=8U+radio*156U;
            ESP_RETURN_ON_ERROR(cerberus_display_fill_rect(x,60,148,50,UI_PANEL),TAG,"count source");
            if (metadata_rejected) snprintf(line,23U,"%s METADATA REJECTED",radio?"C5":"S3");
            else if (!analysis && !strcmp(state,"RECEIVING"))
                snprintf(line,23U,"%s RECEIVING CH%u",radio?"C5":"S3",v->remote.channel);
            else snprintf(line,sizeof(line),"%s %.19s",radio?"C5":"S3",analysis?"PROCESSING":state);
            ESP_RETURN_ON_ERROR(cerberus_display_draw_text(x+6U,66,line,metadata_rejected?UI_BAD:UI_TEXT,UI_PANEL,1),TAG,"count source state");
            uint64_t window=0; bool saturated=false;
            for (unsigned i=0;i<42U;++i) {
                uint64_t n=v->completed.channel_window_us[i];
                if (v->published.valid&&!v->committed) {
                    uint64_t extra=v->published.channel_window_us[i];
                    if (UINT64_MAX-n<extra) { n=UINT64_MAX;saturated=true; } else n+=extra;
                }
                if (UINT64_MAX-window<n) { window=UINT64_MAX;saturated=true; } else window+=n;
            }
            wifi_count_short(window/1000000U,number,sizeof(number));
            char channel[4]="--";
            if (v->remote.channel) snprintf(channel,sizeof(channel),"%u",v->remote.channel);
            if (analysis) snprintf(line,23U,"COUNTS + DISPLAY");
            else if (s->scan.recording && wifi_count_readout(v,line,23U)) { /* retain queue loss line below */ }
            else if (!v->published.valid && !v->completed.segments) snprintf(line,23U,"RX --s CH%s",channel);
            else snprintf(line,23U,"RX %.10ss%s CH%s",number,saturated?"+":"",channel);
            ESP_RETURN_ON_ERROR(cerberus_display_draw_text(x+6U,80,line,UI_MUTED,UI_PANEL,1),TAG,"count duration");
            if (analysis && (!s->scan.recording || !v->completed.segments)) snprintf(line,23U,"RECEIVER: C5");
            else if (!s->scan.recording && wifi_count_readout(v,line,23U)) { /* copied-part progress */ }
            else if (!s->scan.recording) snprintf(line,sizeof(line),v->published.valid?"COUNTERS VALIDATED":"AWAITING COUNTERS");
            else if (!v->published.valid && !v->completed.segments) snprintf(line,sizeof(line),"Q -- / DROP --");
            else {
                uint64_t dropped=v->completed.raw_dropped_full;
                if(UINT64_MAX-dropped<v->completed.raw_dropped_other)dropped=UINT64_MAX;
                else dropped+=v->completed.raw_dropped_other;
                uint64_t current=(uint64_t)v->published.recording.dropped_full+v->published.recording.dropped_other;
                if(!v->committed){if(UINT64_MAX-dropped<current)dropped=UINT64_MAX;else dropped+=current;}
                wifi_count_short(dropped,number,sizeof(number));
                /* wifi_count_short emits at most ten characters. */
                snprintf(line,23U,"Q%u / DROP %.10s",v->published.recording.pending,number);
            }
            ESP_RETURN_ON_ERROR(cerberus_display_draw_text(x+6U,94,line,UI_MUTED,UI_PANEL,1),TAG,"count queue losses");
        }
        static const char *const plans[]={"FIXED CHANNEL","SELECTED CHANNELS","ALL CHANNELS"};
        ESP_RETURN_ON_ERROR(wifi_count_button(8,118,196,plans[s->mode],!s->closing&&!s->start_pending&&!s->scan.retuning&&!s->job),TAG,"count plan");
        ESP_RETURN_ON_ERROR(wifi_count_button(212,118,46,"-",s->mode==WIFI_PACKET_COUNT_FIXED&&!s->closing&&!s->start_pending&&!s->scan.retuning),TAG,"count ch minus");
        ESP_RETURN_ON_ERROR(wifi_count_button(266,118,46,"+",s->mode==WIFI_PACKET_COUNT_FIXED&&!s->closing&&!s->start_pending&&!s->scan.retuning),TAG,"count ch plus");
        if (s->mode==WIFI_PACKET_COUNT_FIXED) snprintf(line,sizeof(line),"PCAP: %s / FIXED CH %u",s->record_enabled?"ON":"OFF",cerberus_wifi_packet_count_channel(s->channel_index));
        else snprintf(line,sizeof(line),"PCAP: %s / DWELL 100ms",s->record_enabled?"ON":"OFF");
        ESP_RETURN_ON_ERROR(wifi_count_button(8,158,304,line,idle),TAG,"count recording");
        for (unsigned row=0;row<4U&&s->page*4U+row<s->scan.count;++row) {
            wifi_count_painted_t *v=&s->painted[row];wifi_count_paint_value(s,s->page*4U+row,v);
            cerberus_privacy_format_wifi_identifier(v->selection.target.mac,privacy_addresses_masked(),mac,sizeof(mac));
            unsigned y=200U+row*40U;
            ESP_RETURN_ON_ERROR(cerberus_display_fill_rect(8,y,304,36,UI_PANEL),TAG,"count row");
            snprintf(line,sizeof(line),"%s %s",v->selection.target.role==CERBERUS_WIFI_PACKET_COUNT_AP?"AP":"STA",mac);
            ESP_RETURN_ON_ERROR(cerberus_display_draw_text(14,y+3U,line,UI_TEXT,UI_PANEL,1),TAG,"count row identity");
            char tx[24],rx[24];wifi_count_short(v->total.observations,number,sizeof(number));
            wifi_count_short(v->total.transmitted,tx,sizeof(tx));wifi_count_short(v->total.addressed,rx,sizeof(rx));
            if (v->counts_valid) snprintf(line,50U,"TOTAL %.10s / TX %.10s / RX %.10s",number,tx,rx);
            else snprintf(line,sizeof(line),"TOTAL -- / TX -- / RX --");
            ESP_RETURN_ON_ERROR(cerberus_display_draw_text(14,y+19U,line,UI_MUTED,UI_PANEL,1),TAG,"count row totals");
        }
        if (!s->scan.count) ESP_RETURN_ON_ERROR(draw_centered(250,"ADD AP OR OBSERVED STATION",UI_MUTED,1),TAG,"count empty");
        if (s->recording.job) {
            bool partial=s->scan.sources[0].error || s->scan.sources[1].error || s->recording.error || s->recording.source[0].error || s->recording.source[1].error ||
                s->recording.source[0].loss_unknown || s->recording.source[1].loss_unknown ||
                s->recording.source[0].discarded || s->recording.source[1].discarded ||
                s->recording.source[0].transport_errors || s->recording.source[1].transport_errors;
            snprintf(line,sizeof(line),"%s /CERBERUS/PCOUNT/%.7s",s->recording.busy?"WRITING":partial?"PARTIAL":"SAVED",s->recording.directory);
            ESP_RETURN_ON_ERROR(draw_centered(361,line,UI_MUTED,1),TAG,"count file");
        }
        ESP_RETURN_ON_ERROR(wifi_count_button(8,374,52,"PREV",s->page>0U),TAG,"count previous");
        ESP_RETURN_ON_ERROR(wifi_count_button(68,374,52,"NEXT",(s->page+1U)*4U<s->scan.count),TAG,"count next");
        ESP_RETURN_ON_ERROR(wifi_count_button(128,374,88,"ADD AP",idle&&!s->scan.segments),TAG,"count add");
        ESP_RETURN_ON_ERROR(wifi_count_button(224,374,88,"NEW LIST",idle),TAG,"count clear");
    }
    if (s->error!=ESP_OK) snprintf(line,sizeof(line),"%.48s",esp_err_to_name(s->error));
    else if (s->scan.error!=ESP_OK) snprintf(line,sizeof(line),"%.48s",esp_err_to_name(s->scan.error));
    else if (s->scan.sources[0].error || s->scan.sources[1].error)
        snprintf(line,sizeof(line),"SOURCE ERROR S3=%X C5=%X",(unsigned)s->scan.sources[0].error,(unsigned)s->scan.sources[1].error);
    else if (s->start_pending) snprintf(line,sizeof(line),s->record_enabled?
        "WAITING FOR RADIO RELEASE / SD READY":"WAITING FOR PREVIOUS RADIO RELEASE");
    else if (s->recording.source[0].error || s->recording.source[1].error)
        snprintf(line,sizeof(line),"PCAP ERROR S3=%X C5=%X",(unsigned)s->recording.source[0].error,(unsigned)s->recording.source[1].error);
    else if (s->recording.source[0].transport_errors || s->recording.source[1].transport_errors)
        snprintf(line,sizeof(line),"PCAP LINK ERR S3=%lu C5=%lu",(unsigned long)s->recording.source[0].transport_errors,(unsigned long)s->recording.source[1].transport_errors);
    else if (s->recording.job) {
        char a[12],b[12],c[12],d[12];
        wifi_count_small32(s->recording.source[0].written,a,sizeof(a));wifi_count_small32(s->recording.source[1].written,b,sizeof(b));
        wifi_count_small32(s->recording.source[0].discarded,c,sizeof(c));wifi_count_small32(s->recording.source[1].discarded,d,sizeof(d));
        /* Each compact uint32 counter is at most seven characters. */
        snprintf(line,sizeof(line),"S3/C5 W:%.7s/%.7s %.5s:%.7s/%.7s",a,b,
            s->recording.source[0].loss_unknown||s->recording.source[1].loss_unknown?"LOSS?":"LOST",c,d);
    }
    else if (s->scan.fallback) snprintf(line,sizeof(line),"C5 ALL CHANNELS / S3 UNAVAILABLE");
    else snprintf(line,sizeof(line),"N%u SEG%lu / INTERRUPT%lu",s->scan.count,
        (unsigned long)s->scan.segments,(unsigned long)s->scan.interruptions);
    ESP_RETURN_ON_ERROR(draw_centered(412,line,s->error?UI_BAD:UI_MUTED,1),TAG,"count status");
    ESP_RETURN_ON_ERROR(wifi_count_button(8,434,148,s->detail?"BACK TO TOTALS":"BACK",true),TAG,"count back");
    bool running=s->start_pending||!wifi_packet_count_scan_released(&s->scan);
    ESP_RETURN_ON_ERROR(wifi_count_button(164,434,148,s->closing?"CLOSING":running?"STOP":"START",
        !s->closing&&(running||(!s->job&&s->scan.count))),TAG,"count start");
    if (!s->detail) s->painted_count=(uint8_t)((s->scan.count-s->page*4U)>4U?4U:s->scan.count-s->page*4U);
    return ESP_OK;
}
static void wifi_packet_count_tap(uint16_t x,uint16_t y)
{
    wifi_packet_count_ui_store_t *s=wifi_packet_count_ui;
    if (y>=434U&&y<470U) { if(x>=8U&&x<156U)wifi_packet_count_back();else if(x>=164U&&x<312U)wifi_count_start_stop();return; }
    if (!s || s->closing) return;
    bool idle=wifi_packet_count_scan_released(&s->scan)&&!s->start_pending&&!s->job;
    if (s->detail) {
        if (idle&&!s->scan.segments&&x>=8U&&x<312U&&y>=374U&&y<410U) {
            s->error=wifi_packet_count_scan_remove(&s->scan,s->selected.index);s->detail=false;s->page=0;
        }
        return;
    }
    if (y>=118U&&y<154U&&!s->start_pending&&!s->scan.retuning) {
        unsigned mode=s->mode,channel=s->channel_index;
        if(x>=8U&&x<204U&&!s->job)mode=(mode+1U)%3U;
        else if(mode==WIFI_PACKET_COUNT_FIXED&&x>=212U&&x<258U)channel=(channel+41U)%42U;
        else if(mode==WIFI_PACKET_COUNT_FIXED&&x>=266U&&x<312U)channel=(channel+1U)%42U;
        else return;
        uint64_t plan=wifi_packet_count_scan_plan(&s->scan,mode,cerberus_wifi_packet_count_channel(channel));
        s->error=idle?ESP_OK:wifi_packet_count_scan_retune(&s->scan,plan);
        if(s->error==ESP_OK){s->mode=(uint8_t)mode;s->channel_index=(uint8_t)channel;}
    } else if (idle&&x>=8U&&x<312U&&y>=158U&&y<194U) s->record_enabled=!s->record_enabled;
    else if (x>=8U&&x<312U&&y>=200U&&y<360U&&(y-200U)%40U<36U) {
        unsigned row=(y-200U)/40U;
        if(row<s->painted_count){s->selected=s->painted[row];s->detail=true;}
    } else if(y>=374U&&y<410U) {
        if(x>=8U&&x<60U&&s->page)--s->page;
        else if(x>=68U&&x<120U&&(s->page+1U)*4U<s->scan.count)++s->page;
        else if(x>=128U&&x<216U&&idle&&!s->scan.segments){s->picking=true;dual_wifi_live_open(false);}
        else if(x>=224U&&x<312U&&idle){wifi_packet_count_scan_init(&s->scan);s->page=0;s->error=ESP_OK;}
    }
    s->painted_count=0;
}
