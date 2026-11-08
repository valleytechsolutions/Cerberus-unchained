/* Real editor, keyboard, Wardriving summary and Start route. Storage and GPS
 * remain value-only boundaries; all coordinates below are synthetic. */
#define CERBERUS_AREA_UI_FIXTURE
#define main original_wardrive_ui_main
#include "wardrive_ui_check.c"
#undef main
#include "keyboard.h"

#define MALLOC_CAP_SPIRAM 1U
#define MALLOC_CAP_8BIT 2U
static unsigned allocations, frees, saves;
static bool fail_alloc;
static esp_err_t settings_error, save_error;
static cerberus_geofence_config_t saved_config;
static void *heap_caps_calloc(size_t n, size_t size, unsigned caps)
{ CHECK(caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)); if (fail_alloc) return NULL; ++allocations; return calloc(n, size); }
static void heap_caps_free(void *p) { if (p) ++frees; free(p); }
static esp_err_t cerberus_settings_geofences_get(cerberus_geofence_config_t *out)
{ *out = saved_config; return settings_error; }
static esp_err_t cerberus_settings_geofences_set(const cerberus_geofence_config_t *config)
{
    ++saves; CHECK(cerberus_geofence_config_valid(config));
    if (save_error) return save_error;
    saved_config = *config; settings_error = ESP_OK; return ESP_OK;
}
#include "wardrive_geofence_ui.h"

static void tap(unsigned x, unsigned y) { wardrive_areas_handle_tap(x, y); }
static void field(unsigned y, const char *text)
{
    tap(100, y); CHECK(wd_area_ui->page == WD_AREA_INPUT);
    CHECK(cerberus_keyboard_begin(&wd_area_ui->keyboard, wd_area_ui->input,
        wd_area_ui->keyboard.capacity, text) == ESP_OK);
    tap(260, 360); CHECK(wd_area_ui->page == WD_AREA_EDIT);
}
static void editor(unsigned slot)
{ tap(100, 112 + slot * 48); CHECK(wd_area_ui->page == WD_AREA_EDIT && wd_area_ui->slot == slot); }

int main(int argc, char **argv)
{
    CHECK(argc == 2); saved_config = cerberus_geofence_defaults();
    original_wardrive_ui_main(argc, argv);
    reset();
    wardrive_handle_tap(220, 358); CHECK(wardrive_areas_is_open() && !wardrive_include_expansions);
    CHECK(strstr(drawn, "ADD AREA") && strstr(drawn, "BACK TO WARDRIVING")); save(argv[1], "area-list");
    editor(0); CHECK(wd_area_ui->coordinates == 0 && !saves);
    tap(60, 352); CHECK(!saves && strstr(drawn, "SET BOTH COORDINATES"));
    field(146, "nan"); CHECK(!wd_area_ui->coordinates && strstr(drawn, "INVALID COORDINATE"));
    field(146, "91"); CHECK(!wd_area_ui->coordinates);
    field(146, "12junk"); CHECK(!wd_area_ui->coordinates);
    field(146, "12.345678"); CHECK(wd_area_ui->coordinates == 1);
    field(186, "-181"); CHECK(wd_area_ui->coordinates == 1);
    field(186, "-42.123456"); CHECK(wd_area_ui->coordinates == 3);
    field(106, "SYNTHETIC AREA");
    for (unsigned i = 0; i < 10; ++i) tap(100, 226);
    CHECK(wd_area_ui->config.areas[0].radius_miles == 0.1);
    tap(100, 226); CHECK(wd_area_ui->config.areas[0].radius_miles == 0.2);
    save(argv[1], "area-editor");
    tap(60, 352); CHECK(saves == 1 && saved_config.areas[0].enabled && wd_area_ui->page == WD_AREA_LIST);
    editor(0); field(106, "UNSAVED"); tap(100, 450);
    CHECK(!strcmp(wd_area_ui->config.areas[0].name, "SYNTHETIC AREA") && saves == 1);
    editor(0); tap(100, 306); tap(60, 352); CHECK(!saved_config.areas[0].enabled && saves == 2);
    editor(0); tap(100, 306); save_error = ESP_FAIL; tap(60, 352);
    CHECK(saves == 3 && !saved_config.areas[0].enabled && wd_area_ui->page == WD_AREA_EDIT);
    CHECK(strstr(drawn, "SAVE FAILED")); save_error = ESP_OK; tap(60, 352); CHECK(saved_config.areas[0].enabled);
    ready_gps(); runtime.gps.latitude = -23; runtime.gps.longitude = 155;
    editor(1); CHECK(wd_area_ui->coordinates == 3 && wd_area_ui->config.areas[1].latitude == -23);
    runtime.gps.fix_age_ms = 6000; tap(100, 266); CHECK(strstr(drawn, "WAIT FOR A FRESH GPS POSITION"));
    runtime.gps.fix_age_ms = 0; tap(60, 352); CHECK(saved_config.areas[1].longitude == 155);
    editor(1); tap(220, 352); CHECK(wd_area_ui->page == WD_AREA_CLEAR); tap(100, 450);
    CHECK(saved_config.areas[1].enabled); editor(1); tap(220, 352); tap(100, 352);
    CHECK(!saved_config.areas[1].enabled && !saved_config.areas[1].name[0]);
    unsigned before = saves; runtime.state = CERBERUS_WARDRIVE_RUNNING;
    tap(100, 112); CHECK(wd_area_ui->page == WD_AREA_LIST && saves == before);
    CHECK(draw_detail_page() == ESP_OK && strstr(drawn, "STOP WARDRIVING BEFORE EDITING"));
    tap(100, 386); CHECK(wd_area_ui->page == WD_AREA_LIST);
    tap(100, 450); CHECK(!wd_area_ui && runtime.state == CERBERUS_WARDRIVE_RUNNING && !stop_requests);
    runtime.geofences_enabled = true; runtime.geofence.state = CERBERUS_GEOFENCE_INSIDE;
    runtime.geofence_phase = CERBERUS_WARDRIVE_AREA_PAUSING; strcpy(runtime.geofence_name, "SYNTHETIC AREA");
    draw(); CHECK(strstr(lines[105], "PAUSING BOTH RADIOS"));
    runtime.geofence_phase = CERBERUS_WARDRIVE_AREA_PAUSED; runtime.geofence_skipped = 27;
    draw(); CHECK(strstr(lines[105], "PAUSED / INSIDE AREA") && strstr(lines[300], "AREA 27"));
    CHECK(!strcmp(lines[324], "SYNTHETIC AREA")); save(argv[1], "area-paused");
    runtime.geofence.state = CERBERUS_GEOFENCE_WAIT_GPS; draw();
    CHECK(strstr(lines[105], "PAUSED / WAITING FOR GPS")); save(argv[1], "area-no-fix");
    runtime.geofence_phase = CERBERUS_WARDRIVE_AREA_RESUMING; draw(); CHECK(strstr(lines[105], "RESUMING BOTH RADIOS"));
    runtime.state = CERBERUS_WARDRIVE_STOPPING; draw(); CHECK(strstr(lines[105], "STOPPING / SAVING"));
    reset(); wardrive_include_expansions = true; wardrive_handle_tap(80, 390);
    CHECK(!wardrive_start_pending && wardrive_start_error == ESP_ERR_NOT_SUPPORTED && !service_starts);
    CHECK(strstr(drawn, "GEOFENCES REQUIRE R1-R6 OFF"));
    wardrive_include_expansions = false; wardrive_handle_tap(80, 390); CHECK(wardrive_start_pending);
    wardrive_handle_tap(220, 358); tap(100, 112); CHECK(wd_area_ui->page == WD_AREA_LIST);
    tap(100, 450); wardrive_handle_tap(80, 390); CHECK(!wardrive_start_pending);
    settings_error = ESP_ERR_INVALID_STATE; wardrive_handle_tap(80, 390);
    CHECK(!wardrive_start_pending && strstr(drawn, "REVIEW GEOFENCE SETTINGS"));
    wardrive_handle_tap(220, 358); CHECK(!wd_area_ui->valid && strstr(drawn, "UNREADABLE"));
    tap(100, 112); CHECK(wd_area_ui->page == WD_AREA_LIST && saves == before);
    tap(100, 386); CHECK(wd_area_ui->page == WD_AREA_RESET); save(argv[1], "area-reset");
    tap(100, 450); CHECK(settings_error == ESP_ERR_INVALID_STATE && saves == before);
    tap(100, 386); tap(100, 352); CHECK(!settings_error && wd_area_ui->valid && saves == before + 1);
    for (unsigned i = 0; i < 5; ++i) CHECK(!saved_config.areas[i].enabled && !saved_config.areas[i].name[0]);
    editor(4); tap(100, 106); CHECK(wd_area_ui->page == WD_AREA_INPUT); save(argv[1], "area-keyboard");
    tap(100, 450); CHECK(wd_area_ui->page == WD_AREA_EDIT); tap(100, 450); tap(100, 450);
    CHECK(!wd_area_ui && allocations == frees);
    fail_alloc = true; wardrive_handle_tap(220, 358);
    CHECK(!wd_area_ui && wardrive_start_error == ESP_ERR_NO_MEM); fail_alloc = false;
    wardrive_handle_tap(220, 358); wardrive_areas_release_if_hidden(true); CHECK(!wd_area_ui && allocations == frees);
    wardrive_handle_tap(220, 358); current_page = 0; wardrive_areas_release_if_hidden(false);
    CHECK(!wd_area_ui && allocations == frees);
    printf("Geofence production UI: %u checks; editing, keyboard, persistence boundaries, active-session lock, Start gating, Back and cleanup passed.\n", checks);
    return 0;
}
