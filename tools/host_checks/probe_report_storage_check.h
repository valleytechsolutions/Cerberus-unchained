/* Extends the actual storage extraction and real temporary filesystem.
 * Fault adapters exercise production calls; sync injection is not SD evidence. */
static void probe_report_setup(void)
{
    setup();
    CHECK(native_mkdir("sdcard/CERBERUS/LOGS") == 0);
    sd_probe_report_next_index = 1U;
    race_final_path = "sdcard/CERBERUS/LOGS/P00001.CSV";
}
static bool probe_report_exists(const char *file)
{
    char path[128];
    snprintf(path, sizeof(path), "sdcard/CERBERUS/LOGS/%s", file);
    struct stat item;
    return stat(path, &item) == 0;
}
static void probe_report_verify(const char *file, const void *expected, size_t length)
{
    char path[128];
    snprintf(path, sizeof(path), "sdcard/CERBERUS/LOGS/%s", file);
    FILE *stream = fopen(path, "rb");
    CHECK(stream != NULL);
    CHECK(fread(output, 1U, sizeof(output), stream) == length);
    CHECK(memcmp(output, expected, length) == 0);
    CHECK(fgetc(stream) == EOF && fclose(stream) == 0);
}
static esp_err_t probe_report_save(size_t length)
{
    return cerberus_storage_probe_report_save(payload, length, name, sizeof(name));
}
static void check_probe_report_storage(void)
{
    CHECK(CERBERUS_STORAGE_PROBE_REPORT_MAX_BYTES == 8192U);
    CHECK(!strcmp(CERBERUS_STORAGE_PROBE_REPORT_PATH, "/CERBERUS/LOGS"));
    CHECK(sizeof(payload) >= CERBERUS_STORAGE_PROBE_REPORT_MAX_BYTES);
    probe_report_setup();
    probe_handoff = true;
    CHECK(probe_report_save(CERBERUS_STORAGE_PROBE_REPORT_MAX_BYTES) == ESP_OK);
    CHECK(!strcmp(name, "P00001.CSV") && !probe_report_exists("P00001.TMP"));
    probe_report_verify(name, payload, CERBERUS_STORAGE_PROBE_REPORT_MAX_BYTES);
    CHECK(handoff_probes > 8 && calls[SYNC] == 1 && calls[RENAME] == 1);
    CHECK(sd_ble_archive_next_index == 1 && sd_airtag_json_next_index == 1 &&
        sd_wifi_archive_next_index == 1 && sd_ap_json_next_index == 1 && sd_ssid_text_next_index == 1);
    probe_handoff = false;
    CHECK(cerberus_storage_reserve_for_usb() == ESP_OK && !sd_mounted && unmounts == 1);
    clean();

    probe_report_setup();
    put("sdcard/CERBERUS/LOGS/P00001.CSV", "KEEP", 4);
    put("sdcard/CERBERUS/LOGS/P00002.TMP", "KEEP", 4);
    CHECK(probe_report_save(91) == ESP_OK && !strcmp(name, "P00003.CSV"));
    probe_report_verify("P00001.CSV", "KEEP", 4);
    probe_report_verify("P00002.TMP", "KEEP", 4);
    probe_report_setup(); race_temp = true;
    CHECK(probe_report_save(91) == ESP_OK && !strcmp(name, "P00002.CSV"));
    probe_report_verify("P00001.TMP", "KEEP", 4);
    probe_report_setup(); race_final = true;
    CHECK(probe_report_save(91) == ESP_ERR_INVALID_STATE && !strcmp(name, "P00001.TMP"));
    probe_report_verify("P00001.CSV", "KEEP", 4);
    probe_report_verify("P00001.TMP", payload, 91);
    CHECK(!calls[RENAME]); clean();
    probe_report_setup(); collide_all = true;
    CHECK(probe_report_save(91) == ESP_ERR_NOT_FOUND && !calls[OPEN] && !name[0]);
    CHECK(calls[STAT] == SD_BLE_ARCHIVE_SEARCH_LIMIT + 2U); clean();
    probe_report_setup(); sd_probe_report_next_index = 99999;
    CHECK(probe_report_save(91) == ESP_OK && !strcmp(name, "P99999.CSV"));
    CHECK(probe_report_save(91) == ESP_OK && !strcmp(name, "P00001.CSV"));
    CHECK(sd_probe_report_next_index == 2U); clean();

    const enum event failures[] = {STAT, OPEN, FDOPEN, WRITE, FLUSH, SYNC, CLOSE, RENAME};
    for (unsigned i = 0; i < sizeof(failures) / sizeof(failures[0]); ++i) {
        probe_report_setup(); fault[failures[i]] = 1;
        CHECK(probe_report_save(91) != ESP_OK && !probe_report_exists("P00001.CSV"));
        if (failures[i] >= FDOPEN)
            CHECK(probe_report_exists("P00001.TMP") && !strcmp(name, "P00001.TMP"));
        CHECK(!calls[RENAME] || failures[i] == RENAME); clean();
        memset(fault, 0, sizeof(fault));
        CHECK(probe_report_save(91) == ESP_OK); clean();
    }
    for (unsigned nth = 2; nth <= 5; ++nth) {
        probe_report_setup(); fault[STAT] = nth;
        CHECK(probe_report_save(91) != ESP_OK && !probe_report_exists("P00001.CSV"));
        if (nth == 5) CHECK(probe_report_exists("P00001.TMP"));
        CHECK(!calls[RENAME]); clean();
    }
    probe_report_setup();
    CHECK(native_rmdir("sdcard/CERBERUS/LOGS") == 0); fault[MKDIR] = 1;
    CHECK(probe_report_save(91) != ESP_OK && !name[0] && !calls[OPEN]); clean();
    probe_report_setup();
    CHECK(native_rmdir("sdcard/CERBERUS/LOGS") == 0);
    put("sdcard/CERBERUS/LOGS", "KEEP", 4);
    CHECK(probe_report_save(91) == ESP_ERR_INVALID_STATE && !calls[OPEN]); clean();
    probe_report_setup(); extra_error = true;
    CHECK(probe_report_save(91) != ESP_OK && !calls[RENAME] && probe_report_exists("P00001.TMP")); clean();
    probe_report_setup(); fault[FDOPEN] = 1; fault[RAW_CLOSE] = 1;
    CHECK(probe_report_save(91) != ESP_OK && calls[RAW_CLOSE] == 1); clean();

    /* Every append failure leaves an earlier report intact and cannot replace
     * the failed temporary entry during a subsequent successful save. */
    const enum event publication_failures[] = {WRITE, FLUSH, SYNC, CLOSE, RENAME};
    for (unsigned i = 0; i < sizeof(publication_failures) / sizeof(publication_failures[0]); ++i) {
        probe_report_setup();
        CHECK(probe_report_save(91) == ESP_OK);
        fault[publication_failures[i]] = calls[publication_failures[i]] + 1U;
        CHECK(probe_report_save(129) != ESP_OK && !strcmp(name, "P00002.TMP"));
        CHECK(!probe_report_exists("P00002.CSV") && probe_report_exists("P00002.TMP"));
        probe_report_verify("P00001.CSV", payload, 91);
        memset(fault, 0, sizeof(fault));
        CHECK(probe_report_save(129) == ESP_OK && !strcmp(name, "P00003.CSV"));
        probe_report_verify("P00001.CSV", payload, 91); clean();
    }
    for (unsigned owner = 0; owner < 7; ++owner) {
        probe_report_setup();
        if (owner == 0) sd_mounted = false;
        if (owner == 1) storage_owner = CERBERUS_STORAGE_OWNER_USB;
        if (owner == 2) sd_recon.active = true;
        if (owner == 3) wardrive_busy = true;
        if (owner == 4) sd_wifi_capture_file = (FILE *)1;
        if (owner == 5) fail_lock = true;
        if (owner == 6) { storage_mutex = NULL; fail_mutex = true; }
        CHECK(probe_report_save(91) != ESP_OK && !calls[STAT] && !calls[OPEN] && !name[0]); clean();
    }
    probe_report_setup();
    CHECK(probe_report_save(0) == ESP_ERR_INVALID_ARG);
    CHECK(probe_report_save(CERBERUS_STORAGE_PROBE_REPORT_MAX_BYTES + 1U) == ESP_ERR_INVALID_ARG);
    CHECK(cerberus_storage_probe_report_save(NULL, 1, name, sizeof(name)) == ESP_ERR_INVALID_ARG);
    CHECK(cerberus_storage_probe_report_save(payload, 1, NULL, 11) == ESP_ERR_INVALID_ARG);
    char small[10]; memset(small, 0xA5, sizeof(small));
    CHECK(cerberus_storage_probe_report_save(payload, 1, small, sizeof(small)) == ESP_ERR_INVALID_ARG && !small[0]);
    for (unsigned i = 1; i < sizeof(small); ++i) CHECK((unsigned char)small[i] == 0xA5);
    CHECK(!calls[STAT] && !calls[OPEN] && !calls[RENAME] && sd_probe_report_next_index == 1); clean();
}
