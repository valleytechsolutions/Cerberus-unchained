#include "recon_journal.h"
#include "sentinel_watch_detect.h"

#include <inttypes.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static void increment(uint32_t *value)
{
    if (*value != UINT32_MAX) ++*value;
}

static void add_saturated(uint32_t *value, uint32_t added)
{
    *value = added > UINT32_MAX - *value ? UINT32_MAX : *value + added;
}

static bool rule_valid(const char *rule, bool required)
{
    for (size_t length = 0U; length <= CERBERUS_RECON_RULE_MAX_LENGTH; ++length) {
        unsigned char c = (unsigned char)rule[length];
        if (!c) return required ? length != 0U : length == 0U;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '.' || c == '_' || c == ':' || c == '-'))
            return false;
    }
    return false;
}

bool cerberus_recon_record_valid(const cerberus_recon_record_t *r)
{
    const uint8_t source_flags = CERBERUS_RECON_SOURCE_TIME_VALID |
        CERBERUS_RECON_SOURCE_SEQUENCE_VALID | CERBERUS_RECON_SOURCE_ANCHOR_VALID;
    const uint32_t limit_flags = CERBERUS_RECON_LIMIT_NAME_TRUNCATED |
        CERBERUS_RECON_LIMIT_RAW_TRUNCATED | CERBERUS_RECON_LIMIT_RETAINED_CAPACITY |
        CERBERUS_RECON_LIMIT_REPORTS_REJECTED | CERBERUS_RECON_LIMIT_SOURCE_UNAVAILABLE;
    if (!r || !r->session_id || !r->sequence || r->radio > CERBERUS_RECON_RADIO_BLE ||
        r->name_length > CERBERUS_RECON_NAME_MAX_LENGTH || r->address_type > 3U ||
        (r->source_flags & ~source_flags) || (r->source_limits & ~limit_flags) ||
        r->source_kind > CERBERUS_RECON_SOURCE_S3_CALLBACK_US ||
        r->alert_state > CERBERUS_RECON_ALERT_QUEUED) return false;
    if (r->radio == CERBERUS_RECON_RADIO_WIFI) {
        if (r->address_type || r->ibeacon ||
            (r->source_kind != CERBERUS_RECON_SOURCE_NONE &&
             r->source_kind != CERBERUS_RECON_SOURCE_C5_AGE_MS) ||
            (r->channel_valid && (!r->channel || r->channel > 196U)) ||
            (r->rssi_valid && (r->rssi < -127 || r->rssi > 0))) return false;
    } else if (r->channel_valid || r->channel ||
        r->source_kind == CERBERUS_RECON_SOURCE_C5_AGE_MS ||
        (r->rssi_valid && (r->rssi < -127 || r->rssi > 20))) return false;

    if (!(r->source_flags & CERBERUS_RECON_SOURCE_TIME_VALID) && r->source_time) return false;
    if (!(r->source_flags & CERBERUS_RECON_SOURCE_SEQUENCE_VALID) && r->source_sequence) return false;
    if ((r->source_flags & CERBERUS_RECON_SOURCE_SEQUENCE_VALID) && !r->source_sequence) return false;
    if (!(r->source_flags & CERBERUS_RECON_SOURCE_ANCHOR_VALID) && r->source_anchor_us) return false;
    if (r->source_kind == CERBERUS_RECON_SOURCE_NONE) {
        if (r->source_flags) return false;
    } else if (!r->source_flags) return false;
    if (r->source_kind == CERBERUS_RECON_SOURCE_C5_AGE_MS) {
        if ((r->source_flags & (CERBERUS_RECON_SOURCE_TIME_VALID | CERBERUS_RECON_SOURCE_ANCHOR_VALID)) !=
            (CERBERUS_RECON_SOURCE_TIME_VALID | CERBERUS_RECON_SOURCE_ANCHOR_VALID) ||
            r->source_anchor_us > r->consumed_us || r->source_time > UINT32_MAX) return false;
    } else if (r->source_flags & CERBERUS_RECON_SOURCE_ANCHOR_VALID) return false;
    if ((r->source_kind == CERBERUS_RECON_SOURCE_S3_ADV_MS32 ||
         r->source_kind == CERBERUS_RECON_SOURCE_S3_RESPONSE_MS32) &&
        r->source_time > UINT32_MAX) return false;
    if (r->source_kind == CERBERUS_RECON_SOURCE_S3_CALLBACK_US &&
        (r->source_flags & CERBERUS_RECON_SOURCE_TIME_VALID) &&
        r->source_time > r->consumed_us) return false;

    if (r->classified) {
        if (r->category <= SENTINEL_WATCH_NONE || r->category >= SENTINEL_WATCH_CATEGORY_COUNT ||
            r->confidence <= SENTINEL_WATCH_CONFIDENCE_NONE ||
            r->confidence > SENTINEL_WATCH_CONFIDENCE_HIGH || r->mode > 2U ||
            !rule_valid(r->rule, true)) return false;
    } else if (r->category || r->confidence || r->mode ||
        r->alert_state == CERBERUS_RECON_ALERT_QUEUED || !rule_valid(r->rule, false)) return false;
    return true;
}

bool cerberus_recon_journal_init(cerberus_recon_journal_t *j,
    uint64_t session_id, uint64_t started_us)
{
    if (!j || !session_id) return false;
    memset(j, 0, sizeof(*j));
    j->session_id = session_id;
    j->started_us = started_us;
    j->last_consumed_us = started_us;
    return true;
}

static bool journal_valid(const cerberus_recon_journal_t *j)
{
    return j && j->session_id && j->head < CERBERUS_RECON_JOURNAL_CAPACITY &&
        j->count <= CERBERUS_RECON_JOURNAL_CAPACITY;
}

bool cerberus_recon_journal_offer(cerberus_recon_journal_t *j,
    const cerberus_recon_record_t *r)
{
    if (!journal_valid(j)) return false;
    if (!cerberus_recon_record_valid(r) || r->session_id != j->session_id ||
        r->sequence <= j->last_sequence || r->consumed_us < j->started_us ||
        r->consumed_us < j->last_consumed_us) {
        increment(&j->counters.invalid);
        return false;
    }
    j->last_sequence = r->sequence;
    j->last_consumed_us = r->consumed_us;
    if (j->count == CERBERUS_RECON_JOURNAL_CAPACITY) {
        increment(&j->counters.dropped);
        return false;
    }
    uint16_t index = (uint16_t)((j->head + j->count) % CERBERUS_RECON_JOURNAL_CAPACITY);
    j->rows[index] = *r;
    ++j->count;
    increment(&j->counters.accepted);
    if (r->radio == CERBERUS_RECON_RADIO_WIFI) increment(&j->counters.accepted_wifi_rows);
    else {
        increment(&j->counters.accepted_ble_rows);
        if (r->ibeacon) increment(&j->counters.accepted_ibeacon_rows);
    }
    return true;
}

const cerberus_recon_record_t *cerberus_recon_journal_peek(const cerberus_recon_journal_t *j)
{
    return journal_valid(j) && j->count ? &j->rows[j->head] : NULL;
}

bool cerberus_recon_journal_commit(cerberus_recon_journal_t *j, uint64_t expected_sequence)
{
    const cerberus_recon_record_t *head = cerberus_recon_journal_peek(j);
    if (!head || !expected_sequence || head->sequence != expected_sequence) return false;
    if (head->radio == CERBERUS_RECON_RADIO_WIFI) increment(&j->counters.written_wifi_rows);
    else {
        increment(&j->counters.written_ble_rows);
        if (head->ibeacon) increment(&j->counters.written_ibeacon_rows);
    }
    memset(&j->rows[j->head], 0, sizeof(j->rows[j->head]));
    j->head = (uint16_t)((j->head + 1U) % CERBERUS_RECON_JOURNAL_CAPACITY);
    --j->count;
    increment(&j->counters.written);
    return true;
}

void cerberus_recon_journal_abandon(cerberus_recon_journal_t *j)
{
    if (!journal_valid(j)) return;
    add_saturated(&j->counters.abandoned, j->count);
    memset(j->rows, 0, sizeof(j->rows));
    j->head = j->count = 0U;
}

typedef struct {
    char *out;
    size_t capacity, used;
    bool failed;
} writer_t;

static void append(writer_t *w, const char *format, ...)
{
    if (w->failed) return;
    va_list args;
    va_start(args, format);
    int length = vsnprintf(w->out ? w->out + w->used : NULL,
        w->out ? w->capacity - w->used : 0U, format, args);
    va_end(args);
    if (length < 0 || (size_t)length > SIZE_MAX - w->used ||
        (w->out && (size_t)length >= w->capacity - w->used)) {
        w->failed = true;
        return;
    }
    w->used += (size_t)length;
}

static void optional_u64(writer_t *w, bool valid, uint64_t value)
{
    if (valid) append(w, "%" PRIu64, value);
}

const char *cerberus_recon_csv_header(void)
{
    return "session_id,sequence,s3_consumed_us,radio,address,address_type,generation,"
        "channel,rssi_dbm,rssi_scope,source_kind,source_time,source_anchor_s3_us,source_sequence,"
        "source_flags,source_limits,received_name_escaped,classified,category,confidence,rule,mode,"
        "alert_state,ibeacon\r\n";
}

static const char *source_label(uint8_t source)
{
    switch (source) {
    case CERBERUS_RECON_SOURCE_S3_ADV_MS32: return "s3_adv_uptime_ms32";
    case CERBERUS_RECON_SOURCE_S3_RESPONSE_MS32: return "s3_rsp_uptime_ms32";
    case CERBERUS_RECON_SOURCE_C5_AGE_MS: return "c5_age_ms_at_s3_request";
    case CERBERUS_RECON_SOURCE_S3_CALLBACK_US: return "s3_callback_uptime_us";
    default: return "unavailable";
    }
}

static const char *alert_label(uint8_t state)
{
    return state == CERBERUS_RECON_ALERT_QUEUED ? "queued" :
        state == CERBERUS_RECON_ALERT_NOT_QUEUED ? "not_queued" : "not_evaluated";
}

static void row_render(writer_t *w, const cerberus_recon_record_t *r)
{
    uint8_t address[6];
    for (size_t i = 0U; i < sizeof(address); ++i)
        address[i] = r->address[r->radio == CERBERUS_RECON_RADIO_BLE ? 5U - i : i];
    append(w, "%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%s,"
        "%02X:%02X:%02X:%02X:%02X:%02X,%u,%u,",
        r->session_id, r->sequence, r->consumed_us,
        r->radio == CERBERUS_RECON_RADIO_WIFI ? "wifi" : "ble",
        address[0], address[1], address[2], address[3], address[4], address[5],
        r->address_type, r->generation);
    if (r->channel_valid) append(w, "%u", r->channel);
    append(w, ",");
    if (r->rssi_valid) append(w, "%d", r->rssi);
    append(w, ",%s,%s,", r->radio == CERBERUS_RECON_RADIO_WIFI ?
        "driver_aggregate" : "sweep_peak", source_label(r->source_kind));
    optional_u64(w, r->source_flags & CERBERUS_RECON_SOURCE_TIME_VALID, r->source_time);
    append(w, ",");
    optional_u64(w, r->source_flags & CERBERUS_RECON_SOURCE_ANCHOR_VALID, r->source_anchor_us);
    append(w, ",");
    optional_u64(w, r->source_flags & CERBERUS_RECON_SOURCE_SEQUENCE_VALID, r->source_sequence);
    append(w, ",%u,%" PRIu32 ",\"bytes:", r->source_flags, r->source_limits);
    for (uint8_t i = 0U; i < r->name_length; ++i) {
        uint8_t c = r->name[i];
        if (c == '"') append(w, "\"\"");
        else if (c == '\\') append(w, "\\\\");
        else if (c >= 32U && c <= 126U) append(w, "%c", c);
        else append(w, "\\x%02X", c);
    }
    append(w, "\",%u,", r->classified ? 1U : 0U);
    if (r->classified) append(w, "%u,%u,%s,%u", r->category, r->confidence, r->rule, r->mode);
    else append(w, ",,,");
    append(w, ",%s,%u\r\n", alert_label(r->alert_state), r->ibeacon ? 1U : 0U);
}

size_t cerberus_recon_row_format(const cerberus_recon_record_t *r,
    char *out, size_t capacity)
{
    if (out && capacity) out[0] = '\0';
    if (!out || !capacity || !cerberus_recon_record_valid(r)) return 0U;
    writer_t measure = {0};
    row_render(&measure, r);
    if (measure.failed || measure.used >= capacity) return 0U;
    writer_t writer = {.out = out, .capacity = capacity};
    row_render(&writer, r);
    if (writer.failed || writer.used != measure.used) { out[0] = '\0'; return 0U; }
    return writer.used;
}

static bool counters_saturated(const cerberus_recon_counters_t *c)
{
    return c->accepted == UINT32_MAX || c->written == UINT32_MAX ||
        c->dropped == UINT32_MAX || c->invalid == UINT32_MAX || c->abandoned == UINT32_MAX ||
        c->accepted_wifi_rows == UINT32_MAX || c->accepted_ble_rows == UINT32_MAX ||
        c->accepted_ibeacon_rows == UINT32_MAX || c->written_wifi_rows == UINT32_MAX ||
        c->written_ble_rows == UINT32_MAX || c->written_ibeacon_rows == UINT32_MAX;
}

static uint32_t bounded_count(uint64_t value)
{
    return value > UINT32_MAX ? UINT32_MAX : (uint32_t)value;
}

static bool row_counters_valid(const cerberus_recon_counters_t *c, uint16_t queued)
{
    if (bounded_count((uint64_t)c->accepted_wifi_rows + c->accepted_ble_rows) != c->accepted ||
        bounded_count((uint64_t)c->written_wifi_rows + c->written_ble_rows) != c->written ||
        c->written_wifi_rows > c->accepted_wifi_rows || c->written_ble_rows > c->accepted_ble_rows ||
        c->accepted_ibeacon_rows > c->accepted_ble_rows ||
        c->written_ibeacon_rows > c->written_ble_rows ||
        c->written_ibeacon_rows > c->accepted_ibeacon_rows) return false;
    /* Before BLE admission saturates, the unwritten iBeacon subset cannot
     * exceed all unwritten BLE rows. After saturation that difference is not
     * an exact count and must not be presented or checked as one. */
    if (c->accepted_ble_rows != UINT32_MAX &&
        c->accepted_ibeacon_rows - c->written_ibeacon_rows >
            c->accepted_ble_rows - c->written_ble_rows) return false;
    uint32_t ble_unwritten = c->accepted_ble_rows - c->written_ble_rows;
    uint32_t ibeacon_unwritten = c->accepted_ibeacon_rows - c->written_ibeacon_rows;
    if (ibeacon_unwritten > ble_unwritten) ble_unwritten = ibeacon_unwritten;
    uint64_t minimum_unwritten = (uint64_t)c->accepted_wifi_rows - c->written_wifi_rows +
        ble_unwritten;
    uint32_t remaining = bounded_count((uint64_t)c->abandoned + queued);
    if (bounded_count(minimum_unwritten) > remaining) return false;
    /* Without accepted-kind saturation these differences are exact, even
     * when their combined accepted/written totals have saturated. */
    if (c->accepted_wifi_rows != UINT32_MAX && c->accepted_ble_rows != UINT32_MAX &&
        bounded_count(minimum_unwritten) != remaining) return false;
    return true;
}

static bool summary_valid(const cerberus_recon_summary_t *s)
{
    if (!s || !s->session_id || s->queued > CERBERUS_RECON_JOURNAL_CAPACITY ||
        s->state < CERBERUS_RECON_MANIFEST_ACTIVE || s->state > CERBERUS_RECON_MANIFEST_ERROR ||
        s->session_name[0] != 'R' || s->session_name[6]) return false;
    bool nonzero = false;
    for (size_t i = 1U; i < 6U; ++i) {
        if (s->session_name[i] < '0' || s->session_name[i] > '9') return false;
        nonzero |= s->session_name[i] != '0';
    }
    if (!nonzero) return false;
    if (s->state == CERBERUS_RECON_MANIFEST_ACTIVE) {
        if (s->ended_us || s->error_code) return false;
    } else if (s->ended_us < s->started_us) return false;
    if (s->state == CERBERUS_RECON_MANIFEST_COMPLETE &&
        (s->queued || s->counters.abandoned || s->error_code)) return false;
    if (s->state == CERBERUS_RECON_MANIFEST_ERROR && !s->error_code) return false;
    if (!row_counters_valid(&s->counters, s->queued) ||
        bounded_count((uint64_t)s->counters.written + s->counters.abandoned + s->queued) !=
            s->counters.accepted)
        return false;
    if (s->state == CERBERUS_RECON_MANIFEST_COMPLETE &&
        (s->counters.accepted_wifi_rows != s->counters.written_wifi_rows ||
         s->counters.accepted_ble_rows != s->counters.written_ble_rows ||
         s->counters.accepted_ibeacon_rows != s->counters.written_ibeacon_rows)) return false;
    if (!s->last_sequence && (s->counters.accepted || s->counters.dropped)) return false;
    return true;
}

static void manifest_render(writer_t *w, const cerberus_recon_summary_t *s)
{
    const char *state = s->state == CERBERUS_RECON_MANIFEST_ACTIVE ? "active" :
        s->state == CERBERUS_RECON_MANIFEST_COMPLETE ? "complete" : "error";
    append(w, "{\n  \"schema\":\"cerberus_recon_completed_sweep_v1\",\n"
        "  \"session_name\":\"%s\",\n  \"session_id\":%" PRIu64 ",\n  \"state\":\"%s\",\n"
        "  \"started_s3_us\":%" PRIu64 ",\n  \"ended_s3_us\":",
        s->session_name, s->session_id, state, s->started_us);
    if (s->state == CERBERUS_RECON_MANIFEST_ACTIVE) append(w, "null");
    else append(w, "%" PRIu64, s->ended_us);
    append(w, ",\n  \"last_sequence\":%" PRIu64 ",\n"
        "  \"accepted\":%" PRIu32 ",\n  \"written\":%" PRIu32 ",\n"
        "  \"dropped\":%" PRIu32 ",\n  \"invalid\":%" PRIu32 ",\n"
        "  \"abandoned\":%" PRIu32 ",\n  \"queued\":%u,\n"
        "  \"counter_lower_bounds\":%s,\n  \"error_code\":%" PRId32 ",\n",
        s->last_sequence, s->counters.accepted, s->counters.written, s->counters.dropped,
        s->counters.invalid, s->counters.abandoned, s->queued,
        counters_saturated(&s->counters) ? "true" : "false", s->error_code);
    append(w, "  \"row_counter_version\":1,\n"
        "  \"accepted_wifi_rows\":%" PRIu32 ",\n  \"accepted_ble_rows\":%" PRIu32 ",\n"
        "  \"accepted_ibeacon_rows\":%" PRIu32 ",\n"
        "  \"written_wifi_rows\":%" PRIu32 ",\n  \"written_ble_rows\":%" PRIu32 ",\n"
        "  \"written_ibeacon_rows\":%" PRIu32 ",\n"
        "  \"row_counter_scope\":\"journal rows including repeats; iBeacon is a BLE hint subset; accepted includes unwritten rows; saturated differences are not exact\",\n",
        s->counters.accepted_wifi_rows, s->counters.accepted_ble_rows,
        s->counters.accepted_ibeacon_rows, s->counters.written_wifi_rows,
        s->counters.written_ble_rows, s->counters.written_ibeacon_rows);
    append(w, "  \"observations\":\"OBS.CSV\",\n"
        "  \"session_identity\":\"durable folder plus boot-local session_id; IDs restart after reboot\",\n"
        "  \"scope\":\"retained rows from completed sweeps; repeats across sweeps retained\",\n"
        "  \"time_basis\":\"S3 consumption uptime is not RF time or UTC; source units and domain are separate\",\n"
        "  \"rssi_basis\":\"Wi-Fi driver aggregate or BLE sweep peak, not necessarily measured with source evidence\",\n"
        "  \"identity\":\"typed broadcast addresses and unauthenticated hints, not physical-device identity\",\n"
        "  \"coverage\":\"bounded receiver inventories and queue; not every RF callback or packet\",\n"
        "  \"name_encoding\":\"bytes: prefix; backslash doubled; nonprinting/high bytes use backslash-x-hex; CSV quote doubling\",\n"
        "  \"alert_state\":\"queued, not_queued or not_evaluated; no inferred suppression reason\",\n"
        "  \"gps\":false\n}\n");
}

size_t cerberus_recon_manifest_format(const cerberus_recon_summary_t *s,
    char *out, size_t capacity)
{
    if (out && capacity) out[0] = '\0';
    if (!out || !capacity || !summary_valid(s)) return 0U;
    writer_t measure = {0};
    manifest_render(&measure, s);
    if (measure.failed || measure.used >= capacity) return 0U;
    writer_t writer = {.out = out, .capacity = capacity};
    manifest_render(&writer, s);
    if (writer.failed || writer.used != measure.used) { out[0] = '\0'; return 0U; }
    return writer.used;
}
