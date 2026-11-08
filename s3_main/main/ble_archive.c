#include "ble_archive.h"
#include "ble_family_wire.h"
#include "ble_response.h"

#include <string.h>

enum {
    ROW_RSSI_VALID = 1U << 0,
    ROW_SOURCE_TIME_VALID = 1U << 1,
    ROW_SCAN_EPOCH_VALID = 1U << 2,
    ROW_AGE_UPPER_VALID = 1U << 3,
    ROW_VALIDITY_MASK = ROW_RSSI_VALID | ROW_SOURCE_TIME_VALID | ROW_SCAN_EPOCH_VALID,
    ROW_V2_VALIDITY_MASK = ROW_VALIDITY_MASK | ROW_AGE_UPPER_VALID,
};

static const uint8_t archive_magic[8] = {'C', 'B', 'L', 'E', 'A', 'R', 'C', 0};
static bool all_zero(const uint8_t *bytes, size_t length);

static bool c5_response_valid(const cerberus_ble_archive_row_t *r)
{
    if (r->receiver != CERBERUS_BLE_ARCHIVE_RECEIVER_C5)
        return all_zero((const uint8_t *)&r->c5_response, sizeof(r->c5_response)) &&
            !r->c5_response_age_upper_valid && !r->c5_response_age_upper_at_save_ms;
    const uint8_t family = r->family.version ? r->family.family_code : 0U;
    if (!cerberus_dual_ble_response_source_valid(&r->c5_response, &r->receive,
            r->advertisement_length, r->advertisement_event_type, r->advertisement_flags,
            r->address, r->address_type, family)) return false;
    if (!r->c5_response_age_upper_valid) return !r->c5_response_age_upper_at_save_ms;
    if (r->c5_response.state != CERBERUS_DUAL_BLE_RESPONSE_PRESENT ||
        !(r->c5_response.flags & CERBERUS_DUAL_BLE_RESPONSE_AGE_VALID) ||
        r->c5_response_age_upper_at_save_ms < r->c5_response.age_ms) return false;
    if (r->age_upper_valid) {
        if (r->c5_response_age_upper_at_save_ms > r->age_upper_at_save_ms) return false;
        const uint64_t elapsed = r->age_upper_at_save_ms - r->c5_response_age_upper_at_save_ms;
        /* Both bounds share the first-BEGIN S3 anchor. Millisecond rounding
         * may straddle one tick; the underlying source interval stays in us. */
        if (elapsed < r->c5_response.interval_us / 1000U ||
            elapsed > (r->c5_response.interval_us + 999U) / 1000U) return false;
    }
    return true;
}

static bool response_valid(const cerberus_ble_archive_row_t *r)
{
    if (r->response_state > CERBERUS_BLE_ARCHIVE_RESPONSE_PRESENT ||
        (r->receiver != CERBERUS_BLE_ARCHIVE_RECEIVER_S3 &&
         r->response_state != CERBERUS_BLE_ARCHIVE_RESPONSE_UNKNOWN)) return false;
    if (r->response_state != CERBERUS_BLE_ARCHIVE_RESPONSE_PRESENT) {
        return !r->response_length && !r->response_original_length &&
            !r->response_event_type && !r->response_flags && !r->response_sid &&
            !r->response_sid_valid && !r->response_time_valid &&
            !r->response_original_sequence && !r->response_sequence &&
            !r->response_original_observed_ms && !r->response_observed_ms &&
            !r->response_source_uptime_ms && all_zero(r->response, sizeof(r->response));
    }
    const uint8_t length = r->response_original_length > CERBERUS_BLE_ARCHIVE_ADV_MAX ?
        CERBERUS_BLE_ARCHIVE_ADV_MAX : r->response_original_length;
    const uint32_t elapsed = r->response_observed_ms - r->response_original_observed_ms;
    if (!r->scan_epoch_valid || !r->scan_epoch || !length || r->response_length != length ||
        r->response_flags != (r->response_original_length > length ? 1U : 0U) ||
        !r->response_original_sequence || r->response_sequence <= r->response_original_sequence ||
        elapsed > 2000U || r->advertisement_event_type > 3U ||
        (r->advertisement_flags & ~1U) ||
        ((r->advertisement_flags & 1U) && r->advertisement_length != CERBERUS_BLE_ARCHIVE_ADV_MAX) ||
        !all_zero(r->response + length, sizeof(r->response) - length)) return false;
    if (r->response_sid_valid ? (r->response_sid > 15U ||
            (r->response_event_type != 0U && r->response_event_type != 3U)) :
        (r->response_sid || r->response_event_type != 4U)) return false;
    if (r->source_time_valid &&
        ((uint32_t)r->original_source_uptime_ms != r->response_original_observed_ms ||
         elapsed > r->copy_ceiling_ms - r->original_source_uptime_ms)) return false;
    if (r->response_time_valid) {
        if (!r->source_time_valid || r->response_source_uptime_ms < r->original_source_uptime_ms ||
            r->response_source_uptime_ms > r->copy_ceiling_ms ||
            r->response_source_uptime_ms - r->original_source_uptime_ms != elapsed ||
            (uint32_t)r->response_source_uptime_ms != r->response_observed_ms) return false;
    } else if (r->response_source_uptime_ms) return false;
    return true;
}

static bool overlaps(const void *left, size_t left_size, const void *right, size_t right_size)
{
    const uintptr_t a = (uintptr_t)left, b = (uintptr_t)right;
    if (!left_size || !right_size) return false;
    return a <= b ? b - a < left_size : a - b < right_size;
}

static size_t name_length(const char *name)
{
    for (size_t i = 0; i <= CERBERUS_BLE_ARCHIVE_NAME_MAX; ++i)
        if (!name[i]) return i;
    return CERBERUS_BLE_ARCHIVE_NAME_MAX + 1U;
}

static bool row_valid(const cerberus_ble_archive_row_t *r, uint64_t saved_ms)
{
    if (r->address_type > 3U || r->index >= CERBERUS_BLE_ARCHIVE_MAX_ROWS_PER_RECEIVER ||
        r->receiver > CERBERUS_BLE_ARCHIVE_RECEIVER_C5 ||
        name_length(r->name) > CERBERUS_BLE_ARCHIVE_NAME_MAX ||
        (r->uuid_length != 0U && r->uuid_length != 2U &&
         r->uuid_length != 4U && r->uuid_length != CERBERUS_BLE_ARCHIVE_UUID_MAX) ||
        r->advertisement_length > CERBERUS_BLE_ARCHIVE_ADV_MAX) return false;
    if (r->rssi_valid ? (r->rssi < -127 || r->rssi > 20) :
        r->rssi != CERBERUS_BLE_ARCHIVE_RSSI_UNAVAILABLE) return false;
    if (r->scan_epoch_valid ? !r->scan_epoch : r->scan_epoch != 0U) return false;
    if (r->source_time_valid) {
        if (r->original_source_uptime_ms > r->copy_ceiling_ms ||
            r->copy_ceiling_ms > saved_ms) return false;
    } else if (r->original_source_uptime_ms || r->copy_ceiling_ms) return false;
    if (r->receiver == CERBERUS_BLE_ARCHIVE_RECEIVER_C5) {
        if (r->rssi_kind != CERBERUS_BLE_ARCHIVE_RSSI_LATEST_ADV ||
            !r->receiver_boot_id || !r->scan_epoch_valid || r->source_time_valid ||
            (!r->age_upper_valid && r->age_upper_at_save_ms)) return false;
    } else if (r->rssi_kind != CERBERUS_BLE_ARCHIVE_RSSI_SWEEP_PEAK ||
        r->receiver_boot_id || r->age_upper_valid || r->age_upper_at_save_ms) return false;
    if ((r->receiver != CERBERUS_BLE_ARCHIVE_RECEIVER_C5 &&
            r->receive.source_kind != CERBERUS_DUAL_BLE_RECEIVE_UNKNOWN) ||
        !cerberus_dual_ble_receive_metadata_valid(&r->receive,
            r->advertisement_length, r->advertisement_event_type, r->advertisement_flags)) return false;
    if (!all_zero((const uint8_t *)&r->family, sizeof(r->family))) {
        cerberus_dual_ble_family_evidence_t evidence;
        if (r->receiver != CERBERUS_BLE_ARCHIVE_RECEIVER_C5 ||
            !cerberus_dual_ble_family_wire_decode(&r->family, &r->receive,
                r->advertisement_length, r->advertisement_event_type,
                r->advertisement_flags, r->address, r->address_type, &evidence)) return false;
    }
    return response_valid(r) && c5_response_valid(r);
}

bool cerberus_ble_archive_validate(const cerberus_ble_archive_t *document)
{
    if (!document || !document->count || document->count > CERBERUS_BLE_ARCHIVE_MAX_ROWS)
        return false;
    uint8_t counts[3] = {0};
    for (size_t i = 0; i < document->count; ++i) {
        const cerberus_ble_archive_row_t *row = &document->rows[i];
        if (!row_valid(row, document->saved_uptime_ms)) return false;
        if (++counts[row->receiver] > CERBERUS_BLE_ARCHIVE_MAX_ROWS_PER_RECEIVER) return false;
        for (size_t j = 0; j < i; ++j)
            if (row->receiver == document->rows[j].receiver &&
                row->address_type == document->rows[j].address_type &&
                !memcmp(row->address, document->rows[j].address, sizeof(row->address))) return false;
    }
    return true;
}

static uint16_t get16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | (uint16_t)p[1] << 8);
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint64_t get64(const uint8_t *p)
{
    return (uint64_t)get32(p) | (uint64_t)get32(p + 4) << 32;
}

static void put16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
}

static void put32(uint8_t *p, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(value >> (8U * i));
}

static void put64(uint8_t *p, uint64_t value)
{
    put32(p, (uint32_t)value);
    put32(p + 4, (uint32_t)(value >> 32));
}

/* Header bytes 28..31 hold the CRC and are treated as zero in the calculation. */
static uint32_t archive_crc(const uint8_t *bytes, size_t length)
{
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0; i < length; ++i) {
        crc ^= i >= 28U && i < 32U ? 0U : bytes[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (UINT32_C(0xedb88320) & (0U - (crc & 1U)));
    }
    return ~crc;
}

static void encode_row(const cerberus_ble_archive_row_t *r, uint8_t *out)
{
    const size_t name_size = name_length(r->name);
    memset(out, 0, CERBERUS_BLE_ARCHIVE_ROW_SIZE);
    memcpy(out, r->address, 6);
    out[6] = r->address_type;
    out[7] = (r->rssi_valid ? ROW_RSSI_VALID : 0U) |
        (r->source_time_valid ? ROW_SOURCE_TIME_VALID : 0U) |
        (r->scan_epoch_valid ? ROW_SCAN_EPOCH_VALID : 0U) |
        (r->age_upper_valid ? ROW_AGE_UPPER_VALID : 0U);
    out[8] = (uint8_t)name_size;
    out[9] = (uint8_t)r->rssi;
    out[10] = r->peer_flags;
    out[11] = r->uuid_length;
    put16(out + 12, r->manufacturer_id);
    out[14] = r->advertisement_length;
    out[15] = r->advertisement_event_type;
    out[16] = r->advertisement_flags;
    out[17] = r->generation;
    out[18] = r->index;
    out[19] = r->receiver;
    put16(out + 20, r->reports_seen);
    out[22] = r->rssi_kind;
    put64(out + 24, r->scan_epoch);
    put64(out + 32, r->original_source_uptime_ms);
    put64(out + 40, r->copy_ceiling_ms);
    memcpy(out + 48, r->name, name_size);
    memcpy(out + 80, r->uuid, r->uuid_length);
    memcpy(out + 96, r->advertisement, r->advertisement_length);
    put64(out + 128, r->age_upper_at_save_ms);
    put64(out + 136, r->receiver_boot_id);
    out[144] = r->response_state;
    out[145] = r->response_length;
    out[146] = r->response_original_length;
    out[147] = r->response_event_type;
    out[148] = r->response_flags;
    out[149] = r->response_sid;
    out[150] = (r->response_sid_valid ? 1U : 0U) | (r->response_time_valid ? 2U : 0U);
    put32(out + 152, r->response_original_sequence);
    put32(out + 156, r->response_sequence);
    put32(out + 160, r->response_original_observed_ms);
    put32(out + 164, r->response_observed_ms);
    put64(out + 168, r->response_source_uptime_ms);
    memcpy(out + 176, r->response, r->response_length);
    out[208] = r->receive.source_kind;
    out[209] = r->receive.properties;
    out[210] = r->receive.callback_length;
    out[211] = r->receive.sid;
    out[212] = r->receive.primary_phy;
    out[213] = r->receive.secondary_phy;
    out[214] = r->receive.data_status;
    out[215] = r->receive.ad_validation;
    /* Explicit portable wire fields, never native row/evidence serialization. */
    const cerberus_dual_ble_family_wire_t *f = &r->family;
    out[216]=f->version; out[217]=f->family_code; out[218]=f->state;
    out[219]=f->categories; out[220]=f->hc_name; out[221]=f->ibeacon_result; out[222]=f->flags;
    out[223]=f->findmy_bits; out[224]=f->findmy_uuid_widths;
    out[225]=f->flipper_bits; out[226]=f->flipper_flags;
    out[227]=f->flock_facts; out[228]=f->flock_accessory_contexts;
    out[229]=f->flock_raven_3100_contexts; out[230]=f->flock_raven_3200_contexts;
    memcpy(out+231,f->meta_pairs,10U); memcpy(out+241,f->ibeacon_uuid,16U);
    memcpy(out+257,f->ibeacon_major_le,2U); memcpy(out+259,f->ibeacon_minor_le,2U);
    out[261]=(uint8_t)f->ibeacon_power; /* Signed two's-complement octet. */
    out[262]=f->reserved[0]; out[263]=f->reserved[1];
    cerberus_dual_ble_response_encode_bytes(&r->c5_response, out + 264);
    out[416] = r->c5_response_age_upper_valid ? 1U : 0U;
    put64(out + 424, r->c5_response_age_upper_at_save_ms);
}

static bool all_zero(const uint8_t *bytes, size_t length)
{
    for (size_t i = 0; i < length; ++i) if (bytes[i]) return false;
    return true;
}

static bool decode_row(const uint8_t *p, uint64_t saved_ms, uint16_t version,
    cerberus_ble_archive_row_t *r)
{
    const bool legacy = version == CERBERUS_BLE_ARCHIVE_LEGACY_VERSION;
    if ((p[7] & ~(legacy ? ROW_VALIDITY_MASK : ROW_V2_VALIDITY_MASK)) || p[8] > CERBERUS_BLE_ARCHIVE_NAME_MAX ||
        p[11] > CERBERUS_BLE_ARCHIVE_UUID_MAX || p[14] > CERBERUS_BLE_ARCHIVE_ADV_MAX ||
        (legacy && (p[19] || p[22])) || p[23] || p[127] ||
        !all_zero(p + 48 + p[8], CERBERUS_BLE_ARCHIVE_NAME_MAX - p[8]) ||
        !all_zero(p + 80 + p[11], CERBERUS_BLE_ARCHIVE_UUID_MAX - p[11]) ||
        !all_zero(p + 96 + p[14], CERBERUS_BLE_ARCHIVE_ADV_MAX - p[14])) return false;
    for (size_t i = 0; i < p[8]; ++i) if (!p[48 + i]) return false;
    memset(r, 0, sizeof(*r));
    memcpy(r->address, p, 6);
    r->address_type = p[6];
    r->rssi_valid = (p[7] & ROW_RSSI_VALID) != 0;
    r->source_time_valid = (p[7] & ROW_SOURCE_TIME_VALID) != 0;
    r->scan_epoch_valid = (p[7] & ROW_SCAN_EPOCH_VALID) != 0;
    r->age_upper_valid = (p[7] & ROW_AGE_UPPER_VALID) != 0;
    /* Avoid implementation-defined unsigned-to-signed narrowing for RSSI. */
    r->rssi = p[9] <= INT8_MAX ? (int8_t)p[9] : (int8_t)((int)p[9] - 256);
    r->peer_flags = p[10];
    r->uuid_length = p[11];
    r->manufacturer_id = get16(p + 12);
    r->advertisement_length = p[14];
    r->advertisement_event_type = p[15];
    r->advertisement_flags = p[16];
    r->generation = p[17];
    r->index = p[18];
    r->receiver = legacy ? CERBERUS_BLE_ARCHIVE_RECEIVER_LEGACY : p[19];
    r->rssi_kind = legacy ? CERBERUS_BLE_ARCHIVE_RSSI_SWEEP_PEAK : p[22];
    r->reports_seen = get16(p + 20);
    r->scan_epoch = get64(p + 24);
    r->original_source_uptime_ms = get64(p + 32);
    r->copy_ceiling_ms = get64(p + 40);
    memcpy(r->name, p + 48, p[8]);
    memcpy(r->uuid, p + 80, p[11]);
    memcpy(r->advertisement, p + 96, p[14]);
    if (!legacy) {
        r->age_upper_at_save_ms = get64(p + 128);
        r->receiver_boot_id = get64(p + 136);
    }
    if (version >= CERBERUS_BLE_ARCHIVE_V3_VERSION) {
        if (p[145] > CERBERUS_BLE_ARCHIVE_ADV_MAX || (p[150] & ~3U) || p[151] || p[207] ||
            !all_zero(p + 176 + p[145], CERBERUS_BLE_ARCHIVE_ADV_MAX - p[145])) return false;
        r->response_state = p[144]; r->response_length = p[145];
        r->response_original_length = p[146]; r->response_event_type = p[147];
        r->response_flags = p[148]; r->response_sid = p[149];
        r->response_sid_valid = (p[150] & 1U) != 0U;
        r->response_time_valid = (p[150] & 2U) != 0U;
        r->response_original_sequence = get32(p + 152);
        r->response_sequence = get32(p + 156);
        r->response_original_observed_ms = get32(p + 160);
        r->response_observed_ms = get32(p + 164);
        r->response_source_uptime_ms = get64(p + 168);
        memcpy(r->response, p + 176, r->response_length);
    }
    if (version >= CERBERUS_BLE_ARCHIVE_V4_VERSION) {
        r->receive.source_kind = p[208]; r->receive.properties = p[209];
        r->receive.callback_length = p[210]; r->receive.sid = p[211];
        r->receive.primary_phy = p[212]; r->receive.secondary_phy = p[213];
        r->receive.data_status = p[214]; r->receive.ad_validation = p[215];
    }
    if (version >= CERBERUS_BLE_ARCHIVE_V5_VERSION) {
        cerberus_dual_ble_family_wire_t *f = &r->family;
        f->version=p[216]; f->family_code=p[217]; f->state=p[218];
        f->categories=p[219]; f->hc_name=p[220]; f->ibeacon_result=p[221]; f->flags=p[222];
        f->findmy_bits=p[223]; f->findmy_uuid_widths=p[224];
        f->flipper_bits=p[225]; f->flipper_flags=p[226];
        f->flock_facts=p[227]; f->flock_accessory_contexts=p[228];
        f->flock_raven_3100_contexts=p[229]; f->flock_raven_3200_contexts=p[230];
        memcpy(f->meta_pairs,p+231,10U); memcpy(f->ibeacon_uuid,p+241,16U);
        memcpy(f->ibeacon_major_le,p+257,2U); memcpy(f->ibeacon_minor_le,p+259,2U);
        f->ibeacon_power=p[261]<=INT8_MAX ? (int8_t)p[261] : (int8_t)((int)p[261]-256);
        f->reserved[0]=p[262]; f->reserved[1]=p[263];
    }
    if (version == CERBERUS_BLE_ARCHIVE_VERSION) {
        if (p[416] > 1U || !all_zero(p + 417, 7U)) return false;
        cerberus_dual_ble_response_decode_bytes(p + 264, &r->c5_response);
        r->c5_response_age_upper_valid = p[416] != 0U;
        r->c5_response_age_upper_at_save_ms = get64(p + 424);
    }
    return row_valid(r, saved_ms);
}

bool cerberus_ble_archive_encode(const cerberus_ble_archive_t *document,
    uint8_t *output, size_t capacity, size_t *written)
{
    if (!written) return false;
    /* Bad aliased metadata cannot be cleared without modifying the input. */
    if ((document && overlaps(written, sizeof(*written), document, sizeof(*document))) ||
        (output && overlaps(written, sizeof(*written), output, capacity))) return false;
    *written = 0;
    if (!output || !cerberus_ble_archive_validate(document)) return false;
    const size_t size = CERBERUS_BLE_ARCHIVE_HEADER_SIZE +
        (size_t)document->count * CERBERUS_BLE_ARCHIVE_ROW_SIZE;
    if (capacity < size || overlaps(document, sizeof(*document), output, size)) return false;
    memset(output, 0, CERBERUS_BLE_ARCHIVE_HEADER_SIZE);
    memcpy(output, archive_magic, sizeof(archive_magic));
    put16(output + 8, CERBERUS_BLE_ARCHIVE_VERSION);
    put16(output + 10, CERBERUS_BLE_ARCHIVE_HEADER_SIZE);
    put16(output + 12, CERBERUS_BLE_ARCHIVE_ROW_SIZE);
    put16(output + 14, document->count);
    put32(output + 16, (uint32_t)size);
    put64(output + 20, document->saved_uptime_ms);
    for (size_t i = 0; i < document->count; ++i)
        encode_row(&document->rows[i], output + CERBERUS_BLE_ARCHIVE_HEADER_SIZE +
            i * CERBERUS_BLE_ARCHIVE_ROW_SIZE);
    put32(output + 28, archive_crc(output, size));
    *written = size;
    return true;
}

bool cerberus_ble_archive_decode(const uint8_t *input, size_t length,
    cerberus_ble_archive_t *document)
{
    if (!input || !document || length < CERBERUS_BLE_ARCHIVE_HEADER_SIZE ||
        length > CERBERUS_BLE_ARCHIVE_MAX_SIZE ||
        overlaps(input, length, document, sizeof(*document)) ||
        memcmp(input, archive_magic, sizeof(archive_magic)) ||
        get16(input + 10) != CERBERUS_BLE_ARCHIVE_HEADER_SIZE) return false;
    const uint16_t version = get16(input + 8);
    const bool legacy = version == CERBERUS_BLE_ARCHIVE_LEGACY_VERSION;
    if (!legacy && version != CERBERUS_BLE_ARCHIVE_V2_VERSION &&
        version != CERBERUS_BLE_ARCHIVE_V3_VERSION && version != CERBERUS_BLE_ARCHIVE_V4_VERSION &&
        version != CERBERUS_BLE_ARCHIVE_V5_VERSION && version != CERBERUS_BLE_ARCHIVE_VERSION) return false;
    const uint16_t row_size = legacy ? CERBERUS_BLE_ARCHIVE_LEGACY_ROW_SIZE :
        version == CERBERUS_BLE_ARCHIVE_V2_VERSION ? CERBERUS_BLE_ARCHIVE_V2_ROW_SIZE :
        version == CERBERUS_BLE_ARCHIVE_V3_VERSION ? CERBERUS_BLE_ARCHIVE_V3_ROW_SIZE :
        version == CERBERUS_BLE_ARCHIVE_V4_VERSION ? CERBERUS_BLE_ARCHIVE_V4_ROW_SIZE :
        version == CERBERUS_BLE_ARCHIVE_V5_VERSION ? CERBERUS_BLE_ARCHIVE_V5_ROW_SIZE : CERBERUS_BLE_ARCHIVE_ROW_SIZE;
    if (get16(input + 12) != row_size) return false;
    const uint16_t count = get16(input + 14);
    const uint64_t saved_ms = get64(input + 20);
    if (!count || count > (legacy ? CERBERUS_BLE_ARCHIVE_MAX_ROWS_PER_RECEIVER : CERBERUS_BLE_ARCHIVE_MAX_ROWS) ||
        length != CERBERUS_BLE_ARCHIVE_HEADER_SIZE + (size_t)count * row_size ||
        get32(input + 16) != length || get32(input + 28) != archive_crc(input, length)) return false;
    cerberus_ble_archive_row_t row;
    uint8_t counts[3] = {0};
    for (size_t i = 0; i < count; ++i) {
        const uint8_t *encoded = input + CERBERUS_BLE_ARCHIVE_HEADER_SIZE + i * row_size;
        if (!decode_row(encoded, saved_ms, version, &row) ||
            ++counts[row.receiver] > CERBERUS_BLE_ARCHIVE_MAX_ROWS_PER_RECEIVER) return false;
        for (size_t j = 0; j < i; ++j)
            if (!memcmp(encoded, input + CERBERUS_BLE_ARCHIVE_HEADER_SIZE +
                j * row_size, 7) && (legacy || encoded[19] ==
                input[CERBERUS_BLE_ARCHIVE_HEADER_SIZE + j * row_size + 19])) return false;
    }
    /* Publication occurs only after every row and the complete file passed. */
    memset(document, 0, sizeof(*document));
    document->saved_uptime_ms = saved_ms;
    document->count = (uint8_t)count;
    for (size_t i = 0; i < count; ++i)
        (void)decode_row(input + CERBERUS_BLE_ARCHIVE_HEADER_SIZE +
            i * row_size, saved_ms, version, &document->rows[i]);
    return true;
}

bool cerberus_ble_archive_age_at_save(const cerberus_ble_archive_t *document,
    size_t index, uint64_t *age_ms)
{
    if (!document || !age_ms || !document->count || document->count > CERBERUS_BLE_ARCHIVE_MAX_ROWS ||
        index >= document->count || !row_valid(&document->rows[index], document->saved_uptime_ms) ||
        !document->rows[index].source_time_valid) return false;
    *age_ms = document->saved_uptime_ms - document->rows[index].original_source_uptime_ms;
    return true;
}

bool cerberus_ble_archive_age_upper_at_save(const cerberus_ble_archive_t *document,
    size_t index, uint64_t *age_ms)
{
    if (!document || !age_ms || !document->count || document->count > CERBERUS_BLE_ARCHIVE_MAX_ROWS ||
        index >= document->count || !row_valid(&document->rows[index], document->saved_uptime_ms) ||
        document->rows[index].receiver != CERBERUS_BLE_ARCHIVE_RECEIVER_C5 ||
        !document->rows[index].age_upper_valid) return false;
    *age_ms = document->rows[index].age_upper_at_save_ms;
    return true;
}

bool cerberus_ble_archive_response_age_at_save(const cerberus_ble_archive_t *document,
    size_t index, uint64_t *age_ms)
{
    if (!document || !age_ms || !document->count || document->count > CERBERUS_BLE_ARCHIVE_MAX_ROWS ||
        index >= document->count || !row_valid(&document->rows[index], document->saved_uptime_ms) ||
        document->rows[index].response_state != CERBERUS_BLE_ARCHIVE_RESPONSE_PRESENT ||
        !document->rows[index].response_time_valid) return false;
    *age_ms = document->saved_uptime_ms - document->rows[index].response_source_uptime_ms;
    return true;
}

bool cerberus_ble_archive_c5_response_age_upper_at_save(const cerberus_ble_archive_t *document,
    size_t index, uint64_t *age_ms)
{
    if (!document || !age_ms || !document->count || document->count > CERBERUS_BLE_ARCHIVE_MAX_ROWS ||
        index >= document->count || !row_valid(&document->rows[index], document->saved_uptime_ms) ||
        document->rows[index].receiver != CERBERUS_BLE_ARCHIVE_RECEIVER_C5 ||
        !document->rows[index].c5_response_age_upper_valid) return false;
    *age_ms = document->rows[index].c5_response_age_upper_at_save_ms;
    return true;
}
