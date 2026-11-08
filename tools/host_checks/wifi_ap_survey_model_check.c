#include "wifi_ap_survey_model.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); abort(); } } while (0)
#include "wifi_ap_survey_fixture.h"
#include "wifi_ap_survey_report.h"
static cerberus_ap_survey_model_t model, backup;
static cerberus_ap_survey_table_t tables[2];
static cerberus_ap_survey_page_t page;

static void stable_merge_and_ages(void)
{
    cerberus_ap_survey_model_init(&model);
    survey_fixture_begin(&tables[0], 1, 6); survey_fixture_begin(&tables[1], 4, 36);
    survey_fixture_ap(&tables[0], 1, 1000000); survey_fixture_client(&tables[0], 1, 9, 1, 2000000);
    survey_fixture_client(&tables[0], 1, 9, 2, 4000000); survey_fixture_ap(&tables[1], 1, 3000000);
    survey_fixture_publish(&model, &tables[0], 1, 7, 5000000, 10000000);
    survey_fixture_publish(&model, &tables[1], 2, 7, 6000000, 10000100);
    CHECK(cerberus_ap_survey_model_page(&model, CERBERUS_AP_SURVEY_AP, NULL, 0, &page));
    CHECK(page.total == 2 && page.rows[0].source == 1 && page.rows[1].source == 2);
    cerberus_ap_survey_record_t selected = page.rows[0];
    CHECK(cerberus_ap_survey_model_select(&model, &selected));
    survey_fixture_ap(&tables[0], 2, 6000000);
    survey_fixture_publish(&model, &tables[0], 1, 7, 7000000, 12000000);
    CHECK(cerberus_ap_survey_model_page(&model, CERBERUS_AP_SURVEY_AP, NULL, 0, &page));
    CHECK(page.total == 3 && page.rows[1].source == 2 && page.rows[2].evidence.ap.bssid[5] == 2);
    CHECK(!memcmp(&selected, &model.selected.frozen, sizeof(selected)) && model.selected.present);
    CHECK(cerberus_ap_survey_model_page(&model, CERBERUS_AP_SURVEY_RELATION, &selected, 0, &page));
    CHECK(page.total == 1 && page.rows[0].evidence.client.row.to_ap_frames == 1);
    uint64_t uplink, downlink;
    CHECK(cerberus_ap_survey_record_age(&page.rows[0], 1, 13000000, &uplink));
    CHECK(cerberus_ap_survey_record_age(&page.rows[0], 2, 13000000, &downlink));
    CHECK(uplink == 6000 && downlink == 4000);
    CHECK(!cerberus_ap_survey_record_age(&page.rows[0], 0, 13000000, &downlink));
    CHECK(!cerberus_ap_survey_record_age(&page.rows[0], 1, 1, &downlink));
    survey_fixture_publish(&model, &tables[0], 1, 7, 8000000, 13000000);
    CHECK(cerberus_ap_survey_model_page(&model, CERBERUS_AP_SURVEY_RELATION, &selected, 0, &page));
    CHECK(cerberus_ap_survey_record_age(&page.rows[0], 1, 14000000, &uplink) && uplink == 7000);
    CHECK(!memcmp(&selected, &model.selected.frozen, sizeof(selected)));
    cerberus_ap_survey_model_unavailable(&model, 1, true);
    CHECK(model.selected.unavailable && model.selected.present);
    cerberus_ap_survey_model_unavailable(&model, 1, false); CHECK(!model.selected.unavailable);
    CHECK(cerberus_ap_survey_model_page(&model, CERBERUS_AP_SURVEY_RELATION, &selected, 4, &page) && !page.copied);
}

static void replacement_and_restarts(void)
{
    cerberus_ap_survey_model_init(&model); survey_fixture_begin(&tables[0], 1, 6);
    for (unsigned i = 1; i <= 32; ++i) survey_fixture_ap(&tables[0], i, i * 1000U);
    survey_fixture_client(&tables[0], 1, 1, 1, 33000);
    survey_fixture_publish(&model, &tables[0], 1, 1, 40000, 40000);
    CHECK(cerberus_ap_survey_model_page(&model, CERBERUS_AP_SURVEY_AP, NULL, 0, &page));
    cerberus_ap_survey_record_t frozen = page.rows[0];
    CHECK(cerberus_ap_survey_model_select(&model, &frozen));
    survey_fixture_ap(&tables[0], 33, 50000);
    survey_fixture_client(&tables[0], 33, 2, 1, 51000);
    survey_fixture_publish(&model, &tables[0], 1, 1, 60000, 60000);
    CHECK(!model.selected.present && !memcmp(&model.selected.latest, &frozen, sizeof(frozen)));
    CHECK(cerberus_ap_survey_model_page(&model, CERBERUS_AP_SURVEY_RELATION, &frozen, 0, &page));
    CHECK(page.total == 1 && page.copied == 1 && page.rows[0].evidence.client.row.bssid[5] == 1);
    CHECK(!memcmp(&model.selected.frozen, &frozen, sizeof(frozen)));
    CHECK(cerberus_ap_survey_model_page(&model, CERBERUS_AP_SURVEY_AP, NULL, 28, &page));
    CHECK(page.total == 32 && page.copied == 4 && page.rows[3].evidence.ap.bssid[5] == 33);
    survey_fixture_ap(&tables[0], 1, 70000);
    survey_fixture_publish(&model, &tables[0], 1, 1, 80000, 80000);
    CHECK(!model.selected.present); /* Same address with a new incarnation is different evidence. */
    CHECK(cerberus_ap_survey_model_select(&model, &frozen) && !model.selected.present);
    survey_fixture_begin(&tables[0], 1, 6); survey_fixture_ap(&tables[0], 1, 1000);
    survey_fixture_publish(&model, &tables[0], 1, 2, 2000, 90000);
    CHECK(!model.selected.present && model.selected.frozen.run_id == 1);
    CHECK(cerberus_ap_survey_model_page(&model, CERBERUS_AP_SURVEY_AP, NULL, 0, &page));
    CHECK(page.total == 1 && page.rows[0].run_id == 2);
    CHECK(cerberus_ap_survey_model_select(&model, &page.rows[0]) && model.selected.present);
    cerberus_ap_survey_model_clear_selection(&model); CHECK(!model.selected.valid);
}

static void rejected_snapshots(void)
{
    cerberus_ap_survey_model_init(&model); survey_fixture_begin(&tables[0], 1, 6);
    survey_fixture_ap(&tables[0], 1, 1000); survey_fixture_publish(&model, &tables[0], 1, 1, 2000, 3000);
    cerberus_ap_survey_snapshot_t snapshot = model.banks[0].snapshot;
    backup = model;
    CHECK(cerberus_ap_survey_model_publish(&model, 0, 1, 4000, 4100, &snapshot) == CERBERUS_AP_SURVEY_MODEL_INVALID);
    CHECK(cerberus_ap_survey_model_publish(&model, 1, 1, 4000, 3900, &snapshot) == CERBERUS_AP_SURVEY_MODEL_INVALID);
    CHECK(cerberus_ap_survey_model_publish(&model, 1, 1, 2999, 4000, &snapshot) == CERBERUS_AP_SURVEY_MODEL_STALE);
    ++snapshot.session_id;
    CHECK(cerberus_ap_survey_model_publish(&model, 1, 1, 4000, 4100, &snapshot) == CERBERUS_AP_SURVEY_MODEL_INVALID);
    snapshot = backup.banks[0].snapshot; ++snapshot.aps[0].bssid[5]; ++snapshot.revision;
    CHECK(cerberus_ap_survey_snapshot_valid(&snapshot));
    CHECK(cerberus_ap_survey_model_publish(&model, 1, 1, 4000, 4100, &snapshot) == CERBERUS_AP_SURVEY_MODEL_INVALID);
    snapshot = backup.banks[0].snapshot; --snapshot.aps[0].rssi;
    CHECK(cerberus_ap_survey_model_publish(&model, 1, 1, 4000, 4100, &snapshot) == CERBERUS_AP_SURVEY_MODEL_INVALID);
    CHECK(!memcmp(&model, &backup, sizeof(model)));
    CHECK(cerberus_ap_survey_model_page(&model, CERBERUS_AP_SURVEY_AP, NULL, 0, &page));
    --page.rows[0].evidence.ap.rssi; CHECK(!cerberus_ap_survey_model_select(&model, &page.rows[0]));
    CHECK(!memcmp(&model, &backup, sizeof(model)));
    model.next_order = UINT64_MAX; backup = model;
    survey_fixture_ap(&tables[0], 2, 3000); CHECK(cerberus_ap_survey_snapshot(&tables[0], 4000, &snapshot));
    CHECK(cerberus_ap_survey_model_publish(&model, 1, 1, 5000, 5100, &snapshot) == CERBERUS_AP_SURVEY_MODEL_INVALID);
    CHECK(!memcmp(&model, &backup, sizeof(model)));
}

static void report_shape(void)
{
    static char report[CERBERUS_AP_SURVEY_REPORT_CAPACITY];
    cerberus_ap_survey_model_init(&model);
    for (unsigned source = 0; source < 2; ++source) {
        survey_fixture_begin(&tables[source], source + 1, source ? 36 : 6);
        for (unsigned id = 1; id <= 32; ++id) {
            survey_fixture_ap(&tables[source], id, id * 1000U);
            survey_fixture_client(&tables[source], id, id, true, id * 1000U + 1U);
        }
        tables[source].live.aps[0].ssid_length = 4;
        memcpy(tables[source].live.aps[0].ssid, "\0\r,\xff", 4);
        memset(tables[source].live.aps[0].ssid + 4, 0, 28);
        survey_fixture_publish(&model, &tables[source], source + 1, 1, 40000, 50000);
    }
    size_t length = cerberus_ap_survey_report(&model, report, sizeof(report));
    CHECK(length && length < sizeof(report));
    CHECK(strstr(report, ",000D2CFF,4,") != NULL);
    unsigned rows = 0;
    for (char *line = report; *line;) {
        char *end = strchr(line, '\n'); CHECK(end != NULL);
        if (*line != '#') {
            unsigned columns = 1;
            for (char *byte = line; byte < end; ++byte) columns += *byte == ',';
            CHECK(columns == 23); ++rows;
        }
        line = end + 1;
    }
    CHECK(rows == 129);
    CHECK(!cerberus_ap_survey_report(&model, report, 16));
}

int main(void)
{
    stable_merge_and_ages(); replacement_and_restarts(); rejected_snapshots();
    report_shape();
    printf("AP/client survey model: %u checks; model=%zu record=%zu page=%zu bytes\n",
        checks, sizeof(model), sizeof(cerberus_ap_survey_record_t), sizeof(page));
    return 0;
}
