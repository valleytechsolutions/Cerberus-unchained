#pragma once
#include "esp_err.h"
#include "wardrive_log.h"
#include <stdio.h>

#define CERBERUS_WARD_LOG_CAPACITY 2997U
#define CERBERUS_WARD_UPLOAD_PROFILE_PATH "/CERBERUS/CONFIG/UPLOAD.JSON"
typedef struct { char name[CERBERUS_WARD_LOG_NAME_CAPACITY]; uint64_t bytes; } cerberus_ward_log_entry_t;
typedef struct {
    bool directory_present;
    uint32_t entries, csv_entries, temporary_entries, unrecognized_csv;
    char first_unrecognized_csv[CERBERUS_WARD_LOG_NAME_CAPACITY];
} cerberus_ward_log_catalog_info_t;
/* One worker owns the lease and every FILE it opens. End only after closing
 * them. USB, capture and File Tools mutations are excluded for its lifetime.
 * The token is never reused; no arbitrary paths are accepted. */
esp_err_t cerberus_ward_log_storage_begin(uint64_t *token);
esp_err_t cerberus_ward_log_storage_end(uint64_t token);
esp_err_t cerberus_ward_log_storage_list(uint64_t token, cerberus_ward_log_entry_t *entries,
    size_t capacity, size_t *count);
esp_err_t cerberus_ward_log_storage_list_info(uint64_t token, cerberus_ward_log_entry_t *entries,
    size_t capacity, size_t *count, cerberus_ward_log_catalog_info_t *info);
esp_err_t cerberus_ward_log_storage_open(uint64_t token, const char *name, FILE **file);
esp_err_t cerberus_ward_log_storage_close(uint64_t token, FILE **file);
esp_err_t cerberus_ward_log_storage_delete(uint64_t token, const char *name, uint64_t expected_bytes);
/* Exact profile path only; the same lease excludes USB/File Tools mutations. */
esp_err_t cerberus_ward_log_profile_read(uint64_t token,void *out,size_t capacity,size_t *bytes);
esp_err_t cerberus_ward_log_profile_remove(uint64_t token,size_t expected_bytes);
/* Receipt key is SHA-256 hex + '-' + three decimal part digits. Exact bounded
 * records only; append + fsync preserves the prior sending intent. Truncated
 * journal tails fail closed so an ambiguous POST is never blindly repeated. */
esp_err_t cerberus_ward_log_receipt_read(uint64_t token, const char *key, void *out, size_t bytes);
esp_err_t cerberus_ward_log_receipt_write(uint64_t token, const char *key, const void *data, size_t bytes);
