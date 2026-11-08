#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "wifi_ap_survey_ui.h"
#include "display_font.h"

static uint16_t pixels[480][320], reference[480][320];
static unsigned calls, text_calls, fail_at;
static char drawn[16384];
static cerberus_ap_survey_ui_cache_t cache, reference_cache;
static cerberus_ap_survey_model_t inventory;
static cerberus_ap_survey_ui_view_t view;
static const char *prefix;
static unsigned checks;
#define CHECK(condition) do { ++checks; if (!(condition)) { \
    fprintf(stderr,"CHECK failed at %s:%d: %s\n",__FILE__,__LINE__,#condition); abort(); } } while (0)

esp_err_t cerberus_display_fill_rect(uint16_t x, uint16_t y, uint16_t width,
    uint16_t height, uint16_t color)
{
    CHECK((unsigned)x+width<=320U && (unsigned)y+height<=480U);
    if (++calls==fail_at) return ESP_FAIL;
    for(unsigned iy=y;iy<(unsigned)y+height;++iy)
        for(unsigned ix=x;ix<(unsigned)x+width;++ix) pixels[iy][ix]=color;
    return ESP_OK;
}

esp_err_t cerberus_display_draw_text(uint16_t x,uint16_t y,const char *text,
    uint16_t fg,uint16_t bg,uint8_t scale)
{
    CHECK(scale==1U || scale==2U);
    CHECK((unsigned)x+strlen(text)*6U*scale<=320U && (unsigned)y+7U*scale<=480U);
    if(++calls==fail_at) return ESP_FAIL;
    ++text_calls;
    size_t used=strlen(drawn);
    CHECK(used+strlen(text)+2U<sizeof(drawn));
    snprintf(drawn+used,sizeof(drawn)-used,"%s\n",text);
    for(size_t i=0;text[i];++i){
        const uint8_t *font=font5x7((unsigned char)text[i]);
        for(unsigned cy=0;cy<7;++cy)for(unsigned cx=0;cx<6;++cx)
            for(unsigned sy=0;sy<scale;++sy)for(unsigned sx=0;sx<scale;++sx)
                pixels[y+cy*scale+sy][x+(i*6U+cx)*scale+sx]=
                    cx<5U && (font[cy]&(1U<<(4U-cx)))?fg:bg;
    }
    return ESP_OK;
}

static void save(const char *name)
{
    char path[1024];snprintf(path,sizeof(path),"%s-%s.ppm",prefix,name);
    FILE *file=fopen(path,"wb");CHECK(file!=NULL);
    CHECK(fprintf(file,"P6\n320 480\n255\n")>0);
    for(unsigned y=0;y<480;++y)for(unsigned x=0;x<320;++x){
        uint16_t p=pixels[y][x];
        uint8_t rgb[3]={(uint8_t)(((p>>11)&31U)*255U/31U),
            (uint8_t)(((p>>5)&63U)*255U/63U),(uint8_t)((p&31U)*255U/31U)};
        if(fwrite(rgb,1,3,file)!=3U){fprintf(stderr,"Cannot write preview %s\n",path);abort();}
    }
    CHECK(fclose(file)==0);
}


#include "wifi_ap_survey_fixture.h"
static cerberus_ap_survey_table_t tables[2];

static void reset(void)
{
    memset(&cache, 0, sizeof(cache)); memset(&view, 0, sizeof(view));
    memset(pixels, 0xA5, sizeof(pixels)); calls = text_calls = fail_at = 0; drawn[0] = 0;
    cerberus_ap_survey_model_init(&inventory);
    view.start = view.back = true; view.page.kind = CERBERUS_AP_SURVEY_AP;
    view.receivers[0].state = view.receivers[1].state = "READY";
    view.back_label = "BACK TO SCAN";
}

static void page(uint8_t kind, const cerberus_ap_survey_record_t *filter, size_t offset)
{
    CHECK(cerberus_ap_survey_model_page(&inventory, kind, filter, offset, &view.page));
    view.ap_count = inventory.banks[0].snapshot.ap_count + inventory.banks[1].snapshot.ap_count;
    view.client_count = inventory.banks[0].snapshot.relation_count + inventory.banks[1].snapshot.relation_count;
    view.selection = inventory.selected;
}

static void update(bool force)
{ drawn[0] = 0; CHECK(cerberus_ap_survey_ui_update(&cache, &view, force) == ESP_OK); }

static void equals_full_render(void)
{
    memcpy(reference, pixels, sizeof(reference)); memset(&reference_cache, 0, sizeof(reference_cache));
    CHECK(cerberus_ap_survey_ui_update(&reference_cache, &view, true) == ESP_OK);
    CHECK(!memcmp(reference, pixels, sizeof(reference)));
}

static void action(unsigned x, unsigned y, cerberus_ap_survey_ui_action_t expected)
{ CHECK(cerberus_ap_survey_ui_tap(&cache, (uint16_t)x, (uint16_t)y).action == expected); }

static void controls(void)
{
    reset(); update(true); save("empty");
    CHECK(strstr(drawn, "NO AP OBSERVATIONS") && strstr(drawn, "COVERAGE S3 UNKNOWN"));
    action(8, 432, CERBERUS_AP_SURVEY_UI_BACK); action(155, 473, CERBERUS_AP_SURVEY_UI_BACK);
    action(164, 432, CERBERUS_AP_SURVEY_UI_START); action(311, 473, CERBERUS_AP_SURVEY_UI_START);
    action(7, 432, CERBERUS_AP_SURVEY_UI_NONE); action(156, 440, CERBERUS_AP_SURVEY_UI_NONE);
    action(164, 431, CERBERUS_AP_SURVEY_UI_NONE); action(311, 474, CERBERUS_AP_SURVEY_UI_NONE);
    action(8, 124, CERBERUS_AP_SURVEY_UI_APS); action(155, 157, CERBERUS_AP_SURVEY_UI_APS);
    action(164, 124, CERBERUS_AP_SURVEY_UI_CLIENTS); action(311, 157, CERBERUS_AP_SURVEY_UI_CLIENTS);
    action(156, 124, CERBERUS_AP_SURVEY_UI_NONE); action(164, 158, CERBERUS_AP_SURVEY_UI_NONE);
    action(20, 180, CERBERUS_AP_SURVEY_UI_NONE); action(270, 410, CERBERUS_AP_SURVEY_UI_NONE);
    unsigned before = calls; update(false); CHECK(calls == before);
    view.start = false; view.stop = true; view.receivers[0].state = "STOPPING";
    view.receivers[1].state = "LINK ERROR"; view.message = "STOPPING / C5 CLEANUP NOT CONFIRMED";
    update(false); equals_full_render(); action(200, 450, CERBERUS_AP_SURVEY_UI_STOP); save("cleanup");
    view.back = false; update(false); action(20, 450, CERBERUS_AP_SURVEY_UI_NONE);
}

static void merged_pages_and_details(void)
{
    reset(); survey_fixture_begin(&tables[0], 7, 6); survey_fixture_begin(&tables[1], 8, 36);
    for (unsigned source = 0; source < 2; ++source) {
        for (unsigned i = 1; i <= 4; ++i) survey_fixture_ap(&tables[source], i, i * 1000000U);
        survey_fixture_client(&tables[source], 1, 1, 1, 5000000);
        survey_fixture_client(&tables[source], 1, 1, 2, 7000000);
        survey_fixture_publish(&inventory, &tables[source], (uint8_t)(source + 1U), 1, 8000000, 9000000);
        view.receivers[source] = (cerberus_ap_survey_ui_receiver_t){.state = "RECEIVING", .passes = 9,
            .channel = source ? 36 : 6, .requested_channels = source ? UINT64_C(3) << 14U : 3,
            .applied_channels = source ? UINT64_C(1) << 14U : 1, .coverage_valid = true};
    }
    view.now_us = 10000000; view.start = false; view.stop = true;
    page(CERBERUS_AP_SURVEY_AP, NULL, 0); update(true); save("ap-list");
    CHECK(strstr(drawn, "APS 8 / OBSERVED CLIENT LINKS 2") && strstr(drawn, "COVERAGE S3 1/2 / C5 1/2"));
    action(8, 170, CERBERUS_AP_SURVEY_UI_SELECT); action(311, 215, CERBERUS_AP_SURVEY_UI_SELECT);
    action(311, 216, CERBERUS_AP_SURVEY_UI_NONE); action(311, 371, CERBERUS_AP_SURVEY_UI_SELECT);
    action(311, 372, CERBERUS_AP_SURVEY_UI_NONE);
    action(106, 398, CERBERUS_AP_SURVEY_UI_NEXT); action(195, 423, CERBERUS_AP_SURVEY_UI_NEXT);
    cerberus_ap_survey_ui_event_t painted = cerberus_ap_survey_ui_tap(&cache, 20, 180);
    CHECK(cerberus_ap_survey_model_select(&inventory, &painted.record));
    cerberus_ap_survey_record_t frozen = inventory.selected.frozen;
    view.selection = inventory.selected; view.detail = true; view.selected_client_count = 1;
    view.back_label = "BACK TO APS"; update(false); equals_full_render(); save("ap-detail");
    CHECK(strstr(drawn, "BSSID 02:01:02:03:04:01") && strstr(drawn, "1 CLIENT LINKS"));
    action(8, 398, CERBERUS_AP_SURVEY_UI_AP_CLIENTS); action(311, 423, CERBERUS_AP_SURVEY_UI_AP_CLIENTS);
    action(311, 424, CERBERUS_AP_SURVEY_UI_NONE);
    cerberus_ap_survey_ui_event_t filter = cerberus_ap_survey_ui_tap(&cache, 100, 410);
    CHECK(!memcmp(&filter.record, &frozen, sizeof(frozen)));
    view.detail = false; page(CERBERUS_AP_SURVEY_RELATION, &filter.record, 0);
    update(false); equals_full_render(); save("filtered-clients");
    CHECK(view.page.total == 1 && strstr(drawn, "THIS AP'S CLIENTS"));
    painted = cerberus_ap_survey_ui_tap(&cache, 20, 180);
    CHECK(cerberus_ap_survey_model_select(&inventory, &painted.record));
    view.selection = inventory.selected; view.detail = true; view.back_label = "BACK TO CLIENTS";
    update(false); equals_full_render(); save("client-directions");
    CHECK(strstr(drawn, "STATION SIGNAL -77 DBM") && strstr(drawn, "AP SIGNAL -31 DBM"));
    CHECK(!strcmp(cache.painted.details[5], "CALLBACK AGE <=4 S"));
    CHECK(!strcmp(cache.painted.details[8], "CALLBACK AGE <=2 S"));
    action(100, 410, CERBERUS_AP_SURVEY_UI_NONE);
    view.now_us += 1000000; update(false); equals_full_render();
    CHECK(!strcmp(cache.painted.details[5], "CALLBACK AGE <=5 S"));
    view.private_values = true; update(false); equals_full_render(); save("private-client");
    CHECK(!strstr(drawn, "02:01:02") && !strstr(drawn, "02:05:06"));
    view.detail = false; view.private_values = false; page(CERBERUS_AP_SURVEY_AP, NULL, 4);
    update(false); equals_full_render(); CHECK(view.page.rows[0].source == 2);
    action(20, 410, CERBERUS_AP_SURVEY_UI_PREVIOUS); action(150, 410, CERBERUS_AP_SURVEY_UI_NONE);
    view.stop = false; view.start = view.save = true; update(false);
    action(204, 398, CERBERUS_AP_SURVEY_UI_SAVE); action(311, 423, CERBERUS_AP_SURVEY_UI_SAVE);
    action(203, 398, CERBERUS_AP_SURVEY_UI_NONE); action(204, 424, CERBERUS_AP_SURVEY_UI_NONE);
    /* Equal visible text does not permit an incarnation change without paint. */
    unsigned before = calls; ++view.page.rows[0].evidence.ap.incarnation;
    fail_at = calls + 1;
    CHECK(cerberus_ap_survey_ui_update(&cache, &view, false) == ESP_FAIL && !cache.valid && calls > before);
    fail_at = 0; update(false); equals_full_render();
    /* Painted row identity survives a replacement before dispatch. */
    frozen = cache.rows[0]; inventory.banks[1].snapshot.aps[0].bssid[5] = 99;
    painted = cerberus_ap_survey_ui_tap(&cache, 20, 180);
    CHECK(!memcmp(&painted.record, &frozen, sizeof(frozen)));
    fail_at = calls + 1; CHECK(cerberus_ap_survey_ui_update(&cache, &view, true) == ESP_FAIL && !cache.valid);
    action(20, 180, CERBERUS_AP_SURVEY_UI_NONE); fail_at = 0; update(false); equals_full_render();
}

static void binary_and_maximum_fields(void)
{
    reset(); survey_fixture_begin(&tables[0], 1, 14); survey_fixture_ap(&tables[0], 1, 0);
    memset(tables[0].live.aps[0].ssid, 0xFF, 32); tables[0].live.aps[0].ssid_length = 32;
    tables[0].live.aps[0].frames = tables[0].live.ap_frames = tables[0].live.received_frames = UINT32_MAX;
    tables[0].live.aps[0].capabilities = UINT16_MAX;
    survey_fixture_publish(&inventory, &tables[0], 1, 1, 1, 0);
    page(CERBERUS_AP_SURVEY_AP, NULL, 0); update(true);
    CHECK(cerberus_ap_survey_model_select(&inventory, &cache.rows[0]));
    view.selection = inventory.selected; view.detail = true; view.now_us = UINT64_MAX;
    view.receivers[0].passes = view.receivers[1].passes = UINT32_MAX;
    view.receivers[0].state = view.receivers[1].state = "RELEASE REQUIRED";
    update(false); equals_full_render(); save("binary-maxima");
    char escaped[130]; snprintf(escaped, sizeof(escaped), "%s%s%s", cache.painted.details[1], cache.painted.details[2], cache.painted.details[3]);
    CHECK(strlen(escaped) == 128 && !memcmp(escaped, "\\xFF\\xFF", 8));
    CHECK(strstr(drawn, "4294967295") && strstr(drawn, "0xFFFF"));
    view.private_values = true; update(false); CHECK(strstr(drawn, "[NAME HIDDEN]") && !strstr(drawn, "\\xFF"));
    view.selection.present = false; view.selection.unavailable = true; update(false); save("retained-unavailable");
    CHECK(strstr(drawn, "RECEIVER UNAVAILABLE / RETAINED RECORD"));
}

int main(int argc, char **argv)
{
    CHECK(argc == 2); prefix = argv[1];
    controls(); merged_pages_and_details(); binary_and_maximum_fields();
    printf("AP/client survey production renderer: %u checks; cache=%zu view=%zu bytes\n", checks, sizeof(cache), sizeof(view));
    return 0;
}
