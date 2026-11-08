#include "wifi_packet_count_diagnostics.h"
#include "freertos/FreeRTOS.h"

_Static_assert(sizeof(wifi_packet_count_diagnostics_t) <= 640U,
    "Keep Packet Count diagnostic copy bounded");
static portMUX_TYPE diagnostics_lock = portMUX_INITIALIZER_UNLOCKED;
static wifi_packet_count_diagnostics_t published;

static bool same_progress(const wifi_packet_count_source_diagnostics_t *a,
    const wifi_packet_count_source_diagnostics_t *b)
{
    return a->requested_session == b->requested_session && a->accepted_session == b->accepted_session &&
        a->phase == b->phase && a->upload_index == b->upload_index &&
        a->kind == b->kind && a->index == b->index && a->part == b->part &&
        a->published_valid == b->published_valid && a->published_session == b->published_session &&
        a->published_revision == b->published_revision;
}

static void publish_copy(wifi_packet_count_diagnostics_t *copy, uint64_t now_ms)
{
    portENTER_CRITICAL(&diagnostics_lock);
    uint64_t sequence = published.publication_sequence;
    bool saturated = published.sequence_saturated;
    if (sequence != UINT64_MAX) ++sequence;
    else saturated = true;
    bool time_valid = now_ms >= published.published_ms;
    uint64_t timestamp = time_valid ? now_ms : published.published_ms;
    if (copy->allocated) {
        copy->tick_gap_valid = time_valid && published.allocated && copy->last_tick_ms >= published.last_tick_ms;
        copy->max_tick_gap_ms = published.allocated ? published.max_tick_gap_ms : 0;
        if (copy->tick_gap_valid) {
            copy->tick_gap_ms = copy->last_tick_ms - published.last_tick_ms;
            if (copy->tick_gap_ms > copy->max_tick_gap_ms) copy->max_tick_gap_ms = copy->tick_gap_ms;
        }
        for (unsigned i = 0; i < WIFI_PACKET_COUNT_SOURCES; ++i) {
            wifi_packet_count_source_diagnostics_t *s = &copy->source[i];
            bool changed = !published.allocated || !same_progress(s, &published.source[i]);
            s->progress_time_valid = time_valid && (changed || published.source[i].progress_time_valid);
            s->last_progress_ms = time_valid && changed ?
                now_ms : published.source[i].last_progress_ms;
        }
    }
    published = *copy;
    published.publication_sequence = sequence;
    published.sequence_saturated = saturated;
    published.published_ms = timestamp;
    published.time_valid = time_valid;
    published.available = true;
    portEXIT_CRITICAL(&diagnostics_lock);
}

void wifi_packet_count_diagnostics_publish(const wifi_packet_count_scan_t *scan,
    const wifi_packet_count_record_status_t *record, uint64_t now_ms,
    uint64_t job, bool start_pending, bool closing, esp_err_t ui_error)
{
    if (!scan) { wifi_packet_count_diagnostics_clear(now_ms); return; }
    wifi_packet_count_diagnostics_t copy = {0};
    copy.allocated = true;
    copy.last_tick_ms = scan->last_tick_ms;
    copy.job = job;
    copy.channel_mask = scan->requested_channels;
    copy.segments = scan->segments;
    copy.interruptions = scan->interruptions;
    copy.ui_error = ui_error;
    copy.error = scan->error;
    copy.s3_admission_error = scan->s3_admission_error;
    copy.dwell_ms = scan->dwell_ms;
    copy.target_count = scan->count;
    copy.start_pending = start_pending;
    copy.closing = closing;
    copy.requested = scan->requested;
    copy.retuning = scan->retuning;
    copy.frozen = scan->frozen;
    copy.fallback = scan->fallback;
    copy.recording = scan->recording;
    copy.record_valid = record && job && record->job == job;
    if (copy.record_valid) {
        copy.record_error = record->error;
        copy.record_busy = record->busy;
        copy.record_ready = record->ready;
        copy.record_finishing = record->finishing;
    }
    for (unsigned i = 0; i < WIFI_PACKET_COUNT_SOURCES; ++i) {
        const wifi_packet_count_source_t *s = &scan->sources[i];
        wifi_packet_count_source_diagnostics_t *d = &copy.source[i];
        d->requested_session = s->request.session_id;
        d->accepted_session = s->session;
        d->channel_mask = s->request.channel_mask;
        d->last_status_ms = s->last_status_ms;
        d->next_action_ms = s->next_action_ms;
        d->pin = s->pin;
        d->revision_floor = s->revision_floor;
        d->error = s->error;
        d->part = s->part;
        d->phase = s->phase;
        d->upload_index = s->upload_index;
        d->kind = s->kind;
        d->index = s->index;
        d->target_count = s->request.target_count;
        d->attempted = s->attempted;
        d->accepted = s->accepted;
        d->start_attempted = s->start_attempted;
        d->control_valid = s->have_control;
        d->stop = s->stop;
        d->close = s->close;
        d->faulted = s->faulted;
        d->clock_invalid = s->clock_invalid;
        d->record_bound = s->record_bound;
        if (d->control_valid) {
            d->state = s->remote.state;
            d->flags = s->remote.flags;
            d->channel = s->remote.channel;
            d->configured = s->remote.configured;
            d->remote_error = s->remote.error_code;
        }
        d->published_valid = s->published.valid;
        if (d->published_valid) {
            d->published_session = s->published.session;
            d->published_revision = s->published.summary.revision;
            d->published_frozen = s->published.frozen;
            d->published_error = s->published.source_error;
            d->callbacks = s->published.summary.callbacks;
            d->matched = s->published.summary.matched_frames;
            d->invalid_metadata = s->published.summary.invalid_metadata;
            d->valid_frames = s->published.summary.valid_frames;
            d->malformed_frames = s->published.summary.malformed_frames;
            d->unsupported_frames = s->published.summary.unsupported_frames;
            d->outside_window_frames = s->published.summary.outside_window_frames;
            d->clock_dropped_frames = s->published.summary.clock_dropped_frames;
            d->raw_dropped_full = s->published.recording.dropped_full;
            d->raw_dropped_other = s->published.recording.dropped_other;
        }
        if (copy.record_valid) {
            const wifi_packet_count_record_source_t *r = &record->source[i];
            d->record_session = r->session;
            d->written = r->written;
            d->discarded = r->discarded;
            d->transport_errors = r->transport_errors;
            d->record_error = r->error;
            d->recorder_bound = r->bound;
            d->recorder_draining = r->draining;
            d->recorder_drained = r->drained;
            d->record_loss_unknown = r->loss_unknown;
        }
    }
    publish_copy(&copy, now_ms);
}

void wifi_packet_count_diagnostics_clear(uint64_t now_ms)
{
    wifi_packet_count_diagnostics_t copy = {0};
    publish_copy(&copy, now_ms);
}

void wifi_packet_count_diagnostics_get(wifi_packet_count_diagnostics_t *out)
{
    if (!out) return;
    portENTER_CRITICAL(&diagnostics_lock);
    *out = published;
    portEXIT_CRITICAL(&diagnostics_lock);
}
