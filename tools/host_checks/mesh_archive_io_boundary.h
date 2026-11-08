/* Storage is modeled here; the separate filesystem fixture runs the actual
 * .MSH writer/reader. The worker and portable codec are always production. */
static uint8_t map_bytes[MESH_ARCHIVE_MAX_BYTES];
static size_t map_length;
static unsigned map_saves, map_loads, map_encodes, map_decodes, map_exports;
static void (*map_codec_hook)(void);
static esp_err_t cerberus_storage_mesh_archive_save(const uint8_t *data, size_t length,
    char *name, size_t capacity)
{
    CHECK(!locked && !storage_active && length && length <= sizeof(map_bytes) && capacity == 256U);
    CHECK(status().worker_active && status().state == CERBERUS_BLE_ARCHIVE_IO_SAVING &&
        status().format == CERBERUS_BLE_ARCHIVE_IO_FORMAT_MESH_ARCHIVE);
    ++map_saves; storage_active = true;
    if (storage_hook) storage_hook();
    if (storage_error == ESP_OK) { memcpy(map_bytes, data, length); map_length = length; strcpy(name, "M00001.MSH"); }
    else if (partial_save) strcpy(name, "M00001.TMP");
    storage_active = false; return storage_error;
}
static esp_err_t cerberus_storage_mesh_archive_load(const char *path, const char *name,
    uint8_t *data, size_t capacity, size_t *length)
{
    CHECK(!locked && !storage_active && capacity == sizeof(map_bytes));
    CHECK(status().worker_active && status().state == CERBERUS_BLE_ARCHIVE_IO_LOADING &&
        status().format == CERBERUS_BLE_ARCHIVE_IO_FORMAT_MESH_ARCHIVE);
    ++map_loads; storage_active = true;
    strcpy(last_path, path); strcpy(last_name, name);
    if (storage_hook) storage_hook();
    if (storage_error == ESP_OK) { memcpy(data, map_bytes, map_length); *length = map_length; }
    else *length = 0U;
    storage_active = false; return storage_error;
}
static void map_codec_interleave(void)
{ void (*hook)(void) = map_codec_hook; map_codec_hook = NULL; if (hook) hook(); }
static bool checked_map_encode(const mesh_archive_t *doc, uint8_t *data, size_t cap, size_t *length)
{
    CHECK(!locked); ++map_encodes;
    bool result = mesh_archive_encode(doc, data, cap, length);
    map_codec_interleave(); return result;
}
static bool checked_map_decode(const uint8_t *data, size_t length, mesh_archive_t *doc,
    mesh_archive_t *workspace)
{
    CHECK(!locked); ++map_decodes;
    bool result = mesh_archive_decode(data, length, doc, workspace);
    map_codec_interleave(); return result;
}
static bool checked_map_report(const mesh_archive_t *doc, char *data, size_t cap, size_t *length)
{
    CHECK(!locked); ++map_exports;
    bool result = mesh_archive_report_encode(doc, data, cap, length);
    map_codec_interleave(); return result;
}
