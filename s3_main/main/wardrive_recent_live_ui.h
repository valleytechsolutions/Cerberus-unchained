/* Private adapter included after the common live UI definitions. Recent
 * results stay inside the Wardriving page, preserving its session ownership. */
#include "wardrive_recent_ui.h"

static wardrive_recent_ui_store_t *wardrive_recent_view;
static bool wardrive_recent_opened;
static esp_err_t wardrive_recent_open_error;

static bool wardrive_recent_active(cerberus_wardrive_state_t state)
{
    return state == CERBERUS_WARDRIVE_STARTING ||
        state == CERBERUS_WARDRIVE_RUNNING || state == CERBERUS_WARDRIVE_STOPPING;
}

static bool wardrive_recent_is_open(void)
{
    return wardrive_recent_opened && current_page == UI_PAGE_WARDRIVE;
}

static void wardrive_recent_close(void)
{
    heap_caps_free(wardrive_recent_view);
    wardrive_recent_view = NULL;
    wardrive_recent_opened = false;
    wardrive_recent_open_error = ESP_OK;
}

static void wardrive_recent_release_if_left(void)
{
    if (current_page != UI_PAGE_WARDRIVE && wardrive_recent_opened)
        wardrive_recent_close();
}

static esp_err_t wardrive_recent_draw(void)
{
    bool stop_available = wardrive_start_pending;
    if (wardrive_recent_view) {
        cerberus_wardrive_copy_recent(&wardrive_recent_view->snapshot);
        stop_available |= wardrive_recent_active(wardrive_recent_view->snapshot.state);
    } else {
        cerberus_wardrive_recent_status_t status;
        cerberus_wardrive_get_recent_status(&status);
        stop_available |= wardrive_recent_active(status.state);
    }
    int64_t now = esp_timer_get_time();
    return wardrive_recent_ui_draw(wardrive_recent_view, wardrive_recent_open_error,
        now < 0 ? UINT64_MAX : (uint64_t)now / 1000U, stop_available);
}

static void wardrive_recent_open(void)
{
    if (current_page != UI_PAGE_WARDRIVE) return;
    wardrive_recent_opened = true;
    if (!wardrive_recent_view)
        wardrive_recent_view = heap_caps_calloc(1U, sizeof(*wardrive_recent_view),
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    wardrive_recent_open_error = wardrive_recent_view ? ESP_OK : ESP_ERR_NO_MEM;
    wardrive_last_draw_ms = 0U;
    ESP_ERROR_CHECK(draw_detail_page());
}

static void wardrive_recent_handle_tap(uint16_t x, uint16_t y)
{
    cerberus_wardrive_recent_status_t status;
    cerberus_wardrive_get_recent_status(&status);
    bool active = wardrive_recent_active(status.state);
    wardrive_recent_ui_action_t action = wardrive_recent_ui_tap(
        wardrive_recent_view, x, y, active || wardrive_start_pending);
    if (action == WARDRIVE_RECENT_UI_BACK) {
        wardrive_recent_close();
    } else if (action == WARDRIVE_RECENT_UI_STOP) {
        wardrive_start_pending = false;
        if (active) cerberus_wardrive_request_stop();
    } else if (action != WARDRIVE_RECENT_UI_REDRAW) {
        return;
    }
    wardrive_last_draw_ms = 0U;
    ESP_ERROR_CHECK(draw_detail_page());
}
