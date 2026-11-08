#pragma once
/* Synthetic frozen capture through the actual production observation model.
 * Caller links mesh_observation.c + mesh_mac.c; no fake validity boundary. */
#include "mesh_scan.h"
#include <string.h>

static inline void mesh_report_fixture_le(uint8_t *p, uint64_t v, size_t n)
{ for (size_t i=0;i<n;++i) p[i]=(uint8_t)(v>>(8U*i)); }

static inline bool mesh_report_test_fill(mesh_scan_t *s, unsigned records)
{
    if (!s || records>100000U) return false;
    memset(s,0,sizeof(*s));
    if (!mesh_observation_init(&s->model,UINT64_C(0x1234567887654321))) return false;
    s->request.mode=CERBERUS_MESH_OBSERVATION_SWEEP;
    s->request.channel_mask=UINT16_MAX;s->request.dwell_ms=100U;
    for (unsigned i=0;i<records;++i) {
        mesh_observation_input_t in={0};
        in.session=s->model.session;in.sequence=i+1U;in.callback_us=1000U+10U*i;
        in.channel=i<64U?11U:(uint8_t)(12U+(i-64U)%15U);
        in.flags=7U;in.rssi=-45;in.lqi=200U;in.length=MESH_MAC_MAX_LENGTH;
        /* 2015 extended/extended with no PANs, ordinary data; distinct full IDs. */
        mesh_report_fixture_le(in.mac,0xec41U,2U);in.mac[2]=(uint8_t)i;
        uint64_t source=i<32U?2U*i+1U:i<64U?2U*(i-32U)+1U:1U;
        uint64_t destination=i<32U?2U*i+2U:i<64U?(2U*(i-32U)+3U)%64U+1U:2U;
        mesh_report_fixture_le(in.mac+3U,UINT64_C(0xfedcba9800000000)+destination,8U);
        mesh_report_fixture_le(in.mac+11U,UINT64_C(0xfedcba9800000000)+source,8U);
        for (unsigned j=19U;j<MESH_MAC_MAX_LENGTH;++j) in.mac[j]=(uint8_t)(j+i);
        if (mesh_observation_ingest(&s->model,&in)!=MESH_OBSERVATION_OK) return false;
        s->observed_mask|=(uint16_t)(1U<<(in.channel-11U));
    }
    s->acked_sequence=s->last_enqueued=records;
    s->callback_time_seen=records!=0U;
    s->first_callback_us=records?1000U:0U;
    s->last_callback_us=records?1000U+10U*(records-1U):0U;
    cerberus_mesh_observation_summary_t *f=&s->final_summary;
    f->version=1U;f->flags=CERBERUS_MESH_OBSERVATION_START_TIME_VALID|
        CERBERUS_MESH_OBSERVATION_STOP_TIME_VALID|CERBERUS_MESH_OBSERVATION_COPY_TIME_VALID|
        CERBERUS_MESH_OBSERVATION_STOPPED;
    f->mode=s->request.mode;f->requested_mask=s->request.channel_mask;f->dwell_ms=s->request.dwell_ms;
    f->tuned_mask=s->observed_mask? s->observed_mask:1U;
    f->started_us=500U;f->stopped_us=2000000U+10U*records;f->copied_us=f->stopped_us+100U;
    f->callbacks=f->enqueued=f->acked=f->last_sequence=records;
    for (unsigned i=0;i<16U;++i) if (f->tuned_mask&(1U<<i)) f->completed_dwell_ms[i]=100U;
    s->remote.session_id=s->model.session; /* Canonical exact RELEASE success. */
    s->frozen=s->final_valid=true;
    return true;
}
