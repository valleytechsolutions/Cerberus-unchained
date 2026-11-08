#include "wardrive_compact.h"
#include "wardrive_log.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#define SORT_ROWS 512U
#define MERGE_ROWS 64U

typedef struct {
    cerberus_ward_log_identity_t identity;
    uint32_t offset;
    uint16_t length;
    uint16_t reserved;
} row_ref_t;
_Static_assert(sizeof(row_ref_t) == 16U, "bounded on-card row index");

typedef struct {
    row_ref_t rows[MERGE_ROWS];
    uint32_t next, remaining;
    unsigned used, count;
} cursor_t;

typedef struct {
    FILE *file;
    row_ref_t *rows;
    unsigned used;
    uint32_t count;
    cerberus_wardrive_compact_stats_t *stats;
} writer_t;
typedef struct {
    cerberus_ward_log_parser_t parser;
    char line[CERBERUS_WARD_LOG_LINE_CAPACITY];
    union {
        row_ref_t sort[SORT_ROWS];
        struct { cursor_t left, right; row_ref_t output[MERGE_ROWS]; } merge;
    } data;
} workspace_t;
_Static_assert(sizeof(((workspace_t *)0)->data) == SORT_ROWS * sizeof(row_ref_t),
    "merge buffers must fit in the existing sort workspace");

size_t cerberus_wardrive_compact_workspace_size(void) { return sizeof(workspace_t); }

static int key_compare(const row_ref_t *a, const row_ref_t *b)
{
    int difference = memcmp(a->identity.address, b->identity.address, 6);
    return difference ? difference : (int)a->identity.type - (int)b->identity.type;
}

static int row_compare(const void *left, const void *right)
{
    const row_ref_t *a = left, *b = right;
    int difference = key_compare(a, b);
    if (!difference) difference = (int)a->identity.rssi - (int)b->identity.rssi;
    if (!difference) difference = a->offset < b->offset ? -1 : a->offset > b->offset;
    return difference;
}

static bool index_read(FILE *file, void *data, size_t bytes, cerberus_wardrive_compact_stats_t *stats)
{
    size_t received = fread(data, 1U, bytes, file);
    stats->index_read_bytes += received;
    return received == bytes;
}

static bool index_write(FILE *file, const void *data, size_t bytes, cerberus_wardrive_compact_stats_t *stats)
{
    size_t written = fwrite(data, 1U, bytes, file);
    stats->index_write_bytes += written;
    return written == bytes;
}

/* Each run has a row count because reduction makes run lengths unequal.
 * Later passes use their in-memory run/ref totals, never stale file tails. */
static bool cursor_open(FILE *file, cursor_t *cursor, uint32_t *next,
    uint32_t available, cerberus_wardrive_compact_stats_t *stats)
{
    uint32_t count;
    if (fseek(file, (long)*next, SEEK_SET) || !index_read(file, &count, sizeof(count), stats) ||
        !count || count > available) return false;
    uint64_t end = (uint64_t)*next + sizeof(count) + (uint64_t)count * sizeof(row_ref_t);
    if (end > INT32_MAX) return false; /* ESP-IDF's seek offset is a signed long. */
    *cursor = (cursor_t){.next = *next + sizeof(count), .remaining = count};
    *next = (uint32_t)end;
    return true;
}

static bool cursor_fill(FILE *file, cursor_t *cursor, cerberus_wardrive_compact_stats_t *stats)
{
    if (cursor->used != cursor->count || !cursor->remaining) return true;
    uint32_t count = cursor->remaining;
    if (count > MERGE_ROWS) count = MERGE_ROWS;
    if (fseek(file, (long)cursor->next, SEEK_SET) ||
        !index_read(file, cursor->rows, count * sizeof(row_ref_t), stats)) return false;
    cursor->next += count * sizeof(row_ref_t);
    cursor->remaining -= count;
    cursor->count = count;
    cursor->used = 0;
    return true;
}

static bool writer_flush(writer_t *writer)
{
    if (writer->used && !index_write(writer->file, writer->rows,
            writer->used * sizeof(row_ref_t), writer->stats)) return false;
    writer->used = 0;
    return true;
}

static bool writer_push(writer_t *writer, const row_ref_t *row)
{
    writer->rows[writer->used++] = *row;
    ++writer->count;
    return writer->used < MERGE_ROWS || writer_flush(writer);
}

static bool writer_pair(writer_t *writer, const row_ref_t *low, const row_ref_t *high)
{
    return writer_push(writer, low) && (low->offset == high->offset || writer_push(writer, high));
}

static uint32_t reduce_sorted(row_ref_t *rows, uint32_t count)
{
    uint32_t kept = 0;
    for (uint32_t i = 0; i < count;) {
        row_ref_t low = rows[i++], high = low;
        while (i < count && !key_compare(&low, &rows[i])) {
            if (rows[i].identity.rssi > high.identity.rssi) high = rows[i];
            ++i;
        }
        rows[kept++] = low;
        if (low.offset != high.offset) rows[kept++] = high;
    }
    return kept;
}

static bool write_sorted_run(FILE *file, workspace_t *w, unsigned count,
    cerberus_wardrive_compact_stats_t *stats)
{
    qsort(w->data.sort, count, sizeof(row_ref_t), row_compare);
    uint32_t kept = reduce_sorted(w->data.sort, count);
    if (!index_write(file, &kept, sizeof(kept), stats) ||
        !index_write(file, w->data.sort, kept * sizeof(row_ref_t), stats)) return false;
    stats->initial_refs += kept;
    return true;
}

static bool merge_runs(FILE *input, FILE *output, workspace_t *w,
    uint32_t runs, uint32_t *refs, void (*yield)(void), cerberus_wardrive_compact_stats_t *stats)
{
    if (fseek(output, 0, SEEK_SET)) return false;
    uint32_t input_next = 0, output_next = 0, available = *refs, kept = 0;
    for (uint32_t run = 0; run < runs; run += 2U) {
        cursor_t *left = &w->data.merge.left, *right = &w->data.merge.right;
        if (!cursor_open(input, left, &input_next, available, stats)) return false;
        available -= left->remaining;
        *right = (cursor_t){0};
        if (run + 1U < runs) {
            if (!cursor_open(input, right, &input_next, available, stats)) return false;
            available -= right->remaining;
        }
        uint32_t placeholder = 0, consumed = 0;
        if (!index_write(output, &placeholder, sizeof(placeholder), stats)) return false;
        writer_t writer = {.file = output, .rows = w->data.merge.output, .stats = stats};
        row_ref_t low = {0}, high = {0};
        bool group = false;
        while (left->remaining || left->used != left->count || right->remaining || right->used != right->count) {
            if (!cursor_fill(input, left, stats) || !cursor_fill(input, right, stats)) return false;
            bool use_left = right->used == right->count ||
                (left->used != left->count &&
                row_compare(&left->rows[left->used], &right->rows[right->used]) <= 0);
            cursor_t *selected = use_left ? left : right;
            row_ref_t row = selected->rows[selected->used++];
            if (!group || key_compare(&low, &row)) {
                if (group && !writer_pair(&writer, &low, &high)) return false;
                low = high = row; group = true;
            } else if (row.identity.rssi > high.identity.rssi) high = row;
            if (yield && ++consumed % SORT_ROWS == 0) yield();
        }
        if (!group || !writer_pair(&writer, &low, &high) || !writer_flush(&writer)) return false;
        uint64_t end = (uint64_t)output_next + sizeof(uint32_t) + (uint64_t)writer.count * sizeof(row_ref_t);
        if (end > INT32_MAX || fseek(output, (long)output_next, SEEK_SET) ||
            !index_write(output, &writer.count, sizeof(writer.count), stats) ||
            fseek(output, (long)end, SEEK_SET)) return false;
        output_next = (uint32_t)end; kept += writer.count;
        if (yield) yield();
    }
    if (available || kept > *refs || fflush(output)) return false;
    *refs = kept;
    return true;
}

static bool copy_row(FILE *source, FILE *output, const row_ref_t *row,
    workspace_t *w, cerberus_wardrive_compact_stats_t *stats)
{
    if (!row->length || row->length >= sizeof(w->line) ||
        (uint64_t)row->offset + row->length > stats->input_bytes ||
        fseek(source, (long)row->offset, SEEK_SET) ||
        fread(w->line, 1, row->length, source) != row->length ||
        fwrite(w->line, 1, row->length, output) != row->length) return false;
    stats->output_bytes += row->length;
    ++stats->output_rows;
    return true;
}

static bool copy_pair(FILE *source, FILE *output, const row_ref_t *low,
    const row_ref_t *high, workspace_t *w, cerberus_wardrive_compact_stats_t *stats)
{
    ++stats->devices;
    /* Preserve capture order within each pair. Equal RSSI keeps one earliest
     * observation, not two copies of an identical extremum. */
    const row_ref_t *first = low->offset < high->offset ? low : high;
    const row_ref_t *second = first == low ? high : low;
    return copy_row(source, output, first, w, stats) &&
        (low->offset == high->offset || copy_row(source, output, second, w, stats));
}

esp_err_t cerberus_wardrive_compact(FILE *source, FILE *a, FILE *b, FILE *output,
    void *memory, size_t capacity, void (*yield)(void),
    cerberus_wardrive_compact_stats_t *stats)
{
    if (!source || !a || !b || !output || !memory || !stats || capacity < sizeof(workspace_t) ||
        source == a || source == b || source == output || a == b || a == output || b == output)
        return ESP_ERR_INVALID_ARG;
    workspace_t *w = memory;
    memset(w, 0, sizeof(*w));
    memset(stats, 0, sizeof(*stats));
    cerberus_ward_log_init(&w->parser);
    unsigned pending = 0;
    uint32_t runs = 0;
    while (fgets(w->line, sizeof(w->line), source)) {
        size_t length = strlen(w->line);
        if (!length || w->line[length - 1] != '\n' ||
            !cerberus_ward_log_feed(&w->parser, w->line, length) ||
            w->parser.summary.kind != CERBERUS_WARD_LOG_WIGLE) return ESP_ERR_INVALID_RESPONSE;
        if (w->parser.summary.lines <= 2U) {
            if (fwrite(w->line, 1, length, output) != length) return ESP_FAIL;
            stats->output_bytes += length;
        } else {
            w->data.sort[pending++] = (row_ref_t){.identity = w->parser.last_row,
                .offset = (uint32_t)stats->input_bytes, .length = (uint16_t)length};
            ++stats->input_rows;
        }
        stats->input_bytes += length;
        if (pending == SORT_ROWS) {
            if (!write_sorted_run(a, w, pending, stats)) return ESP_FAIL;
            ++runs;
            pending = 0;
            if (yield) yield();
        }
    }
    if (ferror(source)) return ESP_FAIL;
    if (!cerberus_ward_log_finish(&w->parser)) return ESP_ERR_INVALID_RESPONSE;
    if (pending) {
        if (!write_sorted_run(a, w, pending, stats)) return ESP_FAIL;
        ++runs;
    }
    if (fflush(a)) return ESP_FAIL;
    FILE *sorted = a, *other = b;
    uint32_t refs = stats->initial_refs;
    while (runs > 1U) {
        if (!merge_runs(sorted, other, w, runs, &refs, yield, stats)) return ESP_FAIL;
        ++stats->merge_passes;
        runs = (runs + 1U) / 2U;
        FILE *swap = sorted; sorted = other; other = swap;
    }
    stats->final_refs = refs;
    cursor_t *cursor = &w->data.merge.left;
    uint32_t next = 0;
    if (!cursor_open(sorted, cursor, &next, refs, stats) || cursor->remaining != refs) return ESP_FAIL;
    row_ref_t low = {0}, high = {0};
    for (uint32_t i = 0; i < refs; ++i) {
        if (!cursor_fill(sorted, cursor, stats)) return ESP_FAIL;
        row_ref_t row = cursor->rows[cursor->used++];
        if (!i) low = high = row;
        else if (key_compare(&low, &row)) {
            if (!copy_pair(source, output, &low, &high, w, stats)) return ESP_FAIL;
            low = high = row;
        } else if (row.identity.rssi > high.identity.rssi) high = row;
        if (yield && i % SORT_ROWS == 0) yield();
    }
    if (!copy_pair(source, output, &low, &high, w, stats) || fflush(output)) return ESP_FAIL;
    return ESP_OK;
}
