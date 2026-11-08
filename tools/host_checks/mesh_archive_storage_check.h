static uint8_t map_storage_payload[18332], map_storage_output[18332];
/* Extends the actual storage extraction and real temporary filesystem.
 * Fault adapters exercise production calls; sync injection is not SD evidence. */
static void mesh_archive_setup(void)
{
    setup();
    CHECK(native_mkdir("sdcard/CERBERUS/MESH") == 0);
    sd_mesh_archive_next_index = 1U;
    race_final_path = "sdcard/CERBERUS/MESH/M00001.MSH";
}
static bool mesh_archive_exists(const char *file)
{
    char path[128];
    snprintf(path, sizeof(path), "sdcard/CERBERUS/MESH/%s", file);
    struct stat item;
    return stat(path, &item) == 0;
}
static void mesh_archive_verify(const char *file, const void *expected, size_t length)
{
    char path[128];
    snprintf(path, sizeof(path), "sdcard/CERBERUS/MESH/%s", file);
    FILE *stream = fopen(path, "rb");
    CHECK(stream != NULL);
    CHECK(fread(map_storage_output, 1U, sizeof(map_storage_output), stream) == length);
    CHECK(memcmp(map_storage_output, expected, length) == 0);
    CHECK(fgetc(stream) == EOF && fclose(stream) == 0);
}
static esp_err_t mesh_archive_save(size_t length)
{
    return cerberus_storage_mesh_archive_save(map_storage_payload, length, name, sizeof(name));
}
static void check_mesh_archive_save_storage(void)
{
    for (unsigned i=0;i<sizeof(map_storage_payload);++i) map_storage_payload[i]=(uint8_t)(i*7U);
    CHECK(CERBERUS_STORAGE_MESH_ARCHIVE_MAX_BYTES == 18332U);
    CHECK(!strcmp(CERBERUS_STORAGE_MESH_ARCHIVE_PATH, "/CERBERUS/MESH"));
    CHECK(sizeof(map_storage_payload) >= CERBERUS_STORAGE_MESH_ARCHIVE_MAX_BYTES);
    mesh_archive_setup();
    probe_handoff = true;
    CHECK(mesh_archive_save(CERBERUS_STORAGE_MESH_ARCHIVE_MAX_BYTES) == ESP_OK);
    CHECK(!strcmp(name, "M00001.MSH") && !mesh_archive_exists("M00001.TMP"));
    mesh_archive_verify(name, map_storage_payload, CERBERUS_STORAGE_MESH_ARCHIVE_MAX_BYTES);
    CHECK(handoff_probes > 8 && calls[SYNC] == 1 && calls[RENAME] == 1);
    CHECK(sd_ble_archive_next_index == 1 && sd_airtag_json_next_index == 1 &&
        sd_wifi_archive_next_index == 1 && sd_ap_json_next_index == 1 && sd_ssid_text_next_index == 1);
    probe_handoff = false;
    CHECK(cerberus_storage_reserve_for_usb() == ESP_OK && !sd_mounted && unmounts == 1);
    clean();

    mesh_archive_setup();
    put("sdcard/CERBERUS/MESH/M00001.MSH", "KEEP", 4);
    put("sdcard/CERBERUS/MESH/M00002.TMP", "KEEP", 4);
    CHECK(mesh_archive_save(91) == ESP_OK && !strcmp(name, "M00003.MSH"));
    mesh_archive_verify("M00001.MSH", "KEEP", 4);
    mesh_archive_verify("M00002.TMP", "KEEP", 4);
    mesh_archive_setup(); race_temp = true;
    CHECK(mesh_archive_save(91) == ESP_OK && !strcmp(name, "M00002.MSH"));
    mesh_archive_verify("M00001.TMP", "KEEP", 4);
    mesh_archive_setup(); race_final = true;
    CHECK(mesh_archive_save(91) == ESP_ERR_INVALID_STATE && !strcmp(name, "M00001.TMP"));
    mesh_archive_verify("M00001.MSH", "KEEP", 4);
    mesh_archive_verify("M00001.TMP", map_storage_payload, 91);
    CHECK(!calls[RENAME]); clean();
    mesh_archive_setup(); collide_all = true;
    CHECK(mesh_archive_save(91) == ESP_ERR_NOT_FOUND && !calls[OPEN] && !name[0]);
    CHECK(calls[STAT] == SD_BLE_ARCHIVE_SEARCH_LIMIT + 2U); clean();
    mesh_archive_setup(); sd_mesh_archive_next_index = 99999;
    CHECK(mesh_archive_save(91) == ESP_OK && !strcmp(name, "M99999.MSH"));
    CHECK(mesh_archive_save(91) == ESP_OK && !strcmp(name, "M00001.MSH"));
    CHECK(sd_mesh_archive_next_index == 2U); clean();

    const enum event failures[] = {STAT, OPEN, FDOPEN, WRITE, FLUSH, SYNC, CLOSE, RENAME};
    for (unsigned i = 0; i < sizeof(failures) / sizeof(failures[0]); ++i) {
        mesh_archive_setup(); fault[failures[i]] = 1;
        CHECK(mesh_archive_save(91) != ESP_OK && !mesh_archive_exists("M00001.MSH"));
        if (failures[i] >= FDOPEN)
            CHECK(mesh_archive_exists("M00001.TMP") && !strcmp(name, "M00001.TMP"));
        CHECK(!calls[RENAME] || failures[i] == RENAME); clean();
        memset(fault, 0, sizeof(fault));
        CHECK(mesh_archive_save(91) == ESP_OK); clean();
    }
    for (unsigned nth = 2; nth <= 5; ++nth) {
        mesh_archive_setup(); fault[STAT] = nth;
        CHECK(mesh_archive_save(91) != ESP_OK && !mesh_archive_exists("M00001.MSH"));
        if (nth == 5) CHECK(mesh_archive_exists("M00001.TMP"));
        CHECK(!calls[RENAME]); clean();
    }
    mesh_archive_setup();
    CHECK(native_rmdir("sdcard/CERBERUS/MESH") == 0); fault[MKDIR] = 1;
    CHECK(mesh_archive_save(91) != ESP_OK && !name[0] && !calls[OPEN]); clean();
    mesh_archive_setup();
    CHECK(native_rmdir("sdcard/CERBERUS/MESH") == 0);
    put("sdcard/CERBERUS/MESH", "KEEP", 4);
    CHECK(mesh_archive_save(91) == ESP_ERR_INVALID_STATE && !calls[OPEN]); clean();
    mesh_archive_setup(); extra_error = true;
    CHECK(mesh_archive_save(91) != ESP_OK && !calls[RENAME] && mesh_archive_exists("M00001.TMP")); clean();
    mesh_archive_setup(); fault[FDOPEN] = 1; fault[RAW_CLOSE] = 1;
    CHECK(mesh_archive_save(91) != ESP_OK && calls[RAW_CLOSE] == 1); clean();

    /* Every append failure leaves an earlier report intact and cannot replace
     * the failed temporary entry during a subsequent successful save. */
    const enum event publication_failures[] = {WRITE, FLUSH, SYNC, CLOSE, RENAME};
    for (unsigned i = 0; i < sizeof(publication_failures) / sizeof(publication_failures[0]); ++i) {
        mesh_archive_setup();
        CHECK(mesh_archive_save(91) == ESP_OK);
        fault[publication_failures[i]] = calls[publication_failures[i]] + 1U;
        CHECK(mesh_archive_save(129) != ESP_OK && !strcmp(name, "M00002.TMP"));
        CHECK(!mesh_archive_exists("M00002.MSH") && mesh_archive_exists("M00002.TMP"));
        mesh_archive_verify("M00001.MSH", map_storage_payload, 91);
        memset(fault, 0, sizeof(fault));
        CHECK(mesh_archive_save(129) == ESP_OK && !strcmp(name, "M00003.MSH"));
        mesh_archive_verify("M00001.MSH", map_storage_payload, 91); clean();
    }
    for (unsigned owner = 0; owner < 7; ++owner) {
        mesh_archive_setup();
        if (owner == 0) sd_mounted = false;
        if (owner == 1) storage_owner = CERBERUS_STORAGE_OWNER_USB;
        if (owner == 2) sd_recon.active = true;
        if (owner == 3) wardrive_busy = true;
        if (owner == 4) sd_wifi_capture_file = (FILE *)1;
        if (owner == 5) fail_lock = true;
        if (owner == 6) { storage_mutex = NULL; fail_mutex = true; }
        CHECK(mesh_archive_save(91) != ESP_OK && !calls[STAT] && !calls[OPEN] && !name[0]); clean();
    }
    mesh_archive_setup();
    CHECK(mesh_archive_save(0) == ESP_ERR_INVALID_ARG);
    CHECK(mesh_archive_save(CERBERUS_STORAGE_MESH_ARCHIVE_MAX_BYTES + 1U) == ESP_ERR_INVALID_ARG);
    CHECK(cerberus_storage_mesh_archive_save(NULL, 1, name, sizeof(name)) == ESP_ERR_INVALID_ARG);
    CHECK(cerberus_storage_mesh_archive_save(map_storage_payload, 1, NULL, 11) == ESP_ERR_INVALID_ARG);
    char small[10]; memset(small, 0xA5, sizeof(small));
    CHECK(cerberus_storage_mesh_archive_save(map_storage_payload, 1, small, sizeof(small)) == ESP_ERR_INVALID_ARG && !small[0]);
    for (unsigned i = 1; i < sizeof(small); ++i) CHECK((unsigned char)small[i] == 0xA5);
    CHECK(!calls[STAT] && !calls[OPEN] && !calls[RENAME] && sd_mesh_archive_next_index == 1); clean();
}

static esp_err_t mesh_archive_load(size_t *length)
{
    memset(map_storage_output, 0xA5, sizeof(map_storage_output)); *length = 99U;
    return cerberus_storage_mesh_archive_load("/CERBERUS/MESH", "M00001.MSH",
        map_storage_output, sizeof(map_storage_output), length);
}
static void mesh_archive_load_failed(size_t length)
{
    CHECK(!length);
    for (unsigned i = 0; i < sizeof(map_storage_output); ++i) CHECK(!map_storage_output[i]);
}
static void check_mesh_archive_storage_aliases(void)
{
    union { size_t alignment; uint8_t bytes[512]; } arena, before;
    mesh_archive_setup();
    for (unsigned offset = 0U; offset < 32U; ++offset) {
        memset(arena.bytes, 0xA5, sizeof(arena.bytes)); before = arena;
        CHECK(cerberus_storage_mesh_archive_save(arena.bytes, 32U,
            (char *)arena.bytes + offset, 11U) == ESP_ERR_INVALID_ARG);
        CHECK(!memcmp(&arena, &before, sizeof(arena)) && !calls[STAT] && !calls[OPEN]);
    }
    /* Adjacent input/output ranges are legal and do not alter the saved bytes. */
    memset(arena.bytes, 0xA5, sizeof(arena.bytes)); before = arena;
    CHECK(cerberus_storage_mesh_archive_save(arena.bytes, 32U, (char *)arena.bytes + 32U, 11U) == ESP_OK);
    mesh_archive_verify((char *)arena.bytes + 32U, before.bytes, 32U);
    for (unsigned pair = 0U; pair < 8U; ++pair) {
        memset(arena.bytes, 0xA5, sizeof(arena.bytes));
        strcpy((char *)arena.bytes, "/CERBERUS/MESH");
        strcpy((char *)arena.bytes + 64U, "M00001.MSH");
        const char *path = (char *)arena.bytes, *file = (char *)arena.bytes + 64U;
        uint8_t *data = arena.bytes + 128U; size_t length = 123U, *result = &length;
        if (pair == 0U) file = path;
        if (pair == 1U) data = arena.bytes;
        if (pair == 2U) data = arena.bytes + strlen(path); /* includes NUL */
        if (pair == 3U) result = (size_t *)arena.bytes;
        if (pair == 4U) data = arena.bytes + 64U;
        if (pair == 5U) data = arena.bytes + 64U + strlen(file);
        if (pair == 6U) result = (size_t *)(arena.bytes + 64U);
        if (pair == 7U) result = (size_t *)(arena.bytes + 128U + 24U);
        before = arena; unsigned probes = calls[STAT], opens = calls[FOPEN], takes = locks;
        CHECK(cerberus_storage_mesh_archive_load(path, file, data, 32U, result) == ESP_ERR_INVALID_ARG);
        CHECK(!memcmp(&arena, &before, sizeof(arena)) && length == 123U &&
            calls[STAT] == probes && calls[FOPEN] == opens && locks == takes);
    }
    /* A bounded missing-NUL input is rejected without unbounded string reads. */
    memset(arena.bytes, 'X', sizeof(arena.bytes)); size_t length = 123U;
    CHECK(cerberus_storage_mesh_archive_load((char *)arena.bytes, "x.MSH", map_storage_output,
        sizeof(map_storage_output), &length) == ESP_ERR_INVALID_ARG && !length);
    clean();
}
static void check_mesh_archive_directory_listing(void)
{
    cerberus_storage_entry_t entries[4], before[4];
    size_t count;
    setup();
    memset(entries, 0xa5, sizeof(entries));
    memcpy(before, entries, sizeof(entries));
    count = 99U;
    CHECK(cerberus_storage_list_directory("/CERBERUS/MESH", entries, 4U, &count) == ESP_ERR_NOT_FOUND);
    CHECK(!count && !memcmp(entries, before, sizeof(entries)) && directory_opens == 1U);
    CHECK(errno == EIO); /* Logger changed errno after its classification snapshot. */
    CHECK(!calls[MKDIR] && !calls[OPEN]);
    struct stat item;
    CHECK(stat("sdcard/CERBERUS/MESH", &item) != 0 && errno == ENOENT);
    clean();

    CHECK(native_mkdir("sdcard/CERBERUS/MESH") == 0);
    count = 99U;
    CHECK(cerberus_storage_list_directory("/CERBERUS/MESH", entries, 4U, &count) == ESP_OK && !count);
    CHECK(native_mkdir("sdcard/CERBERUS/MESH/sub") == 0);
    put("sdcard/CERBERUS/MESH/B.MSH", "BB", 2U);
    put("sdcard/CERBERUS/MESH/a.MSH", "A", 1U);
    CHECK(cerberus_storage_list_directory("/CERBERUS/MESH", entries, 4U, &count) == ESP_OK && count == 3U);
    CHECK(entries[0].is_directory && !strcmp(entries[0].name, "sub") && !entries[0].size_bytes);
    CHECK(!entries[1].is_directory && !strcmp(entries[1].name, "a.MSH") && entries[1].size_bytes == 1U);
    CHECK(!entries[2].is_directory && !strcmp(entries[2].name, "B.MSH") && entries[2].size_bytes == 2U);
    CHECK(cerberus_storage_list_directory("/CERBERUS/MESH", entries, 2U, &count) == ESP_OK && count == 2U);
    clean();

    const int errors[] = {EIO, EACCES, ENOTDIR, EMFILE, ENOMEM};
    for (unsigned i = 0U; i < sizeof(errors) / sizeof(errors[0]); ++i) {
        directory_open_error = errors[i];
        memcpy(entries, before, sizeof(entries)); count = 99U;
        CHECK(cerberus_storage_list_directory("/CERBERUS/MESH", entries, 4U, &count) == ESP_FAIL);
        CHECK(!count && !memcmp(entries, before, sizeof(entries)));
        clean();
    }
    directory_open_error = 0;
    unsigned opened = directory_opens;
    sd_mounted = false; count = 99U;
    CHECK(cerberus_storage_list_directory("/CERBERUS/MESH", entries, 4U, &count) == ESP_ERR_INVALID_STATE);
    CHECK(!count && directory_opens == opened); clean();
    sd_mounted = true; fail_lock = true; count = 99U;
    CHECK(cerberus_storage_list_directory("/CERBERUS/MESH", entries, 4U, &count) == ESP_ERR_TIMEOUT);
    CHECK(!count && directory_opens == opened); clean();
    fail_lock = false;
}

static void check_mesh_archive_storage(void)
{
    check_mesh_archive_directory_listing();
    check_mesh_archive_save_storage();
    check_mesh_archive_storage_aliases();
    mesh_archive_setup(); size_t length = 0U;
    CHECK(mesh_archive_save(sizeof(map_storage_payload)) == ESP_OK);
    short_read = true; probe_handoff = true;
    CHECK(mesh_archive_load(&length) == ESP_OK && length == sizeof(map_storage_payload) &&
        !memcmp(map_storage_payload, map_storage_output, length));
    CHECK(calls[READ] > 2U && handoff_probes > 2U); probe_handoff = false; clean();
    const enum event failures[] = {STAT, FOPEN, READ, GETC, CLOSE};
    for (unsigned i = 0; i < sizeof(failures) / sizeof(failures[0]); ++i) {
        mesh_archive_setup(); CHECK(mesh_archive_save(149U) == ESP_OK);
        fault[failures[i]] = calls[failures[i]] + 1U;
        CHECK(mesh_archive_load(&length) != ESP_OK); mesh_archive_load_failed(length); clean();
    }
    for (unsigned i = 0; i < 5U; ++i) {
        mesh_archive_setup(); CHECK(mesh_archive_save(149U) == ESP_OK);
        if (!i) extra_error = true;
        if (i == 1U) no_eof = true;
        if (i == 2U) { short_read = true; zero_read_at = 3U; }
        if (i == 3U) stat_size_delta = 1;
        if (i == 4U) stat_size_delta = -1;
        CHECK(mesh_archive_load(&length) != ESP_OK); mesh_archive_load_failed(length); clean();
    }
    for (unsigned owner = 0; owner < 7U; ++owner) {
        mesh_archive_setup(); CHECK(mesh_archive_save(149U) == ESP_OK);
        unsigned opened = calls[FOPEN];
        if (!owner) sd_mounted = false;
        if (owner == 1U) storage_owner = CERBERUS_STORAGE_OWNER_USB;
        if (owner == 2U) sd_recon.active = true;
        if (owner == 3U) wardrive_busy = true;
        if (owner == 4U) sd_wifi_capture_file = (FILE *)1;
        if (owner == 5U) fail_lock = true;
        if (owner == 6U) { storage_mutex = NULL; fail_mutex = true; }
        CHECK(mesh_archive_load(&length) != ESP_OK && calls[FOPEN] == opened);
        mesh_archive_load_failed(length); clean();
    }
    mesh_archive_setup();
    CHECK(mesh_archive_load(&length) == ESP_ERR_NOT_FOUND); mesh_archive_load_failed(length);
    put("sdcard/CERBERUS/MESH/M00001.MSH", "", 0U);
    CHECK(mesh_archive_load(&length) == ESP_ERR_INVALID_SIZE); mesh_archive_load_failed(length);
    CHECK(remove("sdcard/CERBERUS/MESH/M00001.MSH") == 0 && native_mkdir("sdcard/CERBERUS/MESH/M00001.MSH") == 0);
    CHECK(mesh_archive_load(&length) == ESP_ERR_INVALID_SIZE); mesh_archive_load_failed(length); clean();
    mesh_archive_setup();
    put("sdcard/CERBERUS/MESH/M00001.MSH", map_storage_payload, sizeof(map_storage_payload));
    CHECK(cerberus_storage_mesh_archive_load("/CERBERUS/MESH", "M00001.MSH", map_storage_output,
        sizeof(map_storage_output) - 1U, &length) == ESP_ERR_INVALID_SIZE && !length);
    const char *invalid[] = {"../x.MSH", "sub/x.MSH", "x.MSH/", "x.MSH\\", "x.TMP", "x.CSV", "x.BLE", "x.WFI", "x.MSH.exe"};
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        unsigned opened = calls[FOPEN];
        CHECK(cerberus_storage_mesh_archive_load("/CERBERUS/MESH", invalid[i], map_storage_output,
            sizeof(map_storage_output), &length) == ESP_ERR_INVALID_ARG && !length && calls[FOPEN] == opened);
    }
    CHECK(cerberus_storage_mesh_archive_load("/../MESH", "x.MSH", map_storage_output,
        sizeof(map_storage_output), &length) == ESP_ERR_INVALID_ARG);
    CHECK(cerberus_storage_mesh_archive_load("/", "x.MSH", map_storage_output,
        sizeof(map_storage_output) + 1U, &length) == ESP_ERR_INVALID_ARG);
    CHECK(native_mkdir("sdcard/COPIES") == 0);
    put("sdcard/COPIES/copied.mSh", "MSH-TEST", 8U);
    CHECK(cerberus_storage_mesh_archive_load("/COPIES", "copied.mSh", map_storage_output,
        sizeof(map_storage_output), &length) == ESP_OK && length == 8U && !memcmp(map_storage_output, "MSH-TEST", 8U));
    clean();

    /* Actual codec -> actual filesystem -> actual decode preserves aggregates
     * beyond the first64 raw witnesses; no live controller is reconstructed. */
    static mesh_scan_t capture;
    static mesh_archive_t document, decoded, workspace;
    mesh_archive_setup();
    CHECK(mesh_report_test_fill(&capture, 80U) && mesh_archive_from_scan(&capture, &document));
    CHECK(document.model.counters.records == 80U && document.model.witness_count == 64U);
    CHECK(mesh_archive_encode(&document, map_storage_payload, sizeof(map_storage_payload), &length));
    CHECK(mesh_archive_save(length) == ESP_OK);
    size_t received = 0U; CHECK(mesh_archive_load(&received) == ESP_OK && received == length);
    CHECK(mesh_archive_decode(map_storage_output, received, &decoded, &workspace) &&
        !memcmp(&decoded, &document, sizeof(document)));
    CHECK(sd_mesh_report_next_index == 1U); clean();
}
