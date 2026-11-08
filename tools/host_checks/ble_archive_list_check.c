#undef NDEBUG
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "ble_archive.h"
/* The real codec validates test input, but presentation must never invoke it. */
#define cerberus_ble_archive_validate(...) forbidden_codec_validation_in_presentation
#include "ble_archive_list.h"
#undef cerberus_ble_archive_validate

static unsigned checks;
#define CHECK(value) do { ++checks; if (!(value)) { fprintf(stderr, "BLE archive list %u: %s\n", __LINE__, #value); exit(1); } } while (0)
static cerberus_ble_archive_t document, original;
static struct { uint32_t before; uint8_t indexes[64]; uint32_t after; } output;

static void seed(uint8_t count)
{
    memset(&document, 0, sizeof(document));
    document.count = count;
    document.saved_uptime_ms = 100000;
    for (unsigned i = 0; i < count; ++i) {
        cerberus_ble_archive_row_t *row = &document.rows[i];
        row->receiver = (uint8_t)(i % 3U);
        row->address[0] = (uint8_t)(i / 3U + 1U);
        row->address[5] = 0x62;
        row->address_type = (uint8_t)(i / 3U % 4U);
        snprintf(row->name, sizeof(row->name), "Entry %02u", i);
        row->rssi = (int8_t)(-100 + i);
        row->rssi_valid = true;
        row->index = (uint8_t)(i / 3U);
        row->generation = 0;
        row->scan_epoch = UINT64_C(0x100000001) + i;
        row->scan_epoch_valid = true;
        row->advertisement_length = 3;
        row->advertisement[0] = 2; row->advertisement[1] = 1; row->advertisement[2] = 6;
        row->advertisement_event_type = 3;
        if (row->receiver == CERBERUS_BLE_ARCHIVE_RECEIVER_C5) {
            row->rssi_kind = CERBERUS_BLE_ARCHIVE_RSSI_LATEST_ADV;
            row->receiver_boot_id = UINT64_C(0xabcdef0100000001);
            row->age_upper_valid = true;
            row->age_upper_at_save_ms = UINT64_MAX - i;
        } else {
            row->source_time_valid = true;
            row->original_source_uptime_ms = 1000U + i;
            row->copy_ceiling_ms = 1100U + i;
        }
    }
    if (count) CHECK(cerberus_ble_archive_validate(&document));
    memset(&output, 0xA5, sizeof(output));
}

static void freeze(void) { memcpy(&original, &document, sizeof(document)); }
static void unchanged(void)
{
    CHECK(!memcmp(&original, &document, sizeof(document)));
    CHECK(output.before == UINT32_C(0xA5A5A5A5) && output.after == UINT32_C(0xA5A5A5A5));
}
static uint8_t order(cerberus_ble_archive_list_source_t source, cerberus_ble_archive_list_sort_t sort)
{ return cerberus_ble_archive_list_order(&document, source, sort, output.indexes); }
static bool expected_source(unsigned filter, unsigned receiver)
{ return filter == 0U || (filter == 3U ? receiver == 0U : receiver == filter); }

static void capacity_and_file_order(void)
{
    seed(64); freeze();
    CHECK(order(CERBERUS_BLE_ARCHIVE_LIST_ALL, CERBERUS_BLE_ARCHIVE_LIST_FILE) == 64);
    for (unsigned i = 0; i < 64; ++i) CHECK(output.indexes[i] == i);
    unchanged();
    for (unsigned source = 1; source < 4; ++source) {
        memset(output.indexes, 0xA5, sizeof(output.indexes));
        unsigned count = order(source, CERBERUS_BLE_ARCHIVE_LIST_FILE), at = 0;
        for (unsigned i = 0; i < 64; ++i) {
            if (!expected_source(source, i % 3U)) continue;
            CHECK(at < count && output.indexes[at++] == i);
        }
        CHECK(at == count && count <= CERBERUS_BLE_ARCHIVE_MAX_ROWS_PER_RECEIVER);
        for (unsigned i = count; i < 64; ++i) CHECK(output.indexes[i] == 0xA5);
        unchanged();
    }
    /* The full64 two-receiver edge also permits32 observations from each. */
    seed(64);
    for (unsigned i = 0; i < 64; ++i) {
        cerberus_ble_archive_row_t *row = &document.rows[i];
        row->receiver = i % 2U ? CERBERUS_BLE_ARCHIVE_RECEIVER_C5 : CERBERUS_BLE_ARCHIVE_RECEIVER_S3;
        row->index = (uint8_t)(i / 2U); row->address[0] = (uint8_t)(i / 2U + 1U);
        row->rssi_kind = i % 2U ? CERBERUS_BLE_ARCHIVE_RSSI_LATEST_ADV : CERBERUS_BLE_ARCHIVE_RSSI_SWEEP_PEAK;
        row->receiver_boot_id = i % 2U ? 1U : 0U;
        row->age_upper_valid = i % 2U != 0; row->age_upper_at_save_ms = i % 2U ? UINT64_MAX : 0;
        row->source_time_valid = i % 2U == 0;
        row->original_source_uptime_ms = i % 2U ? 0U : 1000U;
        row->copy_ceiling_ms = i % 2U ? 0U : 1100U;
    }
    CHECK(cerberus_ble_archive_validate(&document)); freeze();
    CHECK(order(CERBERUS_BLE_ARCHIVE_LIST_S3, CERBERUS_BLE_ARCHIVE_LIST_FILE) == 32);
    for (unsigned i = 0; i < 32; ++i) CHECK(output.indexes[i] == i * 2U);
    CHECK(order(CERBERUS_BLE_ARCHIVE_LIST_C5, CERBERUS_BLE_ARCHIVE_LIST_FILE) == 32);
    for (unsigned i = 0; i < 32; ++i) CHECK(output.indexes[i] == i * 2U + 1U);
    memset(output.indexes, 0xA5, sizeof(output.indexes));
    CHECK(order(CERBERUS_BLE_ARCHIVE_LIST_LEGACY, CERBERUS_BLE_ARCHIVE_LIST_FILE) == 0);
    for (unsigned i = 0; i < 64; ++i) CHECK(output.indexes[i] == 0xA5);
    unchanged();
}

static void identities_and_stable_ties(void)
{
    seed(6);
    for (unsigned i = 0; i < 6; ++i) {
        memset(document.rows[i].address, 0x55, 6);
        document.rows[i].address_type = (uint8_t)(i / 3U);
        document.rows[i].rssi = -42;
        strcpy(document.rows[i].name, i % 2U ? "SAME" : "same");
    }
    CHECK(cerberus_ble_archive_validate(&document)); freeze();
    for (unsigned sort = 0; sort < 3; ++sort) {
        CHECK(order(CERBERUS_BLE_ARCHIVE_LIST_ALL, sort) == 6);
        for (unsigned i = 0; i < 6; ++i) CHECK(output.indexes[i] == i);
        unchanged();
    }
    const char *names[] = {"Zulu", "alpha", "ALPHA", "beta", "alpha", "A"};
    const uint8_t expected[] = {5, 1, 2, 4, 3, 0};
    for (unsigned i = 0; i < 6; ++i) strcpy(document.rows[i].name, names[i]);
    freeze();
    for (unsigned sort = 1; sort < 3; ++sort) {
        CHECK(order(CERBERUS_BLE_ARCHIVE_LIST_ALL, sort) == 6);
        CHECK(!memcmp(expected, output.indexes, sizeof(expected))); unchanged();
    }
}

static void signal_and_history(void)
{
    seed(8);
    const int8_t rssi[] = {-30, -20, 127, -128, 21, 20, -127, 20};
    const bool valid[] = {true, true, true, true, true, false, true, true};
    const uint8_t expected[] = {7, 1, 0, 6, 2, 3, 4, 5};
    for (unsigned i = 0; i < 8; ++i) {
        document.rows[i].rssi = rssi[i]; document.rows[i].rssi_valid = valid[i];
    }
    freeze();
    CHECK(order(CERBERUS_BLE_ARCHIVE_LIST_ALL, CERBERUS_BLE_ARCHIVE_LIST_SIGNAL) == 8);
    CHECK(!memcmp(expected, output.indexes, sizeof(expected))); unchanged();
    CHECK(!cerberus_ble_archive_list_rssi_valid(NULL));
    for (unsigned i = 0; i < 8; ++i)
        CHECK(cerberus_ble_archive_list_rssi_valid(&document.rows[i]) == (i == 0 || i == 1 || i == 6 || i == 7));
    /* Sorting cannot use save time, source exact time, upper age, response
     * metadata, or receive context to reinterpret any signal measurement. */
    document.saved_uptime_ms = UINT64_MAX;
    for (unsigned i = 0; i < 8; ++i) {
        document.rows[i].original_source_uptime_ms = UINT64_MAX - i;
        document.rows[i].age_upper_at_save_ms = i;
        document.rows[i].response_sequence = 42U + i;
        document.rows[i].receive.sid = (uint8_t)i;
    }
    freeze();
    CHECK(order(CERBERUS_BLE_ARCHIVE_LIST_ALL, CERBERUS_BLE_ARCHIVE_LIST_SIGNAL) == 8);
    CHECK(!memcmp(expected, output.indexes, sizeof(expected))); unchanged();
}

static void labels(void)
{
    seed(1); cerberus_ble_archive_row_t *row = &document.rows[0];
    char text[40]; memset(text, '!', sizeof(text));
    CHECK(!cerberus_ble_archive_list_label(NULL, text, sizeof(text)));
    for (unsigned i = 0; i < sizeof(text); ++i) CHECK(text[i] == '!');
    CHECK(!cerberus_ble_archive_list_label(row, NULL, 1));
    row->name[0] = 0; freeze();
    CHECK(cerberus_ble_archive_list_label(row, text, sizeof(text)) == strlen("[NO RETAINED NAME]"));
    CHECK(!strcmp(text, "[NO RETAINED NAME]"));
    CHECK(cerberus_ble_archive_list_label(row, NULL, 0) == strlen("[NO RETAINED NAME]")); unchanged();
    memset(row->address, 0xff, 6);
    CHECK(cerberus_ble_archive_list_label(row, text, sizeof(text)) == strlen("[NO RETAINED NAME]"));
    CHECK(!strcmp(text, "[NO RETAINED NAME]"));
    for (unsigned byte = 1; byte <= 255; ++byte) {
        row->name[0] = (char)byte; row->name[1] = 0;
        CHECK(cerberus_ble_archive_list_label(row, text, sizeof(text)) == 1);
        CHECK(text[0] == (byte >= 32U && byte <= 126U ? (char)byte : '.') && text[1] == 0);
        CHECK((unsigned char)row->name[0] == byte);
    }
    memset(row->name, 'Q', sizeof(row->name)); freeze();
    for (size_t capacity = 0; capacity <= sizeof(text) - 2U; ++capacity) {
        memset(text, '!', sizeof(text));
        CHECK(cerberus_ble_archive_list_label(row, text + 1, capacity) == 32);
        CHECK(text[0] == '!' && text[capacity + 1U] == '!');
        if (capacity) {
            size_t expected = capacity - 1U < 32U ? capacity - 1U : 32U;
            CHECK(strlen(text + 1) == expected);
            for (size_t i = 0; i < expected; ++i) CHECK(text[1U + i] == 'Q');
        }
        unchanged();
    }
}

static void invalid_arguments_and_receiver_domain(void)
{
    seed(1); freeze();
    CHECK(!cerberus_ble_archive_list_order(NULL, CERBERUS_BLE_ARCHIVE_LIST_ALL, CERBERUS_BLE_ARCHIVE_LIST_FILE, output.indexes));
    CHECK(!cerberus_ble_archive_list_order(&document, CERBERUS_BLE_ARCHIVE_LIST_ALL, CERBERUS_BLE_ARCHIVE_LIST_FILE, NULL));
    for (int source = -1; source <= 5; ++source) {
        if (source >= 0 && source < 4) continue;
        CHECK(!order((cerberus_ble_archive_list_source_t)source, CERBERUS_BLE_ARCHIVE_LIST_FILE));
    }
    for (int sort = -1; sort <= 4; ++sort) {
        if (sort >= 0 && sort < 3) continue;
        CHECK(!order(CERBERUS_BLE_ARCHIVE_LIST_ALL, (cerberus_ble_archive_list_sort_t)sort));
    }
    for (unsigned count = 65; count <= UINT8_MAX; ++count) {
        document.count = count;
        CHECK(!order(CERBERUS_BLE_ARCHIVE_LIST_ALL, CERBERUS_BLE_ARCHIVE_LIST_FILE));
    }
    document.count = 1; unchanged();
    for (unsigned i = 0; i < 64; ++i) CHECK(output.indexes[i] == 0xA5);
    seed(0); freeze();
    for (unsigned source = 0; source < 4; ++source) for (unsigned sort = 0; sort < 3; ++sort)
        CHECK(!order(source, sort));
    for (unsigned i = 0; i < 64; ++i) CHECK(output.indexes[i] == 0xA5);
    unchanged();
    seed(64);
    /* Domain failure even in a filtered-out row must be atomic. Every position
     * is checked before publishing indexes, including the final input row. */
    for (unsigned position = 0; position < 64; ++position) {
        uint8_t original_receiver = document.rows[position].receiver;
        for (unsigned invalid = 3; invalid <= 255; invalid += 252) {
            document.rows[position].receiver = invalid; freeze();
            for (unsigned source = 0; source < 4; ++source) for (unsigned sort = 0; sort < 3; ++sort) {
                CHECK(!order(source, sort));
                for (unsigned i = 0; i < 64; ++i) CHECK(output.indexes[i] == 0xA5);
                unchanged();
            }
        }
        document.rows[position].receiver = original_receiver;
    }
}

typedef struct { unsigned index, receiver; const char *key; int rssi; bool measured; } oracle_row_t;
static unsigned oracle_sort;
static int oracle_compare(const void *left, const void *right)
{
    const oracle_row_t *a = left, *b = right;
    if (oracle_sort != CERBERUS_BLE_ARCHIVE_LIST_FILE) {
        if (oracle_sort == CERBERUS_BLE_ARCHIVE_LIST_SIGNAL) {
            if (a->measured != b->measured) return a->measured ? -1 : 1;
            if (a->measured && a->rssi != b->rssi) return a->rssi > b->rssi ? -1 : 1;
        }
        int names = strcmp(a->key, b->key);
        if (names) return names;
    }
    return a->index < b->index ? -1 : a->index > b->index;
}

static void independent_oracle(void)
{
    static const char *const names[] = {"beta", "ALPHA", "alpha", "a\001", "a.", "", "z"};
    static const char *const keys[] = {"beta", "alpha", "alpha", "a.", "a.", "[no retained name]", "z"};
    for (unsigned count = 0; count <= 64; ++count) {
        seed(count); oracle_row_t all[64];
        for (unsigned i = 0; i < count; ++i) {
            unsigned k = (i * 13U + count * 7U) % 17U;
            strcpy(document.rows[i].name, names[k % 7U]);
            bool measured = k % 5U != 0U;
            int signal = (int)(k * 5U) - 100;
            document.rows[i].rssi = measured ? signal : 127;
            document.rows[i].rssi_valid = measured;
            all[i] = (oracle_row_t){i, document.rows[i].receiver, keys[k % 7U], signal, measured};
        }
        if (count) CHECK(cerberus_ble_archive_validate(&document)); freeze();
        for (unsigned source = 0; source < 4; ++source) for (unsigned sort = 0; sort < 3; ++sort) {
            oracle_row_t expected[64]; unsigned visible = 0;
            for (unsigned i = 0; i < count; ++i)
                if (expected_source(source, all[i].receiver)) expected[visible++] = all[i];
            oracle_sort = sort; qsort(expected, visible, sizeof(expected[0]), oracle_compare);
            memset(output.indexes, 0xA5, sizeof(output.indexes));
            CHECK(order(source, sort) == visible);
            uint64_t seen = 0;
            for (unsigned i = 0; i < visible; ++i) {
                CHECK(output.indexes[i] == expected[i].index);
                CHECK(!(seen & (UINT64_C(1) << output.indexes[i])));
                seen |= UINT64_C(1) << output.indexes[i];
            }
            for (unsigned i = visible; i < 64; ++i) CHECK(output.indexes[i] == 0xA5);
            unchanged();
        }
    }
}

int main(void)
{
    capacity_and_file_order(); identities_and_stable_ties(); signal_and_history();
    labels(); invalid_arguments_and_receiver_domain(); independent_oracle();
    printf("BLE archive list PASS: %u checks; immutable64 indexes, source filters, file/name/signal order\n", checks);
    return 0;
}
