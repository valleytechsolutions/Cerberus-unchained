#include "wardrive_compact.h"
#include "wardrive_rssi_filter.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

static unsigned yields;
static void progress(void) { ++yields; }

static FILE *scratch_a, *scratch_b;
static uint64_t index_read_bytes, index_write_bytes;
static unsigned index_reads, index_writes, index_seeks;
static size_t measured_read(void *data, size_t size, size_t count, FILE *file)
{
    size_t received = fread(data, size, count, file);
    if (file == scratch_a || file == scratch_b) { index_read_bytes += received * size; ++index_reads; }
    return received;
}
static size_t measured_write(const void *data, size_t size, size_t count, FILE *file)
{
    size_t written = fwrite(data, size, count, file);
    if (file == scratch_a || file == scratch_b) { index_write_bytes += written * size; ++index_writes; }
    return written;
}
static int measured_seek(FILE *file, long offset, int origin)
{
    if (file == scratch_a || file == scratch_b) ++index_seeks;
    return fseek(file, offset, origin);
}
#define fread measured_read
#define fwrite measured_write
#define fseek measured_seek
#ifndef CERBERUS_COMPACT_IMPLEMENTATION
#define CERBERUS_COMPACT_IMPLEMENTATION "wardrive_compact.c"
#endif
#include CERBERUS_COMPACT_IMPLEMENTATION
#undef fread
#undef fwrite
#undef fseek

#ifndef CERBERUS_COMPACT_BASELINE
typedef struct { char **paths; FILE *file; uint16_t part; uint32_t bytes; } test_journals_t;
static esp_err_t select_journal(void *context, uint16_t part, FILE **file, uint32_t *bytes)
{
    test_journals_t *s = context;
    if (!s->file || s->part != part) {
        if (s->file) {
            FILE *previous = s->file; s->file = NULL;
            if (fclose(previous)) return ESP_FAIL;
        }
        s->file = fopen(s->paths[part ? 6U + part : 1U], "rb");
        if (!s->file || fseek(s->file, 0, SEEK_END)) return ESP_FAIL;
        long end = ftell(s->file);
        if (end <= 0 || (uint64_t)end > UINT32_MAX) return ESP_FAIL;
        s->part = part; s->bytes = (uint32_t)end;
    }
    *file = s->file; *bytes = s->bytes;
    return ESP_OK;
}
#endif

int main(int argc, char **argv)
{
    bool multiple = argc >= 7 && !strcmp(argv[6], "parts");
    assert(argc == 6 || argc == 7 || (multiple && argc - 6 <= UINT16_MAX));
    FILE *source = fopen(argv[1], "rb"), *a = fopen(argv[2], "w+b"),
        *b = fopen(argv[3], "w+b"), *output = fopen(argv[4], "w+b");
    assert(source && a && b && output);
    scratch_a = a; scratch_b = b;
    unsigned filtered = 0, received = 0;
    if (argc == 7 && !multiple) {
        FILE *journal = tmpfile();
        assert(journal);
        cerberus_wardrive_rssi_filter_t *filter = strcmp(argv[6], "no-cache") == 0 ?
            NULL : calloc(1U, sizeof(*filter));
        assert(filter || strcmp(argv[6], "no-cache") == 0);
        cerberus_ward_log_parser_t parser;
        cerberus_ward_log_init(&parser);
        char line[CERBERUS_WARD_LOG_LINE_CAPACITY];
        while (fgets(line, sizeof(line), source)) {
            size_t length = strlen(line);
            assert(cerberus_ward_log_feed(&parser, line, length));
            if (parser.summary.lines <= 2U) {
                assert(fwrite(line, 1U, length, journal) == length);
                continue;
            }
            ++received;
            if (cerberus_wardrive_rssi_needs_append(filter, &parser.last_row)) {
                assert(fwrite(line, 1U, length, journal) == length);
                cerberus_wardrive_rssi_record_append(filter, &parser.last_row);
            } else ++filtered;
        }
        assert(!ferror(source) && cerberus_ward_log_finish(&parser));
        assert(fclose(source) == 0 && fflush(journal) == 0 && fseek(journal, 0, SEEK_SET) == 0);
        source = journal;
        free(filter);
    }
    size_t capacity = cerberus_wardrive_compact_workspace_size();
    assert(capacity < 12U * 1024U);
    void *workspace = malloc(capacity);
    assert(workspace);
    cerberus_wardrive_compact_stats_t stats;
    esp_err_t error;
#ifndef CERBERUS_COMPACT_BASELINE
    if (multiple) {
        assert(!fclose(source));
        test_journals_t context = {.paths = argv};
        cerberus_wardrive_journals_t journals = {.context = &context,
            .count = (uint16_t)(argc - 6), .select = select_journal};
        error = cerberus_wardrive_compact_journals(&journals, a, b, output,
            workspace, capacity, progress, &stats);
        source = context.file;
    } else
#endif
        error = cerberus_wardrive_compact(source, a, b, output, workspace, capacity, progress, &stats);
#ifndef CERBERUS_COMPACT_BASELINE
    assert(stats.index_read_bytes == index_read_bytes && stats.index_write_bytes == index_write_bytes);
#endif
    assert(fclose(source) == 0 && fclose(a) == 0 && fclose(b) == 0 && fclose(output) == 0);
    free(workspace);
    FILE *report = fopen(argv[5], "w");
    assert(report);
    fprintf(report, "{\"error\":%d,\"input_rows\":%u,\"devices\":%u,\"output_rows\":%u,"
        "\"input_bytes\":%llu,\"output_bytes\":%llu,\"workspace_bytes\":%u,\"yields\":%u,"
        "\"received\":%u,\"filtered\":%u,\"filter_bytes\":%u,"
        "\"index_read_bytes\":%llu,\"index_write_bytes\":%llu,\"index_reads\":%u,\"index_writes\":%u,\"index_seeks\":%u,"
        "\"initial_refs\":%u,\"final_refs\":%u,\"merge_passes\":%u,\"input_parts\":%u}\n",
        error, stats.input_rows, stats.devices, stats.output_rows,
        (unsigned long long)stats.input_bytes, (unsigned long long)stats.output_bytes,
        (unsigned)capacity, yields, received, filtered, (unsigned)sizeof(cerberus_wardrive_rssi_filter_t),
        (unsigned long long)index_read_bytes, (unsigned long long)index_write_bytes, index_reads, index_writes, index_seeks,
        stats.initial_refs, stats.final_refs, stats.merge_passes, stats.input_parts);
    assert(fclose(report) == 0);
    return 0;
}
