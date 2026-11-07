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
    uint32_t next, end;
    unsigned used, count;
} cursor_t;
typedef struct {
    cerberus_ward_log_parser_t parser;
    char line[CERBERUS_WARD_LOG_LINE_CAPACITY];
    union {
        row_ref_t sort[SORT_ROWS];
        struct { cursor_t left, right; } merge;
    } data;
} workspace_t;

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

static bool cursor_fill(FILE *file, cursor_t *cursor)
{
    if (cursor->used != cursor->count || cursor->next == cursor->end) return true;
    uint32_t count = cursor->end - cursor->next;
    if (count > MERGE_ROWS) count = MERGE_ROWS;
    if (fseek(file, (long)cursor->next * (long)sizeof(row_ref_t), SEEK_SET) ||
        fread(cursor->rows, sizeof(row_ref_t), count, file) != count) return false;
    cursor->next += count;
    cursor->count = count;
    cursor->used = 0;
    return true;
}

static bool merge_runs(FILE *input, FILE *output, workspace_t *w,
    uint32_t count, uint32_t width, void (*yield)(void))
{
    if (fseek(output, 0, SEEK_SET)) return false;
    for (uint32_t begin = 0; begin < count; begin += width * 2U) {
        uint32_t middle = begin + width < count ? begin + width : count;
        uint32_t end = middle + width < count ? middle + width : count;
        cursor_t *left = &w->data.merge.left, *right = &w->data.merge.right;
        *left = (cursor_t){.next = begin, .end = middle};
        *right = (cursor_t){.next = middle, .end = end};
        for (uint32_t i = begin; i < end; ++i) {
            if (!cursor_fill(input, left) || !cursor_fill(input, right)) return false;
            bool use_left = right->used == right->count ||
                (left->used != left->count &&
                row_compare(&left->rows[left->used], &right->rows[right->used]) <= 0);
            cursor_t *selected = use_left ? left : right;
            if (fwrite(&selected->rows[selected->used++], sizeof(row_ref_t), 1, output) != 1)
                return false;
            if (yield && i % SORT_ROWS == 0) yield();
        }
    }
    return fflush(output) == 0;
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
            qsort(w->data.sort, pending, sizeof(row_ref_t), row_compare);
            if (fwrite(w->data.sort, sizeof(row_ref_t), pending, a) != pending) return ESP_FAIL;
            pending = 0;
            if (yield) yield();
        }
    }
    if (ferror(source)) return ESP_FAIL;
    if (!cerberus_ward_log_finish(&w->parser)) return ESP_ERR_INVALID_RESPONSE;
    if (pending) {
        qsort(w->data.sort, pending, sizeof(row_ref_t), row_compare);
        if (fwrite(w->data.sort, sizeof(row_ref_t), pending, a) != pending) return ESP_FAIL;
    }
    if (fflush(a)) return ESP_FAIL;
    FILE *sorted = a, *other = b;
    for (uint32_t width = SORT_ROWS; width < stats->input_rows; width *= 2U) {
        if (!merge_runs(sorted, other, w, stats->input_rows, width, yield)) return ESP_FAIL;
        FILE *swap = sorted; sorted = other; other = swap;
    }
    if (fseek(sorted, 0, SEEK_SET)) return ESP_FAIL;
    row_ref_t low = {0}, high = {0};
    for (uint32_t i = 0; i < stats->input_rows; ++i) {
        row_ref_t row;
        if (fread(&row, sizeof(row), 1, sorted) != 1) return ESP_FAIL;
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
