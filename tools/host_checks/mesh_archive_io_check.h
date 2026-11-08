static mesh_archive_t map_input, map_expected, map_output, map_sentinel, map_nested;
static uint8_t map_expected_bytes[MESH_ARCHIVE_MAX_BYTES];
static size_t map_expected_length;
static void fill_map(unsigned records)
{
    CHECK(mesh_report_test_fill(&mesh_input, records));
    CHECK(mesh_archive_from_scan(&mesh_input, &map_input) && mesh_archive_valid(&map_input));
    map_expected = map_input;
    CHECK(mesh_archive_encode(&map_input, map_expected_bytes, sizeof(map_expected_bytes), &map_expected_length));
    memset(&map_output, 0xA5, sizeof(map_output)); map_sentinel = map_output;
}
static uint64_t start_map(unsigned operation)
{
    uint64_t id = 0U;
    esp_err_t error = operation == 0U ? cerberus_ble_archive_io_save_mesh_archive(&map_input, &id) :
        operation == 1U ? cerberus_ble_archive_io_load_mesh_archive("/COPIES", "frozen.MSH", &id) :
        cerberus_ble_archive_io_export_mesh_archive_csv(&map_input, &id);
    CHECK(error == ESP_OK && id > last_job); last_job = id;
    size_t payload_size = operation == 0U ? sizeof(archive_io_context->payload.mesh_archive) :
        operation == 1U ? sizeof(archive_io_context->payload.mesh_archive_load) :
        sizeof(archive_io_context->payload.mesh_archive_csv);
    CHECK(allocation_size == offsetof(ble_archive_io_context_t, payload) + payload_size);
    CHECK(status().operation == (operation == 0U ? CERBERUS_BLE_ARCHIVE_IO_SAVE_MESH_ARCHIVE :
        operation == 1U ? CERBERUS_BLE_ARCHIVE_IO_LOAD_MESH_ARCHIVE : CERBERUS_BLE_ARCHIVE_IO_EXPORT_MESH_ARCHIVE_CSV));
    CHECK(status().format == (operation == 2U ? CERBERUS_BLE_ARCHIVE_IO_FORMAT_MESH_REPORT :
        CERBERUS_BLE_ARCHIVE_IO_FORMAT_MESH_ARCHIVE));
    CHECK(!strcmp(status().relative_path, operation == 0U ? "/CERBERUS/MESH" : operation == 1U ? "/COPIES" : "/CERBERUS/LOGS"));
    CHECK(cerberus_ble_archive_io_copy_mesh_archive(id, &map_output) == ESP_ERR_INVALID_STATE &&
        !memcmp(&map_output, &map_sentinel, sizeof(map_output)));
    return id;
}
static void map_pin_release(void)
{
    uint64_t id = status().job_id, rejected = 99U;
    CHECK(cerberus_ble_archive_io_copy_mesh_archive(id, &map_nested) == ESP_OK &&
        !memcmp(&map_nested, &map_expected, sizeof(map_nested)));
    cerberus_ble_archive_io_release(id);
    CHECK(outstanding == 1U && !status().result_ready);
    CHECK(cerberus_ble_archive_io_save_mesh_archive(&map_expected, &rejected) == ESP_ERR_INVALID_STATE && rejected == 0U);
}
static void seed_map_load(void)
{ memcpy(map_bytes, map_expected_bytes, map_expected_length); map_length = map_expected_length; }
static void check_mesh_archive_operations(void)
{
    const unsigned counts[] = {0U, 1U, 16U, 64U, 80U};
    for (unsigned n = 0; n < sizeof(counts) / sizeof(counts[0]); ++n) {
        reset(); fill_map(counts[n]); uint64_t id = start_map(0U);
        memset(&map_input, 0xCC, sizeof(map_input)); run_worker();
        CHECK(status().state == CERBERUS_BLE_ARCHIVE_IO_COMPLETE && status().result_ready &&
            status().bytes == map_expected_length && status().count == map_expected.model.address_count);
        CHECK(map_saves == 1U && map_encodes == 1U && !map_loads && !map_decodes && !map_exports);
        CHECK(!strcmp(status().file_name, "M00001.MSH") && map_length == map_expected_length &&
            !memcmp(map_bytes, map_expected_bytes, map_length));
        console_check(CERBERUS_BLE_ARCHIVE_IO_COMPLETE);
        CHECK(strstr(console_text, "ble_archive_format=MESH_ARCHIVE\r\n"));
        CHECK(cerberus_ble_archive_io_copy_mesh_archive(id, &map_output) == ESP_OK &&
            !memcmp(&map_output, &map_expected, sizeof(map_output)));
        memset(&output, 0xA5, sizeof(output)); untouched = output;
        CHECK(cerberus_ble_archive_io_copy_result(id, &output) == ESP_ERR_INVALID_STATE && !memcmp(&output, &untouched, sizeof(output)));
        memset(&wifi_output, 0xA5, sizeof(wifi_output)); wifi_untouched = wifi_output;
        CHECK(cerberus_ble_archive_io_copy_wifi(id, &wifi_output) == ESP_ERR_INVALID_STATE &&
            !memcmp(&wifi_output, &wifi_untouched, sizeof(wifi_output)));
        CHECK(!saves && !loads && !json_saves && !wifi_saves && !ap_json_saves && !ssid_text_saves && !mesh_saves);
        cerberus_ble_archive_io_release(id); CHECK(!outstanding);

        reset(); fill_map(counts[n]); seed_map_load(); id = start_map(1U); run_worker();
        CHECK(status().state == CERBERUS_BLE_ARCHIVE_IO_COMPLETE && status().result_ready && map_loads == 1U && map_decodes == 1U);
        CHECK(!strcmp(last_path, "/COPIES") && !strcmp(last_name, "frozen.MSH"));
        CHECK(cerberus_ble_archive_io_copy_mesh_archive(id, &map_output) == ESP_OK &&
            !memcmp(&map_output, &map_expected, sizeof(map_output)));
        copy_hook = map_pin_release;
        CHECK(cerberus_ble_archive_io_copy_mesh_archive(id, &map_output) == ESP_OK && !outstanding &&
            !memcmp(&map_output, &map_expected, sizeof(map_output)));
        CHECK(cerberus_ble_archive_io_copy_mesh_archive(id, &map_output) == ESP_ERR_INVALID_STATE);

        reset(); fill_map(counts[n]);
        CHECK(mesh_archive_report_encode(&map_input, mesh_expected_bytes, sizeof(mesh_expected_bytes), &mesh_expected_length));
        id = start_map(2U); memset(&map_input, 0xCC, sizeof(map_input)); run_worker();
        CHECK(status().state == CERBERUS_BLE_ARCHIVE_IO_COMPLETE && mesh_saves == 1U && map_exports == 1U &&
            !map_saves && !map_loads && !map_encodes && !map_decodes);
        CHECK(mesh_length == mesh_expected_length && !memcmp(mesh_bytes, mesh_expected_bytes, mesh_length));
        CHECK(cerberus_ble_archive_io_copy_mesh_archive(id, &map_output) == ESP_ERR_INVALID_STATE &&
            !memcmp(&map_output, &map_sentinel, sizeof(map_output)));
    }
    /* Actual cancellation decisions at each irreversible/atomic boundary. */
    for (unsigned operation = 0; operation < 3U; ++operation)
    for (unsigned stage = 0; stage < 3U; ++stage)
    for (unsigned release = 0; release < 2U; ++release) {
        reset(); fill_map(80U); seed_map_load(); uint64_t id = start_map(operation);
        void (*hook)(void) = release ? release_hook : cancel_hook;
        if (!stage) {
            if (release) cerberus_ble_archive_io_release(id);
            else cerberus_ble_archive_io_cancel(id);
        } else if (stage == 1U) map_codec_hook = hook; else storage_hook = hook;
        run_worker(); bool published = operation != 1U && stage == 2U;
        CHECK(status().state == (published ? CERBERUS_BLE_ARCHIVE_IO_COMPLETE : CERBERUS_BLE_ARCHIVE_IO_CANCELED));
        CHECK(status().result_ready == (published && !release));
        CHECK((map_saves + mesh_saves) == (published ? 1U : 0U));
        if (operation == 1U) CHECK(map_loads == (stage ? 1U : 0U));
        cerberus_ble_archive_io_release(id); CHECK(!outstanding);
    }
    /* CRC, truncation and trailing bytes reject without publishing any model. */
    for (unsigned fault = 0; fault < 3U; ++fault) {
        reset(); fill_map(1U); seed_map_load();
        if (!fault) map_bytes[map_length - 1U] ^= 1U;
        else if (fault == 1U) --map_length;
        else map_bytes[map_length++] = 0U;
        uint64_t id = start_map(1U); run_worker();
        CHECK(status().state == CERBERUS_BLE_ARCHIVE_IO_ERROR && status().error == ESP_ERR_INVALID_RESPONSE && !outstanding);
        CHECK(cerberus_ble_archive_io_copy_mesh_archive(id, &map_output) == ESP_ERR_INVALID_STATE &&
            !memcmp(&map_output, &map_sentinel, sizeof(map_output)));
    }
    for (unsigned operation = 0; operation < 3U; ++operation) {
        reset(); fill_map(1U); seed_map_load(); uint64_t id = start_map(operation);
        storage_error = ESP_FAIL; partial_save = true; run_worker();
        CHECK(status().state == CERBERUS_BLE_ARCHIVE_IO_ERROR && !outstanding && !status().result_ready);
        if (operation != 1U) CHECK(!strcmp(status().file_name, "M00001.TMP"));
        CHECK(cerberus_ble_archive_io_copy_mesh_archive(id, &map_output) == ESP_ERR_INVALID_STATE);
        for (unsigned failure = 0; failure < 4U; ++failure) {
            reset(); fill_map(1U); seed_map_load();
            if (!failure) free_internal = 24575U;
            if (failure == 1U) largest_internal = 8191U;
            if (failure == 2U) fail_alloc = true;
            if (failure == 3U) fail_task = true;
            esp_err_t error = operation == 0U ? cerberus_ble_archive_io_save_mesh_archive(&map_input, &id) :
                operation == 1U ? cerberus_ble_archive_io_load_mesh_archive("/", "x.MSH", &id) :
                cerberus_ble_archive_io_export_mesh_archive_csv(&map_input, &id);
            CHECK(error == ESP_ERR_NO_MEM && !outstanding && !pending_task && !map_saves && !map_loads && !mesh_saves);
        }
        reset(); fill_map(1U); seed_map_load(); release_in_alloc = true;
        id = start_map(operation); run_worker();
        CHECK(status().state == CERBERUS_BLE_ARCHIVE_IO_CANCELED && !outstanding &&
            !map_saves && !map_loads && !mesh_saves);
    }
    reset(); fill_map(1U); seed_map_load(); uint64_t current = start_map(1U);
    cerberus_ble_archive_io_cancel(current - 1U); cerberus_ble_archive_io_release(current - 1U);
    CHECK(!status().cancel_requested && status().worker_active);
    run_worker();
    CHECK(cerberus_ble_archive_io_copy_mesh_archive(current - 1U, &map_output) == ESP_ERR_INVALID_STATE &&
        !memcmp(&map_output, &map_sentinel, sizeof(map_output)));
    CHECK(cerberus_ble_archive_io_copy_mesh_archive(current, &map_output) == ESP_OK);
    reset(); fill_map(1U); immediate_task = true;
    CHECK(cerberus_ble_archive_io_save_mesh_archive(&map_input, &current) == ESP_OK && !pending_task && status().result_ready);
    reset(); fill(1U); current = start_save(); run_worker();
    memset(&map_output, 0xA5, sizeof(map_output)); map_sentinel = map_output;
    CHECK(cerberus_ble_archive_io_copy_mesh_archive(current, &map_output) == ESP_ERR_INVALID_STATE &&
        !memcmp(&map_output, &map_sentinel, sizeof(map_output)));
    reset(); fill_map(1U); uint64_t id = 7U;
    CHECK(cerberus_ble_archive_io_save_mesh_archive(NULL, &id) == ESP_ERR_INVALID_ARG && !id);
    CHECK(cerberus_ble_archive_io_export_mesh_archive_csv(NULL, &id) == ESP_ERR_INVALID_ARG && !id);
    CHECK(cerberus_ble_archive_io_save_mesh_archive(&map_input, NULL) == ESP_ERR_INVALID_ARG);
    CHECK(cerberus_ble_archive_io_load_mesh_archive(NULL, "x.MSH", &id) == ESP_ERR_INVALID_ARG);
    char long_path[512], long_name[256]; memset(long_path, 'X', sizeof(long_path)); memset(long_name, 'X', sizeof(long_name));
    CHECK(cerberus_ble_archive_io_load_mesh_archive(long_path, "x.MSH", &id) == ESP_ERR_INVALID_ARG && !id);
    CHECK(cerberus_ble_archive_io_load_mesh_archive("/", long_name, &id) == ESP_ERR_INVALID_ARG && !id);
    CHECK(cerberus_ble_archive_io_copy_mesh_archive(0U, NULL) == ESP_ERR_INVALID_ARG);
    map_input.model.session = 0U;
    CHECK(cerberus_ble_archive_io_save_mesh_archive(&map_input, &id) == ESP_ERR_INVALID_ARG && !id && !outstanding);
    printf("Mesh native worker save=%zu load=%zu CSV=%zu; document=%zu encoded=%u.\n",
        archive_io_allocation_size(CERBERUS_BLE_ARCHIVE_IO_SAVE_MESH_ARCHIVE),
        archive_io_allocation_size(CERBERUS_BLE_ARCHIVE_IO_LOAD_MESH_ARCHIVE),
        archive_io_allocation_size(CERBERUS_BLE_ARCHIVE_IO_EXPORT_MESH_ARCHIVE_CSV),
        sizeof(mesh_archive_t), MESH_ARCHIVE_MAX_BYTES);
    reset();
}
