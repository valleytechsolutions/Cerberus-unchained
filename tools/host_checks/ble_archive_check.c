#ifdef NDEBUG
#undef NDEBUG
#endif
#include "ble_archive.h"
#include "ble_family_wire.h"
#include "ble_archive_response_fixture.h"
#include <assert.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

static unsigned checks;
#define CHECK(c) do { ++checks; assert(c); } while (0)
#define HEADER CERBERUS_BLE_ARCHIVE_HEADER_SIZE
#define ROW CERBERUS_BLE_ARCHIVE_ROW_SIZE
#define MAX CERBERUS_BLE_ARCHIVE_MAX_SIZE

static cerberus_ble_archive_t sample(unsigned count)
{
    cerberus_ble_archive_t d = {.count = (uint8_t)count, .saved_uptime_ms = UINT64_C(0x1020304050607080)};
    for (unsigned i = 0; i < count && i < 64; ++i) {
        cerberus_ble_archive_row_t *r = &d.rows[i];
        for (unsigned j = 0; j < 6; ++j) r->address[j] = (uint8_t)(j + 1);
        r->address[5] = (uint8_t)(6 + i);
        r->address_type = 2;
        r->name[0] = 'A'; r->name[1] = 1; r->name[2] = (char)0xff;
        r->rssi = -55; r->rssi_valid = true;
        r->manufacturer_id = 0x004c; r->peer_flags = 0xa5;
        r->uuid_length = 2; r->uuid[0] = 0x0d; r->uuid[1] = 0x18;
        r->advertisement_length = 3;
        r->advertisement[0] = 2; r->advertisement[1] = 1; r->advertisement[2] = 6;
        r->advertisement_event_type = 3; r->advertisement_flags = 0xf8;
        r->reports_seen = 0x1234;
        r->generation = 0xfe; r->index = (uint8_t)(31 - i % 32);
        r->scan_epoch_valid = true; r->scan_epoch = UINT64_C(0x0102030405060708);
        r->source_time_valid = true;
        r->original_source_uptime_ms = UINT64_C(0x100000001);
        r->copy_ceiling_ms = UINT64_C(0x100000005);
        if (i >= 32U) {
            r->receiver = CERBERUS_BLE_ARCHIVE_RECEIVER_C5;
            r->rssi_kind = CERBERUS_BLE_ARCHIVE_RSSI_LATEST_ADV;
            r->source_time_valid = false;
            r->original_source_uptime_ms = r->copy_ceiling_ms = 0U;
            r->receiver_boot_id = UINT64_C(0x8877665544332211);
            r->age_upper_valid = true; r->age_upper_at_save_ms = UINT64_C(0xfedcba9876543210);
        }
    }
    return d;
}

static void put16(uint8_t *p, unsigned n) { p[0] = (uint8_t)n; p[1] = (uint8_t)(n >> 8); }
static void put32(uint8_t *p, uint32_t n) { for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(n >> (i * 8)); }
static void put64(uint8_t *p, uint64_t n) { for (unsigned i = 0; i < 8; ++i) p[i] = (uint8_t)(n >> (i * 8)); }

/* Independent table-based CRC oracle, checked against Python zlib's fixed
 * golden bytes below. Production uses a bit-at-a-time CRC implementation. */
static uint32_t crc_table[256];
static void crc_init(void)
{
    for (unsigned i = 0; i < 256; ++i) {
        uint32_t c = i;
        for (unsigned j = 0; j < 8; ++j) c = c & 1U ? (c >> 1) ^ UINT32_C(0xedb88320) : c >> 1;
        crc_table[i] = c;
    }
}
static void seal(uint8_t *p, size_t length)
{
    memset(p + 28, 0, 4);
    uint32_t c = UINT32_MAX;
    for (size_t i = 0; i < length; ++i) c = crc_table[(c ^ p[i]) & 255U] ^ (c >> 8);
    put32(p + 28, ~c);
}

static size_t encode(const cerberus_ble_archive_t *d, uint8_t *out)
{
    size_t n = SIZE_MAX;
    CHECK(cerberus_ble_archive_encode(d, out, MAX, &n));
    CHECK(n == HEADER + (size_t)d->count * ROW);
    return n;
}

static void bad_decode(const uint8_t *p, size_t n)
{
    cerberus_ble_archive_t d, before;
    memset(&d, 0xa7, sizeof(d)); before = d;
    CHECK(!cerberus_ble_archive_decode(p, n, &d));
    CHECK(!memcmp(&d, &before, sizeof(d)));
}

static void bad_model(const cerberus_ble_archive_t *d)
{
    uint8_t out[MAX], before[MAX];
    memset(out, 0xb8, sizeof(out)); memcpy(before, out, sizeof(out));
    size_t written = 999;
    CHECK(!cerberus_ble_archive_validate(d));
    CHECK(!cerberus_ble_archive_encode(d, out, sizeof(out), &written));
    CHECK(!written);
    CHECK(!memcmp(out, before, sizeof(out)));
}

static void from_hex(const char *hex, uint8_t *out, size_t count)
{
    CHECK(strlen(hex) == count * 2U);
    for (size_t i = 0; i < count; ++i) {
        unsigned value; CHECK(sscanf(hex + 2U * i, "%2x", &value) == 1);
        out[i] = (uint8_t)value;
    }
}

static void golden_and_bounds(void)
{
    static const char hex[] =
        "43424c45415243000100200080000100a0000000807060504030201031ca8e79"
        "010203040506020703c9a5024c000303f8fe1f00341200000807060504030201"
        "010000000100000005000000010000004101ff00000000000000000000000000"
        "000000000000000000000000000000000d180000000000000000000000000000"
        "0201060000000000000000000000000000000000000000000000000000000000";
    static const char v2_hex[] =
        "43424c45415243000200200090000100b000000080706050403020102841cd20"
        "010203040506020703c9a5024c000303f8fe1f00341200000807060504030201"
        "010000000100000005000000010000004101ff00000000000000000000000000"
        "000000000000000000000000000000000d180000000000000000000000000000"
        "0201060000000000000000000000000000000000000000000000000000000000"
        "00000000000000000000000000000000";
    static const char v3_hex[] =
        "43424c454152430003002000d0000100f00000008070605040302010cd84a537"
        "010203040506020703c9a5024c000303f8fe1f00341200000807060504030201"
        "010000000100000005000000010000004101ff00000000000000000000000000"
        "000000000000000000000000000000000d180000000000000000000000000000"
        "0201060000000000000000000000000000000000000000000000000000000000"
        "00000000000000000000000000000000"
        "0000000000000000000000000000000000000000000000000000000000000000"
        "0000000000000000000000000000000000000000000000000000000000000000";
    static const char v4_hex[] =
        "43424c454152430004002000d8000100f80000008070605040302010e3ef3fcb"
        "010203040506020703c9a5024c000303f8fe1f00341200000807060504030201"
        "010000000100000005000000010000004101ff00000000000000000000000000"
        "000000000000000000000000000000000d180000000000000000000000000000"
        "0201060000000000000000000000000000000000000000000000000000000000"
        "0000000000000000000000000000000000000000000000000000000000000000"
        "0000000000000000000000000000000000000000000000000000000000000000"
        "000000000000000000000000000000000000000000000000";
    /* Independent Python struct/zlib v5 vector; old v4 vector stays literal. */
    static const char v5_golden[] =
        "43424c45415243000500200008010100280100008070605040302010c7105e75"
        "010203040506020703c9a5024c000303f8fe1f00341200000807060504030201"
        "010000000100000005000000010000004101ff00000000000000000000000000"
        "000000000000000000000000000000000d180000000000000000000000000000"
        "0201060000000000000000000000000000000000000000000000000000000000"
        "0000000000000000000000000000000000000000000000000000000000000000"
        "0000000000000000000000000000000000000000000000000000000000000000"
        "0000000000000000000000000000000000000000000000000000000000000000"
        "0000000000000000000000000000000000000000000000000000000000000000"
        "0000000000000000";

    /* Independent v6 empty-extension vector, Python struct/zlib. */
    static const char v6_golden[] =
        "43424c454152430006002000b0010100d0010000807060504030201002a8bbe9"
        "010203040506020703c9a5024c000303f8fe1f00341200000807060504030201"
        "010000000100000005000000010000004101ff00000000000000000000000000"
        "000000000000000000000000000000000d180000000000000000000000000000"
        "0201060000000000000000000000000000000000000000000000000000000000"
        "0000000000000000000000000000000000000000000000000000000000000000"
        "0000000000000000000000000000000000000000000000000000000000000000"
        "0000000000000000000000000000000000000000000000000000000000000000"
        "0000000000000000000000000000000000000000000000000000000000000000"
        "0000000000000000000000000000000000000000000000000000000000000000"
        "0000000000000000000000000000000000000000000000000000000000000000"
        "0000000000000000000000000000000000000000000000000000000000000000"
        "0000000000000000000000000000000000000000000000000000000000000000"
        "0000000000000000000000000000000000000000000000000000000000000000"
        "00000000000000000000000000000000";

    uint8_t bytes[MAX + 1], expected[464], v5[296], v4[248], v3[240], v2[176], legacy[160];
    from_hex(hex, legacy, sizeof(legacy));
    from_hex(v2_hex, v2, sizeof(v2));
    from_hex(v3_hex, v3, sizeof(v3));
    from_hex(v4_hex, v4, sizeof(v4));
    from_hex(v5_golden, v5, sizeof(v5));
    from_hex(v6_golden, expected, sizeof(expected));
    cerberus_ble_archive_t d = sample(1), copy = d, decoded;
    CHECK(cerberus_ble_archive_decode(v5,sizeof(v5),&decoded));
    CHECK(!memcmp(&decoded,&copy,sizeof(copy)));
    for(size_t cut=0;cut<sizeof(v5);++cut)bad_decode(v5,cut);
    /* This fixed v1 zlib vector predates the v2 writer. Compatibility is
     * checked independently, not by constructing v1 with the new encoder. */
    CHECK(cerberus_ble_archive_decode(legacy, sizeof(legacy), &decoded));
    CHECK(!memcmp(&decoded, &copy, sizeof(copy)));
    CHECK(decoded.rows[0].receiver == CERBERUS_BLE_ARCHIVE_RECEIVER_LEGACY);
    CHECK(decoded.rows[0].response_state == CERBERUS_BLE_ARCHIVE_RESPONSE_UNKNOWN);
    CHECK(cerberus_ble_archive_decode(v2, sizeof(v2), &decoded));
    CHECK(!memcmp(&decoded, &copy, sizeof(copy)));
    CHECK(cerberus_ble_archive_decode(v4, sizeof(v4), &decoded));
    CHECK(!memcmp(&decoded, &copy, sizeof(copy)));
    for(size_t cut=0;cut<sizeof(v4);++cut)bad_decode(v4,cut);
    for(size_t byte=0;byte<sizeof(v4);++byte){uint8_t corrupt[248];memcpy(corrupt,v4,sizeof(corrupt));
        corrupt[byte]^=1;bad_decode(corrupt,sizeof(corrupt));}
    CHECK(cerberus_ble_archive_decode(v3, sizeof(v3), &decoded));
    CHECK(!memcmp(&decoded, &copy, sizeof(copy)));
    for (size_t cut = 0; cut < sizeof(v3); ++cut) bad_decode(v3, cut);
    for (size_t i = 0; i < sizeof(v3); ++i) {
        uint8_t corrupt[240]; memcpy(corrupt, v3, sizeof(corrupt)); corrupt[i] ^= 1U;
        bad_decode(corrupt, sizeof(corrupt));
    }
    for (size_t cut = 0; cut < sizeof(v2); ++cut) bad_decode(v2, cut);
    for (size_t i = 0; i < sizeof(v2); ++i) {
        uint8_t corrupt[176]; memcpy(corrupt, v2, sizeof(corrupt)); corrupt[i] ^= 1U;
        bad_decode(corrupt, sizeof(corrupt));
    }
    for (size_t cut = 0; cut < sizeof(legacy); ++cut) bad_decode(legacy, cut);
    for (size_t i = 0; i < sizeof(legacy); ++i) {
        uint8_t corrupt[160]; memcpy(corrupt, legacy, sizeof(corrupt)); corrupt[i] ^= 1U;
        bad_decode(corrupt, sizeof(corrupt));
    }
    const unsigned legacy_reserved[] = {19, 22, 23, 127};
    for (unsigned i = 0; i < sizeof(legacy_reserved) / sizeof(legacy_reserved[0]); ++i) {
        uint8_t corrupt[160]; memcpy(corrupt, legacy, sizeof(corrupt));
        corrupt[HEADER + legacy_reserved[i]] = 1U; seal(corrupt, sizeof(corrupt));
        bad_decode(corrupt, sizeof(corrupt));
    }
    uint8_t legacy_full[CERBERUS_BLE_ARCHIVE_LEGACY_MAX_SIZE + 128];
    memcpy(legacy_full, legacy, HEADER);
    for (unsigned i = 0; i < 33; ++i) {
        memcpy(legacy_full + HEADER + i * 128U, legacy + HEADER, 128U);
        legacy_full[HEADER + i * 128U + 5U] = (uint8_t)i;
    }
    put16(legacy_full + 14, 32); put32(legacy_full + 16, CERBERUS_BLE_ARCHIVE_LEGACY_MAX_SIZE);
    seal(legacy_full, CERBERUS_BLE_ARCHIVE_LEGACY_MAX_SIZE);
    CHECK(cerberus_ble_archive_decode(legacy_full, CERBERUS_BLE_ARCHIVE_LEGACY_MAX_SIZE, &decoded));
    CHECK(decoded.count == 32 && !decoded.rows[31].receiver && !decoded.rows[31].receiver_boot_id);
    for (size_t offset = 0; offset < CERBERUS_BLE_ARCHIVE_LEGACY_MAX_SIZE; ++offset) {
        legacy_full[offset] ^= 1U; bad_decode(legacy_full, CERBERUS_BLE_ARCHIVE_LEGACY_MAX_SIZE);
        legacy_full[offset] ^= 1U;
    }
    put16(legacy_full + 14, 33); put32(legacy_full + 16, sizeof(legacy_full));
    seal(legacy_full, sizeof(legacy_full)); bad_decode(legacy_full, sizeof(legacy_full));
    memset(bytes, 0xba, sizeof(bytes));
    const size_t n = encode(&d, bytes);
    CHECK(n == sizeof(expected));
    CHECK(!memcmp(bytes, expected, n));
    CHECK(bytes[n] == 0xba);
    CHECK(!memcmp(&d, &copy, sizeof(d)));
    CHECK(cerberus_ble_archive_decode(bytes, n, &decoded));
    CHECK(!memcmp(&decoded, &copy, sizeof(copy)));
    for (size_t cap = 0; cap < n; ++cap) {
        uint8_t out[MAX]; memset(out, 0xcc, sizeof(out));
        size_t written = 11;
        CHECK(!cerberus_ble_archive_encode(&d, out, cap, &written));
        CHECK(!written);
        for (size_t j = 0; j < sizeof(out); ++j) CHECK(out[j] == 0xcc);
    }
    size_t written = 11;
    CHECK(!cerberus_ble_archive_encode(&d, NULL, n, &written)); CHECK(!written);
    CHECK(!cerberus_ble_archive_encode(&d, bytes, n, NULL));
    bad_model(NULL); bad_decode(NULL, n);
    CHECK(!cerberus_ble_archive_decode(bytes, n, NULL));
    CHECK(!cerberus_ble_archive_validate(NULL));
    for (size_t cut = 0; cut < n; ++cut) bad_decode(bytes, cut);
    bad_decode(bytes, n + 1);
    bad_decode(bytes, SIZE_MAX);
    CHECK(!cerberus_ble_archive_encode(&d, (uint8_t *)&d, sizeof(d), &written));
    CHECK(!memcmp(&d, &copy, sizeof(d)));
    CHECK(!cerberus_ble_archive_decode((const uint8_t *)&d, n, &d));
    CHECK(!memcmp(&d, &copy, sizeof(d)));
    CHECK(!cerberus_ble_archive_encode(&d, (uint8_t *)&d + sizeof(d) - 1, n, &written));
    CHECK(!memcmp(&d, &copy, sizeof(d)));
}

static void receiver_semantics(void)
{
    static const char golden_c5[] =
        "43424c45415243000200200090000100b0000000807060504030201074f54671"
        "010203040506020d03c9a5024c000303f8fe1f02341201000807060504030201"
        "000000000000000000000000000000004101ff00000000000000000000000000"
        "000000000000000000000000000000000d180000000000000000000000000000"
        "0201060000000000000000000000000000000000000000000000000000000000"
        "1032547698badcfe1122334455667788";
    cerberus_ble_archive_t c5 = sample(1), d, decoded;
    cerberus_ble_archive_row_t *r = &c5.rows[0];
    r->receiver = CERBERUS_BLE_ARCHIVE_RECEIVER_C5; r->rssi_kind = CERBERUS_BLE_ARCHIVE_RSSI_LATEST_ADV;
    r->source_time_valid = false; r->original_source_uptime_ms = r->copy_ceiling_ms = 0U;
    r->age_upper_valid = true; r->age_upper_at_save_ms = UINT64_C(0xfedcba9876543210);
    r->receiver_boot_id = UINT64_C(0x8877665544332211);
    uint8_t expected[176], wire[MAX]; from_hex(golden_c5, expected, sizeof(expected));
    CHECK(cerberus_ble_archive_decode(expected, sizeof(expected), &decoded));
    CHECK(!memcmp(&c5, &decoded, sizeof(c5)));
    CHECK(encode(&c5, wire) == HEADER + ROW);
    CHECK(cerberus_ble_archive_decode(wire, HEADER + ROW, &decoded));
    CHECK(!memcmp(&c5, &decoded, sizeof(c5)));
    /* Independent old v2 wire rows remain valid at the full 64-row bound. */
    uint8_t v2_full[CERBERUS_BLE_ARCHIVE_V2_MAX_SIZE];
    memcpy(v2_full, expected, HEADER);
    for (unsigned i = 0; i < 64U; ++i) {
        uint8_t *old = v2_full + HEADER + i * CERBERUS_BLE_ARCHIVE_V2_ROW_SIZE;
        memcpy(old, expected + HEADER, CERBERUS_BLE_ARCHIVE_V2_ROW_SIZE);
        old[5] = (uint8_t)i;
        if (i < 32U) {old[7] = 5; old[19] = 1; old[22] = 0; memset(old + 128, 0, 16);}
    }
    put16(v2_full + 14, 64); put32(v2_full + 16, sizeof(v2_full)); seal(v2_full, sizeof(v2_full));
    CHECK(cerberus_ble_archive_decode(v2_full, sizeof(v2_full), &decoded));
    CHECK(decoded.count == 64 && decoded.rows[0].receiver == 1 && decoded.rows[63].receiver == 2);
    for (unsigned i = 0; i < 64U; ++i) CHECK(decoded.rows[i].response_state == 0);
    for (size_t cut = 0; cut < sizeof(v2_full); ++cut) bad_decode(v2_full, cut);
    for (size_t i = 0; i < sizeof(v2_full); ++i) {
        v2_full[i] ^= 1U; bad_decode(v2_full, sizeof(v2_full)); v2_full[i] ^= 1U;
    }
    uint64_t age = 41;
    CHECK(!cerberus_ble_archive_age_at_save(&c5, 0, &age) && age == 41);
    CHECK(cerberus_ble_archive_age_upper_at_save(&c5, 0, &age) && age == r->age_upper_at_save_ms);
    CHECK(age > c5.saved_uptime_ms); /* A C5 bound is not an invented S3 uptime. */
    c5.saved_uptime_ms = 0; r->age_upper_at_save_ms = UINT64_MAX;
    CHECK(cerberus_ble_archive_age_upper_at_save(&c5, 0, &age) && age == UINT64_MAX);
    r->age_upper_at_save_ms = 0;
    CHECK(cerberus_ble_archive_age_upper_at_save(&c5, 0, &age) && age == 0);
    r->age_upper_valid = false; age = 41;
    CHECK(!cerberus_ble_archive_age_upper_at_save(&c5, 0, &age) && age == 41);
    CHECK(!cerberus_ble_archive_age_upper_at_save(NULL, 0, &age) && age == 41);
    CHECK(!cerberus_ble_archive_age_upper_at_save(&c5, SIZE_MAX, &age) && age == 41);
    CHECK(!cerberus_ble_archive_age_upper_at_save(&c5, 0, NULL));
    d = c5; d.rows[0].receiver_boot_id = 0; bad_model(&d);
    d = c5; d.rows[0].scan_epoch_valid = false; d.rows[0].scan_epoch = 0; bad_model(&d);
    d = c5; d.rows[0].source_time_valid = true; bad_model(&d);
    d = c5; d.rows[0].original_source_uptime_ms = 1; bad_model(&d);
    d = c5; d.rows[0].copy_ceiling_ms = 1; bad_model(&d);
    d = c5; d.rows[0].age_upper_at_save_ms = 1; bad_model(&d);
    for (unsigned receiver = 0; receiver < 256; ++receiver) {
        d = sample(1); d.rows[0].receiver = (uint8_t)receiver;
        CHECK(cerberus_ble_archive_validate(&d) == (receiver <= CERBERUS_BLE_ARCHIVE_RECEIVER_S3));
        d = c5; d.rows[0].receiver = (uint8_t)receiver;
        CHECK(cerberus_ble_archive_validate(&d) == (receiver == CERBERUS_BLE_ARCHIVE_RECEIVER_C5));
    }
    for (unsigned kind = 0; kind < 256; ++kind) {
        d = sample(1); d.rows[0].rssi_kind = (uint8_t)kind;
        CHECK(cerberus_ble_archive_validate(&d) == (kind == CERBERUS_BLE_ARCHIVE_RSSI_SWEEP_PEAK));
        d = c5; d.rows[0].rssi_kind = (uint8_t)kind;
        CHECK(cerberus_ble_archive_validate(&d) == (kind == CERBERUS_BLE_ARCHIVE_RSSI_LATEST_ADV));
    }
    for (unsigned receiver = 0; receiver <= 1; ++receiver) {
        d = sample(1); d.rows[0].receiver = (uint8_t)receiver;
        CHECK(!cerberus_ble_archive_age_upper_at_save(&d, 0, &age) && age == 41);
        d.rows[0].receiver_boot_id = 1; bad_model(&d); d.rows[0].receiver_boot_id = 0;
        d.rows[0].age_upper_valid = true; bad_model(&d); d.rows[0].age_upper_valid = false;
        d.rows[0].age_upper_at_save_ms = 1; bad_model(&d);
    }
    d = sample(64);
    for (unsigned i = 0; i < 32; ++i) d.rows[i].receiver = CERBERUS_BLE_ARCHIVE_RECEIVER_S3;
    memcpy(d.rows[32].address, d.rows[0].address, 6); d.rows[32].address_type = d.rows[0].address_type;
    CHECK(cerberus_ble_archive_validate(&d));
    size_t n = encode(&d, wire); CHECK(n == MAX);
    CHECK(cerberus_ble_archive_decode(wire, n, &decoded) && decoded.count == 64);
    CHECK(decoded.rows[0].receiver == 1 && decoded.rows[32].receiver == 2);
    d.rows[33] = d.rows[32]; bad_model(&d); /* Duplicate inside C5 source. */
    d = sample(33); d.rows[32] = d.rows[0]; d.rows[32].address[5] = 99;
    bad_model(&d); /*33 legacy observations, even though total<=64. */
    for (unsigned i = 0; i < 33; ++i) d.rows[i].receiver = CERBERUS_BLE_ARCHIVE_RECEIVER_S3;
    bad_model(&d); /*33 S3 observations. */
    for (unsigned i = 0; i < 33; ++i) {d.rows[i] = c5.rows[0]; d.rows[i].address[5] = (uint8_t)i;}
    d.saved_uptime_ms = 0; bad_model(&d); /*33 C5 observations. */
}

static void model_validation(void)
{
    cerberus_ble_archive_t good = sample(1), d;
    for (unsigned count = 0; count < 256; ++count) {
        d = sample(count <= 64 ? count : 1); d.count = (uint8_t)count;
        CHECK(cerberus_ble_archive_validate(&d) == (count >= 1 && count <= 64));
    }
    for (unsigned type = 0; type < 256; ++type) {
        d = good; d.rows[0].address_type = (uint8_t)type;
        CHECK(cerberus_ble_archive_validate(&d) == (type <= 3));
    }
    for (int signal = -128; signal <= 127; ++signal) {
        d = good; d.rows[0].rssi = (int8_t)signal;
        CHECK(cerberus_ble_archive_validate(&d) == (signal >= -127 && signal <= 20));
        d.rows[0].rssi_valid = false;
        CHECK(cerberus_ble_archive_validate(&d) == (signal == 127));
    }
    for (unsigned len = 0; len < 256; ++len) {
        d = good; d.rows[0].uuid_length = (uint8_t)len;
        CHECK(cerberus_ble_archive_validate(&d) == (len == 0 || len == 2 || len == 4 || len == 16));
        d = good; d.rows[0].advertisement_length = (uint8_t)len;
        CHECK(cerberus_ble_archive_validate(&d) == (len <= 31));
        d = good; d.rows[0].index = (uint8_t)len;
        CHECK(cerberus_ble_archive_validate(&d) == (len < 32));
    }
    d = good; memset(d.rows[0].name, 'x', sizeof(d.rows[0].name)); bad_model(&d);
    d.rows[0].name[32] = 0; CHECK(cerberus_ble_archive_validate(&d));
    d = good; d.rows[0].scan_epoch = 0; bad_model(&d);
    d = good; d.rows[0].scan_epoch_valid = false; bad_model(&d);
    d.rows[0].scan_epoch = 0; CHECK(cerberus_ble_archive_validate(&d));
    d = good; d.rows[0].source_time_valid = false; bad_model(&d);
    d.rows[0].original_source_uptime_ms = 0; bad_model(&d);
    d.rows[0].copy_ceiling_ms = 0; CHECK(cerberus_ble_archive_validate(&d));
    d = good; d.rows[0].copy_ceiling_ms = d.rows[0].original_source_uptime_ms - 1; bad_model(&d);
    d = good; d.saved_uptime_ms = d.rows[0].copy_ceiling_ms - 1; bad_model(&d);
    d = sample(2); d.rows[1] = d.rows[0]; bad_model(&d);
    d.rows[1].address_type = 3; CHECK(cerberus_ble_archive_validate(&d));
    d = sample(32); d.rows[31] = d.rows[1]; bad_model(&d);
}

static void time_and_projection(void)
{
    uint8_t wire[MAX], wire2[MAX];
    cerberus_ble_archive_t d = sample(1), decoded;
    uint64_t age = 999;
    CHECK(cerberus_ble_archive_age_at_save(&d, 0, &age));
    CHECK(age == d.saved_uptime_ms - d.rows[0].original_source_uptime_ms);
    const uint64_t saved_age = age;
    size_t n = encode(&d, wire);
    CHECK(cerberus_ble_archive_decode(wire, n, &decoded));
    CHECK(cerberus_ble_archive_age_at_save(&decoded, 0, &age) && age == saved_age);
    /* There is intentionally no current-clock argument: a reboot cannot make
     * this historical row fresh or reinterpret its uptime as the new boot. */
    CHECK(!cerberus_ble_archive_age_at_save(NULL, 0, &age) && age == saved_age);
    CHECK(!cerberus_ble_archive_age_at_save(&d, 1, &age) && age == saved_age);
    CHECK(!cerberus_ble_archive_age_at_save(&d, SIZE_MAX, &age) && age == saved_age);
    CHECK(!cerberus_ble_archive_age_at_save(&d, 0, NULL));
    d.rows[0].source_time_valid = false;
    d.rows[0].original_source_uptime_ms = d.rows[0].copy_ceiling_ms = 0;
    CHECK(!cerberus_ble_archive_age_at_save(&d, 0, &age) && age == saved_age);
    n = encode(&d, wire);
    CHECK(cerberus_ble_archive_decode(wire, n, &decoded));
    CHECK(!decoded.rows[0].source_time_valid);
    d.rows[0].source_time_valid = true; d.saved_uptime_ms = 0;
    CHECK(cerberus_ble_archive_age_at_save(&d, 0, &age) && !age);
    n = encode(&d, wire); CHECK(cerberus_ble_archive_decode(wire, n, &decoded));
    d.saved_uptime_ms = UINT64_MAX;
    CHECK(cerberus_ble_archive_age_at_save(&d, 0, &age) && age == UINT64_MAX);
    d.rows[0].original_source_uptime_ms = d.rows[0].copy_ceiling_ms = UINT64_MAX;
    CHECK(cerberus_ble_archive_age_at_save(&d, 0, &age) && !age);
    d.rows[0].copy_ceiling_ms = 0;
    age = 99; CHECK(!cerberus_ble_archive_age_at_save(&d, 0, &age) && age == 99);

    /* Ignored model tails/unused rows do not enter the canonical file, while
     * meaningful non-ASCII/control name bytes and all ADV octets survive. */
    d = sample(1); memset(d.rows[0].name, 'z', 32); d.rows[0].name[32] = 0;
    n = encode(&d, wire); CHECK(cerberus_ble_archive_decode(wire, n, &decoded));
    CHECK(!memcmp(d.rows[0].name, decoded.rows[0].name, 33));
    d.rows[0].name[0] = 0;
    n = encode(&d, wire); CHECK(cerberus_ble_archive_decode(wire, n, &decoded));
    for (size_t i = 0; i < 33; ++i) CHECK(!decoded.rows[0].name[i]);
    memset(d.rows[0].name + 1, 0xa5, 32);
    memset(d.rows[0].uuid + 2, 0xb6, 14);
    memset(d.rows[0].advertisement + 3, 0xc7, 28);
    memset(d.rows + 1, 0xdd, sizeof(d.rows) - sizeof(d.rows[0]));
    CHECK(encode(&d, wire2) == n && !memcmp(wire, wire2, n));
    for (unsigned byte = 0; byte < 256; ++byte) {
        d = sample(1); d.rows[0].advertisement_length = 31;
        d.rows[0].advertisement_flags = (uint8_t)byte;
        d.rows[0].advertisement_event_type = (uint8_t)byte;
        d.rows[0].peer_flags = (uint8_t)byte;
        for (unsigned i = 0; i < 31; ++i) d.rows[0].advertisement[i] = (uint8_t)(byte + i);
        n = encode(&d, wire); CHECK(cerberus_ble_archive_decode(wire, n, &decoded));
        CHECK(!memcmp(d.rows[0].advertisement, decoded.rows[0].advertisement, 31));
        CHECK(decoded.rows[0].advertisement_flags == byte);
        CHECK(decoded.rows[0].advertisement_event_type == byte);
        CHECK(decoded.rows[0].peer_flags == byte);
    }
}

static void hostile_files(void)
{
    uint8_t wire[MAX + 1], changed[MAX + 1];
    cerberus_ble_archive_t d = sample(64);
    const size_t n = encode(&d, wire);
    CHECK(n == MAX);
    for (size_t cut = 0; cut < n; ++cut) bad_decode(wire, cut);
    bad_decode(wire, n + 1);
    for (size_t i = 0; i < n; ++i) {
        memcpy(changed, wire, n); changed[i] ^= 1;
        bad_decode(changed, n); /* Any byte, including the full final row. */
    }
    memcpy(changed, wire, n); memcpy(changed + HEADER + 31 * ROW, changed + HEADER, 7);
    seal(changed, n); bad_decode(changed, n); /* Duplicate last row is atomic. */
    memcpy(changed, wire, n); changed[HEADER + 31 * ROW + 127] = 1;
    seal(changed, n); bad_decode(changed, n);
    const unsigned invalid_u16_offsets[] = {8, 10, 12, 14};
    for (unsigned k = 0; k < sizeof(invalid_u16_offsets) / sizeof(invalid_u16_offsets[0]); ++k)
        for (unsigned value = 0; value <= 65535; value += 257) {
            memcpy(changed, wire, n); put16(changed + invalid_u16_offsets[k], value); seal(changed, n);
            bad_decode(changed, n);
        }
    memcpy(changed, wire, n); put32(changed + 16, UINT32_MAX); seal(changed, n); bad_decode(changed, n);
    memcpy(changed, wire, n); put64(changed + HEADER + 40, UINT64_MAX); seal(changed, n); bad_decode(changed, n);
    memcpy(changed, wire, n); put64(changed + HEADER + 32, UINT64_MAX); seal(changed, n); bad_decode(changed, n);
}

static cerberus_ble_archive_t response_sample(void)
{
    cerberus_ble_archive_t d = sample(1);
    cerberus_ble_archive_row_t *r = &d.rows[0];
    r->receiver = CERBERUS_BLE_ARCHIVE_RECEIVER_S3;
    r->advertisement_flags = 0;
    r->response_state = CERBERUS_BLE_ARCHIVE_RESPONSE_PRESENT;
    r->response_length = r->response_original_length = 3;
    r->response_event_type = 4;
    r->response_original_sequence = 17; r->response_sequence = 19;
    r->response_original_observed_ms = 1; r->response_observed_ms = 3;
    r->response_time_valid = true; r->response_source_uptime_ms = UINT64_C(0x100000003);
    r->response[0] = 2; r->response[1] = 10; r->response[2] = 1;
    return d;
}

static void response_semantics(void)
{
    static const char golden[] =
        "43424c454152430003002000d0000100f000000080706050403020100b2e8d21"
        "010203040506020703c9a5024c00030300fe1f01341200000807060504030201"
        "010000000100000005000000010000004101ff00000000000000000000000000"
        "000000000000000000000000000000000d180000000000000000000000000000"
        "0201060000000000000000000000000000000000000000000000000000000000"
        "0000000000000000000000000000000002030304000002001100000013000000"
        "01000000030000000300000001000000020a0100000000000000000000000000"
        "00000000000000000000000000000000";
    static const char v4_golden[] =
        "43424c454152430004002000d8000100f800000080706050403020100ac87290"
        "010203040506020703c9a5024c00030300fe1f01341200000807060504030201"
        "010000000100000005000000010000004101ff00000000000000000000000000"
        "000000000000000000000000000000000d180000000000000000000000000000"
        "0201060000000000000000000000000000000000000000000000000000000000"
        "0000000000000000000000000000000002030304000002001100000013000000"
        "01000000030000000300000001000000020a0100000000000000000000000000"
        "000000000000000000000000000000000000000000000000";
    /* Independent Python struct/zlib v5 vector; old v4 vector stays literal. */
    static const char v5_golden[] =
        "43424c4541524300050020000801010028010000807060504030201066d73d51"
        "010203040506020703c9a5024c00030300fe1f01341200000807060504030201"
        "010000000100000005000000010000004101ff00000000000000000000000000"
        "000000000000000000000000000000000d180000000000000000000000000000"
        "0201060000000000000000000000000000000000000000000000000000000000"
        "0000000000000000000000000000000002030304000002001100000013000000"
        "01000000030000000300000001000000020a0100000000000000000000000000"
        "0000000000000000000000000000000000000000000000000000000000000000"
        "0000000000000000000000000000000000000000000000000000000000000000"
        "0000000000000000";

    /* Independent v6 empty-extension vector, Python struct/zlib. */
    static const char v6_golden[] =
        "43424c454152430006002000b0010100d00100008070605040302010f419601a"
        "010203040506020703c9a5024c00030300fe1f01341200000807060504030201"
        "010000000100000005000000010000004101ff00000000000000000000000000"
        "000000000000000000000000000000000d180000000000000000000000000000"
        "0201060000000000000000000000000000000000000000000000000000000000"
        "0000000000000000000000000000000002030304000002001100000013000000"
        "01000000030000000300000001000000020a0100000000000000000000000000"
        "0000000000000000000000000000000000000000000000000000000000000000"
        "0000000000000000000000000000000000000000000000000000000000000000"
        "0000000000000000000000000000000000000000000000000000000000000000"
        "0000000000000000000000000000000000000000000000000000000000000000"
        "0000000000000000000000000000000000000000000000000000000000000000"
        "0000000000000000000000000000000000000000000000000000000000000000"
        "0000000000000000000000000000000000000000000000000000000000000000"
        "00000000000000000000000000000000";

    cerberus_ble_archive_t good = response_sample(), d, decoded;
    uint8_t expected[464], v5[296], v4[248], v3[240], wire[MAX], changed[MAX];
    from_hex(golden, v3, sizeof(v3));
    from_hex(v4_golden, v4, sizeof(v4));
    from_hex(v5_golden, v5, sizeof(v5));
    from_hex(v6_golden, expected, sizeof(expected));
    CHECK(cerberus_ble_archive_decode(v5,sizeof(v5),&decoded));
    CHECK(!memcmp(&decoded,&good,sizeof(good)));
    for(size_t cut=0;cut<sizeof(v5);++cut)bad_decode(v5,cut);
    CHECK(cerberus_ble_archive_decode(v4, sizeof(v4), &decoded));
    CHECK(!memcmp(&decoded, &good, sizeof(good)));
    for(size_t cut=0;cut<sizeof(v4);++cut)bad_decode(v4,cut);
    for(size_t byte=0;byte<sizeof(v4);++byte){uint8_t corrupt[248];memcpy(corrupt,v4,sizeof(corrupt));
        corrupt[byte]^=1;bad_decode(corrupt,sizeof(corrupt));}
    CHECK(cerberus_ble_archive_decode(v3, sizeof(v3), &decoded));
    CHECK(!memcmp(&good, &decoded, sizeof(good))); /* Immutable v3 PRESENT vector retains its response. */
    for (size_t cut = 0; cut < sizeof(v3); ++cut) bad_decode(v3, cut);
    for (size_t i = 0; i < sizeof(v3); ++i) {
        uint8_t corrupt[240]; memcpy(corrupt, v3, sizeof(corrupt)); corrupt[i] ^= 1U;
        bad_decode(corrupt, sizeof(corrupt));
    }
    uint8_t v3_full[CERBERUS_BLE_ARCHIVE_V3_MAX_SIZE];
    memcpy(v3_full, v3, HEADER);
    for (unsigned i = 0; i < 64; ++i) {
        uint8_t *old = v3_full + HEADER + i * CERBERUS_BLE_ARCHIVE_V3_ROW_SIZE;
        memcpy(old, v3 + HEADER, CERBERUS_BLE_ARCHIVE_V3_ROW_SIZE);
        old[5] = (uint8_t)(i % 32U);
        if (i >= 32U) {
            old[7] = 5; old[19] = 2; old[22] = 1; memset(old + 32, 0, 16);
            put64(old + 136, UINT64_C(0x8877665544332211)); memset(old + 144, 0, 64);
        }
    }
    put16(v3_full + 14, 64); put32(v3_full + 16, sizeof(v3_full)); seal(v3_full, sizeof(v3_full));
    CHECK(cerberus_ble_archive_decode(v3_full, sizeof(v3_full), &decoded));
    CHECK(decoded.count == 64 && decoded.rows[31].response_state == 2 && decoded.rows[32].response_state == 0);
    for (unsigned i = 0; i < 64; ++i) {
        cerberus_dual_ble_receive_metadata_t zero = {0};
        CHECK(!memcmp(&decoded.rows[i].receive, &zero, sizeof(zero)));
        if (i < 32U) CHECK(decoded.rows[i].response_source_uptime_ms == good.rows[0].response_source_uptime_ms);
    }
    for (size_t cut = 0; cut < sizeof(v3_full); ++cut) bad_decode(v3_full, cut);
    for (size_t i = 0; i < sizeof(v3_full); ++i) {
        v3_full[i] ^= 1U; bad_decode(v3_full, sizeof(v3_full)); v3_full[i] ^= 1U;
    }
    CHECK(encode(&good, wire) == sizeof(expected)); CHECK(!memcmp(wire, expected, sizeof(expected)));
    CHECK(cerberus_ble_archive_decode(expected, sizeof(expected), &decoded));
    CHECK(!memcmp(&good, &decoded, sizeof(good)));
    uint64_t age = 71, original_age = 0;
    CHECK(cerberus_ble_archive_response_age_at_save(&decoded, 0, &age));
    CHECK(cerberus_ble_archive_age_at_save(&decoded, 0, &original_age));
    CHECK(age + 2 == original_age && age == good.saved_uptime_ms - UINT64_C(0x100000003));
    CHECK(!cerberus_ble_archive_response_age_at_save(NULL, 0, &age));
    CHECK(!cerberus_ble_archive_response_age_at_save(&good, 1, &age));
    CHECK(!cerberus_ble_archive_response_age_at_save(&good, SIZE_MAX, &age));
    CHECK(!cerberus_ble_archive_response_age_at_save(&good, 0, NULL));
    d = good; d.rows[0].response_time_valid = false; d.rows[0].response_source_uptime_ms = 0;
    const uint64_t before_age = age;
    CHECK(cerberus_ble_archive_validate(&d));
    CHECK(!cerberus_ble_archive_response_age_at_save(&d, 0, &age) && age == before_age);
    d.rows[0].copy_ceiling_ms = d.rows[0].original_source_uptime_ms + 1U;
    bad_model(&d); /* UNKNOWN exact response time cannot hide a response after its frozen copy. */
    d.rows[0].source_time_valid = false;
    d.rows[0].original_source_uptime_ms = d.rows[0].copy_ceiling_ms = 0;
    CHECK(cerberus_ble_archive_validate(&d)); /* Exact64 time can be unknown; source32 stays historical. */
    d.rows[0].response_time_valid = true; bad_model(&d);
    for (unsigned value = 0; value < 256; ++value) {
        d = good; d.rows[0].response_state = (uint8_t)value;
        CHECK(cerberus_ble_archive_validate(&d) == (value == 2));
        d = good; d.rows[0].response_flags = (uint8_t)value;
        CHECK(cerberus_ble_archive_validate(&d) == (value == 0));
        d = good; d.rows[0].response_event_type = (uint8_t)value;
        CHECK(cerberus_ble_archive_validate(&d) == (value == 4));
        d.rows[0].response_sid_valid = true; d.rows[0].response_sid = 15;
        CHECK(cerberus_ble_archive_validate(&d) == (value == 0 || value == 3));
        d = good; d.rows[0].response_original_length = (uint8_t)value;
        d.rows[0].response_length = (uint8_t)(value > 31 ? 31 : value);
        d.rows[0].response_flags = value > 31 ? 1 : 0;
        memset(d.rows[0].response, 0, 31);
        for (unsigned i = 0; i < d.rows[0].response_length; ++i) d.rows[0].response[i] = (uint8_t)i;
        CHECK(cerberus_ble_archive_validate(&d) == (value > 0));
        if (value) {size_t n = encode(&d, wire); CHECK(cerberus_ble_archive_decode(wire, n, &decoded));
            CHECK(!memcmp(&d, &decoded, sizeof(d)));}
    }
#define BAD_FIELD(field, value) do { d = good; d.rows[0].field = (value); bad_model(&d); } while (0)
    BAD_FIELD(response_length, 32); BAD_FIELD(response_length, 2);
    BAD_FIELD(response_original_length, 32); BAD_FIELD(response_original_length, 0);
    BAD_FIELD(response_sid, 1); BAD_FIELD(response_original_sequence, 0);
    BAD_FIELD(response_sequence, 17); BAD_FIELD(response_sequence, 0);
    BAD_FIELD(response_original_observed_ms, 2); BAD_FIELD(response_observed_ms, 0);
    BAD_FIELD(response_source_uptime_ms, UINT64_C(0x100000002));
    BAD_FIELD(response_source_uptime_ms, UINT64_C(0x100000006));
    BAD_FIELD(response_time_valid, false); BAD_FIELD(scan_epoch, 0);
    BAD_FIELD(scan_epoch_valid, false); BAD_FIELD(receiver, 0);
    BAD_FIELD(advertisement_event_type, 4); BAD_FIELD(advertisement_flags, 2);
    BAD_FIELD(advertisement_flags, 1); /* TRUNCATED requires a full31 original prefix. */
#undef BAD_FIELD
    d = good; d.rows[0].response[30] = 1; bad_model(&d);
    d = good; d.rows[0].response_sid_valid = true; d.rows[0].response_event_type = 3;
    d.rows[0].response_sid = 16; bad_model(&d);
    d.rows[0].response_sid = 15; CHECK(cerberus_ble_archive_validate(&d));
    d = good; d.rows[0].response_time_valid = false; d.rows[0].response_source_uptime_ms = 0;
    d.rows[0].copy_ceiling_ms = d.rows[0].original_source_uptime_ms + 2000U;
    d.rows[0].response_observed_ms = 2001; CHECK(cerberus_ble_archive_validate(&d));
    d.rows[0].response_observed_ms = 2002; bad_model(&d);
    d = good; d.rows[0].original_source_uptime_ms = UINT64_C(0x1ffffffff);
    d.rows[0].copy_ceiling_ms = UINT64_C(0x200000002);
    d.rows[0].response_original_observed_ms = UINT32_MAX; d.rows[0].response_observed_ms = 1;
    d.rows[0].response_source_uptime_ms = UINT64_C(0x200000001);
    CHECK(cerberus_ble_archive_validate(&d)); /* Modular source32 wrap, exact64 ordering. */
    d.rows[0].response_source_uptime_ms = 1; bad_model(&d);
    d = good; d.saved_uptime_ms = UINT64_MAX;
    d.rows[0].original_source_uptime_ms = d.rows[0].copy_ceiling_ms = UINT64_MAX;
    d.rows[0].response_original_observed_ms = d.rows[0].response_observed_ms = UINT32_MAX;
    d.rows[0].response_source_uptime_ms = UINT64_MAX;
    CHECK(cerberus_ble_archive_response_age_at_save(&d, 0, &age) && age == 0);
    d = good; d.rows[0].original_source_uptime_ms = d.rows[0].copy_ceiling_ms = 0;
    d.rows[0].response_original_observed_ms = d.rows[0].response_observed_ms = 0;
    d.rows[0].response_source_uptime_ms = 0; d.saved_uptime_ms = UINT64_MAX;
    CHECK(cerberus_ble_archive_response_age_at_save(&d, 0, &age) && age == UINT64_MAX);
    for (unsigned state = 0; state <= 1; ++state) {
        d = sample(1); d.rows[0].receiver = 1; d.rows[0].response_state = (uint8_t)state;
        CHECK(cerberus_ble_archive_validate(&d)); size_t n = encode(&d, wire);
        CHECK(cerberus_ble_archive_decode(wire, n, &decoded) && decoded.rows[0].response_state == state);
        CHECK(!cerberus_ble_archive_response_age_at_save(&decoded, 0, &age));
        for (unsigned offset = 145; offset < 208; ++offset) {
            memcpy(changed, wire, n); changed[HEADER + offset] = 1; seal(changed, n); bad_decode(changed, n);
        }
        d.rows[0].receiver = 0; CHECK(cerberus_ble_archive_validate(&d) == (state == 0));
    }
    d = sample(64);
    for (unsigned i = 0; i < 32; ++i) {d.rows[i] = good.rows[0]; d.rows[i].address[5] = (uint8_t)i;}
    size_t n = encode(&d, wire); CHECK(n == MAX && cerberus_ble_archive_decode(wire, n, &decoded));
    CHECK(decoded.rows[31].response_state == 2 && decoded.rows[32].response_state == 0);
    CHECK(!memcmp(&decoded, &d, sizeof(d)));
}

static cerberus_ble_archive_t receive_sample(void)
{
    cerberus_ble_archive_t d = sample(1);
    cerberus_ble_archive_row_t *r = &d.rows[0];
    r->receiver = CERBERUS_BLE_ARCHIVE_RECEIVER_C5; r->rssi_kind = CERBERUS_BLE_ARCHIVE_RSSI_LATEST_ADV;
    r->source_time_valid = false; r->original_source_uptime_ms = r->copy_ceiling_ms = 0;
    r->receiver_boot_id = UINT64_C(0x8877665544332211);
    r->age_upper_valid = true; r->age_upper_at_save_ms = UINT64_C(0xfedcba9876543210);
    r->advertisement_flags = 0;
    r->receive = (cerberus_dual_ble_receive_metadata_t){.source_kind=2,.callback_length=3,
        .sid=7,.primary_phy=3,.secondary_phy=1,.ad_validation=1};
    return d;
}

static void receive_semantics(void)
{
    static const char golden[] =
        "43424c454152430004002000d8000100f8000000807060504030201045e6bb85"
        "010203040506020d03c9a5024c00030300fe1f02341201000807060504030201"
        "000000000000000000000000000000004101ff00000000000000000000000000"
        "000000000000000000000000000000000d180000000000000000000000000000"
        "0201060000000000000000000000000000000000000000000000000000000000"
        "1032547698badcfe112233445566778800000000000000000000000000000000"
        "0000000000000000000000000000000000000000000000000000000000000000"
        "000000000000000000000000000000000200030703010001";
    /* Independent Python struct/zlib v5 vector; old v4 vector stays literal. */
    static const char v5_golden[] =
        "43424c4541524300050020000801010028010000807060504030201048ff8a9f"
        "010203040506020d03c9a5024c00030300fe1f02341201000807060504030201"
        "000000000000000000000000000000004101ff00000000000000000000000000"
        "000000000000000000000000000000000d180000000000000000000000000000"
        "0201060000000000000000000000000000000000000000000000000000000000"
        "1032547698badcfe112233445566778800000000000000000000000000000000"
        "0000000000000000000000000000000000000000000000000000000000000000"
        "0000000000000000000000000000000002000307030100010000000000000000"
        "0000000000000000000000000000000000000000000000000000000000000000"
        "0000000000000000";

    /* Independent v6 empty-extension vector, Python struct/zlib. */
    static const char v6_golden[] =
        "43424c454152430006002000b0010100d00100008070605040302010c327f5c3"
        "010203040506020d03c9a5024c00030300fe1f02341201000807060504030201"
        "000000000000000000000000000000004101ff00000000000000000000000000"
        "000000000000000000000000000000000d180000000000000000000000000000"
        "0201060000000000000000000000000000000000000000000000000000000000"
        "1032547698badcfe112233445566778800000000000000000000000000000000"
        "0000000000000000000000000000000000000000000000000000000000000000"
        "0000000000000000000000000000000002000307030100010000000000000000"
        "0000000000000000000000000000000000000000000000000000000000000000"
        "0000000000000000000000000000000000000000000000000000000000000000"
        "0000000000000000000000000000000000000000000000000000000000000000"
        "0000000000000000000000000000000000000000000000000000000000000000"
        "0000000000000000000000000000000000000000000000000000000000000000"
        "0000000000000000000000000000000000000000000000000000000000000000"
        "00000000000000000000000000000000";

    cerberus_ble_archive_t good = receive_sample(), d, decoded;
    uint8_t expected[464], v5[296], v4[248], wire[MAX], changed[MAX];
    from_hex(golden, v4, sizeof(v4));
    from_hex(v5_golden, v5, sizeof(v5));
    from_hex(v6_golden, expected, sizeof(expected));
    CHECK(cerberus_ble_archive_decode(v5,sizeof(v5),&decoded));
    CHECK(!memcmp(&decoded,&good,sizeof(good)));
    for(size_t cut=0;cut<sizeof(v5);++cut)bad_decode(v5,cut);
    CHECK(cerberus_ble_archive_decode(v4, sizeof(v4), &decoded));
    CHECK(!memcmp(&decoded, &good, sizeof(good)));
    for(size_t cut=0;cut<sizeof(v4);++cut)bad_decode(v4,cut);
    for(size_t byte=0;byte<sizeof(v4);++byte){uint8_t corrupt[248];memcpy(corrupt,v4,sizeof(corrupt));
        corrupt[byte]^=1;bad_decode(corrupt,sizeof(corrupt));}
    CHECK(encode(&good, wire) == sizeof(expected) && !memcmp(wire, expected, sizeof(expected)));
    CHECK(cerberus_ble_archive_decode(expected, sizeof(expected), &decoded));
    CHECK(!memcmp(&good, &decoded, sizeof(good)));
    uint64_t age = 0;
    CHECK(cerberus_ble_archive_age_upper_at_save(&decoded, 0, &age) && age == good.rows[0].age_upper_at_save_ms);
    CHECK(!cerberus_ble_archive_age_at_save(&decoded, 0, &age));
    CHECK(!decoded.rows[0].response_state && !decoded.rows[0].source_time_valid);
    for (unsigned field = 0; field < 8; ++field) for (unsigned value = 0; value < 256; ++value) {
        d = good; uint8_t *meta = (uint8_t *)&d.rows[0].receive; meta[field] = (uint8_t)value;
        bool valid = field == 0 ? value == 2 : field == 1 ? (value == 0 || value == 2 || value == 4 || value == 6) :
            field == 2 ? value == 3 : field == 3 ? (value <= 15 || value == 255) :
            field == 4 ? (value == 1 || value == 3) : field == 5 ? value <= 3 :
            field == 6 ? value == 0 : (value == 1 || value == 2);
        CHECK(cerberus_ble_archive_validate(&d) == valid);
        memcpy(changed, expected, sizeof(expected)); changed[HEADER + 208 + field] = (uint8_t)value;
        seal(changed, sizeof(expected));
        if (valid) {
            CHECK(cerberus_ble_archive_decode(changed, sizeof(expected), &decoded));
            CHECK(!memcmp(&decoded, &d, sizeof(d)));
        } else {bad_model(&d); bad_decode(changed, sizeof(expected));}
    }
    /* UNKNOWN is eight zero bytes on every source; old bytes cannot silently
     * turn into observed zero-valued PHY/SID/properties. */
    for (unsigned receiver = 0; receiver < 3; ++receiver) {
        d = receiver == 2 ? good : sample(1); d.rows[0].receiver = (uint8_t)receiver;
        memset(&d.rows[0].receive, 0, sizeof(d.rows[0].receive));
        CHECK(cerberus_ble_archive_validate(&d)); size_t n = encode(&d, wire);
        CHECK(cerberus_ble_archive_decode(wire, n, &decoded));
        cerberus_dual_ble_receive_metadata_t zero = {0};
        CHECK(!memcmp(&decoded.rows[0].receive, &zero, sizeof(zero)));
        for (unsigned byte = 0; byte < 8; ++byte) {
            memcpy(changed, wire, n); changed[HEADER + 208 + byte] = 1; seal(changed, n); bad_decode(changed, n);
        }
        if (receiver != 2) {d.rows[0].receive = good.rows[0].receive; bad_model(&d);}
    }
    d = good; d.rows[0].advertisement_length = 2; bad_model(&d);
    d = good; d.rows[0].advertisement_event_type = 0; bad_model(&d);
    d = good; d.rows[0].advertisement_event_type = 4; bad_model(&d);
    d = good; d.rows[0].advertisement_flags = 1; bad_model(&d);
    d = good; d.rows[0].advertisement_flags = 4; bad_model(&d);
    d = good; d.rows[0].advertisement_flags = 2; CHECK(cerberus_ble_archive_validate(&d));
    /* Actual full-container VALID and preview/parser MALFORMED are independent. */
    for (unsigned length = 0; length < 256; ++length) {
        d = good; d.rows[0].receive.callback_length = (uint8_t)length;
        d.rows[0].advertisement_length = (uint8_t)(length > 31 ? 31 : length);
        d.rows[0].advertisement_flags = length > 31 ? 3 : 0;
        memset(d.rows[0].advertisement, 0, sizeof(d.rows[0].advertisement));
        for (unsigned i = 0; i < d.rows[0].advertisement_length; ++i) d.rows[0].advertisement[i] = (uint8_t)i;
        size_t n = encode(&d, wire); CHECK(cerberus_ble_archive_decode(wire, n, &decoded));
        CHECK(!memcmp(&decoded, &d, sizeof(d)));
    }
    d = good;
    d.rows[0].receive = (cerberus_dual_ble_receive_metadata_t){.source_kind=1,
        .callback_length=3,.sid=255,.data_status=255,.ad_validation=1};
    CHECK(cerberus_ble_archive_validate(&d));
    d.rows[0].receive.data_status = 0; bad_model(&d); /* Legacy status is unavailable, not COMPLETE. */
    d.rows[0].receive.data_status = 255; d.rows[0].receive.sid = 0; bad_model(&d);
    const uint8_t properties[] = {0x10,0x12,0x13,0x15}, events[] = {3,2,0,1};
    for (unsigned i = 0; i < 4; ++i) {
        d = good; d.rows[0].receive.properties = properties[i]; d.rows[0].receive.sid = 255;
        d.rows[0].receive.primary_phy = 1; d.rows[0].receive.secondary_phy = 0;
        d.rows[0].advertisement_event_type = events[i];
        size_t n = encode(&d, wire); CHECK(cerberus_ble_archive_decode(wire, n, &decoded));
        CHECK(!memcmp(&d, &decoded, sizeof(d)));
        d.rows[0].receive.sid = 7; bad_model(&d);
    }
    d = sample(64); const cerberus_ble_archive_t response = response_sample();
    for (unsigned i = 0; i < 64; ++i) {
        d.rows[i] = i < 32 ? response.rows[0] : good.rows[0];
        d.rows[i].address[5] = (uint8_t)(i % 32); /* Same typed identity is independent across receivers. */
    }
    size_t n = encode(&d, wire); CHECK(n == MAX && cerberus_ble_archive_decode(wire, n, &decoded));
    CHECK(!memcmp(&d, &decoded, sizeof(d)));
    CHECK(decoded.rows[31].response_state == 2 && decoded.rows[63].receive.source_kind == 2);
    for (unsigned byte = 208; byte < 216; ++byte) {
        memcpy(changed, wire, n); changed[HEADER + 63 * ROW + byte] = 0xfe; seal(changed, n); bad_decode(changed, n);
    }
}


static cerberus_ble_archive_t family_sample(void)
{
    cerberus_ble_archive_t d=receive_sample();cerberus_ble_archive_row_t *r=&d.rows[0];
    uint8_t full[255]={39,0x30};memcpy(full+40,(uint8_t[]){6,9,'H','C','-','0','5'},7);
    r->advertisement_length=31;r->advertisement_flags=3;memcpy(r->advertisement,full,31);
    r->receive.callback_length=255;
    cerberus_dual_ble_family_evidence_t e;
    CHECK(cerberus_dual_ble_family_parse_ad(full,255,r->address,r->address_type,&e));
    CHECK(e.categories==CERBERUS_DUAL_BLE_FAMILY_HC_NAME_HINT&&e.hc_name==5);
    CHECK(cerberus_dual_ble_family_wire_encode(&e,6,&r->family));
    return d;
}

static void family_semantics(void)
{
    /* Independent v5 full255 HC-name proof vector, Python struct/zlib. */
    static const char golden[] =
        "43424c45415243000500200008010100280100008070605040302010c3258300"
        "010203040506020d03c9a5024c001f0303fe1f02341201000807060504030201"
        "000000000000000000000000000000004101ff00000000000000000000000000"
        "000000000000000000000000000000000d180000000000000000000000000000"
        "2730000000000000000000000000000000000000000000000000000000000000"
        "1032547698badcfe112233445566778800000000000000000000000000000000"
        "0000000000000000000000000000000000000000000000000000000000000000"
        "000000000000000000000000000000000200ff07030100010106012005000100"
        "0000000000000000000000000000000000000000000000000000000000000000"
        "0000000000000000";

    /* Independent v6 empty-extension vector, Python struct/zlib. */
    static const char v6_golden[] =
        "43424c454152430006002000b0010100d00100008070605040302010999ec312"
        "010203040506020d03c9a5024c001f0303fe1f02341201000807060504030201"
        "000000000000000000000000000000004101ff00000000000000000000000000"
        "000000000000000000000000000000000d180000000000000000000000000000"
        "2730000000000000000000000000000000000000000000000000000000000000"
        "1032547698badcfe112233445566778800000000000000000000000000000000"
        "0000000000000000000000000000000000000000000000000000000000000000"
        "000000000000000000000000000000000200ff07030100010106012005000100"
        "0000000000000000000000000000000000000000000000000000000000000000"
        "0000000000000000000000000000000000000000000000000000000000000000"
        "0000000000000000000000000000000000000000000000000000000000000000"
        "0000000000000000000000000000000000000000000000000000000000000000"
        "0000000000000000000000000000000000000000000000000000000000000000"
        "0000000000000000000000000000000000000000000000000000000000000000"
        "00000000000000000000000000000000";

    _Static_assert(ROW==432&&MAX==27680,"v6 explicit wire sizes");
    uint8_t expected[464], v5[296],wire[MAX],changed[MAX],reencoded[MAX];
    from_hex(golden, v5, sizeof(v5));
    from_hex(v6_golden, expected, sizeof(expected));
    cerberus_ble_archive_t good=family_sample(),d,decoded;
    CHECK(cerberus_ble_archive_decode(v5,sizeof(v5),&decoded));
    CHECK(!memcmp(&decoded,&good,sizeof(good)));
    for(size_t cut=0;cut<sizeof(v5);++cut)bad_decode(v5,cut);
    CHECK(encode(&good,wire)==sizeof(expected)&&!memcmp(wire,expected,sizeof(expected)));
    CHECK(cerberus_ble_archive_decode(expected,sizeof(expected),&decoded));
    CHECK(!memcmp(&good,&decoded,sizeof(good)));
    CHECK(decoded.rows[0].family.hc_name==5&&decoded.rows[0].receive.callback_length==255);
    for(size_t cut=0;cut<sizeof(expected);++cut)bad_decode(expected,cut);
    for(size_t offset=0;offset<sizeof(expected);++offset){
        memcpy(changed,expected,sizeof(expected));changed[offset]^=1;bad_decode(changed,sizeof(expected));
    }
    for(unsigned fault=0;fault<20;++fault){
        d=good;
        if(fault==0)d.rows[0].family.version=0; /* Not absent unless ALL48 bytes are zero. */
        if(fault==1)d.rows[0].family.version=2;
        if(fault==2)d.rows[0].family.family_code=0;
        if(fault==3)d.rows[0].family.family_code=7;
        if(fault==4)d.rows[0].family.reserved[0]=1;
        if(fault==5)d.rows[0].family.reserved[1]=1;
        if(fault==6)d.rows[0].family.flags|=8;
        if(fault==7)d.rows[0].family.meta_pairs[9]=0x80;
        if(fault==8)d.rows[0].family.categories=0;
        if(fault==9)d.rows[0].family.hc_name=4;
        if(fault==10)d.rows[0].family.state=CERBERUS_DUAL_BLE_FAMILY_NO_PROVENANCE;
        if(fault==11)memset(&d.rows[0].receive,0,sizeof(d.rows[0].receive));
        if(fault==12)d.rows[0].receive.ad_validation=2;
        if(fault==13)d.rows[0].receiver_boot_id=0;
        if(fault==14){d.rows[0].scan_epoch_valid=false;d.rows[0].scan_epoch=0;}
        if(fault==15)d.rows[0].address_type=4;
        if(fault==16)d.rows[0].advertisement_event_type=4;
        if(fault==17)d.rows[0].advertisement_flags=0;
        if(fault>=18){
            d.rows[0].receiver=fault==18?CERBERUS_BLE_ARCHIVE_RECEIVER_S3:CERBERUS_BLE_ARCHIVE_RECEIVER_LEGACY;
            d.rows[0].rssi_kind=0;d.rows[0].receiver_boot_id=0;d.rows[0].age_upper_valid=false;d.rows[0].age_upper_at_save_ms=0;
            memset(&d.rows[0].receive,0,sizeof(d.rows[0].receive));
        }
        bad_model(&d);
    }
    const unsigned bad_offsets[]={216,217,218,219,220,221,222,224,225,226,227,228,229,230,240,262,263};
    for(unsigned j=0;j<sizeof(bad_offsets)/sizeof(*bad_offsets);++j){
        memcpy(changed,expected,sizeof(expected));changed[HEADER+bad_offsets[j]]=0xff;
        seal(changed,sizeof(expected));bad_decode(changed,sizeof(expected));
    }
    /* CRC-correct mutations cover every family octet and every value. Accepted
     * representations must re-encode byte-for-byte; failures keep the old model. */
    for(unsigned offset=216;offset<264;++offset)for(unsigned value=0;value<256;++value){
        memcpy(changed,expected,sizeof(expected));changed[HEADER+offset]=(uint8_t)value;seal(changed,sizeof(expected));
        memset(&decoded,0xa5,sizeof(decoded));d=decoded;
        if(cerberus_ble_archive_decode(changed,sizeof(expected),&decoded)){
            CHECK(cerberus_ble_archive_validate(&decoded));CHECK(encode(&decoded,reencoded)==sizeof(expected));
            CHECK(!memcmp(changed,reencoded,sizeof(expected)));
        }else CHECK(!memcmp(&d,&decoded,sizeof(d)));
    }
    /* Genuine unavailable and complete-nohint records keep that state; these
     * are not malformed transport and cannot resurrect an old short hint. */
    for(unsigned kind=0;kind<3;++kind){
        d=good;cerberus_dual_ble_family_evidence_t e;
        if(kind==0){CHECK(cerberus_dual_ble_family_parse_ad(NULL,0,d.rows[0].address,2,&e));}
        else cerberus_dual_ble_family_unavailable(&e,kind==1?CERBERUS_DUAL_BLE_FAMILY_MALFORMED:CERBERUS_DUAL_BLE_FAMILY_RECEIVE_UNKNOWN);
        CHECK(cerberus_dual_ble_family_wire_encode(&e,6,&d.rows[0].family));
        if(kind==1)d.rows[0].receive.ad_validation=2;
        if(kind==2)memset(&d.rows[0].receive,0,sizeof(d.rows[0].receive));
        size_t n=encode(&d,wire);CHECK(cerberus_ble_archive_decode(wire,n,&decoded));CHECK(!memcmp(&d,&decoded,sizeof(d)));
    }
    /* Mixed64 rows keep full evidence only on C5, while S3 response proof and
     * the same typed addresses remain independent source observations. */
    d=sample(64);const cerberus_ble_archive_t response=response_sample();
    for(unsigned i=0;i<64;++i){d.rows[i]=i<32?response.rows[0]:good.rows[0];d.rows[i].address[5]=(uint8_t)(i%32);}
    size_t n=encode(&d,wire);CHECK(n==MAX&&cerberus_ble_archive_decode(wire,n,&decoded));CHECK(!memcmp(&d,&decoded,sizeof(d)));
    CHECK(decoded.rows[31].response_state==2&&decoded.rows[63].family.hc_name==5);
    /* All-or-nothing preflight, including a fault in the LAST row. */
    memcpy(changed,wire,n);changed[HEADER+63*ROW+263]=1;seal(changed,n);bad_decode(changed,n);
    /* Construct a v4 compatibility fixture by retaining its documented216
     * byte prefix; the omitted proof must stay absent upon subsequent v5 save. */
    uint8_t v4[CERBERUS_BLE_ARCHIVE_V4_MAX_SIZE];memcpy(v4,wire,HEADER);
    for(unsigned i=0;i<64;++i)memcpy(v4+HEADER+i*216,wire+HEADER+i*ROW,216);
    put16(v4+8,4);put16(v4+12,216);put32(v4+16,sizeof(v4));seal(v4,sizeof(v4));
    CHECK(cerberus_ble_archive_decode(v4,sizeof(v4),&decoded));
    cerberus_dual_ble_family_wire_t absent={0};
    for(unsigned i=0;i<64;++i)CHECK(!memcmp(&decoded.rows[i].family,&absent,sizeof(absent)));
    CHECK(decoded.rows[31].response_state==2&&decoded.rows[63].receive.callback_length==255);
    CHECK(encode(&decoded,changed)==MAX&&memcmp(wire,changed,MAX));
    CHECK(cerberus_ble_archive_decode(changed,MAX,&d));CHECK(!memcmp(&d,&decoded,sizeof(d)));
    for(size_t cut=0;cut<sizeof(v4);++cut)bad_decode(v4,cut);
    for(size_t byte=0;byte<sizeof(v4);++byte){v4[byte]^=1;bad_decode(v4,sizeof(v4));v4[byte]^=1;}
}

static void canonical_mutations(void)
{
    uint8_t good[MAX], wire[MAX], reencoded[MAX];
    for (unsigned kind = 0; kind < 3; ++kind) {
    cerberus_ble_archive_t seed = kind == 2 ? receive_sample() : kind == 1 ? response_sample() : sample(1);
    const size_t n = encode(&seed, good);
    for (size_t offset = 0; offset < n; ++offset) {
        if (offset >= 28 && offset < 32) continue;
        for (unsigned value = 0; value < 256; ++value) {
            memcpy(wire, good, n); wire[offset] = (uint8_t)value; seal(wire, n);
            cerberus_ble_archive_t result, before;
            memset(&result, 0xc5, sizeof(result)); before = result;
            if (cerberus_ble_archive_decode(wire, n, &result)) {
                CHECK(cerberus_ble_archive_validate(&result));
                CHECK(encode(&result, reencoded) == n);
                CHECK(!memcmp(wire, reencoded, n));
                for (unsigned i = 1; i < 64; ++i) {
                    cerberus_ble_archive_row_t zero = {0};
                    CHECK(!memcmp(&result.rows[i], &zero, sizeof(zero)));
                }
            } else CHECK(!memcmp(&result, &before, sizeof(result)));
        }
    }
    }
}

static uint32_t rng_state = UINT32_C(0x1379bdf);
static uint32_t random32(void)
{
    rng_state ^= rng_state << 13; rng_state ^= rng_state >> 17; rng_state ^= rng_state << 5;
    return rng_state;
}

static void generated_roundtrips(void)
{
    static const uint8_t uuid_lengths[] = {0, 2, 4, 16};
    uint8_t first[MAX], second[MAX];
    for (unsigned iteration = 0; iteration < 1500; ++iteration) {
        cerberus_ble_archive_t d = sample(1 + random32() % 64), decoded;
        d.saved_uptime_ms = (uint64_t)random32() << 32 | random32();
        for (unsigned i = 0; i < d.count; ++i) {
            cerberus_ble_archive_row_t *r = &d.rows[i];
            r->address_type = (uint8_t)(random32() % 4);
            memset(r->name, 0, sizeof(r->name));
            const unsigned length = random32() % 33;
            for (unsigned j = 0; j < length; ++j) r->name[j] = (char)(1 + random32() % 255);
            r->rssi_valid = (random32() & 1) != 0;
            r->rssi = r->rssi_valid ? (int8_t)((int)(random32() % 148) - 127) : 127;
            r->peer_flags = (uint8_t)random32(); r->manufacturer_id = (uint16_t)random32();
            r->uuid_length = uuid_lengths[random32() % 4];
            for (unsigned j = 0; j < 16; ++j) r->uuid[j] = (uint8_t)random32();
            r->advertisement_length = (uint8_t)(random32() % 32);
            for (unsigned j = 0; j < 31; ++j) r->advertisement[j] = (uint8_t)random32();
            r->advertisement_event_type = (uint8_t)random32();
            r->advertisement_flags = (uint8_t)random32();
            r->reports_seen = (uint16_t)random32(); r->generation = (uint8_t)random32();
            r->index = (uint8_t)(random32() % 32);
            r->scan_epoch_valid = (random32() & 1) != 0;
            r->scan_epoch = r->scan_epoch_valid ? (uint64_t)random32() + 1 : 0;
            r->source_time_valid = (random32() & 1) != 0;
            r->original_source_uptime_ms = r->source_time_valid ? d.saved_uptime_ms / 3 : 0;
            r->copy_ceiling_ms = r->source_time_valid ? d.saved_uptime_ms / 2 : 0;
            if (r->receiver == CERBERUS_BLE_ARCHIVE_RECEIVER_C5) {
                r->scan_epoch_valid = true; r->scan_epoch = (uint64_t)random32() + 1U;
                r->source_time_valid = false;
                r->original_source_uptime_ms = r->copy_ceiling_ms = 0U;
                r->age_upper_valid = (random32() & 1U) != 0U;
                r->age_upper_at_save_ms = r->age_upper_valid ? (uint64_t)random32() << 32 | random32() : 0U;
            }
        }
        cerberus_ble_archive_t before = d;
        const size_t n = encode(&d, first);
        CHECK(!memcmp(&d, &before, sizeof(d)));
        CHECK(cerberus_ble_archive_decode(first, n, &decoded));
        CHECK(cerberus_ble_archive_validate(&decoded));
        CHECK(encode(&decoded, second) == n && !memcmp(first, second, n));
        for (unsigned i = 0; i < d.count; ++i) {
            CHECK(!memcmp(d.rows[i].name, decoded.rows[i].name, sizeof(d.rows[i].name)));
            CHECK(!memcmp(d.rows[i].advertisement, decoded.rows[i].advertisement, d.rows[i].advertisement_length));
            CHECK(decoded.rows[i].address_type == d.rows[i].address_type);
            CHECK(!memcmp(decoded.rows[i].address, d.rows[i].address, 6));
        }
    }
}

static cerberus_ble_archive_t c5_response_sample(void)
{
    cerberus_ble_archive_t d=sample(1);
    cerberus_ble_archive_row_t *r=&d.rows[0];
    r->receiver=CERBERUS_BLE_ARCHIVE_RECEIVER_C5;
    r->rssi_kind=CERBERUS_BLE_ARCHIVE_RSSI_LATEST_ADV;
    r->source_time_valid=false;r->original_source_uptime_ms=r->copy_ceiling_ms=0U;
    r->receiver_boot_id=UINT64_C(0x8877665544332211);
    CHECK(archive_test_response_row(r,6U,6U));
    return d;
}

static void c5_response_semantics(void)
{
    /* Independent struct/zlib vector, not generated by the production codec. */
    static const char c5_response_golden[] =
        "43424c454152430006002000b0010100d001000080706050403020105da73dd2"
        "010203040506020d03c9a5024c00030300fe1f02341201000807060504030201"
        "000000000000000000000000000000004101ff00000000000000000000000000"
        "000000000000000000000000000000000d180000000000000000000000000000"
        "0201060000000000000000000000000000000000000000000000000000000000"
        "0d02000000000000112233445566778800000000000000000000000000000000"
        "0000000000000000000000000000000000000000000000000000000000000000"
        "0000000000000000000000000000000002020307010200010106010000000100"
        "0000000800000000000000000000000000000000000000000000000000000000"
        "00000000000000000102071f0305d10108070605040302010907060504030201"
        "7b000000d0070000020aff070102000127300000000000000000000000000000"
        "00000000000000000000000000000048432d3035000000000000000000000000"
        "0000000000000000000000000000000106012005000100000000000000000000"
        "0000000000000000000000000000000000000000000000000000000000000000"
        "01000000000000000b02000000000000";
    cerberus_ble_archive_t d=c5_response_sample(), decoded;
    uint8_t good[MAX], wire[MAX], again[MAX], expected[464];
    from_hex(c5_response_golden,expected,sizeof(expected));
    size_t n=encode(&d,good);
    for(size_t i=0;i<sizeof(expected);++i) if(good[i]!=expected[i])
        fprintf(stderr,"C5 golden byte %zu: got %02x expected %02x\n",i,good[i],expected[i]);
    CHECK(n==sizeof(expected) && !memcmp(good,expected,n));
    CHECK(cerberus_ble_archive_decode(expected,sizeof(expected),&decoded));
    CHECK(!memcmp(&d,&decoded,sizeof(d)));
    uint64_t age=0;
    CHECK(cerberus_ble_archive_c5_response_age_upper_at_save(&decoded,0,&age) && age==523U);
    age=999U;
    CHECK(!cerberus_ble_archive_c5_response_age_upper_at_save(&decoded,1,&age) && age==999U);
    CHECK(!cerberus_ble_archive_c5_response_age_upper_at_save(NULL,0,&age) && age==999U);
    CHECK(!cerberus_ble_archive_c5_response_age_upper_at_save(&decoded,0,NULL));
    /* Response evidence is from full callback bytes beyond the raw preview. */
    for(uint8_t family=1;family<=6;++family) for(uint8_t selected=0;selected<=1;++selected) {
        d=c5_response_sample();
        CHECK(archive_test_response_row(&d.rows[0],family,selected?family:0U));
        n=encode(&d,wire);
        CHECK(cerberus_ble_archive_decode(wire,n,&decoded));
        CHECK(!memcmp(&d,&decoded,sizeof(d)));
        cerberus_dual_ble_family_evidence_t facts;
        CHECK(cerberus_dual_ble_response_family(&decoded.rows[0].c5_response,
            decoded.rows[0].address,decoded.rows[0].address_type,&facts));
        CHECK(facts.categories & (1U<<(family-1U)));
        CHECK(decoded.rows[0].c5_response.receive.callback_length==255U);
        CHECK(decoded.rows[0].reports_seen==0x1234U && decoded.rows[0].rssi==-55);
        CHECK(!decoded.rows[0].source_time_valid && !decoded.rows[0].response_time_valid);
    }
    /* Every extension byte/value with a repaired checksum is either canonical
     * and byte-stable, or rejected without modifying the destination. */
    n=sizeof(expected);
    for(size_t offset=HEADER+264U;offset<HEADER+ROW;++offset) for(unsigned value=0;value<256U;++value) {
        memcpy(wire,expected,n);wire[offset]=(uint8_t)value;seal(wire,n);
        cerberus_ble_archive_t before;
        memset(&decoded,0xa7,sizeof(decoded));before=decoded;
        if(cerberus_ble_archive_decode(wire,n,&decoded)) {
            CHECK(cerberus_ble_archive_validate(&decoded));
            CHECK(encode(&decoded,again)==n && !memcmp(wire,again,n));
        } else CHECK(!memcmp(&decoded,&before,sizeof(decoded)));
    }
    d=c5_response_sample();
    for(unsigned fault=0;fault<18U;++fault) {
        cerberus_ble_archive_t bad=d;
        cerberus_ble_archive_row_t *r=&bad.rows[0];
        switch(fault) {
        case 0:r->c5_response.version=2;break;
        case 1:r->c5_response.state=3;break;
        case 2:r->c5_response.original_sequence=0;break;
        case 3:r->c5_response.response_sequence=r->c5_response.original_sequence;break;
        case 4:r->c5_response.interval_us=2000001U;break;
        case 5:r->c5_response.receive.sid=8;break;
        case 6:r->c5_response.receive.ad_validation=2;break;
        case 7:r->c5_response.family.family_code=5;break;
        case 8:r->c5_response.reserved_tail=1;break;
        case 9:r->c5_response_age_upper_at_save_ms=526;break;
        case 10:r->c5_response_age_upper_at_save_ms=522;break;
        case 11:r->c5_response_age_upper_valid=false;break;
        case 12:r->c5_response.age_ms=UINT32_MAX;break;
        case 13:r->c5_response.flags&=(uint8_t)~CERBERUS_DUAL_BLE_RESPONSE_AGE_VALID;break;
        case 14:r->c5_response.name_length=33;break;
        case 15:r->c5_response.raw_length=32;break;
        case 16:r->c5_response.flags|=16U;break;
        case 17:r->response_state=CERBERUS_BLE_ARCHIVE_RESPONSE_PRESENT;break;
        }
        bad_model(&bad);
    }
    /* NONE asserts only a retained eligible original. UNKNOWN is all-zero;
     * neither fabricates response time, RSSI, content, or an absence claim. */
    d=c5_response_sample();
    memset(&d.rows[0].c5_response,0,sizeof(d.rows[0].c5_response));
    d.rows[0].c5_response.version=1;d.rows[0].c5_response.state=1;
    d.rows[0].c5_response.original_sequence=UINT64_MAX;
    d.rows[0].c5_response_age_upper_valid=false;d.rows[0].c5_response_age_upper_at_save_ms=0;
    n=encode(&d,wire);CHECK(cerberus_ble_archive_decode(wire,n,&decoded));
    age=999;CHECK(!cerberus_ble_archive_c5_response_age_upper_at_save(&decoded,0,&age) && age==999);
    memset(&d.rows[0].c5_response,0,sizeof(d.rows[0].c5_response));
    n=encode(&d,wire);CHECK(cerberus_ble_archive_decode(wire,n,&decoded));
    CHECK(!memcmp(&d,&decoded,sizeof(d)));
    d=c5_response_sample();d.rows[0].c5_response.flags&=(uint8_t)~CERBERUS_DUAL_BLE_RESPONSE_AGE_VALID;
    d.rows[0].c5_response.age_ms=UINT32_MAX;d.rows[0].c5_response_age_upper_valid=false;
    d.rows[0].c5_response_age_upper_at_save_ms=0;n=encode(&d,wire);
    CHECK(cerberus_ble_archive_decode(wire,n,&decoded) && !memcmp(&d,&decoded,sizeof(d)));
    /* The final row is validated before any output is changed. */
    d=sample(64);
    for(unsigned i=32;i<64;++i) CHECK(archive_test_response_row(&d.rows[i],(uint8_t)(1U+i%6U),0));
    n=encode(&d,wire);CHECK(n==MAX && cerberus_ble_archive_decode(wire,n,&decoded));
    CHECK(!memcmp(&d,&decoded,sizeof(d)));
    wire[HEADER+63U*ROW+417U]=1;seal(wire,n);bad_decode(wire,n);
    d.rows[63].c5_response.reserved_tail=1;bad_model(&d);
    d=sample(1);age=999;CHECK(!cerberus_ble_archive_c5_response_age_upper_at_save(&d,0,&age) && age==999);
}

int main(void)
{
    crc_init();
    golden_and_bounds();
    receiver_semantics();
    model_validation();
    time_and_projection();
    hostile_files();
    response_semantics();
    receive_semantics();
    family_semantics();
    c5_response_semantics();
    canonical_mutations();
    generated_roundtrips();
    printf("BLE archive codec: PASS %u checks; header=%u row=%u max=%u; host document=%zu row=%zu bytes\n",
        checks, HEADER, ROW, MAX, sizeof(cerberus_ble_archive_t), sizeof(cerberus_ble_archive_row_t));
    return 0;
}
