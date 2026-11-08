#include "probe_sniff_model.h"
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static bool overlaps(const void *a, size_t an, const void *b, size_t bn)
{
    if (!a || !b || !an || !bn) return false;
    uintptr_t x = (uintptr_t)a, y = (uintptr_t)b;
    return x <= y ? y - x < an : x - y < bn;
}

static bool same_key(const cerberus_probe_sniff_row_t *a,
    const cerberus_probe_sniff_row_t *b, cerberus_probe_sniff_group_t group)
{
    bool mac = memcmp(a->transmitter, b->transmitter, sizeof(a->transmitter)) == 0;
    bool name = a->ssid_length == b->ssid_length &&
        memcmp(a->ssid, b->ssid, a->ssid_length) == 0;
    return group == CERBERUS_PROBE_SNIFF_SENDERS ? mac :
        group == CERBERUS_PROBE_SNIFF_NETWORKS ? name : mac && name;
}

bool cerberus_probe_sniff_snapshot_valid(const cerberus_probe_sniff_snapshot_t *s)
{
    if (!s || !cerberus_probe_sniff_control_valid(&s->control, false, false,
        s->control.session_id) || !s->control.session_id || !(s->control.flags & CERBERUS_PROBE_SNIFF_BOUND) ||
        s->control.flags & ~(CERBERUS_PROBE_SNIFF_BOUND | CERBERUS_PROBE_SNIFF_OWNED |
            CERBERUS_PROBE_SNIFF_RECEIVING | CERBERUS_PROBE_SNIFF_DATA_READY |
            CERBERUS_PROBE_SNIFF_OVERFLOW | CERBERUS_PROBE_SNIFF_SATURATED) ||
        s->control.state > CERBERUS_PROBE_SNIFF_ERROR ||
        s->control.mode > CERBERUS_PROBE_SNIFF_HOP_BOTH ||
        (s->control.mode == CERBERUS_PROBE_SNIFF_FIXED ?
            !cerberus_probe_sniff_channel_valid(s->requested_channel) : s->requested_channel != 0U) ||
        s->control.capacity != CERBERUS_PROBE_SNIFF_CAPACITY ||
        s->control.count > CERBERUS_PROBE_SNIFF_CAPACITY ||
        s->count > s->control.count || !cerberus_probe_sniff_summary_valid(&s->summary) ||
        s->count > s->summary.count || s->summary.revision > s->control.revision ||
        ((s->control.flags & CERBERUS_PROBE_SNIFF_RECEIVING) &&
            !(s->control.flags & CERBERUS_PROBE_SNIFF_OWNED))) return false;
    if (s->frozen && (!(s->summary.flags & CERBERUS_PROBE_SNIFF_STOPPED) ||
        !(s->control.flags & CERBERUS_PROBE_SNIFF_DATA_READY) ||
        (s->control.flags & (CERBERUS_PROBE_SNIFF_OWNED | CERBERUS_PROBE_SNIFF_RECEIVING)) ||
        (s->control.state != CERBERUS_PROBE_SNIFF_COMPLETE &&
            s->control.state != CERBERUS_PROBE_SNIFF_ERROR) ||
        s->count != s->control.count || s->count != s->summary.count ||
        s->summary.revision != s->control.revision)) return false;
    uint64_t frames = 0U, retries = 0U;
    for (uint8_t i = 0U; i < s->count; ++i) {
        const cerberus_probe_sniff_row_t *r = &s->rows[i].row;
        if (!cerberus_probe_sniff_projection_valid(&s->rows[i]) ||
            ((s->summary.flags & CERBERUS_PROBE_SNIFF_START_TIME_VALID) &&
                r->first_callback_us < s->summary.started_source_us) ||
            ((s->summary.flags & CERBERUS_PROBE_SNIFF_STOP_TIME_VALID) &&
                r->last_callback_us > s->summary.stopped_source_us)) return false;
        for (uint8_t prior = 0U; prior < i; ++prior)
            if (same_key(r, &s->rows[prior].row, CERBERUS_PROBE_SNIFF_PAIRS)) return false;
        frames += r->frames;
        retries += r->retry_frames;
    }
    uint32_t retained = frames > UINT32_MAX ? UINT32_MAX : (uint32_t)frames;
    if (s->frozen ? retained != s->summary.retained_frames :
        (s->summary.retained_frames != UINT32_MAX && frames > s->summary.retained_frames)) return false;
    if (s->summary.retry_frames != UINT32_MAX && retries > s->summary.retry_frames) return false;
    return true;
}

uint8_t cerberus_probe_sniff_order(const cerberus_probe_sniff_snapshot_t *s,
    cerberus_probe_sniff_group_t group, uint8_t indexes[CERBERUS_PROBE_SNIFF_CAPACITY])
{
    if (!s || !indexes || overlaps(s, sizeof(*s), indexes, CERBERUS_PROBE_SNIFF_CAPACITY) ||
        s->count > CERBERUS_PROBE_SNIFF_CAPACITY ||
        (unsigned)group > CERBERUS_PROBE_SNIFF_NETWORKS) return 0U;
    for (uint8_t i = 0U; i < s->count; ++i)
        if (s->rows[i].row.ssid_length > sizeof(s->rows[i].row.ssid)) return 0U;
    uint8_t count = 0U;
    for (uint8_t i = 0U; i < s->count; ++i) {
        bool found = false;
        if (group != CERBERUS_PROBE_SNIFF_PAIRS)
            for (uint8_t j = 0U; j < count; ++j)
                if (same_key(&s->rows[i].row, &s->rows[indexes[j]].row, group)) {
                    found = true; break;
                }
        if (!found) indexes[count++] = i;
    }
    return count;
}

bool cerberus_probe_sniff_group_view(const cerberus_probe_sniff_snapshot_t *s,
    cerberus_probe_sniff_group_t group, uint8_t representative,
    cerberus_probe_sniff_group_view_t *out)
{
    if (!out || overlaps(s, s ? sizeof(*s) : 0U, out, sizeof(*out))) return false;
    memset(out, 0, sizeof(*out));
    if (!s || s->count > CERBERUS_PROBE_SNIFF_CAPACITY || representative >= s->count ||
        (unsigned)group > CERBERUS_PROBE_SNIFF_NETWORKS) return false;
    for (uint8_t i = 0U; i < s->count; ++i)
        if (s->rows[i].row.ssid_length > sizeof(s->rows[i].row.ssid)) return false;
    out->representative = out->latest = representative;
    out->first_callback_us = UINT64_MAX;
    uint64_t frames = 0U, retries = 0U;
    for (uint8_t i = 0U; i < s->count; ++i) {
        const cerberus_probe_sniff_row_t *r = &s->rows[i].row;
        if (group == CERBERUS_PROBE_SNIFF_PAIRS ? i != representative :
            !same_key(r, &s->rows[representative].row, group)) continue;
        ++out->pairs;
        frames += r->frames; retries += r->retry_frames;
        if (r->first_callback_us < out->first_callback_us) out->first_callback_us = r->first_callback_us;
        if (r->last_callback_us > out->last_callback_us) {
            out->last_callback_us = r->last_callback_us; out->latest = i;
        }
        if (r->flags & CERBERUS_PROBE_SNIFF_ROW_SATURATED) out->saturated = true;
    }
    out->frames = frames > UINT32_MAX ? UINT32_MAX : (uint32_t)frames;
    out->retry_frames = retries > UINT32_MAX ? UINT32_MAX : (uint32_t)retries;
    out->saturated |= frames >= UINT32_MAX || retries >= UINT32_MAX;
    return true;
}

void cerberus_probe_sniff_name(const cerberus_probe_sniff_row_t *r, char *text, size_t capacity)
{
    if (!text || !capacity) return;
    text[0] = '\0';
    if (!r || r->ssid_length > sizeof(r->ssid)) return;
    if (!r->ssid_length) { (void)snprintf(text, capacity, "[WILDCARD]"); return; }
    size_t n = r->ssid_length < capacity - 1U ? r->ssid_length : capacity - 1U;
    for (size_t i = 0U; i < n; ++i)
        text[i] = r->ssid[i] >= 32U && r->ssid[i] <= 126U ? (char)r->ssid[i] : '.';
    text[n] = '\0';
}

typedef struct { char *out; size_t capacity, used; bool okay; } report_writer_t;
static void append(report_writer_t *w, const char *format, ...)
{
    if (!w->okay) return;
    va_list args;
    va_start(args, format);
    size_t available = w->out ? w->capacity - w->used : 0U;
    int n = vsnprintf(w->out ? w->out + w->used : NULL, available, format, args);
    va_end(args);
    if (n < 0 || (size_t)n > CERBERUS_PROBE_SNIFF_REPORT_MAX_BYTES - w->used ||
        (w->out && (size_t)n >= available)) { w->okay = false; return; }
    w->used += (size_t)n;
}

static bool format_report(const cerberus_probe_sniff_snapshot_t *s, char *output,
    size_t capacity, size_t *length)
{
    report_writer_t w = {output, capacity, 0U, true};
    const cerberus_probe_sniff_summary_t *m = &s->summary;
    append(&w, "# Cerberus passive Probe Sniff evidence v1\r\n"
        "# C5 callback observations, including repeats; not unique devices or raw frames.\r\n"
        "# Exact MAC and SSID bytes are retained. Source times are C5 uptime, not UTC/RF time.\r\n"
        "session_id,%" PRIu64 "\r\nsource_error,%" PRId32 "\r\nmode,%u\r\nrequested_channel,%u\r\nstate,%u\r\n"
        "revision,%" PRIu32 "\r\nretained_pairs,%u\r\ncapacity,%u\r\nsummary_flags,%u\r\n",
        s->control.session_id, s->control.error_code, s->control.mode, s->requested_channel, s->control.state,
        m->revision, s->count, m->capacity, m->flags);
    append(&w, "started_source_us,%" PRIu64 "\r\nstopped_source_us,%" PRIu64
        "\r\ncopied_source_us,%" PRIu64 "\r\nvalid_callbacks,%" PRIu32
        "\r\nretained_callbacks,%" PRIu32 "\r\ncapacity_dropped_callbacks,%" PRIu32
        "\r\nretry_bit_callbacks,%" PRIu32 "\r\nmalformed_callbacks,%" PRIu32
        "\r\nunsupported_callbacks,%" PRIu32 "\r\nclock_dropped_callbacks,%" PRIu32 "\r\n",
        m->started_source_us, m->stopped_source_us, m->copied_source_us,
        m->valid_frames, m->retained_frames, m->capacity_dropped_frames, m->retry_frames,
        m->malformed_frames, m->unsupported_frames, m->clock_dropped_frames);
    append(&w, "attempted_channel_mask,%016" PRIX64 "\r\nsuccessful_channel_mask,%016" PRIX64
        "\r\nchannel_failures,%" PRIu32 "\r\ndwell_ms,%u\r\n"
        "# Channel mask bits use the reported channel map below; tuning does not prove complete RF coverage.\r\nchannel_map",
        m->attempted_channels, m->successful_channels, m->channel_failures, m->dwell_ms);
    for (uint8_t i = 0U; i < CERBERUS_PROBE_SNIFF_CHANNEL_COUNT; ++i)
        append(&w, ",%u", cerberus_probe_sniff_channel_at(i));
    append(&w, "\r\n# Row flags:1=latest retry bit,2=saturated counts,4=valid source copy time.\r\n"
        "transmitter,ssid_length,ssid_hex,last_channel,last_rssi_dbm,flags,first_callback_us,last_callback_us,copied_source_us,callbacks,retry_bit_callbacks,last_sequence_control\r\n");
    static const char hex[] = "0123456789ABCDEF";
    for (uint8_t i = 0U; i < s->count; ++i) {
        const cerberus_probe_sniff_row_t *r = &s->rows[i].row;
        char name[65];
        for (uint8_t j = 0U; j < r->ssid_length; ++j) {
            name[2U*j] = hex[r->ssid[j] >> 4U]; name[2U*j+1U] = hex[r->ssid[j] & 15U];
        }
        name[2U*r->ssid_length] = '\0';
        append(&w, "%02X:%02X:%02X:%02X:%02X:%02X,%u,%s,%u,%d,%u,%" PRIu64
            ",%" PRIu64 ",%" PRIu64 ",%" PRIu32 ",%" PRIu32 ",%u\r\n",
            r->transmitter[0], r->transmitter[1], r->transmitter[2], r->transmitter[3],
            r->transmitter[4], r->transmitter[5], r->ssid_length, name, r->last_channel,
            r->last_rssi, r->flags, r->first_callback_us, r->last_callback_us,
            s->rows[i].copied_source_us, r->frames, r->retry_frames, r->last_sequence_control);
    }
    *length = w.okay ? w.used : 0U;
    return w.okay;
}

bool cerberus_probe_sniff_report(const cerberus_probe_sniff_snapshot_t *s,
    char *output, size_t capacity, size_t *length)
{
    if (!length || overlaps(s, s ? sizeof(*s) : 0U, length, sizeof(*length)) ||
        overlaps(output, capacity, length, sizeof(*length))) return false;
    *length = 0U;
    if (!s || !s->frozen || !cerberus_probe_sniff_snapshot_valid(s) ||
        (!output && capacity)) return false;
    size_t needed = 0U;
    if (!format_report(s, NULL, 0U, &needed)) return false;
    if (!output) { *length = needed; return true; }
    if (capacity <= needed) return false; /* room for formatter's terminal NUL */
    if (overlaps(s, sizeof(*s), output, capacity)) return false;
    return format_report(s, output, capacity, length);
}
