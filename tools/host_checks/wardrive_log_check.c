#include "wardrive_log.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void boundaries(void)
{
    assert(cerberus_ward_log_name(NULL) == CERBERUS_WARD_LOG_UNKNOWN);
    assert(cerberus_ward_log_name("WIGLE001.CSV") == CERBERUS_WARD_LOG_WIGLE);
    assert(cerberus_ward_log_name("UNLOC999.CSV") == CERBERUS_WARD_LOG_UNLOCATED);
    assert(cerberus_ward_log_name("CERBWardrive001.CSV") == CERBERUS_WARD_LOG_WIGLE);
    assert(cerberus_ward_log_name("CERBWardrive999.CSV") == CERBERUS_WARD_LOG_WIGLE);
    const char *bad[] = {"WIGLE000.CSV", "WIGLE001.TMP", "WIGLE001.csv", "WIGLE1000.CSV",
        "UNLOC01.CSV", "../WIGLE001.CSV", "/WIGLE001.CSV", "WIGLE001.CSV/", "OTHER001.CSV", "",
        "CERBWardrive000.CSV","CERBWardrive01.CSV","CERBWardrive1000.CSV","CERBWardrive001.TMP",
        "../CERBWardrive001.CSV","CERBWardrive001.CSV/","CERBWardrive001.csv"};
    for (size_t i = 0; i < sizeof(bad)/sizeof(bad[0]); ++i)
        assert(cerberus_ward_log_name(bad[i]) == CERBERUS_WARD_LOG_UNKNOWN);
    char raw[CERBERUS_WARD_LOG_NAME_CAPACITY]; memset(raw, 'x', sizeof(raw));
    assert(cerberus_ward_log_name(raw) == CERBERUS_WARD_LOG_UNKNOWN);
    assert(!cerberus_ward_log_feed(NULL, "", 0));
    assert(!cerberus_ward_log_finish(NULL));
    cerberus_ward_log_parser_t p;
    cerberus_ward_log_init(&p);
    assert(cerberus_ward_log_feed(&p, NULL, 0));
    assert(!cerberus_ward_log_feed(&p, NULL, 1));
    assert(p.summary.error == CERBERUS_WARD_LOG_ARGUMENT);
    cerberus_ward_log_init(&p); p.summary.bytes = CERBERUS_WARD_LOG_MAX_BYTES;
    assert(!cerberus_ward_log_feed(&p, "a", 1));
    assert(p.summary.error == CERBERUS_WARD_LOG_LIMIT);
    uint32_t state = 0x91827364;
    unsigned char bytes[1400];
    for (unsigned trial = 0; trial < 10000; ++trial) {
        state = state * 1664525U + 1013904223U;
        size_t n = state % sizeof(bytes);
        for (size_t i = 0; i < n; ++i) { state = state * 1664525U + 1013904223U; bytes[i] = (unsigned char)(state >> 24); }
        cerberus_ward_log_init(&p);
        (void)cerberus_ward_log_feed(&p, bytes, n);
        (void)cerberus_ward_log_finish(&p);
        assert(p.used < sizeof(p.line));
    }
}

int main(int argc, char **argv)
{
    boundaries();
    if (argc != 3) return 2;
    FILE *f = fopen(argv[1], "rb"); assert(f);
    unsigned long block = strtoul(argv[2], NULL, 10);
    assert(block > 0 && block <= 4096);
    unsigned char bytes[4096]; cerberus_ward_log_parser_t parser;
    cerberus_ward_log_init(&parser);
    size_t n; bool valid = true;
    while (valid && (n = fread(bytes, 1, block, f)) != 0)
        valid = cerberus_ward_log_feed(&parser, bytes, n);
    assert(!ferror(f)); assert(fclose(f) == 0);
    valid = valid && cerberus_ward_log_finish(&parser);
    cerberus_ward_log_summary_t *s = &parser.summary;
    printf("{\"valid\":%s,\"kind\":%u,\"error\":%u,\"rows\":%lu,\"wifi_24\":%lu,"
        "\"wifi_5\":%lu,\"ble\":%lu,\"missing_accuracy\":%lu,\"missing_altitude\":%lu,"
        "\"legacy\":%s,\"first_utc\":\"%s\",\"last_utc\":\"%s\"}\n",
        valid ? "true" : "false", (unsigned)s->kind, (unsigned)s->error,
        (unsigned long)s->rows, (unsigned long)s->wifi_24, (unsigned long)s->wifi_5,
        (unsigned long)s->ble, (unsigned long)s->missing_accuracy, (unsigned long)s->missing_altitude,
        s->legacy_metadata ? "true" : "false", s->first_utc, s->last_utc);
    return 0;
}
