#pragma once

#include "ssid_list.h"
#include "esp_err.h"

typedef enum {
    CERBERUS_SSID_SESSION_IDLE,
    CERBERUS_SSID_SESSION_LOADING,
    CERBERUS_SSID_SESSION_SAVING,
    CERBERUS_SSID_SESSION_PREVIEW,
    CERBERUS_SSID_SESSION_COMPLETE,
    CERBERUS_SSID_SESSION_CANCELED,
    CERBERUS_SSID_SESSION_ERROR,
} cerberus_ssid_list_session_state_t;

typedef enum {
    CERBERUS_SSID_EDIT_ADD, CERBERUS_SSID_EDIT_NAME, CERBERUS_SSID_EDIT_REMOVE,
    CERBERUS_SSID_EDIT_REMOVE_SELECTED, CERBERUS_SSID_EDIT_CLEAR,
    CERBERUS_SSID_EDIT_SELECT, CERBERUS_SSID_EDIT_SELECT_ALL,
    CERBERUS_SSID_EDIT_GENERATE,
} cerberus_ssid_list_edit_kind_t;

typedef struct {
    cerberus_ssid_list_edit_kind_t kind;
    uint8_t index;
    bool selected;
    /* ADD/NAME: raw name bytes. GENERATE: caller-supplied entropy, with count
     * names and exactly count * ENTROPY_PER_NAME bytes. Copied in this call. */
    const uint8_t *bytes;
    size_t length, count;
} cerberus_ssid_list_edit_t;

typedef struct {
    cerberus_ssid_list_session_state_t state;
    esp_err_t error;
    cerberus_ssid_list_result_t model_result;
    uint64_t revision, selected, job_id, preview_id, saved_revision;
    uint8_t count, preview_count;
    bool allocated, dirty, busy, closing, preview_ready;
    char file_name[256]; /* Last exact job result; never a writable destination. */
} cerberus_ssid_list_session_status_t;

/* One optional PSRAM workspace, retained for the boot after Back/sleep. Open
 * allocates lazily; no new worker, filesystem call or radio ownership. The
 * existing archive worker copies saves before returning and owns its buffers. */
esp_err_t cerberus_ssid_list_session_open(void);
/* Copied snapshot only; no borrowed model/worker pointers. */
esp_err_t cerberus_ssid_list_session_copy(cerberus_ssid_list_t *out);
esp_err_t cerberus_ssid_list_session_edit(uint64_t expected_revision,
    const cerberus_ssid_list_edit_t *edit);
/* A load only prepares a copied preview. The working list changes only when
 * apply_preview is explicitly called with both displayed identities. */
esp_err_t cerberus_ssid_list_session_load(const char *relative_path, const char *file_name);
esp_err_t cerberus_ssid_list_session_prepare_document(const cerberus_wifi_marauder_ssid_t *document);
esp_err_t cerberus_ssid_list_session_copy_preview(cerberus_wifi_marauder_ssid_t *out,
    uint64_t *preview_id);
esp_err_t cerberus_ssid_list_session_apply_preview(uint64_t preview_id,
    uint64_t expected_revision, bool append, bool unique);
void cerberus_ssid_list_session_discard_preview(void);
/* New S#####.TXT through the existing no-overwrite writer. Edits may continue
 * during Save; only a successful exact-revision snapshot clears dirty. */
esp_err_t cerberus_ssid_list_session_save(uint64_t expected_revision);
/* Poll advances exact-job copy/release and returns a bounded snapshot. Lost or
 * contradictory status never authorizes another job or an import application.
 * A newer serialized worker ID proves the old job released, but is not ours. */
esp_err_t cerberus_ssid_list_session_poll(cerberus_ssid_list_session_status_t *out);
/* Cancels/releases only the owned job and discards its preview. Keep polling
 * until busy=false before leaving; the working list and dirty bit survive.
 * An in-flight Save can still complete, as reported by the storage worker. */
void cerberus_ssid_list_session_close(void);
