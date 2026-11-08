#pragma once

#include "wifi_ap_survey_model.h"
#include "wifi_ap_survey_session.h"
#include "wifi_ap_survey_ui.h"
#include "s3_wifi_ap_survey.h"
#include "wifi_capture_plan.h"
#include "wifi_ap_survey_report.h"

typedef struct {
    cerberus_ap_survey_model_t model;
    cerberus_ap_survey_session_t session;
    cerberus_c5_ap_survey_reader_t reader;
    cerberus_ap_survey_ui_cache_t cache;
    cerberus_ap_survey_ui_view_t view;
    cerberus_ap_survey_record_t filter;
    uint32_t last_tick, last_draw;
    uint8_t kind, offset;
    bool reader_active, detail, filtered, pending_start, exit_pending;
    char message[49];
} ap_survey_live_store_t;

static ap_survey_live_store_t *ap_survey_live;
static uint64_t ap_survey_run_id;

static bool ap_survey_visible(void)
{
    return current_page == UI_PAGE_WIFI && wifi_mode == UI_WIFI_MODE_AP_SURVEY;
}

static bool ap_survey_busy(void)
{
    return ap_survey_live && (ap_survey_live->pending_start ||
        !cerberus_ap_survey_session_released(&ap_survey_live->session));
}

static int32_t ap_survey_exchange(void *context, uint8_t source, uint8_t command,
    const void *request, size_t bytes, cerberus_ap_survey_control_t *out)
{
    (void)context;
    return source == CERBERUS_AP_SURVEY_SOURCE_S3 ?
        cerberus_s3_wifi_ap_survey_exchange(command, request, bytes, out, sizeof(*out)) :
        c5_spi_link_wifi_ap_survey_exchange(command, request, bytes, out, sizeof(*out));
}

static void ap_survey_snapshot_cancel(void *context, uint8_t source)
{
    ap_survey_live_store_t *store = context;
    if (source == CERBERUS_AP_SURVEY_SOURCE_C5) {
        c5_spi_link_wifi_ap_survey_reader_cancel(&store->reader);
        store->reader_active = false;
    }
}

static int32_t ap_survey_snapshot_step(void *context, uint8_t source, uint64_t session,
    cerberus_ap_survey_snapshot_t *out, bool *complete, uint64_t *request_started_us,
    uint64_t *receipt_us)
{
    ap_survey_live_store_t *store = context;
    *complete = false;
    if (source == CERBERUS_AP_SURVEY_SOURCE_S3) {
        *request_started_us = (uint64_t)esp_timer_get_time();
        esp_err_t error = cerberus_s3_wifi_ap_survey_snapshot(session, out);
        *receipt_us = (uint64_t)esp_timer_get_time();
        *complete = error == ESP_OK;
        return error;
    }
    if (!store->reader_active) {
        esp_err_t error = c5_spi_link_wifi_ap_survey_reader_begin(&store->reader, session, out,
            (uint32_t)(esp_timer_get_time() / 1000LL) + 15000U);
        if (error != ESP_OK) return error;
        store->reader_active = true;
    } else if (store->reader.session_id != session || store->reader.staging != out) {
        ap_survey_snapshot_cancel(store, source);
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t error = c5_spi_link_wifi_ap_survey_reader_step(&store->reader, complete);
    if (error == ESP_OK && *complete) {
        *request_started_us = store->reader.request_started_us;
        *receipt_us = store->reader.receipt_us;
        /* Completed staging belongs to the coordinator until publish returns. */
        store->reader_active = false;
    } else if (error != ESP_OK) ap_survey_snapshot_cancel(store, source);
    return error;
}

static bool ap_survey_publish(void *context, uint8_t source, uint64_t run_id,
    uint64_t request_started_us, uint64_t receipt_us, const cerberus_ap_survey_snapshot_t *snapshot)
{
    ap_survey_live_store_t *store = context;
    cerberus_ap_survey_model_result_t result = cerberus_ap_survey_model_publish(&store->model,
        source, run_id, request_started_us, receipt_us, snapshot);
    return result == CERBERUS_AP_SURVEY_MODEL_APPLIED || result == CERBERUS_AP_SURVEY_MODEL_DUPLICATE;
}

static void ap_survey_unavailable(void *context, uint8_t source, bool unavailable)
{
    ap_survey_live_store_t *store = context;
    cerberus_ap_survey_model_unavailable(&store->model, source, unavailable);
}

static const char *ap_survey_receiver_state(const cerberus_ap_survey_radio_session_t *radio,
    bool pending)
{
    if (pending) return "WAITING";
    if (radio->close_requested || radio->stop_requested) return "STOPPING";
    if (radio->error) return radio->error == ESP_ERR_NOT_SUPPORTED ? "UNSUPPORTED" : "ERROR";
    if (radio->read_error) return "READ ERROR";
    if (radio->phase == CERBERUS_AP_SURVEY_SESSION_INFO || radio->phase == CERBERUS_AP_SURVEY_SESSION_START)
        return "STARTING";
    if (radio->remote.flags & CERBERUS_AP_SURVEY_RECEIVING) return "RECEIVING";
    if (radio->remote.flags & CERBERUS_AP_SURVEY_HELD) return "TUNING / HELD";
    return radio->published ? "STOPPED" : "READY";
}

static esp_err_t ap_survey_draw(bool force)
{
    ap_survey_live_store_t *store = ap_survey_live;
    if (!store) {
        ESP_RETURN_ON_ERROR(cerberus_display_fill_rect(0, 0, 320, 480, UI_BACKGROUND), TAG, "survey clear");
        ESP_RETURN_ON_ERROR(draw_centered(140, "AP / CLIENT SURVEY", UI_TEXT, 2), TAG, "survey title");
        ESP_RETURN_ON_ERROR(draw_centered(180, "NO MEMORY FOR SURVEY", UI_BAD, 1), TAG, "survey memory");
        return draw_button(8, 432, 148, 42, "BACK TO SCAN");
    }
    cerberus_ap_survey_ui_view_t *view = &store->view;
    memset(view, 0, sizeof(*view));
    view->now_us = (uint64_t)esp_timer_get_time();
    view->detail = store->detail;
    view->private_values = privacy_addresses_masked();
    view->selection = store->model.selected;
    bool released = cerberus_ap_survey_session_released(&store->session);
    view->start = released && !store->pending_start && !store->exit_pending;
    view->stop = !released || store->pending_start;
    view->back = true;
    view->save = released && !store->pending_start && !store->exit_pending &&
        (store->model.banks[0].published || store->model.banks[1].published);
    view->message = store->message;
    view->back_label = store->exit_pending ? "STOPPING" : store->detail ?
        (store->model.selected.frozen.kind == CERBERUS_AP_SURVEY_AP ? "BACK TO APS" : "BACK TO CLIENTS") :
        store->filtered ? "BACK TO AP" : "BACK TO SCAN";
    for (unsigned i = 0; i < 2U; ++i) {
        const cerberus_ap_survey_radio_session_t *radio = &store->session.radios[i];
        const cerberus_ap_survey_bank_t *bank = &store->model.banks[i];
        view->receivers[i].state = ap_survey_receiver_state(radio, store->pending_start);
        view->receivers[i].channel = radio->have_control ? radio->remote.channel : 0U;
        view->receivers[i].coverage_valid = bank->published && !bank->unavailable;
        view->receivers[i].passes = bank->snapshot.completed_passes;
        view->receivers[i].requested_channels = bank->snapshot.requested_channels;
        view->receivers[i].applied_channels = bank->snapshot.applied_channels;
        view->ap_count += bank->snapshot.ap_count;
        view->client_count += bank->snapshot.relation_count;
    }
    if (!cerberus_ap_survey_model_page(&store->model, store->kind,
        store->filtered ? &store->filter : NULL, store->offset, &view->page)) return ESP_ERR_INVALID_STATE;
    if (store->offset && !view->page.copied) {
        store->offset = view->page.total ? (uint8_t)(((view->page.total - 1U) / 4U) * 4U) : 0U;
        (void)cerberus_ap_survey_model_page(&store->model, store->kind,
            store->filtered ? &store->filter : NULL, store->offset, &view->page);
    }
    if (store->detail && store->model.selected.frozen.kind == CERBERUS_AP_SURVEY_AP) {
        cerberus_ap_survey_page_t clients;
        if (cerberus_ap_survey_model_page(&store->model, CERBERUS_AP_SURVEY_RELATION,
            &store->model.selected.frozen, 0U, &clients)) view->selected_client_count = clients.total;
    }
    esp_err_t error = cerberus_ap_survey_ui_update(&store->cache, view, force);
    if (error == ESP_OK) store->last_draw = (uint32_t)(view->now_us / 1000U);
    return error;
}

static void ap_survey_open(void)
{
    /* Entering a paused workspace never schedules a finite scan or RF start. */
    current_page = UI_PAGE_WIFI;
    wifi_mode = UI_WIFI_MODE_AP_SURVEY;
    ui_capability_blocked = false;
    ui_capability_blocked_reason = NULL;
    if (!ap_survey_live) {
        ap_survey_live = heap_caps_calloc(1, sizeof(*ap_survey_live), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (ap_survey_live) {
            cerberus_ap_survey_model_init(&ap_survey_live->model);
            cerberus_ap_survey_session_init(&ap_survey_live->session);
            ap_survey_live->kind = CERBERUS_AP_SURVEY_AP;
        }
    }
    ESP_ERROR_CHECK(ap_survey_draw(true));
}

static bool ap_survey_finish_exit(void)
{
    if (ap_survey_live && !cerberus_ap_survey_session_released(&ap_survey_live->session)) return false;
    if (ap_survey_live) ap_survey_snapshot_cancel(ap_survey_live, CERBERUS_AP_SURVEY_SOURCE_C5);
    heap_caps_free(ap_survey_live); ap_survey_live = NULL;
    if (ap_survey_visible()) {
        current_page = UI_PAGE_FIELD_MENU; field_menu.attack = false; field_menu.page = 0;
        ESP_ERROR_CHECK(draw_current_page());
    }
    return true;
}

static void ap_survey_close(void)
{
    if (!ap_survey_live) { (void)ap_survey_finish_exit(); return; }
    ap_survey_live->pending_start = false;
    ap_survey_live->exit_pending = true;
    cerberus_ap_survey_session_close(&ap_survey_live->session);
    (void)ap_survey_finish_exit();
}

static void ap_survey_save(void)
{
    ap_survey_live_store_t *store = ap_survey_live;
    if (!store || store->pending_start || store->exit_pending ||
        !cerberus_ap_survey_session_released(&store->session)) return;
    char *report = heap_caps_malloc(CERBERUS_AP_SURVEY_REPORT_CAPACITY, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    size_t length = report ? cerberus_ap_survey_report(&store->model, report, CERBERUS_AP_SURVEY_REPORT_CAPACITY) : 0;
    char name[32] = "";
    esp_err_t error = !report ? ESP_ERR_NO_MEM : !length ? ESP_ERR_INVALID_SIZE :
        cerberus_storage_write_wifi_report(report, length, name, sizeof(name));
    heap_caps_free(report);
    if (error == ESP_OK) snprintf(store->message, sizeof(store->message), "SAVED %.31s", name);
    else snprintf(store->message, sizeof(store->message), "SAVE: %.40s", esp_err_to_name(error));
}

static void ap_survey_tap(uint16_t x, uint16_t y)
{
    ap_survey_live_store_t *store = ap_survey_live;
    if (!store) {
        if (x >= 8U && x < 156U && y >= 432U && y < 474U) ap_survey_close();
        return;
    }
    cerberus_ap_survey_ui_event_t event = cerberus_ap_survey_ui_tap(&store->cache, x, y);
    if (store->exit_pending && event.action != CERBERUS_AP_SURVEY_UI_BACK && event.action != CERBERUS_AP_SURVEY_UI_STOP) return;
    switch (event.action) {
    case CERBERUS_AP_SURVEY_UI_START:
        store->pending_start = true; store->message[0] = '\0'; break;
    case CERBERUS_AP_SURVEY_UI_STOP:
        store->pending_start = false; cerberus_ap_survey_session_stop(&store->session); break;
    case CERBERUS_AP_SURVEY_UI_BACK:
        if (store->exit_pending) { ap_survey_close(); if (!ap_survey_live) return; }
        else if (store->detail) {
            store->detail = false; cerberus_ap_survey_model_clear_selection(&store->model);
        } else if (store->filtered) {
            store->filtered = false; store->kind = CERBERUS_AP_SURVEY_AP; store->offset = 0;
            store->detail = cerberus_ap_survey_model_select(&store->model, &store->filter);
        } else { ap_survey_close(); if (!ap_survey_live) return; }
        break;
    case CERBERUS_AP_SURVEY_UI_APS:
    case CERBERUS_AP_SURVEY_UI_CLIENTS:
        store->kind = event.action == CERBERUS_AP_SURVEY_UI_APS ? CERBERUS_AP_SURVEY_AP : CERBERUS_AP_SURVEY_RELATION;
        store->filtered = false; store->offset = 0; break;
    case CERBERUS_AP_SURVEY_UI_SELECT:
        store->detail = cerberus_ap_survey_model_select(&store->model, &event.record); break;
    case CERBERUS_AP_SURVEY_UI_AP_CLIENTS:
        if (!store->detail || !store->model.selected.valid ||
            !cerberus_ap_survey_record_equal(&event.record, &store->model.selected.frozen)) return;
        store->filter = event.record; store->filtered = true; store->detail = false;
        store->kind = CERBERUS_AP_SURVEY_RELATION; store->offset = 0;
        cerberus_ap_survey_model_clear_selection(&store->model); break;
    case CERBERUS_AP_SURVEY_UI_PREVIOUS: if (store->offset >= 4U) store->offset -= 4U; break;
    case CERBERUS_AP_SURVEY_UI_NEXT: store->offset += 4U; break;
    case CERBERUS_AP_SURVEY_UI_SAVE: ap_survey_save(); break;
    default: return;
    }
    if (ap_survey_visible()) ESP_ERROR_CHECK(ap_survey_draw(false));
}

static bool ap_survey_previous_owners_released(void)
{
    cerberus_dual_wifi_status_t dual;
    cerberus_dual_wifi_scan_get_status(&dual);
    return dual_wifi_live_released() && !dual_wifi_ui_busy(&dual) &&
        remote_id_takeover_ready() && !remote_id_keeps_awake() && !mac_monitor_keeps_awake() &&
        !capture_busy() && !station_busy() && !ap_busy() && !multissid_busy() && !pinescan_busy();
}

static void ap_survey_tick(uint32_t now, bool asleep)
{
    ap_survey_live_store_t *store = ap_survey_live;
    if (!store) return;
    if (asleep || !ap_survey_visible()) {
        store->pending_start = false;
        cerberus_ap_survey_session_close(&store->session);
        if (!ap_survey_visible()) store->exit_pending = true;
    }
    if ((uint32_t)(now - store->last_tick) < 20U) return;
    store->last_tick = now;
    if (store->pending_start && !store->exit_pending && !asleep) {
        if (ap_survey_previous_owners_released()) {
            store->pending_start = false;
            if (ap_survey_run_id == UINT64_MAX || !cerberus_ap_survey_session_start(&store->session,
                ++ap_survey_run_id, cerberus_wifi_capture_plan_band_mask(CERBERUS_WIFI_CAPTURE_PLAN_2G),
                cerberus_wifi_capture_plan_band_mask(CERBERUS_WIFI_CAPTURE_PLAN_5G))) {
                snprintf(store->message, sizeof(store->message), "CANNOT START / SESSION NOT RELEASED");
            } else {
                cerberus_ap_survey_model_init(&store->model);
                store->detail = store->filtered = false; store->offset = 0U;
                store->kind = CERBERUS_AP_SURVEY_AP; store->message[0] = '\0';
            }
        } else snprintf(store->message, sizeof(store->message), "WAITING FOR PREVIOUS RADIO OWNERS");
    }
    cerberus_ap_survey_session_ops_t ops = {.context = store, .exchange = ap_survey_exchange,
        .snapshot_step = ap_survey_snapshot_step, .snapshot_cancel = ap_survey_snapshot_cancel,
        .publish = ap_survey_publish, .unavailable = ap_survey_unavailable};
    cerberus_ap_survey_session_tick(&store->session, (uint64_t)esp_timer_get_time(), &ops);
    if (store->exit_pending && ap_survey_finish_exit()) return;
    if (!asleep && ap_survey_visible() && (uint32_t)(now - store->last_draw) >= 500U)
        ESP_ERROR_CHECK(ap_survey_draw(false));
}
