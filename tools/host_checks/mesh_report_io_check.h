/* Actual observation model/report -> immutable serialized archive worker.
 * The storage fixture separately exercises the real filesystem writer. */
#include "mesh_report_fixture.h"
static mesh_scan_t mesh_input, mesh_expected;
static char mesh_expected_bytes[MESH_REPORT_MAX_BYTES + 1U];
static size_t mesh_expected_length;
static void fill_mesh(unsigned count)
{
    CHECK(mesh_report_test_fill(&mesh_input, count));
    CHECK(mesh_report_valid(&mesh_input));
    mesh_expected = mesh_input;
    CHECK(mesh_report_encode(&mesh_expected, mesh_expected_bytes,
        sizeof(mesh_expected_bytes), &mesh_expected_length));
    CHECK(mesh_expected_length && mesh_expected_length <= MESH_REPORT_MAX_BYTES);
}
static uint64_t start_mesh(void)
{
    uint64_t id = 0;
    CHECK(cerberus_ble_archive_io_save_mesh(&mesh_input, &id) == ESP_OK && id > last_job);
    last_job = id;
    CHECK(status().operation == CERBERUS_BLE_ARCHIVE_IO_SAVE_MESH &&
        status().format == CERBERUS_BLE_ARCHIVE_IO_FORMAT_MESH_REPORT);
    CHECK(!strcmp(status().relative_path, "/CERBERUS/LOGS"));
    CHECK(allocation_size == offsetof(ble_archive_io_context_t, payload) +
        sizeof(archive_io_context->payload.mesh));
    CHECK(sizeof(archive_io_context->payload.mesh.bytes) == MESH_REPORT_MAX_BYTES + 1U);
    CHECK(!memcmp(&archive_io_context->payload.mesh.snapshot, &mesh_expected, sizeof(mesh_expected)));
    return id;
}
static void check_mesh_operations(void)
{
    const unsigned counts[] = {0, 1, 16, 64, 80};
    for (unsigned n = 0; n < sizeof(counts) / sizeof(counts[0]); ++n) {
        reset(); fill_mesh(counts[n]);
        uint64_t id = start_mesh();
        CHECK(!mesh_saves && !mesh_formats); check_probe_no_typed_copy(id);
        memset(&mesh_input, 0xA5, sizeof(mesh_input)); run_worker();
        CHECK(status().state == CERBERUS_BLE_ARCHIVE_IO_COMPLETE && status().result_ready);
        CHECK(status().count == mesh_expected.model.address_count &&
            status().bytes == mesh_expected_length && mesh_length == mesh_expected_length);
        CHECK(mesh_saves == 1 && mesh_formats == 1 &&
            !memcmp(mesh_bytes, mesh_expected_bytes, mesh_length));
        CHECK(!strcmp(status().file_name, "M00001.CSV"));
        CHECK(!saves && !loads && !json_saves && !json_loads && !wifi_saves && !wifi_loads &&
            !ap_json_saves && !ap_json_loads && !ssid_text_saves && !ssid_text_loads && !probe_saves);
        check_probe_no_typed_copy(id); console_check(CERBERUS_BLE_ARCHIVE_IO_COMPLETE);
        CHECK(strstr(console_text, "ble_archive_format=MESH_REPORT\r\n"));
    }
    /* Cancellation before an I/O call skips storage. Once a synchronous save
     * starts, publish its actual result without pretending cancellation undid it. */
    for (unsigned phase = 0; phase < 3; ++phase) for (unsigned release = 0; release < 2; ++release) {
        reset(); fill_mesh(80); uint64_t id = start_mesh();
        if (!phase) { if (release) cerberus_ble_archive_io_release(id); else cerberus_ble_archive_io_cancel(id); }
        else if (phase == 1) mesh_format_hook = release ? release_hook : cancel_hook;
        else storage_hook = release ? release_hook : cancel_hook;
        run_worker(); CHECK(status().cancel_requested);
        CHECK(status().state == (phase == 2 ? CERBERUS_BLE_ARCHIVE_IO_COMPLETE : CERBERUS_BLE_ARCHIVE_IO_CANCELED));
        CHECK(mesh_saves == (phase == 2 ? 1U : 0U) && mesh_formats == (phase == 0 ? 0U : 1U));
        CHECK(status().result_ready == (phase == 2 && !release));
        cerberus_ble_archive_io_release(id); CHECK(!outstanding);
    }
    for (unsigned failure = 0; failure < 5; ++failure) {
        reset(); fill_mesh(1);
        if (failure == 0) free_internal = 24575;
        if (failure == 1) largest_internal = 8191;
        if (failure == 2) fail_alloc = true;
        if (failure == 3) fail_task = true;
        if (failure == 4) { release_in_alloc = true; immediate_task = true; }
        uint64_t id = 0; esp_err_t result = cerberus_ble_archive_io_save_mesh(&mesh_input, &id);
        CHECK(id > last_job); last_job = id;
        CHECK(result == (failure == 4 ? ESP_OK : ESP_ERR_NO_MEM));
        CHECK(status().state == (failure == 4 ? CERBERUS_BLE_ARCHIVE_IO_CANCELED : CERBERUS_BLE_ARCHIVE_IO_ERROR));
        CHECK(!pending_task && !outstanding && !mesh_saves);
    }
    reset(); fill_mesh(16); storage_error = ESP_FAIL; partial_save = true;
    uint64_t id = start_mesh(); run_worker();
    CHECK(status().state == CERBERUS_BLE_ARCHIVE_IO_ERROR && status().error == ESP_FAIL &&
        !status().bytes && !status().result_ready && !outstanding);
    CHECK(!strcmp(status().file_name, "M00001.TMP")); check_probe_no_typed_copy(id);
    reset(); fill_mesh(1); id = start_mesh();
    archive_io_context->payload.mesh.snapshot.frozen = false;
    run_worker();
    CHECK(status().state == CERBERUS_BLE_ARCHIVE_IO_ERROR && status().error == ESP_ERR_INVALID_RESPONSE);
    CHECK(!mesh_saves && !outstanding);
    for (unsigned fault = 0; fault < 7; ++fault) {
        reset(); fill_mesh(1);
        if (fault == 0) mesh_input.frozen = false;
        if (fault == 1) mesh_input.final_valid = false;
        if (fault == 2) mesh_input.faulted = true;
        if (fault == 3) mesh_input.request.session_id = mesh_input.model.session;
        if (fault == 4) mesh_input.model.address_count = MESH_OBSERVATION_ADDRESSES + 1U;
        if (fault == 5) ++mesh_input.final_summary.acked;
        if (fault == 6) mesh_input.final_summary.flags &= (uint8_t)~CERBERUS_MESH_OBSERVATION_STOPPED;
        unsigned before = allocations; uint64_t prior = archive_io_last_job; id = 99;
        CHECK(cerberus_ble_archive_io_save_mesh(&mesh_input, &id) == ESP_ERR_INVALID_ARG && !id);
        CHECK(allocations == before && archive_io_last_job == prior && !pending_task && !mesh_saves);
    }
    reset(); fill_mesh(1); id = 99;
    CHECK(cerberus_ble_archive_io_save_mesh(NULL, &id) == ESP_ERR_INVALID_ARG && !id);
    CHECK(cerberus_ble_archive_io_save_mesh(&mesh_input, NULL) == ESP_ERR_INVALID_ARG);
    id = start_mesh(); uint64_t stale = id; run_worker(); cerberus_ble_archive_io_release(id);
    id = start_mesh(); cerberus_ble_archive_io_cancel(stale); cerberus_ble_archive_io_release(stale);
    CHECK(!status().cancel_requested && outstanding == 1); run_worker();
    CHECK(status().state == CERBERUS_BLE_ARCHIVE_IO_COMPLETE);
    reset(); fill_mesh(0); immediate_task = true;
    id = start_mesh(); CHECK(!pending_task && status().state == CERBERUS_BLE_ARCHIVE_IO_COMPLETE);
    reset(); fill_mesh(80);
    printf("Mesh report worker allocation=%zu; frozen scan=%zu; CSV cap=%u plus NUL.\n",
        archive_io_allocation_size(CERBERUS_BLE_ARCHIVE_IO_SAVE_MESH), sizeof(mesh_input), MESH_REPORT_MAX_BYTES);
}
