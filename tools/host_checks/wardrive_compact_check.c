#include "wardrive_compact.h"
#include "wardrive_rssi_filter.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

static unsigned yields;
static void progress(void) { ++yields; }

int main(int argc, char **argv)
{
    assert(argc == 6 || argc == 7);
    FILE *source = fopen(argv[1], "rb"), *a = fopen(argv[2], "w+b"),
        *b = fopen(argv[3], "w+b"), *output = fopen(argv[4], "w+b");
    assert(source && a && b && output);
    unsigned filtered = 0, received = 0;
    if (argc == 7) {
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
    esp_err_t error = cerberus_wardrive_compact(source, a, b, output,
        workspace, capacity, progress, &stats);
    assert(fclose(source) == 0 && fclose(a) == 0 && fclose(b) == 0 && fclose(output) == 0);
    free(workspace);
    FILE *report = fopen(argv[5], "w");
    assert(report);
    fprintf(report, "{\"error\":%d,\"input_rows\":%u,\"devices\":%u,\"output_rows\":%u,"
        "\"input_bytes\":%llu,\"output_bytes\":%llu,\"workspace_bytes\":%u,\"yields\":%u,"
        "\"received\":%u,\"filtered\":%u,\"filter_bytes\":%u}\n",
        error, stats.input_rows, stats.devices, stats.output_rows,
        (unsigned long long)stats.input_bytes, (unsigned long long)stats.output_bytes,
        (unsigned)capacity, yields, received, filtered, (unsigned)sizeof(cerberus_wardrive_rssi_filter_t));
    assert(fclose(report) == 0);
    return 0;
}
