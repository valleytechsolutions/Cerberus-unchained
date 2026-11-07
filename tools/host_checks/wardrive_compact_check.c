#include "wardrive_compact.h"
#include <assert.h>
#include <stdlib.h>

static unsigned yields;
static void progress(void) { ++yields; }

int main(int argc, char **argv)
{
    assert(argc == 6);
    FILE *source = fopen(argv[1], "rb"), *a = fopen(argv[2], "w+b"),
        *b = fopen(argv[3], "w+b"), *output = fopen(argv[4], "w+b");
    assert(source && a && b && output);
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
        "\"input_bytes\":%llu,\"output_bytes\":%llu,\"workspace_bytes\":%u,\"yields\":%u}\n",
        error, stats.input_rows, stats.devices, stats.output_rows,
        (unsigned long long)stats.input_bytes, (unsigned long long)stats.output_bytes,
        (unsigned)capacity, yields);
    assert(fclose(report) == 0);
    return 0;
}
