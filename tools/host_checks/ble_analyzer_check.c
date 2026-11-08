#undef NDEBUG
#include "ble_analyzer.h"

#include <assert.h>
#include <limits.h>
#include <string.h>
#include <stdio.h>

static cerberus_ble_analyzer_report_t report(const uint8_t *ad, size_t length)
{
    cerberus_ble_analyzer_report_t result = {
        .address = {1, 2, 3, 4, 5, 6},
        .address_type = 1,
        .rssi_dbm = -62,
        .advertisement = ad,
        .advertisement_length = length,
        .complete = true,
    };
    return result;
}

static unsigned family_checks;
#define FAMILY_CHECK(value) do { ++family_checks; assert(value); } while (0)

static void check_findmy_flipper_events(void)
{
    cerberus_ble_analyzer_t analyzer;
    cerberus_ble_analyzer_view_t view;
    const uint8_t fmna[] = {3,3,0x44,0xfd};
    const uint8_t flipper[] = {3,3,0x81,0x30};
    const uint8_t dult[] = {17,7,0x85,0x2a,0x9f,0x57,0xc5,0x2a,0xed,0x88,
        0x26,0xc2,0xf4,0x12,1,0,0x19,0x15};
    const uint8_t named_base[] = {3,3,0x80,0x30,8,9,'F','l','i','p','p','e','r'};
    const uint8_t base[] = {3,3,0x80,0x30};
    const uint8_t name[] = {8,9,'F','l','i','p','p','e','r'};
    uint8_t apple[31] = {30,0xff,0x4c,0,0x12,0x19};
    uint32_t now = 1000U;
    cerberus_ble_analyzer_begin(&analyzer, now);

    /* The currently enabled native path receives legacy reports. All of these
     * individual list encodings fit in one legacy report. A response callback
     * stands alone: there is intentionally no prior-ADV binding in Analyzer. */
    for (unsigned family = 0U; family < 2U; ++family) {
        for (unsigned width_index = 0U; width_index < 3U; ++width_index) {
            const unsigned widths[] = {2U, 4U, 16U};
            unsigned width = widths[width_index];
            for (unsigned complete_list = 0U; complete_list < 2U; ++complete_list) {
                for (unsigned response = 0U; response < 2U; ++response) {
                    unsigned variants = family ? 3U : 1U;
                    for (unsigned variant = 0U; variant < variants; ++variant) {
                        uint8_t ad[18] = {0};
                        ad[0] = (uint8_t)(width + 1U);
                        ad[1] = (uint8_t)(2U + width_index * 2U + complete_list);
                        unsigned offset = 2U;
                        if (width == 16U) {
                            const uint8_t uuid_base[] = {0xfb,0x34,0x9b,0x5f,0x80,0,0,0x80,0,0x10,0,0,0,0,0,0};
                            memcpy(ad + 2U, uuid_base, sizeof(uuid_base));
                            offset = 14U;
                        }
                        ad[offset] = family ? (uint8_t)(0x81U + variant) : 0x44U;
                        ad[offset + 1U] = family ? 0x30U : 0xfdU;
                        cerberus_ble_analyzer_report_t r = report(ad, width + 2U);
                        r.scan_response = response != 0U;
                        uint32_t old_findmy = analyzer.findmy_hint_events;
                        uint32_t old_flipper = analyzer.flipper_hint_events;
                        FAMILY_CHECK(cerberus_ble_analyzer_observe(&analyzer, &r, ++now));
                        cerberus_ble_analyzer_snapshot(&analyzer, now + 3U, &view);
                        FAMILY_CHECK(view.last_report_valid && view.last_scan_response == (response != 0U) &&
                            view.last_seen_age_ms == 3U && view.last_fresh);
                        FAMILY_CHECK(view.findmy_hint_events == old_findmy + (family == 0U) &&
                            view.flipper_hint_events == old_flipper + (family != 0U));
                        FAMILY_CHECK(view.last_findmy.bits == (family ? 0U : CERBERUS_BLE_FINDMY_HINT_FMNA) &&
                            view.last_findmy.fmna_uuid_widths == (family ? 0U : (1U << width_index)));
                        FAMILY_CHECK(view.last_flipper.bits == (family ? (1U << (variant + 1U)) : 0U) &&
                            view.last_flipper.flags == (family ? (1U << width_index) : 0U));
                        FAMILY_CHECK(view.evidence_kind == (family ? CERBERUS_BLE_ANALYZER_EVIDENCE_FLIPPER_HINT :
                            CERBERUS_BLE_ANALYZER_EVIDENCE_FINDMY_HINT));
                    }
                }
            }
        }
    }

    uint8_t mixed[sizeof(fmna) + sizeof(dult) + sizeof(flipper)];
    memcpy(mixed, fmna, sizeof(fmna));
    memcpy(mixed + sizeof(fmna), dult, sizeof(dult));
    memcpy(mixed + sizeof(fmna) + sizeof(dult), flipper, sizeof(flipper));
    uint8_t mixed_before[sizeof(mixed)]; memcpy(mixed_before, mixed, sizeof(mixed));
    cerberus_ble_analyzer_report_t r = report(mixed, sizeof(mixed));
    r.scan_response = true;
    cerberus_ble_analyzer_begin(&analyzer, ++now);
    FAMILY_CHECK(cerberus_ble_analyzer_observe(&analyzer, &r, ++now));
    cerberus_ble_analyzer_snapshot(&analyzer, now, &view);
    FAMILY_CHECK(view.last_findmy.bits == (CERBERUS_BLE_FINDMY_HINT_FMNA | CERBERUS_BLE_FINDMY_HINT_DULT) &&
        view.last_flipper.bits == CERBERUS_BLE_FLIPPER_HINT_BLACK);
    FAMILY_CHECK(view.findmy_hint_events == 1U && view.flipper_hint_events == 1U &&
        view.total_events == 1U && view.current_events == 1U && view.last_scan_response);
    FAMILY_CHECK(view.evidence_kind == CERBERUS_BLE_ANALYZER_EVIDENCE_FINDMY_HINT);
    FAMILY_CHECK(memcmp(mixed, mixed_before, sizeof(mixed)) == 0);
    cerberus_ble_analyzer_view_t frozen = view;

    /* No hints/complete names join across events, including a same-address RSP. */
    r = report(base, sizeof(base));
    FAMILY_CHECK(cerberus_ble_analyzer_observe(&analyzer, &r, ++now));
    r = report(name, sizeof(name)); r.scan_response = true;
    FAMILY_CHECK(cerberus_ble_analyzer_observe(&analyzer, &r, ++now));
    cerberus_ble_analyzer_snapshot(&analyzer, now, &view);
    FAMILY_CHECK(!view.last_findmy.bits && !view.last_flipper.bits &&
        view.findmy_hint_events == 1U && view.flipper_hint_events == 1U);
    r = report(named_base, sizeof(named_base));
    FAMILY_CHECK(cerberus_ble_analyzer_observe(&analyzer, &r, ++now));
    cerberus_ble_analyzer_snapshot(&analyzer, now, &view);
    FAMILY_CHECK(view.last_flipper.bits == CERBERUS_BLE_FLIPPER_HINT_BASE &&
        view.last_flipper.flags == (CERBERUS_BLE_FLIPPER_UUID16 | CERBERUS_BLE_FLIPPER_NAME) &&
        view.evidence_kind == CERBERUS_BLE_ANALYZER_EVIDENCE_WATCH);
    const uint8_t named_data[] = {3,0x16,0x82,0x30,8,9,'F','l','i','p','p','e','r'};
    r = report(named_data, sizeof(named_data)); r.scan_response = true;
    FAMILY_CHECK(cerberus_ble_analyzer_observe(&analyzer, &r, ++now));
    cerberus_ble_analyzer_snapshot(&analyzer, now, &view);
    FAMILY_CHECK(view.last_flipper.bits == CERBERUS_BLE_FLIPPER_HINT_WHITE &&
        view.last_flipper.flags == (CERBERUS_BLE_FLIPPER_UUID16 | CERBERUS_BLE_FLIPPER_NAME |
            CERBERUS_BLE_FLIPPER_SERVICE_DATA) && view.last_scan_response);

    /* UUID-like bytes in other fields, non-base UUIDs and unnamed service data
     * must not become independent hints after wiring in the strict parsers. */
    const uint8_t no_hints[][6] = {
        {3,0x16,0x44,0xfd,0,0}, {3,0x16,0x81,0x30,0,0},
        {5,0x30,0x44,0xfd,0x81,0x30}, {5,5,0x44,0xfd,1,0},
        {5,5,0x81,0x30,1,0},
    };
    for (unsigned i = 0U; i < sizeof(no_hints) / sizeof(no_hints[0]); ++i) {
        r = report(no_hints[i], sizeof(no_hints[i]));
        FAMILY_CHECK(cerberus_ble_analyzer_observe(&analyzer, &r, ++now));
        cerberus_ble_analyzer_snapshot(&analyzer, now, &view);
        FAMILY_CHECK(view.last_report_valid && !view.last_findmy.bits && !view.last_flipper.bits &&
            view.evidence_kind == CERBERUS_BLE_ANALYZER_EVIDENCE_NONE);
    }
    r = report(apple, sizeof(apple));
    FAMILY_CHECK(cerberus_ble_analyzer_observe(&analyzer, &r, ++now));
    cerberus_ble_analyzer_snapshot(&analyzer, now, &view);
    FAMILY_CHECK(view.last_findmy.bits == CERBERUS_BLE_FINDMY_HINT_APPLE &&
        !view.last_findmy.fmna_uuid_widths && !view.last_flipper.bits &&
        view.evidence_kind == CERBERUS_BLE_ANALYZER_EVIDENCE_WATCH);

    /* Repeated qualifying fields count once per family, not once per field. */
    uint8_t duplicate[16];
    memcpy(duplicate, fmna, 4U); memcpy(duplicate + 4U, fmna, 4U);
    memcpy(duplicate + 8U, flipper, 4U); memcpy(duplicate + 12U, flipper, 4U);
    r = report(duplicate, sizeof(duplicate));
    uint32_t findmy_before = analyzer.findmy_hint_events, flipper_before = analyzer.flipper_hint_events;
    FAMILY_CHECK(cerberus_ble_analyzer_observe(&analyzer, &r, ++now));
    FAMILY_CHECK(analyzer.findmy_hint_events == findmy_before + 1U &&
        analyzer.flipper_hint_events == flipper_before + 1U);

    /* Host-only complete long inputs establish parser coverage, never native
     * extended reception. Exercise the first field and a field at byte 254. */
    uint8_t longest[255] = {246U, 0x30U};
    memcpy(longest + 247U, fmna, sizeof(fmna));
    memcpy(longest + 251U, flipper, sizeof(flipper));
    uint8_t long_apple[35]; memcpy(long_apple, apple, 31U); memcpy(long_apple + 31U, flipper, 4U);
    for (unsigned kind = 0U; kind < 2U; ++kind) {
        r = report(kind ? long_apple : longest, kind ? sizeof(long_apple) : sizeof(longest));
        r.scan_response = kind != 0U;
        FAMILY_CHECK(cerberus_ble_analyzer_observe(&analyzer, &r, ++now));
        cerberus_ble_analyzer_snapshot(&analyzer, now, &view);
        FAMILY_CHECK(view.last_report_valid && view.last_findmy.bits ==
            (kind ? CERBERUS_BLE_FINDMY_HINT_APPLE : CERBERUS_BLE_FINDMY_HINT_FMNA) &&
            view.last_flipper.bits == CERBERUS_BLE_FLIPPER_HINT_BLACK && view.last_scan_response == (kind != 0U));
    }

    uint8_t invalid_suffix[sizeof(mixed) + 3U];
    memcpy(invalid_suffix, mixed, sizeof(mixed));
    memcpy(invalid_suffix + sizeof(mixed), (uint8_t[]){2,3,0x44}, 3U);
    const uint8_t ordinary[] = {2,1,6};
    uint8_t oversized[256] = {0};
    for (unsigned kind = 0U; kind < 6U; ++kind) {
        r = report(mixed, sizeof(mixed));
        FAMILY_CHECK(cerberus_ble_analyzer_observe(&analyzer, &r, ++now));
        findmy_before = analyzer.findmy_hint_events; flipper_before = analyzer.flipper_hint_events;
        if (kind == 0U) { r.advertisement = ordinary; r.advertisement_length = sizeof(ordinary); }
        else if (kind == 1U) { r.advertisement = invalid_suffix; r.advertisement_length = sizeof(invalid_suffix); }
        else if (kind == 2U) r.complete = false;
        else if (kind == 3U) r.advertisement_flags = 1U;
        else if (kind == 4U) { r.advertisement = oversized; r.advertisement_length = sizeof(oversized); }
        else { r.advertisement = NULL; r.advertisement_length = 0U; }
        r.scan_response = true;
        FAMILY_CHECK(cerberus_ble_analyzer_observe(&analyzer, &r, ++now));
        cerberus_ble_analyzer_snapshot(&analyzer, now + 4U, &view);
        FAMILY_CHECK(!view.last_findmy.bits && !view.last_findmy.fmna_uuid_widths &&
            !view.last_flipper.bits && !view.last_flipper.flags && view.last_scan_response && view.last_seen_age_ms == 4U);
        FAMILY_CHECK(view.findmy_hint_events == findmy_before && view.flipper_hint_events == flipper_before &&
            view.evidence_kind == CERBERUS_BLE_ANALYZER_EVIDENCE_NONE);
    }
    FAMILY_CHECK(frozen.last_findmy.bits == (CERBERUS_BLE_FINDMY_HINT_FMNA | CERBERUS_BLE_FINDMY_HINT_DULT) &&
        frozen.last_flipper.bits == CERBERUS_BLE_FLIPPER_HINT_BLACK && frozen.total_events == 1U);

    r = report(mixed, sizeof(mixed));
    analyzer.findmy_hint_events = UINT32_MAX - 1U;
    analyzer.flipper_hint_events = UINT32_MAX - 1U;
    for (unsigned i = 0U; i < 3U; ++i) {
        FAMILY_CHECK(cerberus_ble_analyzer_observe(&analyzer, &r, ++now));
        cerberus_ble_analyzer_snapshot(&analyzer, now, &view);
        FAMILY_CHECK(view.findmy_hint_events == UINT32_MAX && view.flipper_hint_events == UINT32_MAX);
    }
    cerberus_ble_analyzer_t saved = analyzer;
    r.advertisement = NULL;
    FAMILY_CHECK(!cerberus_ble_analyzer_observe(&analyzer, &r, ++now));
    FAMILY_CHECK(memcmp(&analyzer, &saved, sizeof(saved)) == 0);
    r = report(mixed, sizeof(mixed)); r.address_type = 4U;
    FAMILY_CHECK(!cerberus_ble_analyzer_observe(&analyzer, &r, ++now));
    FAMILY_CHECK(memcmp(&analyzer, &saved, sizeof(saved)) == 0);
    cerberus_ble_analyzer_stop(&analyzer);
    r.address_type = 1U;
    FAMILY_CHECK(!cerberus_ble_analyzer_observe(&analyzer, &r, ++now));
    cerberus_ble_analyzer_snapshot(&analyzer, now, &view);
    FAMILY_CHECK(!view.active && view.findmy_hint_events == UINT32_MAX && view.flipper_hint_events == UINT32_MAX);
    cerberus_ble_analyzer_begin(&analyzer, ++now);
    cerberus_ble_analyzer_snapshot(&analyzer, now, &view);
    FAMILY_CHECK(view.active && !view.has_last && !view.findmy_hint_events && !view.flipper_hint_events &&
        !view.last_findmy.bits && !view.last_flipper.bits);
}

static void check_family_priority(void)
{
    const uint8_t watch[] = {3,3,0x81,0xfc};
    const uint8_t meta[] = {3,0xff,0x5f,0xfd};
    const uint8_t hc[] = {6,9,'H','C','-','0','5'};
    const uint8_t flock[] = {3,0xff,0xc8,9};
    const uint8_t ibeacon[27] = {26,0xff,0x4c,0,2,0x15};
    const struct { const uint8_t *bytes; size_t size; cerberus_ble_analyzer_evidence_kind_t kind; } cases[] = {
        {watch,sizeof(watch),CERBERUS_BLE_ANALYZER_EVIDENCE_WATCH},
        {meta,sizeof(meta),CERBERUS_BLE_ANALYZER_EVIDENCE_META_ID_HINT},
        {hc,sizeof(hc),CERBERUS_BLE_ANALYZER_EVIDENCE_HC_NAME_HINT},
        {flock,sizeof(flock),CERBERUS_BLE_ANALYZER_EVIDENCE_FLOCK_HINT},
        {ibeacon,sizeof(ibeacon),CERBERUS_BLE_ANALYZER_EVIDENCE_IBEACON_FORMAT},
    };
    const uint8_t both[] = {3,3,0x44,0xfd,3,3,0x81,0x30};
    for (unsigned i = 0U; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        uint8_t mixed[40] = {0};
        memcpy(mixed, cases[i].bytes, cases[i].size);
        memcpy(mixed + cases[i].size, both, sizeof(both));
        cerberus_ble_analyzer_t original, enriched;
        cerberus_ble_analyzer_view_t before, after;
        cerberus_ble_analyzer_begin(&original, 1000U);
        cerberus_ble_analyzer_begin(&enriched, 1000U);
        cerberus_ble_analyzer_report_t r = report(cases[i].bytes, cases[i].size);
        FAMILY_CHECK(cerberus_ble_analyzer_observe(&original, &r, 1001U));
        r.advertisement = mixed; r.advertisement_length = cases[i].size + sizeof(both);
        FAMILY_CHECK(cerberus_ble_analyzer_observe(&enriched, &r, 1001U));
        cerberus_ble_analyzer_snapshot(&original, 1200U, &before);
        cerberus_ble_analyzer_snapshot(&enriched, 1200U, &after);
        FAMILY_CHECK(before.evidence_kind == cases[i].kind && after.evidence_kind == before.evidence_kind &&
            after.evidence_confidence == before.evidence_confidence && after.evidence_label == before.evidence_label);
        FAMILY_CHECK(after.watch_match_events == before.watch_match_events &&
            after.ibeacon_format_events == before.ibeacon_format_events &&
            after.meta_hint_events == before.meta_hint_events && after.hc_name_hint_events == before.hc_name_hint_events &&
            after.flock_hint_events == before.flock_hint_events);
        FAMILY_CHECK(after.findmy_hint_events == 1U && after.flipper_hint_events == 1U &&
            after.last_findmy.bits == CERBERUS_BLE_FINDMY_HINT_FMNA &&
            after.last_flipper.bits == CERBERUS_BLE_FLIPPER_HINT_BLACK);
        FAMILY_CHECK(after.total_events == before.total_events && after.current_events == before.current_events &&
            after.history_count == before.history_count && memcmp(after.history,before.history,sizeof(after.history)) == 0);
    }
    printf("BLE Analyzer independent Find My/Flipper events: %u checks passed.\n", family_checks);
}

static void check_latest_hc(void)
{
    cerberus_ble_analyzer_t analyzer;
    cerberus_ble_analyzer_view_t view;
    const uint8_t ordinary[]={2,1,6}, malformed[]={6,9,'H','C','-','0','5',2,3,1};
    uint8_t hc[]={6,9,'H','C','-','0','3'},oversized[256]={0};
    const uint8_t codes[]={CERBERUS_BLE_HC_03,CERBERUS_BLE_HC_05,CERBERUS_BLE_HC_06};
    cerberus_ble_analyzer_begin(&analyzer,1000U);
    for(unsigned i=0U;i<3U;++i){
        hc[6]=(uint8_t)('0'+codes[i]);
        cerberus_ble_analyzer_report_t r=report(hc,sizeof(hc));r.scan_response=i!=0U;
        assert(cerberus_ble_analyzer_observe(&analyzer,&r,1010U+i*10U));
        cerberus_ble_analyzer_snapshot(&analyzer,1013U+i*10U,&view);
        assert(view.last_report_valid && view.last_hc_name==codes[i] &&
            view.last_scan_response==(i!=0U) && view.last_seen_age_ms==3U &&
            view.hc_name_hint_events==i+1U && view.evidence_kind==CERBERUS_BLE_ANALYZER_EVIDENCE_HC_NAME_HINT);
    }
    cerberus_ble_analyzer_view_t selected=view;
    /* Every accepted ordinary/invalid event replaces latest-event HC data,
     * including source kind, while the cumulative counter stays cumulative. */
    for(unsigned kind=0U;kind<8U;++kind){
        cerberus_ble_analyzer_report_t r=report(hc,sizeof(hc));
        assert(cerberus_ble_analyzer_observe(&analyzer,&r,1100U+kind*10U));
        uint32_t count=analyzer.hc_name_hint_events;
        r.scan_response=true;
        if(kind==0U){r.advertisement=ordinary;r.advertisement_length=sizeof(ordinary);}
        else if(kind==1U){r.advertisement=malformed;r.advertisement_length=sizeof(malformed);}
        else if(kind==2U)r.complete=false;
        else if(kind==3U)r.advertisement_flags=1U;
        else if(kind==4U){r.advertisement=oversized;r.advertisement_length=sizeof(oversized);}
        else if(kind==5U)hc[1]=8U;
        else if(kind==6U)hc[6]=0U;
        else hc[6]='4';
        assert(cerberus_ble_analyzer_observe(&analyzer,&r,1101U+kind*10U));
        cerberus_ble_analyzer_snapshot(&analyzer,1102U+kind*10U,&view);
        assert(!view.last_hc_name && view.last_scan_response && view.last_seen_age_ms==1U &&
            view.hc_name_hint_events==count && view.evidence_kind==CERBERUS_BLE_ANALYZER_EVIDENCE_NONE);
        hc[1]=9U;hc[6]='6';
    }
    assert(selected.last_hc_name==CERBERUS_BLE_HC_06 && selected.hc_name_hint_events==3U);
    uint8_t conflict[14];memcpy(conflict,hc,sizeof(hc));memcpy(conflict+7U,hc,sizeof(hc));conflict[13]='3';
    cerberus_ble_analyzer_report_t r=report(conflict,sizeof(conflict));
    assert(cerberus_ble_analyzer_observe(&analyzer,&r,1200U));
    cerberus_ble_analyzer_snapshot(&analyzer,1200U,&view);
    assert(!view.last_report_valid && !view.last_hc_name);
    uint8_t extended[255]={247U,0x30U};memcpy(extended+248U,hc,sizeof(hc));
    r=report(extended,sizeof(extended));r.scan_response=true;
    assert(cerberus_ble_analyzer_observe(&analyzer,&r,1210U));
    cerberus_ble_analyzer_snapshot(&analyzer,1210U,&view);
    assert(view.last_report_valid && view.last_hc_name==CERBERUS_BLE_HC_06 && view.last_scan_response);
    /* A rejected API argument is not an accepted event and cannot erase it. */
    r.advertisement=NULL;
    assert(!cerberus_ble_analyzer_observe(&analyzer,&r,1220U));
    cerberus_ble_analyzer_snapshot(&analyzer,1220U,&view);
    assert(view.last_hc_name==CERBERUS_BLE_HC_06 && view.last_seen_age_ms==10U);
    analyzer.hc_name_hint_events=UINT32_MAX;
    r=report(hc,sizeof(hc));
    assert(cerberus_ble_analyzer_observe(&analyzer,&r,1230U));
    cerberus_ble_analyzer_snapshot(&analyzer,1230U,&view);
    assert(view.hc_name_hint_events==UINT32_MAX && view.last_hc_name==CERBERUS_BLE_HC_06);
}

static void check_extended_meta(void)
{
    cerberus_ble_analyzer_t analyzer;
    cerberus_ble_analyzer_view_t view;
    const uint8_t meta[]={3,0xff,0x5f,0xfd};
    const uint8_t hc[]={6,9,'H','C','-','0','5'};
    const uint8_t blocked[]={3,0xff,0x4c,0};
    uint8_t extended[255]={239,0x30};
    memcpy(extended+240U,meta,sizeof(meta));
    memcpy(extended+244U,hc,sizeof(hc));
    cerberus_ble_analyzer_report_t r=report(extended,sizeof(extended));
    r.scan_response=true;
    cerberus_ble_analyzer_begin(&analyzer,1000U);
    assert(cerberus_ble_analyzer_observe(&analyzer,&r,1010U));
    cerberus_ble_analyzer_snapshot(&analyzer,1020U,&view);
    assert(view.last_report_valid && view.last_scan_response && view.last_seen_age_ms==10U);
    assert(view.meta_hint_events==1U && view.hc_name_hint_events==1U);
    assert(view.watch_match_events==0U && view.ibeacon_format_events==0U);
    assert(cerberus_ble_meta_evidence_count(&view.last_meta)==1U &&
        cerberus_ble_meta_evidence_has(&view.last_meta,0U,CERBERUS_BLE_META_MFG16));
    assert(view.last_name_length==5U && !memcmp(view.last_name,"HC-05",5U) &&
        view.last_hc_name==CERBERUS_BLE_HC_05 && view.evidence_kind==CERBERUS_BLE_ANALYZER_EVIDENCE_META_ID_HINT);
    memcpy(extended+251U,blocked,sizeof(blocked));r.scan_response=false;
    assert(cerberus_ble_analyzer_observe(&analyzer,&r,1030U));
    cerberus_ble_analyzer_snapshot(&analyzer,1030U,&view);
    assert(view.last_report_valid && !view.last_scan_response &&
        cerberus_ble_meta_evidence_count(&view.last_meta)==2U && cerberus_ble_meta_evidence_blocked(&view.last_meta));
    assert(view.meta_hint_events==1U && view.hc_name_hint_events==2U);
    assert(view.evidence_kind==CERBERUS_BLE_ANALYZER_EVIDENCE_HC_NAME_HINT);
    r=report(meta,sizeof(meta));
    assert(cerberus_ble_analyzer_observe(&analyzer,&r,1040U));
    cerberus_ble_analyzer_snapshot(&analyzer,1040U,&view);
    assert(view.meta_hint_events==2U && !cerberus_ble_meta_evidence_blocked(&view.last_meta) && !view.last_hc_name);

    /* No old latest-event matrix/name/hint survives a malformed suffix,
     * incomplete source, oversize source or source truncation flag. */
    uint8_t oversized[256]={0};
    for(unsigned kind=0;kind<4U;++kind){
        r=report(extended,sizeof(extended));r.scan_response=true;
        if(kind==0U)extended[251]=4U;
        else if(kind==1U)r.complete=false;
        else if(kind==2U){r.advertisement=oversized;r.advertisement_length=sizeof(oversized);}
        else r.advertisement_flags=1U;
        assert(cerberus_ble_analyzer_observe(&analyzer,&r,1050U+kind));
        cerberus_ble_analyzer_snapshot(&analyzer,1050U+kind,&view);
        assert(!view.last_report_valid && view.last_scan_response && !view.last_name_length && !view.last_hc_name &&
            !cerberus_ble_meta_evidence_count(&view.last_meta));
        assert(view.evidence_kind==CERBERUS_BLE_ANALYZER_EVIDENCE_NONE &&
            view.meta_hint_events==2U && view.hc_name_hint_events==2U);
        extended[251]=3U;
    }
    assert(view.malformed_reports==1U && view.unclassified_reports==3U);
    const uint8_t ordinary[]={2,1,6};
    r=report(ordinary,sizeof(ordinary));
    assert(cerberus_ble_analyzer_observe(&analyzer,&r,1060U));
    cerberus_ble_analyzer_snapshot(&analyzer,1060U,&view);
    assert(view.last_report_valid && !view.last_scan_response && !cerberus_ble_meta_evidence_count(&view.last_meta));

    /* Watch keeps its whole-report bound while independent iBeacon parsing
     * now accepts the complete extended report and preserves its payload. */
    uint8_t long_beacon[34]={26,0xff,0x4c,0,2,0x15};
    memcpy(long_beacon+27U,hc,sizeof(hc));
    r=report(long_beacon,sizeof(long_beacon));
    cerberus_ble_analyzer_begin(&analyzer,2000U);
    assert(cerberus_ble_analyzer_observe(&analyzer,&r,2010U));
    cerberus_ble_analyzer_snapshot(&analyzer,2010U,&view);
    assert(view.last_report_valid && view.ibeacon_format_events==1U && view.watch_match_events==0U &&
        view.last_ibeacon_valid);
    assert(view.hc_name_hint_events==1U && cerberus_ble_meta_evidence_blocked(&view.last_meta) &&
        view.last_hc_name==CERBERUS_BLE_HC_05 && view.evidence_kind==CERBERUS_BLE_ANALYZER_EVIDENCE_IBEACON_FORMAT);
    r.advertisement_length=27U;
    assert(cerberus_ble_analyzer_observe(&analyzer,&r,2020U));
    cerberus_ble_analyzer_snapshot(&analyzer,2020U,&view);
    assert(view.ibeacon_format_events==2U);
}

static void check_latest_ibeacon(void)
{
    cerberus_ble_analyzer_t analyzer;
    cerberus_ble_analyzer_view_t view;
    uint8_t a[27]={26,0xff,0x4c,0,2,0x15},b[27];
    a[6]=1U;a[22]=1U;a[23]=2U;a[24]=3U;a[25]=4U;a[26]=0x80U;
    memcpy(b,a,sizeof(b));b[6]=2U;b[23]=9U;b[26]=127U;
    cerberus_ble_analyzer_report_t r=report(a,sizeof(a));
    cerberus_ble_analyzer_begin(&analyzer,1000U);
    assert(cerberus_ble_analyzer_observe(&analyzer,&r,1010U));
    cerberus_ble_analyzer_snapshot(&analyzer,1011U,&view);
    assert(view.last_ibeacon_valid && view.last_ibeacon.uuid[0]==1U &&
        view.last_ibeacon.major==258U && view.last_ibeacon.minor==772U &&
        view.last_ibeacon.measured_power_dbm==-128 && !view.last_scan_response && view.last_seen_age_ms==1U);
    r.advertisement=b;r.scan_response=true;r.address[0]=9U;
    assert(cerberus_ble_analyzer_observe(&analyzer,&r,1020U));
    cerberus_ble_analyzer_snapshot(&analyzer,1021U,&view);
    assert(view.last_ibeacon_valid && view.last_ibeacon.uuid[0]==2U &&
        view.last_ibeacon.major==265U && view.last_ibeacon.measured_power_dbm==127 &&
        view.last_scan_response && view.last_address[0]==9U && view.ibeacon_format_events==2U);
    const uint8_t ordinary[]={2,1,6}, malformed[]={3,9,'x'};
    uint8_t oversized[256]={0};
    cerberus_ibeacon_t zero={0};
    for(unsigned kind=0U;kind<5U;++kind){
        r=report(a,sizeof(a));assert(cerberus_ble_analyzer_observe(&analyzer,&r,1030U+kind*2U));
        r.scan_response=true;
        if(kind==0U){r.advertisement=ordinary;r.advertisement_length=sizeof(ordinary);}
        else if(kind==1U){r.advertisement=malformed;r.advertisement_length=sizeof(malformed);}
        else if(kind==2U)r.complete=false;
        else if(kind==3U)r.advertisement_flags=1U;
        else {r.advertisement=oversized;r.advertisement_length=sizeof(oversized);}
        assert(cerberus_ble_analyzer_observe(&analyzer,&r,1031U+kind*2U));
        cerberus_ble_analyzer_snapshot(&analyzer,1031U+kind*2U,&view);
        assert(!view.last_ibeacon_valid && !memcmp(&view.last_ibeacon,&zero,sizeof(zero)) &&
            view.last_scan_response && view.last_report_valid==(kind==0U));
    }
    /* Full255-byte evidence is accepted without truncation. */
    uint8_t extended[255]={227U,0x30U};memcpy(extended+228U,b,sizeof(b));
    r=report(extended,sizeof(extended));
    assert(cerberus_ble_analyzer_observe(&analyzer,&r,1100U));
    cerberus_ble_analyzer_snapshot(&analyzer,1100U,&view);
    assert(view.last_report_valid && view.last_ibeacon_valid && view.last_ibeacon.uuid[0]==2U);
    uint8_t duplicates[61];memcpy(duplicates,a,sizeof(a));memcpy(duplicates+27U,a,sizeof(a));
    const uint8_t hc[]={6,9,'H','C','-','0','5'};memcpy(duplicates+54U,hc,sizeof(hc));
    r=report(duplicates,sizeof(duplicates));
    assert(cerberus_ble_analyzer_observe(&analyzer,&r,1110U));
    cerberus_ble_analyzer_snapshot(&analyzer,1110U,&view);
    assert(view.last_ibeacon_valid && view.last_ibeacon.uuid[0]==1U);
    uint32_t matches=view.ibeacon_format_events;
    memcpy(duplicates+27U,b,sizeof(b));
    assert(cerberus_ble_analyzer_observe(&analyzer,&r,1120U));
    cerberus_ble_analyzer_snapshot(&analyzer,1120U,&view);
    assert(view.last_report_valid && !view.last_ibeacon_valid && view.ibeacon_format_events==matches);
    assert(!memcmp(&view.last_ibeacon,&zero,sizeof(zero)) && view.hc_name_hint_events==2U &&
        cerberus_ble_meta_evidence_blocked(&view.last_meta));
    assert(view.evidence_kind==CERBERUS_BLE_ANALYZER_EVIDENCE_HC_NAME_HINT);
    /* Independent payload publication does not override Watch's existing
     * equal-confidence tie priority. */
    uint8_t watch_and_beacon[31];memcpy(watch_and_beacon,a,sizeof(a));
    const uint8_t axon[]={3,3,0x81,0xfc};memcpy(watch_and_beacon+27U,axon,sizeof(axon));
    r=report(watch_and_beacon,sizeof(watch_and_beacon));
    assert(cerberus_ble_analyzer_observe(&analyzer,&r,1130U));
    cerberus_ble_analyzer_snapshot(&analyzer,1130U,&view);
    assert(view.last_ibeacon_valid && view.evidence_kind==CERBERUS_BLE_ANALYZER_EVIDENCE_WATCH);
}

static void check_latest_flock(void)
{
    cerberus_ble_analyzer_t analyzer;
    cerberus_ble_analyzer_view_t view;
    const uint8_t ordinary[]={2,1,6},company[]={3,0xff,0xc8,9};
    const uint8_t fs[]={15,9,'F','S',' ','E','x','t',' ','B','a','t','t','e','r','y'};
    const uint8_t payload[]={4,0xff,0xc8,9,0x42},meta[]={3,0x16,0x5f,0xfd};
    const uint8_t hc[]={6,9,'H','C','-','0','5'};
    const uint8_t raven_a[]={3,3,0,0x31},raven_b[]={3,0x16,0,0x32};
    const uint8_t malformed[]={3,9,'x'};
    uint8_t corroborated[sizeof(fs)+sizeof(payload)];
    memcpy(corroborated,fs,sizeof(fs));memcpy(corroborated+sizeof(fs),payload,sizeof(payload));
    cerberus_ble_analyzer_begin(&analyzer,1000U);
    cerberus_ble_analyzer_report_t r=report(company,sizeof(company));
    assert(cerberus_ble_analyzer_observe(&analyzer,&r,1010U));
    cerberus_ble_analyzer_snapshot(&analyzer,1010U,&view);
    assert(cerberus_ble_flock_hints_level(&view.last_flock)==CERBERUS_BLE_FLOCK_LEVEL_CLUE &&
        !view.last_flock_oui_clue && view.flock_hint_events==1U &&
        view.evidence_kind==CERBERUS_BLE_ANALYZER_EVIDENCE_FLOCK_HINT && view.evidence_confidence==SENTINEL_WATCH_CONFIDENCE_LOW);
    r=report(fs,sizeof(fs));r.scan_response=true;
    assert(cerberus_ble_analyzer_observe(&analyzer,&r,1020U));
    cerberus_ble_analyzer_snapshot(&analyzer,1020U,&view);
    assert(view.last_scan_response && view.flock_hint_events==2U &&
        cerberus_ble_flock_hints_level(&view.last_flock)==CERBERUS_BLE_FLOCK_LEVEL_SIGNATURE &&
        !(view.last_flock.facts&CERBERUS_BLE_FLOCK_MFG_09C8));
    assert(view.evidence_kind==CERBERUS_BLE_ANALYZER_EVIDENCE_WATCH);
    r=report(raven_a,sizeof(raven_a));
    assert(cerberus_ble_analyzer_observe(&analyzer,&r,1030U));
    r=report(raven_b,sizeof(raven_b));r.scan_response=true;
    assert(cerberus_ble_analyzer_observe(&analyzer,&r,1040U));
    cerberus_ble_analyzer_snapshot(&analyzer,1040U,&view);
    assert(view.flock_hint_events==2U && !cerberus_ble_flock_hints_level(&view.last_flock) &&
        !view.last_flock.raven_3100_contexts && view.last_flock.raven_3200_contexts==CERBERUS_BLE_FLOCK_DATA16);

    /* Specific packet signatures can outrank low Meta/HC fallback; weak
     * address/company clues cannot mask their equal-confidence display. */
    uint8_t long_sig[40]={19U,0x30U};memcpy(long_sig+20U,fs,sizeof(fs));memcpy(long_sig+36U,meta,sizeof(meta));
    r=report(long_sig,sizeof(long_sig));
    assert(cerberus_ble_analyzer_observe(&analyzer,&r,1050U));
    cerberus_ble_analyzer_snapshot(&analyzer,1050U,&view);
    assert(view.evidence_kind==CERBERUS_BLE_ANALYZER_EVIDENCE_FLOCK_HINT &&
        view.evidence_confidence==SENTINEL_WATCH_CONFIDENCE_MEDIUM && cerberus_ble_meta_evidence_allowed(&view.last_meta));
    uint8_t weak_meta[sizeof(company)+sizeof(meta)];memcpy(weak_meta,company,sizeof(company));memcpy(weak_meta+sizeof(company),meta,sizeof(meta));
    r=report(weak_meta,sizeof(weak_meta));
    assert(cerberus_ble_analyzer_observe(&analyzer,&r,1060U));
    cerberus_ble_analyzer_snapshot(&analyzer,1060U,&view);
    assert(view.evidence_kind==CERBERUS_BLE_ANALYZER_EVIDENCE_META_ID_HINT &&
        cerberus_ble_flock_hints_level(&view.last_flock)==CERBERUS_BLE_FLOCK_LEVEL_CLUE);
    r=report(hc,sizeof(hc));memcpy(r.address,(uint8_t[]){1,2,3,0x81,0x8e,0x58},6U);r.address_type=0U;
    assert(cerberus_ble_analyzer_observe(&analyzer,&r,1070U));
    cerberus_ble_analyzer_snapshot(&analyzer,1070U,&view);
    assert(view.last_flock_oui_clue && !cerberus_ble_flock_hints_level(&view.last_flock) &&
        view.evidence_kind==CERBERUS_BLE_ANALYZER_EVIDENCE_HC_NAME_HINT);
    r.advertisement=ordinary;r.advertisement_length=sizeof(ordinary);r.scan_response=true;
    assert(cerberus_ble_analyzer_observe(&analyzer,&r,1080U));
    cerberus_ble_analyzer_snapshot(&analyzer,1080U,&view);
    assert(view.last_flock_oui_clue && view.last_scan_response && !cerberus_ble_flock_hints_level(&view.last_flock) &&
        view.evidence_kind==CERBERUS_BLE_ANALYZER_EVIDENCE_FLOCK_HINT && strstr(view.evidence_reason,"address"));
    uint32_t clue_events=view.flock_hint_events;
    r.address_type=1U;
    assert(cerberus_ble_analyzer_observe(&analyzer,&r,1090U));
    cerberus_ble_analyzer_snapshot(&analyzer,1090U,&view);
    assert(!view.last_flock_oui_clue && view.flock_hint_events==clue_events &&
        !cerberus_ble_flock_hints_level(&view.last_flock));
    const uint8_t watch_flock[31]={3,3,0x80,0x30,8,9,'F','l','i','p','p','e','r',
        17,7,0x6f,0x2e,0x17,0xdf,0x14,0x18,0xe5,0x9f,0xa8,0x46,0x32,0x95,0x38,0xbb,0xcc,0xe8};
    r=report(watch_flock,sizeof(watch_flock));
    assert(cerberus_ble_analyzer_observe(&analyzer,&r,1100U));
    cerberus_ble_analyzer_snapshot(&analyzer,1100U,&view);
    assert(view.evidence_kind==CERBERUS_BLE_ANALYZER_EVIDENCE_WATCH && strstr(view.evidence_label,"Flipper") &&
        cerberus_ble_flock_hints_level(&view.last_flock)==CERBERUS_BLE_FLOCK_LEVEL_SIGNATURE);
    uint8_t beacon_and_flock[48]={26,0xff,0x4c,0,2,0x15};memcpy(beacon_and_flock+27U,corroborated,sizeof(corroborated));
    r=report(beacon_and_flock,sizeof(beacon_and_flock));
    assert(cerberus_ble_analyzer_observe(&analyzer,&r,1110U));
    cerberus_ble_analyzer_snapshot(&analyzer,1110U,&view);
    assert(view.last_ibeacon_valid && view.evidence_kind==CERBERUS_BLE_ANALYZER_EVIDENCE_IBEACON_FORMAT &&
        cerberus_ble_flock_hints_level(&view.last_flock)==CERBERUS_BLE_FLOCK_LEVEL_CORROBORATED);
    uint8_t extended[255]={233U,0x30U};memcpy(extended+234U,corroborated,sizeof(corroborated));
    r=report(extended,sizeof(extended));
    assert(cerberus_ble_analyzer_observe(&analyzer,&r,1120U));
    cerberus_ble_analyzer_snapshot(&analyzer,1120U,&view);
    assert(view.last_report_valid && view.evidence_kind==CERBERUS_BLE_ANALYZER_EVIDENCE_FLOCK_HINT &&
        view.evidence_confidence==SENTINEL_WATCH_CONFIDENCE_HIGH);
    uint8_t oversized[256]={0};cerberus_ble_flock_hints_t zero={0};
    for(unsigned kind=0U;kind<4U;++kind){
        r=report(corroborated,sizeof(corroborated));r.address_type=0U;
        memcpy(r.address,(uint8_t[]){1,2,3,0x81,0x8e,0x58},6U);
        assert(cerberus_ble_analyzer_observe(&analyzer,&r,1130U+kind*2U));
        uint32_t before=analyzer.flock_hint_events;r.scan_response=true;
        if(kind==0U){r.advertisement=malformed;r.advertisement_length=sizeof(malformed);}
        else if(kind==1U)r.complete=false;
        else if(kind==2U)r.advertisement_flags=1U;
        else {r.advertisement=oversized;r.advertisement_length=sizeof(oversized);}
        assert(cerberus_ble_analyzer_observe(&analyzer,&r,1131U+kind*2U));
        cerberus_ble_analyzer_snapshot(&analyzer,1131U+kind*2U,&view);
        assert(!view.last_report_valid && !view.last_flock_oui_clue && view.last_scan_response &&
            !memcmp(&view.last_flock,&zero,sizeof(zero)) && view.flock_hint_events==before);
    }
    analyzer.flock_hint_events=UINT32_MAX;r=report(company,sizeof(company));
    assert(cerberus_ble_analyzer_observe(&analyzer,&r,1200U) && analyzer.flock_hint_events==UINT32_MAX);
}

static unsigned window_checks;
#define WINDOW(x) do { ++window_checks; assert(x); } while (0)
static cerberus_ble_analyzer_window_t window(const cerberus_ble_analyzer_view_t *view)
{
    cerberus_ble_analyzer_window_t result;
    WINDOW(cerberus_ble_analyzer_window(view,&result));return result;
}
static void check_completed_window(void)
{
    cerberus_ble_analyzer_t analyzer;
    cerberus_ble_analyzer_view_t view;
    cerberus_ble_analyzer_report_t event=report(NULL,0);
    event.complete=false; /* Every accepted callback counts, not just classified ADs. */
    cerberus_ble_analyzer_begin(&analyzer,1000);
    cerberus_ble_analyzer_snapshot(&analyzer,1199,&view);
    cerberus_ble_analyzer_window_t stats=window(&view);
    WINDOW(!stats.buckets&&!stats.events&&!stats.peak&&!stats.capped);
    WINDOW(cerberus_ble_analyzer_observe(&analyzer,&event,1199));
    cerberus_ble_analyzer_snapshot(&analyzer,1199,&view);stats=window(&view);
    WINDOW(view.current_events==1&&!stats.buckets&&!stats.events&&!stats.peak);
    cerberus_ble_analyzer_snapshot(&analyzer,1200,&view);stats=window(&view);
    WINDOW(stats.buckets==1&&stats.events==1&&stats.peak==1&&!stats.capped);
    cerberus_ble_analyzer_snapshot(&analyzer,1800,&view);stats=window(&view);
    WINDOW(stats.buckets==4&&stats.events==1&&stats.peak==1); /* silence is in denominator */
    WINDOW(stats.events*10U/stats.buckets==2); /* displayed mean0.2, not integer0 or mean1 */
    cerberus_ble_analyzer_t saved=analyzer;
    cerberus_ble_analyzer_snapshot(&analyzer,1799,&view);stats=window(&view);
    WINDOW(stats.buckets==4&&stats.events==1&&!memcmp(&saved,&analyzer,sizeof(saved)));

    /* More than two rotations, using an independently computed event sequence.
     * Run again across uint32 time rollover; report counts are identical. */
    for(unsigned wrapped=0;wrapped<2;++wrapped){
        uint32_t base=wrapped?UINT32_MAX-350U:20000U;
        cerberus_ble_analyzer_begin(&analyzer,base);
        for(unsigned bucket=0;bucket<150;++bucket){
            unsigned count=(bucket*7U)%11U;
            for(unsigned n=0;n<count;++n)
                WINDOW(cerberus_ble_analyzer_observe(&analyzer,&event,base+bucket*200U+1U));
            cerberus_ble_analyzer_snapshot(&analyzer,base+(bucket+1U)*200U,&view);
            stats=window(&view);unsigned n=bucket<64U?bucket+1U:64U,sum=0,peak=0;
            for(unsigned age=0;age<n;++age){
                unsigned expected=((bucket-age)*7U)%11U;
                WINDOW(view.history[age]==expected);sum+=expected;if(expected>peak)peak=expected;
            }
            WINDOW(stats.buckets==n&&stats.events==sum&&stats.peak==peak&&!stats.capped);
            cerberus_ble_analyzer_view_t partial=view;partial.current_events=UINT16_MAX;
            cerberus_ble_analyzer_window_t unchanged=window(&partial);
            WINDOW(!memcmp(&stats,&unchanged,sizeof(stats))); /* current never scales/statistically dominates */
        }
    }
    /* Exactly64 elapsed buckets retains the current one as the oldest visible
     * bucket. At65 it has aged out. Test a nonzero prior ring cursor as well. */
    for(unsigned cursor=0;cursor<2;++cursor)for(unsigned steps=64;steps<=65;++steps){
        cerberus_ble_analyzer_begin(&analyzer,5000);
        if(cursor)cerberus_ble_analyzer_advance(&analyzer,5200);
        uint32_t start=5000+cursor*200;
        for(unsigned n=0;n<3;++n)WINDOW(cerberus_ble_analyzer_observe(&analyzer,&event,start+1));
        cerberus_ble_analyzer_snapshot(&analyzer,start+steps*200,&view);stats=window(&view);
        WINDOW(stats.buckets==64&&stats.events==(steps==64?3U:0U)&&stats.peak==(steps==64?3U:0U));
        WINDOW(view.history[63]==(steps==64?3U:0U)&&!view.current_events);
    }
    cerberus_ble_analyzer_begin(&analyzer,0);analyzer.current_events=UINT16_MAX-1U;analyzer.total_events=UINT32_MAX-1U;
    for(unsigned n=0;n<3;++n)WINDOW(cerberus_ble_analyzer_observe(&analyzer,&event,1));
    cerberus_ble_analyzer_snapshot(&analyzer,200,&view);stats=window(&view);
    WINDOW(view.total_events==UINT32_MAX&&stats.events==UINT16_MAX&&stats.peak==UINT16_MAX&&stats.capped);
    view.history_count=64;for(unsigned i=0;i<64;++i)view.history[i]=UINT16_MAX;stats=window(&view);
    WINDOW(stats.events==4194240U&&stats.events*10U/stats.buckets==655350U&&stats.capped);
    /* A large valid forward jump removes all old samples in bounded work. */
    cerberus_ble_analyzer_snapshot(&analyzer,INT32_MAX,&view);stats=window(&view);
    WINDOW(stats.buckets==64&&!stats.events&&!stats.peak&&!stats.capped);
    cerberus_ble_analyzer_begin(&analyzer,0);WINDOW(cerberus_ble_analyzer_observe(&analyzer,&event,1));
    cerberus_ble_analyzer_stop(&analyzer);cerberus_ble_analyzer_snapshot(&analyzer,1000,&view);stats=window(&view);
    WINDOW(!stats.buckets&&!stats.events&&view.current_events==1&&!view.active);
    memset(&stats,0xa5,sizeof(stats));view.history_count=65;
    WINDOW(!cerberus_ble_analyzer_window(&view,&stats));
    cerberus_ble_analyzer_window_t zero={0};WINDOW(!memcmp(&stats,&zero,sizeof(stats)));
    memset(&stats,0xa5,sizeof(stats));WINDOW(!cerberus_ble_analyzer_window(NULL,&stats));WINDOW(!memcmp(&stats,&zero,sizeof(stats)));
    WINDOW(!cerberus_ble_analyzer_window(&view,NULL));
    printf("BLE Analyzer completed-window statistics: %u checks passed.\n",window_checks);
}

int main(void)
{
    check_findmy_flipper_events();
    check_family_priority();
    check_completed_window();
    check_latest_hc();
    cerberus_ble_analyzer_t analyzer;
    cerberus_ble_analyzer_view_t view;
    uint8_t empty[] = {0};
    cerberus_ble_analyzer_report_t r = report(empty, sizeof(empty));
    cerberus_ble_analyzer_begin(&analyzer, 1000U);
    assert(cerberus_ble_analyzer_observe(&analyzer, &r, 1001U));
    assert(cerberus_ble_analyzer_observe(&analyzer, &r, 1002U));
    cerberus_ble_analyzer_snapshot(&analyzer, 1199U, &view);
    assert(view.active && view.current_events == 2U && view.history_count == 0U);
    assert(view.total_events == 2U && view.last_fresh && view.last_seen_age_ms == 197U);
    cerberus_ble_analyzer_snapshot(&analyzer, 1200U, &view);
    assert(view.current_events == 0U && view.history_count == 1U && view.history[0] == 2U);
    assert(cerberus_ble_analyzer_observe(&analyzer, &r, 1201U));
    cerberus_ble_analyzer_snapshot(&analyzer, 1400U, &view);
    assert(view.history_count == 2U && view.history[0] == 1U && view.history[1] == 2U);
    cerberus_ble_analyzer_snapshot(&analyzer, 17000U, &view);
    assert(view.history_count == CERBERUS_BLE_ANALYZER_HISTORY);
    assert(view.history[0] == 0U && view.history[63] == 0U);
    assert(!view.last_fresh && view.total_events == 3U);

    uint8_t malformed[] = {3U, 0x09U, 'A'};
    r = report(malformed, sizeof(malformed));
    assert(cerberus_ble_analyzer_observe(&analyzer, &r, 17001U));
    cerberus_ble_analyzer_snapshot(&analyzer, 17001U, &view);
    assert(view.malformed_reports == 1U && view.evidence_kind ==
        CERBERUS_BLE_ANALYZER_EVIDENCE_NONE);

    uint8_t hc_name[] = {6U, 0x09U, 'H', 'C', '-', '0', '5'};
    r = report(hc_name, sizeof(hc_name));
    assert(cerberus_ble_analyzer_observe(&analyzer, &r, 17002U));
    cerberus_ble_analyzer_snapshot(&analyzer, 17002U, &view);
    assert(view.hc_name_hint_events == 1U && view.last_name_length == 5U);
    assert(view.evidence_kind == CERBERUS_BLE_ANALYZER_EVIDENCE_HC_NAME_HINT);
    assert(view.evidence_confidence == SENTINEL_WATCH_CONFIDENCE_LOW);
    assert(strstr(view.evidence_reason, "purpose unknown") != NULL);

    uint8_t meta[] = {3U, 0xFFU, 0x5FU, 0xFDU};
    r = report(meta, sizeof(meta));
    assert(cerberus_ble_analyzer_observe(&analyzer, &r, 17003U));
    cerberus_ble_analyzer_snapshot(&analyzer, 17003U, &view);
    assert(view.meta_hint_events == 1U &&
        view.evidence_kind == CERBERUS_BLE_ANALYZER_EVIDENCE_META_ID_HINT &&
        view.evidence_confidence == SENTINEL_WATCH_CONFIDENCE_LOW);

    uint8_t blocked_meta[] = {3U, 0xFFU, 0x5FU, 0xFDU,
                              3U, 0xFFU, 0x4CU, 0x00U};
    r = report(blocked_meta, sizeof(blocked_meta));
    assert(cerberus_ble_analyzer_observe(&analyzer, &r, 17004U));
    cerberus_ble_analyzer_snapshot(&analyzer, 17004U, &view);
    assert(view.meta_hint_events == 1U);
    assert(view.evidence_kind != CERBERUS_BLE_ANALYZER_EVIDENCE_META_ID_HINT);

    uint8_t ibeacon[27] = {26U, 0xFFU, 0x4CU, 0U, 0x02U, 0x15U};
    r = report(ibeacon, sizeof(ibeacon));
    assert(cerberus_ble_analyzer_observe(&analyzer, &r, 17005U));
    cerberus_ble_analyzer_snapshot(&analyzer, 17005U, &view);
    assert(view.ibeacon_format_events == 1U &&
        view.evidence_kind == CERBERUS_BLE_ANALYZER_EVIDENCE_IBEACON_FORMAT &&
        view.evidence_confidence == SENTINEL_WATCH_CONFIDENCE_MEDIUM);

    uint8_t findmy[31] = {30U, 0xFFU, 0x4CU, 0U, 0x12U, 0x19U};
    r = report(findmy, sizeof(findmy));
    assert(cerberus_ble_analyzer_observe(&analyzer, &r, 17006U));
    cerberus_ble_analyzer_snapshot(&analyzer, 17006U, &view);
    assert(view.watch_match_events >= 1U &&
        view.evidence_kind == CERBERUS_BLE_ANALYZER_EVIDENCE_WATCH &&
        view.evidence_confidence == SENTINEL_WATCH_CONFIDENCE_HIGH);

    r.complete = false;
    assert(cerberus_ble_analyzer_observe(&analyzer, &r, 17007U));
    cerberus_ble_analyzer_snapshot(&analyzer, 17007U, &view);
    assert(view.unclassified_reports == 1U &&
        view.evidence_kind == CERBERUS_BLE_ANALYZER_EVIDENCE_NONE);
    r.advertisement = NULL;
    assert(!cerberus_ble_analyzer_observe(&analyzer, &r, 17008U));
    cerberus_ble_analyzer_snapshot(&analyzer, 17008U, &view);
    assert(view.total_events == 10U);

    cerberus_ble_analyzer_begin(&analyzer, UINT32_MAX - 99U);
    r = report(empty, sizeof(empty));
    assert(cerberus_ble_analyzer_observe(&analyzer, &r, UINT32_MAX - 98U));
    cerberus_ble_analyzer_snapshot(&analyzer, 101U, &view);
    assert(view.history_count == 1U && view.history[0] == 1U);
    assert(view.last_seen_age_ms == 200U && view.last_fresh);

    cerberus_ble_analyzer_stop(&analyzer);
    assert(!cerberus_ble_analyzer_observe(&analyzer, &r, 102U));
    cerberus_ble_analyzer_snapshot(&analyzer, 102U, &view);
    assert(!view.active && view.total_events == 1U);
    /* Analyzer and inventory must reject the same malformed/short-name hints. */
    cerberus_ble_analyzer_begin(&analyzer, 20000U);
    uint8_t semantic_bad[] = {3,0xFF,0x5F,0xFD,2,0x03,0x01};
    r = report(semantic_bad, sizeof(semantic_bad));
    assert(cerberus_ble_analyzer_observe(&analyzer, &r, 20001U));
    hc_name[1] = 0x08U;
    r = report(hc_name, sizeof(hc_name));
    assert(cerberus_ble_analyzer_observe(&analyzer, &r, 20002U));
    cerberus_ble_analyzer_snapshot(&analyzer, 20002U, &view);
    assert(view.malformed_reports == 1U && view.meta_hint_events == 0U &&
        view.hc_name_hint_events == 0U);
    check_extended_meta();
    check_latest_ibeacon();
    check_latest_flock();
    return 0;
}
