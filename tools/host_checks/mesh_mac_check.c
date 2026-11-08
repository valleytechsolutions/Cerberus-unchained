/* Original synthetic tests. No captured network data or upstream test vectors. */
#include "mesh_mac.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned long checks;
#define CHECK(c) do { ++checks; if (!(c)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); exit(1); } } while (0)

typedef struct { uint8_t bytes[125]; size_t length; } frame_t;
typedef struct { uint8_t d, s, compressed, dp, sp; } pan_case_t;

/* Independent explicit 2015 presence oracle; not a call into the parser. */
static const pan_case_t pan_cases[] = {
    {0,0,0,0,0}, {0,0,1,1,0}, {0,2,0,0,1}, {0,2,1,0,0},
    {0,3,0,0,1}, {0,3,1,0,0}, {2,0,0,1,0}, {2,0,1,0,0},
    {3,0,0,1,0}, {3,0,1,0,0}, {2,2,0,1,1}, {2,2,1,1,0},
    {2,3,0,1,1}, {2,3,1,1,0}, {3,2,0,1,1}, {3,2,1,1,0},
    {3,3,0,1,0}, {3,3,1,0,0}
};

static void put(frame_t *f, uint64_t value, size_t size)
{
    CHECK(size <= sizeof(f->bytes) - f->length);
    for (size_t i = 0; i < size; ++i) f->bytes[f->length++] = (uint8_t)(value >> (i * 8U));
}

static void fill(frame_t *f, size_t count, uint8_t byte)
{
    for (size_t i = 0; i < count; ++i) put(f, byte, 1U);
}

static uint16_t fcf(unsigned version, unsigned type, unsigned d, unsigned s,
                    bool compressed, bool suppressed, bool security, bool ies)
{
    return (uint16_t)(type | (security ? 8U : 0U) | (compressed ? 0x40U : 0U) |
                      (suppressed ? 0x100U : 0U) | (ies ? 0x200U : 0U) |
                      (d << 10U) | (version << 12U) | (s << 14U));
}

static frame_t base(unsigned version, const pan_case_t *p, bool suppressed,
                    bool security, bool ies, unsigned type)
{
    frame_t f = {{0},0};
    put(&f, fcf(version,type,p->d,p->s,p->compressed,suppressed,security,ies),2U);
    if (!suppressed) put(&f,0x5aU,1U);
    if (p->dp) put(&f,0x1234U,2U);
    if (p->d) put(&f,p->d == 2U ? 0x5678U : UINT64_C(0x0102030405060708),p->d == 2U ? 2U : 8U);
    if (p->sp) put(&f,0x9abcU,2U);
    if (p->s) put(&f,p->s == 2U ? 0x1357U : UINT64_C(0x1112131415161718),p->s == 2U ? 2U : 8U);
    return f;
}

static mesh_mac_result_t parse_unchanged_on_failure(const uint8_t *bytes, size_t length,
                                                   mesh_mac_observation_t *out)
{
    memset(out,0xa6,sizeof(*out));
    mesh_mac_observation_t before;
    memcpy(&before,out,sizeof(before));
    mesh_mac_result_t result = mesh_mac_parse(bytes,length,out);
    if (result != MESH_MAC_HEADER_OK) CHECK(memcmp(&before,out,sizeof(before)) == 0);
    return result;
}

static void all_shorter_fail(const frame_t *f)
{
    for (size_t i = 0; i < f->length; ++i) {
        mesh_mac_observation_t out;
        CHECK(parse_unchanged_on_failure(f->bytes,i,&out) == MESH_MAC_MALFORMED);
    }
}

static void pan_tables(void)
{
    for (unsigned version = 0; version < 3; ++version) {
        for (size_t i = 0; i < sizeof(pan_cases)/sizeof(pan_cases[0]); ++i) {
            pan_case_t p = pan_cases[i];
            bool supported = version == 2U || !p.compressed || (p.d && p.s);
            if (version != 2U) { p.dp = p.d != 0U; p.sp = p.s != 0U && !p.compressed; }
            unsigned variants = version == 2U ? 2U : 1U;
            for (unsigned suppressed = 0; suppressed < variants; ++suppressed) {
                frame_t f = base(version,&p,suppressed,false,false,1U);
                mesh_mac_observation_t out;
                mesh_mac_result_t result = parse_unchanged_on_failure(f.bytes,f.length,&out);
                if (!supported) { CHECK(result == MESH_MAC_UNSUPPORTED); continue; }
                CHECK(result == MESH_MAC_HEADER_OK);
                CHECK(out.version == version && out.frame_type == 1U);
                CHECK(out.sequence_present == !suppressed);
                CHECK(out.sequence == (suppressed ? 0U : 0x5aU));
                CHECK(out.header_length == f.length && out.payload_length == 0U);
                CHECK(out.destination.mode == p.d && out.source.mode == p.s);
                CHECK(out.destination.value == (p.d == 2U ? 0x5678U : p.d == 3U ? UINT64_C(0x0102030405060708) : 0U));
                CHECK(out.source.value == (p.s == 2U ? 0x1357U : p.s == 3U ? UINT64_C(0x1112131415161718) : 0U));
                CHECK(out.destination_pan.origin == (p.dp ? MESH_MAC_PAN_EXPLICIT : MESH_MAC_PAN_UNKNOWN));
                CHECK(out.destination_pan.value == (p.dp ? 0x1234U : 0U));
                bool inherited = p.compressed && p.d && p.s && !(version == 2U && p.d == 3U && p.s == 3U);
                CHECK(out.source_pan.origin == (p.sp ? MESH_MAC_PAN_EXPLICIT : inherited ? MESH_MAC_PAN_COMPRESSED_SAME : MESH_MAC_PAN_UNKNOWN));
                CHECK(out.source_pan.value == (p.sp ? 0x9abcU : inherited ? 0x1234U : 0U));
                all_shorter_fail(&f);
            }
        }
    }
}

static void security_bounds(void)
{
    const pan_case_t p = {2,2,1,1,0};
    const unsigned key_sizes[] = {0,1,5,9};
    const unsigned mic_sizes[] = {0,4,8,16};
    for (unsigned version = 1; version <= 2; ++version) {
        for (unsigned level = 1; level < 8; ++level) {
            for (unsigned key = 0; key < 4; ++key) {
                for (unsigned suppressed = 0; suppressed < (version == 2U ? 2U : 1U); ++suppressed) {
                    for (unsigned asn = 0; asn < (version == 2U ? 2U : 1U); ++asn) {
                        frame_t f = base(version,&p,false,true,false,1U);
                        put(&f,level | (key << 3U) | (suppressed ? 0x20U : 0U) | (asn ? 0x40U : 0U),1U);
                        if (!suppressed) put(&f,UINT32_C(0x87654321),4U);
                        for (unsigned j = 0; j < key_sizes[key]; ++j) put(&f,0x90U+j,1U);
                        size_t header = f.length;
                        fill(&f,mic_sizes[level & 3U],0xd3U);
                        mesh_mac_observation_t out;
                        CHECK(mesh_mac_parse(f.bytes,f.length,&out) == MESH_MAC_HEADER_OK);
                        CHECK(out.security_enabled && out.security_level == level && out.key_id_mode == key);
                        CHECK(out.encrypted == (level >= 4U) && out.asn_in_nonce == (bool)asn);
                        CHECK(out.frame_counter_present == !suppressed);
                        CHECK(out.frame_counter == (suppressed ? 0U : UINT32_C(0x87654321)));
                        CHECK(out.key_identifier_length == key_sizes[key]);
                        for (unsigned j = 0; j < 9U; ++j) CHECK(out.key_identifier[j] == (j < key_sizes[key] ? 0x90U+j : 0U));
                        CHECK(out.header_length == header && out.payload_length == 0U);
                        CHECK(out.mic_length == mic_sizes[level & 3U]);
                        all_shorter_fail(&f);
                    }
                }
            }
        }
    }
    for (unsigned version = 0; version < 3; ++version) {
        const uint8_t controls[] = {0U,0x80U,0x21U,0x41U,0x61U};
        for (size_t i = 0; i < sizeof(controls); ++i) {
            frame_t f = base(version,&p,false,true,false,1U);
            put(&f,controls[i],1U); fill(&f,25U,0U);
            mesh_mac_observation_t out;
            bool unsupported = version == 0U || controls[i] == 0U || (controls[i] & 0x80U) || version == 1U;
            CHECK(parse_unchanged_on_failure(f.bytes,f.length,&out) == (unsupported ? MESH_MAC_UNSUPPORTED : MESH_MAC_HEADER_OK));
        }
    }
}

static void ie_bounds(void)
{
    const pan_case_t p = {2,3,1,1,0};
    mesh_mac_observation_t out;
    frame_t f = base(2U,&p,true,false,true,1U);
    CHECK(parse_unchanged_on_failure(f.bytes,f.length,&out) == MESH_MAC_MALFORMED);
    put(&f,(0x1aU << 7U) | 4U,2U); put(&f,0x12345678U,4U);
    CHECK(mesh_mac_parse(f.bytes,f.length,&out) == MESH_MAC_HEADER_OK);
    CHECK(out.header_length == f.length && out.header_ies_present && out.payload_length == 0U);
    all_shorter_fail(&f);
    put(&f,0x7fU << 7U,2U); put(&f,0xdeU,1U); put(&f,0xadU,1U);
    CHECK(mesh_mac_parse(f.bytes,f.length,&out) == MESH_MAC_HEADER_OK);
    CHECK(out.payload_length == 2U && !out.payload_ies_indicated);
    f.bytes[f.length-4U] = 1U; /* Nonzero HT2 length. */
    CHECK(parse_unchanged_on_failure(f.bytes,f.length,&out) == MESH_MAC_MALFORMED);

    for (unsigned encrypted = 0; encrypted < 2; ++encrypted) {
        f = base(2U,&p,false,encrypted,true,1U);
        if (encrypted) { put(&f,5U,1U); put(&f,7U,4U); }
        put(&f,0x7eU << 7U,2U);
        size_t end_header = f.length;
        /* Deliberately not valid plaintext IEs: payload stays opaque. */
        put(&f,0xffffU,2U); put(&f,0x00U,1U);
        if (encrypted) fill(&f,4U,0x77U);
        CHECK(mesh_mac_parse(f.bytes,f.length,&out) == MESH_MAC_HEADER_OK);
        CHECK(out.payload_ies_indicated && out.header_length == end_header && out.payload_length == 3U);
        CHECK(out.encrypted == (bool)encrypted);
        frame_t empty = f;
        empty.length = end_header;
        if (encrypted) fill(&empty,4U,0x77U);
        CHECK(parse_unchanged_on_failure(empty.bytes,empty.length,&out) == MESH_MAC_MALFORMED);
    }
    f = base(2U,&p,false,false,true,1U); put(&f,0x8000U,2U);
    CHECK(parse_unchanged_on_failure(f.bytes,f.length,&out) == MESH_MAC_UNSUPPORTED);
    f = base(2U,&p,false,false,true,1U); put(&f,127U,2U); fill(&f,10U,0U);
    CHECK(parse_unchanged_on_failure(f.bytes,f.length,&out) == MESH_MAC_MALFORMED);
    f = base(2U,&p,false,true,true,1U); put(&f,1U,1U); put(&f,0U,4U);
    put(&f,(0x22U << 7U) | 3U,2U); fill(&f,2U,0U); fill(&f,4U,0U);
    CHECK(parse_unchanged_on_failure(f.bytes,f.length,&out) == MESH_MAC_MALFORMED); /* IE cannot consume MIC. */
    f = base(2U,&p,false,false,true,1U); put(&f,0U,2U); put(&f,1U,1U);
    CHECK(parse_unchanged_on_failure(f.bytes,f.length,&out) == MESH_MAC_MALFORMED); /* Trailing half descriptor. */
    f = base(2U,&p,true,false,true,1U);
    while (f.length + 2U <= sizeof(f.bytes)) put(&f,0U,2U);
    CHECK(mesh_mac_parse(f.bytes,f.length,&out) == MESH_MAC_HEADER_OK); /* Bounded many-zero-length IE list. */
}

static void identities_and_rejections(void)
{
    const uint8_t golden[] = {0x41,0x88,0x5a,0x34,0x12,0x78,0x56,0x57,0x13,0xde,0xad};
    mesh_mac_observation_t out;
    CHECK(mesh_mac_parse(golden,sizeof(golden),&out) == MESH_MAC_HEADER_OK);
    CHECK(out.source.value == 0x1357U && out.destination.value == 0x5678U);
    CHECK(out.source_pan.origin == MESH_MAC_PAN_COMPRESSED_SAME && out.source_pan.value == 0x1234U);
    CHECK(out.header_length == 9U && out.payload_length == 2U);
    const pan_case_t pair = {2,2,1,1,0};
    frame_t f = base(1U,&pair,false,false,false,1U);
    f.bytes[5] = 0xffU; f.bytes[6] = 0xffU;
    f.bytes[7] = 0xfeU; f.bytes[8] = 0xffU;
    CHECK(mesh_mac_parse(f.bytes,f.length,&out) == MESH_MAC_HEADER_OK);
    CHECK(out.destination.broadcast && out.source.short_sentinel);
    CHECK(!out.source.broadcast && !out.destination.short_sentinel);
    f.bytes[3] = 0U; f.bytes[4] = 0U;
    CHECK(mesh_mac_parse(f.bytes,f.length,&out) == MESH_MAC_HEADER_OK);
    CHECK(out.destination_pan.origin == MESH_MAC_PAN_EXPLICIT && out.destination_pan.value == 0U);
    const pan_case_t ext = {3,3,1,0,0};
    f = base(2U,&ext,true,false,false,1U);
    CHECK(mesh_mac_parse(f.bytes,f.length,&out) == MESH_MAC_HEADER_OK);
    CHECK(out.source_pan.origin == MESH_MAC_PAN_UNKNOWN && out.destination_pan.origin == MESH_MAC_PAN_UNKNOWN);
    CHECK(out.source.value != out.destination.value && out.source.mode == MESH_MAC_ADDRESS_EXT64);
    mesh_mac_observation_t frozen = out;
    memset(f.bytes,0,sizeof(f.bytes));
    CHECK(memcmp(&frozen,&out,sizeof(out)) == 0);

    const pan_case_t none = {0,0,0,0,0};
    for (unsigned version = 0; version < 3U; ++version) {
        f = base(version,&none,false,false,false,2U);
        CHECK(mesh_mac_parse(f.bytes,f.length,&out) == MESH_MAC_HEADER_OK);
        CHECK(out.source.mode == 0U && out.destination.mode == 0U); /* ACK is not linked by sequence. */
        f = base(version,&pair,false,false,false,3U);
        if (version < 2U) {
            CHECK(parse_unchanged_on_failure(f.bytes,f.length,&out) == MESH_MAC_MALFORMED);
            put(&f,4U,1U);
        }
        CHECK(mesh_mac_parse(f.bytes,f.length,&out) == MESH_MAC_HEADER_OK);
        CHECK(out.legacy_command_id_present == (version < 2U));
        CHECK(out.legacy_command_id == (version < 2U ? 4U : 0U));
    }
    for (unsigned type = 4U; type < 8U; ++type) {
        f = base(2U,&none,false,false,false,type);
        CHECK(parse_unchanged_on_failure(f.bytes,f.length,&out) == MESH_MAC_UNSUPPORTED);
    }
    const uint16_t rejected[] = {0x3001U,0x2401U,0x6001U,0x2081U,0x0101U,0x0201U,0x9842U};
    for (size_t i = 0; i < sizeof(rejected)/sizeof(rejected[0]); ++i) {
        f = (frame_t){{0},0}; put(&f,rejected[i],2U); fill(&f,40U,0U);
        CHECK(parse_unchanged_on_failure(f.bytes,f.length,&out) == MESH_MAC_UNSUPPORTED);
    }
    CHECK(parse_unchanged_on_failure(NULL,0U,&out) == MESH_MAC_INVALID_ARGUMENT);
    CHECK(mesh_mac_parse(golden,sizeof(golden),NULL) == MESH_MAC_INVALID_ARGUMENT);
    CHECK(parse_unchanged_on_failure(golden,SIZE_MAX,&out) == MESH_MAC_MALFORMED);
    union { mesh_mac_observation_t aligned; uint8_t bytes[256]; } aliased;
    memset(&aliased,0x55,sizeof(aliased)); memcpy(aliased.bytes,golden,sizeof(golden));
    uint8_t old[sizeof(aliased)]; memcpy(old,&aliased,sizeof(old));
    CHECK(mesh_mac_parse(aliased.bytes,sizeof(golden),&aliased.aligned) == MESH_MAC_INVALID_ARGUMENT);
    CHECK(memcmp(old,&aliased,sizeof(old)) == 0);
}

static uint32_t rng_state = UINT32_C(0x4d455348);
static uint32_t random_word(void)
{
    rng_state ^= rng_state << 13U; rng_state ^= rng_state >> 17U; rng_state ^= rng_state << 5U;
    return rng_state;
}

static void random_boundaries(void)
{
    for (unsigned iteration = 0; iteration < 20000U; ++iteration) {
        struct { uint64_t first; uint8_t data[126]; uint64_t last; } input;
        memset(&input,0xa5,sizeof(input));
        size_t length = random_word() % 127U;
        for (size_t j = 0; j < length; ++j) input.data[j] = (uint8_t)random_word();
        uint8_t before[sizeof(input)]; memcpy(before,&input,sizeof(before));
        struct { uint64_t first; mesh_mac_observation_t value; uint64_t last; } output;
        memset(&output,0x63,sizeof(output));
        uint64_t first = output.first, last = output.last;
        mesh_mac_result_t result = parse_unchanged_on_failure(input.data,length,&output.value);
        CHECK(memcmp(before,&input,sizeof(before)) == 0);
        CHECK(output.first == first && output.last == last);
        if (result == MESH_MAC_HEADER_OK) {
            const mesh_mac_observation_t *o = &output.value;
            CHECK((size_t)o->header_length + o->payload_length + o->mic_length == length);
            CHECK(o->version <= 2U && o->frame_type <= 3U);
            CHECK(o->source.mode == 0U || o->source.mode == 2U || o->source.mode == 3U);
            CHECK(o->destination.mode == 0U || o->destination.mode == 2U || o->destination.mode == 3U);
            CHECK(o->key_identifier_length <= 9U);
            CHECK(o->source_pan.origin != MESH_MAC_PAN_UNKNOWN || o->source_pan.value == 0U);
            CHECK(o->destination_pan.origin != MESH_MAC_PAN_UNKNOWN || o->destination_pan.value == 0U);
            mesh_mac_observation_t again;
            CHECK(mesh_mac_parse(input.data,length,&again) == MESH_MAC_HEADER_OK);
            CHECK(memcmp(o,&again,sizeof(again)) == 0);
        } else CHECK(result == MESH_MAC_MALFORMED || result == MESH_MAC_UNSUPPORTED);
    }
}

int main(void)
{
    pan_tables(); security_bounds(); ie_bounds(); identities_and_rejections(); random_boundaries();
    printf("mesh MAC header: %lu checks PASS; observation=%zu bytes\n",checks,sizeof(mesh_mac_observation_t));
    return 0;
}
