#include "airtag_json.h"
#include "ble_findmy_hints.h"
#include "cerberus_protocol.h"
#include <limits.h>
#include <string.h>

typedef struct {
    const uint8_t *bytes;
    size_t length, pos;
    cerberus_airtag_json_result_t error;
} reader_t;
typedef bool (*character_fn)(void *, uint32_t);

static bool overlap(const void *a, size_t an, const void *b, size_t bn)
{
    if (!an || !bn) return false;
    uintptr_t x = (uintptr_t)a, y = (uintptr_t)b;
    return an > UINTPTR_MAX - x || bn > UINTPTR_MAX - y ||
        (x < y + bn && y < x + an);
}
static bool fail(reader_t *r, cerberus_airtag_json_result_t error)
{ if (r->error == CERBERUS_AIRTAG_JSON_OK) r->error = error; return false; }
static void space(reader_t *r)
{
    while (r->pos < r->length && (r->bytes[r->pos] == ' ' || r->bytes[r->pos] == '\t' ||
        r->bytes[r->pos] == '\r' || r->bytes[r->pos] == '\n')) ++r->pos;
}
static bool take(reader_t *r, uint8_t c)
{ space(r); if (r->pos < r->length && r->bytes[r->pos] == c) { ++r->pos; return true; } return false; }
static int hex(uint32_t c)
{
    if (c >= '0' && c <= '9') return (int)c - '0';
    if (c >= 'a' && c <= 'f') return (int)c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return (int)c - 'A' + 10;
    return -1;
}
static bool hex4(reader_t *r, uint32_t *value)
{
    *value = 0;
    for (unsigned i = 0; i < 4; ++i) {
        if (r->pos == r->length) return false;
        int n = hex(r->bytes[r->pos++]);
        if (n < 0) return false;
        *value = (*value << 4) | (unsigned)n;
    }
    return true;
}
/* Validate scalar Unicode, including ignored strings. The semantic fields
 * accept only ASCII via their sinks. No temporary full string is constructed. */
static bool string(reader_t *r, character_fn emit, void *context)
{
    if (!take(r, '"')) return fail(r, CERBERUS_AIRTAG_JSON_INVALID);
    while (r->pos < r->length) {
        uint32_t c = r->bytes[r->pos++];
        if (c == '"') return true;
        if (c < 0x20U) return fail(r, CERBERUS_AIRTAG_JSON_INVALID);
        if (c == '\\') {
            if (r->pos == r->length) return fail(r, CERBERUS_AIRTAG_JSON_INVALID);
            c = r->bytes[r->pos++];
            if (c == 'u') {
                if (!hex4(r, &c)) return fail(r, CERBERUS_AIRTAG_JSON_INVALID);
                if (c >= 0xd800U && c <= 0xdbffU) {
                    uint32_t low;
                    if (r->length - r->pos < 2 || r->bytes[r->pos++] != '\\' ||
                        r->bytes[r->pos++] != 'u' || !hex4(r, &low) || low < 0xdc00U || low > 0xdfffU)
                        return fail(r, CERBERUS_AIRTAG_JSON_INVALID);
                    c = 0x10000U + ((c - 0xd800U) << 10) + low - 0xdc00U;
                } else if (c >= 0xdc00U && c <= 0xdfffU) return fail(r, CERBERUS_AIRTAG_JSON_INVALID);
            } else if (c == 'b') c = '\b';
            else if (c == 'f') c = '\f';
            else if (c == 'n') c = '\n';
            else if (c == 'r') c = '\r';
            else if (c == 't') c = '\t';
            else if (c != '"' && c != '\\' && c != '/') return fail(r, CERBERUS_AIRTAG_JSON_INVALID);
        } else if (c >= 0x80U) {
            unsigned remaining; uint32_t minimum;
            if (c >= 0xc2U && c <= 0xdfU) { remaining = 1; minimum = 0x80U; c &= 0x1fU; }
            else if (c >= 0xe0U && c <= 0xefU) { remaining = 2; minimum = 0x800U; c &= 0xfU; }
            else if (c >= 0xf0U && c <= 0xf4U) { remaining = 3; minimum = 0x10000U; c &= 7U; }
            else return fail(r, CERBERUS_AIRTAG_JSON_INVALID);
            while (remaining--) {
                if (r->pos == r->length || (r->bytes[r->pos] & 0xc0U) != 0x80U)
                    return fail(r, CERBERUS_AIRTAG_JSON_INVALID);
                c = (c << 6) | (r->bytes[r->pos++] & 0x3fU);
            }
            if (c < minimum || c > 0x10ffffU || (c >= 0xd800U && c <= 0xdfffU))
                return fail(r, CERBERUS_AIRTAG_JSON_INVALID);
        }
        if (emit && !emit(context, c)) return fail(r, CERBERUS_AIRTAG_JSON_INVALID);
    }
    return fail(r, CERBERUS_AIRTAG_JSON_INVALID);
}
static bool number(reader_t *r)
{
    space(r); size_t p = r->pos;
    if (p < r->length && r->bytes[p] == '-') ++p;
    if (p == r->length) return fail(r, CERBERUS_AIRTAG_JSON_INVALID);
    if (r->bytes[p] == '0') ++p;
    else {
        if (r->bytes[p] < '1' || r->bytes[p] > '9') return fail(r, CERBERUS_AIRTAG_JSON_INVALID);
        do { ++p; } while (p < r->length && r->bytes[p] >= '0' && r->bytes[p] <= '9');
    }
    if (p < r->length && r->bytes[p] == '.') {
        ++p; size_t start = p;
        while (p < r->length && r->bytes[p] >= '0' && r->bytes[p] <= '9') ++p;
        if (p == start) return fail(r, CERBERUS_AIRTAG_JSON_INVALID);
    }
    if (p < r->length && (r->bytes[p] == 'e' || r->bytes[p] == 'E')) {
        ++p; if (p < r->length && (r->bytes[p] == '+' || r->bytes[p] == '-')) ++p;
        size_t start = p;
        while (p < r->length && r->bytes[p] >= '0' && r->bytes[p] <= '9') ++p;
        if (p == start) return fail(r, CERBERUS_AIRTAG_JSON_INVALID);
    }
    r->pos = p; return true;
}
static bool skip(reader_t *r, unsigned depth)
{
    space(r);
    if (r->pos == r->length) return fail(r, CERBERUS_AIRTAG_JSON_INVALID);
    uint8_t c = r->bytes[r->pos];
    if (c == '"') return string(r, NULL, NULL);
    if (c == '[' || c == '{') {
        if (depth >= CERBERUS_AIRTAG_JSON_MAX_DEPTH) return fail(r, CERBERUS_AIRTAG_JSON_LIMIT);
        ++r->pos; uint8_t end = c == '[' ? ']' : '}';
        if (take(r, end)) return true;
        do {
            if (c == '{' && (!string(r, NULL, NULL) || !take(r, ':'))) return fail(r, CERBERUS_AIRTAG_JSON_INVALID);
            if (!skip(r, depth + 1)) return false;
            if (take(r, end)) return true;
        } while (take(r, ','));
        return fail(r, CERBERUS_AIRTAG_JSON_INVALID);
    }
    const char *literal = c == 't' ? "true" : c == 'f' ? "false" : c == 'n' ? "null" : NULL;
    if (literal) {
        size_t n = strlen(literal);
        if (n > r->length - r->pos || memcmp(r->bytes + r->pos, literal, n)) return fail(r, CERBERUS_AIRTAG_JSON_INVALID);
        r->pos += n; return true;
    }
    return number(r);
}
typedef struct { char bytes[18]; size_t count; } text_t;
static bool text_character(void *context, uint32_t c)
{
    text_t *t = context;
    if (t->count < sizeof(t->bytes) - 1) t->bytes[t->count] = c <= 0x7fU ? (char)c : '?';
    ++t->count; return true;
}
typedef struct {
    cerberus_airtag_json_t *out;
    size_t offset, length;
    unsigned phase;
    uint8_t byte;
    bool limit;
} payload_t;
static bool payload_character(void *context, uint32_t c)
{
    payload_t *p = context;
    if (p->phase == 0 || p->phase == 4) {
        if (c == ' ') { p->phase = 0; return true; }
        if (p->phase == 4 || c != '0') return false;
        p->phase = 1; return true;
    }
    if (p->phase == 1) { if (c != 'x' && c != 'X') return false; p->phase = 2; return true; }
    int n = hex(c); if (n < 0) return false;
    if (p->phase == 2) { p->byte = (uint8_t)(n << 4); p->phase = 3; return true; }
    p->byte |= (uint8_t)n;
    if (p->offset + p->length >= CERBERUS_AIRTAG_JSON_PAYLOAD_CAPACITY) { p->limit = true; return false; }
    if (p->out) p->out->payload[p->offset + p->length] = p->byte;
    ++p->length; p->phase = 4; return true;
}
static bool size_field(reader_t *r, size_t *value)
{
    space(r); size_t start = r->pos; *value = 0;
    while (r->pos < r->length && r->bytes[r->pos] >= '0' && r->bytes[r->pos] <= '9') {
        unsigned d = r->bytes[r->pos++] - '0';
        if (*value > (CERBERUS_AIRTAG_JSON_PAYLOAD_CAPACITY - d) / 10U)
            return fail(r, CERBERUS_AIRTAG_JSON_LIMIT);
        *value = *value * 10U + d;
    }
    return r->pos > start && (r->pos == start + 1 || r->bytes[start] != '0') ? true : fail(r, CERBERUS_AIRTAG_JSON_INVALID);
}
static bool record(reader_t *r, cerberus_airtag_json_t *out, size_t index, size_t *used)
{
    if (!take(r, '{')) return fail(r, CERBERUS_AIRTAG_JSON_INVALID);
    unsigned fields = 0; size_t declared = 0;
    payload_t payload = {.out = out, .offset = *used};
    uint8_t address[6] = {0};
    do {
        text_t key = {0};
        if (!string(r, text_character, &key) || !take(r, ':')) return fail(r, CERBERUS_AIRTAG_JSON_INVALID);
        unsigned field = key.count == 3 && !memcmp(key.bytes,"mac",3) ? 1U :
            key.count == 7 && !memcmp(key.bytes,"payload",7) ? 2U :
            key.count == 12 && !memcmp(key.bytes,"payload_size",12) ? 4U : 0U;
        if (field && (fields & field)) return fail(r, CERBERUS_AIRTAG_JSON_INVALID);
        fields |= field;
        if (field == 1U) {
            text_t mac = {0};
            if (!string(r, text_character, &mac) || mac.count != 17) return fail(r, CERBERUS_AIRTAG_JSON_INVALID);
            for (size_t i = 0; i < 6; ++i) {
                int hi = hex((uint8_t)mac.bytes[i*3]), lo = hex((uint8_t)mac.bytes[i*3+1]);
                if (hi < 0 || lo < 0 || (i < 5 && mac.bytes[i*3+2] != ':')) return fail(r, CERBERUS_AIRTAG_JSON_INVALID);
                address[5-i] = (uint8_t)(hi*16+lo);
            }
        } else if (field == 2U) {
            if (!string(r, payload_character, &payload)) {
                if (payload.limit) r->error = CERBERUS_AIRTAG_JSON_LIMIT;
                return false;
            }
            if (payload.phase != 0 && payload.phase != 4) return fail(r, CERBERUS_AIRTAG_JSON_INVALID);
        } else if (field == 4U) { if (!size_field(r, &declared)) return false; }
        else if (!skip(r, 2U)) return false;
        if (take(r, '}')) {
            if (fields != 7U || declared != payload.length) return fail(r, CERBERUS_AIRTAG_JSON_INVALID);
            if (out) {
                memcpy(out->records[index].address,address,6);
                out->records[index].offset = (uint16_t)*used;
                out->records[index].length = (uint16_t)payload.length;
            }
            *used += payload.length; return true;
        }
    } while (take(r, ','));
    return fail(r, CERBERUS_AIRTAG_JSON_INVALID);
}
static cerberus_airtag_json_result_t parse(const uint8_t *input, size_t length, cerberus_airtag_json_t *out)
{
    reader_t r = {.bytes=input,.length=length}; size_t count=0, used=0;
    if (!take(&r, '[')) return CERBERUS_AIRTAG_JSON_INVALID;
    if (!take(&r, ']')) {
        do {
            if (count == CERBERUS_AIRTAG_JSON_MAX_RECORDS) return CERBERUS_AIRTAG_JSON_LIMIT;
            if (!record(&r,out,count++,&used)) return r.error;
            if (take(&r, ']')) goto finished;
        } while (take(&r, ','));
        return CERBERUS_AIRTAG_JSON_INVALID;
    }
finished:
    space(&r); if (r.pos != r.length) return CERBERUS_AIRTAG_JSON_INVALID;
    if (out) { out->count=(uint8_t)count; out->payload_used=(uint16_t)used; }
    return CERBERUS_AIRTAG_JSON_OK;
}
cerberus_airtag_json_result_t cerberus_airtag_json_validate(const cerberus_airtag_json_t *doc)
{
    if (!doc) return CERBERUS_AIRTAG_JSON_INVALID;
    if (doc->count > CERBERUS_AIRTAG_JSON_MAX_RECORDS || doc->payload_used > CERBERUS_AIRTAG_JSON_PAYLOAD_CAPACITY)
        return CERBERUS_AIRTAG_JSON_LIMIT;
    size_t used = 0;
    for (size_t i=0;i<doc->count;++i) {
        if (doc->records[i].offset != used) return CERBERUS_AIRTAG_JSON_INVALID;
        used += doc->records[i].length;
        if (used > CERBERUS_AIRTAG_JSON_PAYLOAD_CAPACITY) return CERBERUS_AIRTAG_JSON_LIMIT;
    }
    return used == doc->payload_used ? CERBERUS_AIRTAG_JSON_OK : CERBERUS_AIRTAG_JSON_INVALID;
}
cerberus_airtag_json_result_t cerberus_airtag_json_decode(const uint8_t *input, size_t length, cerberus_airtag_json_t *out)
{
    if (!input || !out || overlap(input,length,out,sizeof(*out))) return CERBERUS_AIRTAG_JSON_INVALID;
    if (length > CERBERUS_AIRTAG_JSON_MAX_BYTES) return CERBERUS_AIRTAG_JSON_LIMIT;
    cerberus_airtag_json_result_t result = parse(input,length,NULL);
    if (result != CERBERUS_AIRTAG_JSON_OK) return result;
    memset(out,0,sizeof(*out)); return parse(input,length,out);
}
typedef struct { uint8_t *out; size_t length; } writer_t;
static void character(writer_t *w, uint8_t c) { if (w->out) w->out[w->length]=c; ++w->length; }
static void literal(writer_t *w, const char *s) { while (*s) character(w,(uint8_t)*s++); }
static void integer(writer_t *w, unsigned n)
{
    unsigned place=1U; while (n/place>=10U) place*=10U;
    do { character(w,(uint8_t)('0'+n/place)); n%=place; place/=10U; } while(place);
}
static size_t write_document(const cerberus_airtag_json_t *doc, uint8_t *out)
{
    static const char upper[] = "0123456789ABCDEF", lower[] = "0123456789abcdef";
    writer_t w = {.out = out};
    character(&w, '[');
    for (size_t i = 0; i < doc->count; ++i) {
        const cerberus_airtag_json_record_t *row = &doc->records[i];
        if (i) character(&w, ',');
        literal(&w, "{\"mac\":\"");
        for (size_t j = 0; j < 6; ++j) {
            uint8_t b = row->address[5-j];
            if (j) character(&w, ':');
            character(&w, upper[b >> 4]);
            character(&w, upper[b & 15]);
        }
        literal(&w, "\",\"payload\":\"");
        for (size_t j = 0; j < row->length; ++j) {
            uint8_t b = doc->payload[row->offset+j];
            if (j) character(&w, ' ');
            literal(&w, "0x");
            character(&w, lower[b >> 4]);
            character(&w, lower[b & 15]);
        }
        literal(&w, "\",\"payload_size\":");
        integer(&w, row->length);
        character(&w, '}');
    }
    character(&w, ']');
    return w.length;
}
cerberus_airtag_json_result_t cerberus_airtag_json_encode(const cerberus_airtag_json_t *doc,
    uint8_t *out, size_t capacity, size_t *written)
{
    if (written && ((doc && overlap(written, sizeof(*written), doc, sizeof(*doc))) ||
        (out && overlap(written, sizeof(*written), out, capacity))))
        return CERBERUS_AIRTAG_JSON_INVALID;
    if (written) *written = 0;
    if (!doc || !out || overlap(doc, sizeof(*doc), out, capacity))
        return CERBERUS_AIRTAG_JSON_INVALID;
    cerberus_airtag_json_result_t result = cerberus_airtag_json_validate(doc);
    if (result != CERBERUS_AIRTAG_JSON_OK) return result;
    size_t length = write_document(doc, NULL);
    if (length > capacity || length > CERBERUS_AIRTAG_JSON_MAX_BYTES)
        return CERBERUS_AIRTAG_JSON_LIMIT;
    write_document(doc, out);
    if (written) *written = length;
    return CERBERUS_AIRTAG_JSON_OK;
}

/* The archive validator already checks sidecar binding. Missing source proof,
 * unknown callback metadata and raw previews are never upgraded for export. */
static const uint8_t *packet(const cerberus_ble_archive_row_t *r, bool response, size_t *length)
{
    if ((r->receiver!=CERBERUS_BLE_ARCHIVE_RECEIVER_S3 && r->receiver!=CERBERUS_BLE_ARCHIVE_RECEIVER_C5) ||
        !r->scan_epoch_valid || !r->scan_epoch) return NULL;
    if (!response) {
        if (!r->advertisement_length || r->advertisement_flags || r->advertisement_event_type>3U) return NULL;
        if (r->receiver==CERBERUS_BLE_ARCHIVE_RECEIVER_C5 &&
            (r->receive.source_kind==CERBERUS_DUAL_BLE_RECEIVE_UNKNOWN ||
             r->receive.callback_length!=r->advertisement_length || r->receive.ad_validation!=CERBERUS_DUAL_BLE_AD_VALID))return NULL;
        *length=r->advertisement_length;return r->advertisement;
    }
    if (r->receiver==CERBERUS_BLE_ARCHIVE_RECEIVER_S3) {
        if(r->response_state!=CERBERUS_BLE_ARCHIVE_RESPONSE_PRESENT || r->response_flags ||
            !r->response_length || r->response_length!=r->response_original_length)return NULL;
        *length=r->response_length;return r->response;
    }
    if(r->c5_response.state!=CERBERUS_DUAL_BLE_RESPONSE_PRESENT || r->c5_response.raw_flags ||
        !r->c5_response.raw_length || r->c5_response.raw_length!=r->c5_response.receive.callback_length ||
        r->c5_response.receive.ad_validation!=CERBERUS_DUAL_BLE_AD_VALID)return NULL;
    *length=r->c5_response.raw_length;return r->c5_response.raw;
}
cerberus_airtag_json_result_t cerberus_airtag_json_from_archive(const cerberus_ble_archive_t *archive,
    cerberus_airtag_json_t *out, cerberus_airtag_json_export_stats_t *stats)
{
    if (!archive || !out || overlap(archive, sizeof(*archive), out, sizeof(*out)) ||
        (stats && (overlap(stats, sizeof(*stats), archive, sizeof(*archive)) ||
                   overlap(stats, sizeof(*stats), out, sizeof(*out)))))
        return CERBERUS_AIRTAG_JSON_INVALID;
    if (!cerberus_ble_archive_validate(archive)) return CERBERUS_AIRTAG_JSON_INVALID;
    cerberus_airtag_json_export_stats_t counts = {0};
    size_t used = 0;
    for (unsigned pass = 0; pass < 2; ++pass) {
        if (pass) memset(out, 0, sizeof(*out));
        for (size_t i = 0; i < archive->count; ++i) {
            const cerberus_ble_archive_row_t *r = &archive->rows[i];
            for (unsigned rsp = 0; rsp < 2; ++rsp) {
                if (rsp && r->response_state != CERBERUS_BLE_ARCHIVE_RESPONSE_PRESENT &&
                    r->c5_response.state != CERBERUS_DUAL_BLE_RESPONSE_PRESENT) continue;
                size_t length = 0;
                const uint8_t *data = packet(r, rsp != 0, &length);
                cerberus_ble_findmy_hints_t hints;
                if (!data || !cerberus_ble_findmy_hints_parse(data, length, &hints)) {
                    if (!pass) ++counts.incomplete;
                    continue;
                }
                if (!hints.bits) {
                    if (!pass) ++counts.filtered;
                    continue;
                }
                if (!pass) {
                    ++counts.eligible;
                    used += length;
                } else {
                    cerberus_airtag_json_record_t *row = &out->records[out->count++];
                    memcpy(row->address, r->address, 6);
                    row->offset = out->payload_used;
                    row->length = (uint16_t)length;
                    memcpy(out->payload + out->payload_used, data, length);
                    out->payload_used += (uint16_t)length;
                }
            }
        }
        if (!pass) {
            if (stats) *stats = counts;
            if (counts.eligible > CERBERUS_AIRTAG_JSON_MAX_RECORDS ||
                used > CERBERUS_AIRTAG_JSON_PAYLOAD_CAPACITY) return CERBERUS_AIRTAG_JSON_LIMIT;
            if (!counts.eligible) return CERBERUS_AIRTAG_JSON_NO_ELIGIBLE;
        }
    }
    return CERBERUS_AIRTAG_JSON_OK;
}
