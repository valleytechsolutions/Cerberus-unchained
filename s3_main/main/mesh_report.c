#include "mesh_report.h"
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static bool overlap(const void *a,size_t an,const void *b,size_t bn)
{
    if (!a || !b || !an || !bn) return false;
    uintptr_t x=(uintptr_t)a,y=(uintptr_t)b;
    return x<=y?y-x<an:x-y<bn;
}
static bool channel_valid(uint8_t channel) { return channel>=11U && channel<=26U; }
static bool time_valid(const mesh_evidence_view_t *s,uint64_t time,bool valid)
{
    const cerberus_mesh_observation_summary_t *f=s->summary;
    return valid ? (!(f->flags&CERBERUS_MESH_OBSERVATION_START_TIME_VALID) || time>=f->started_us) &&
        (!(f->flags&CERBERUS_MESH_OBSERVATION_STOP_TIME_VALID) || time<=f->stopped_us) &&
        (!(f->flags&CERBERUS_MESH_OBSERVATION_COPY_TIME_VALID) || time<=f->copied_us) : time==0U;
}
static bool pan_valid(const mesh_evidence_view_t *s,const mesh_observation_pan_key_t *p)
{
    return channel_valid(p->channel) && (s->summary->tuned_mask&(1U<<(p->channel-11U))) &&
        (p->known || !p->value);
}
static bool same_pan(const mesh_observation_pan_key_t *a,const mesh_observation_pan_key_t *b)
{ return a->value==b->value && a->channel==b->channel && a->known==b->known; }
static bool origins_valid(const mesh_observation_pan_key_t *p,uint8_t bits)
{ return p->known ? bits && !(bits&~6U) : bits==1U; }
static bool same_address(const mesh_observation_address_key_t *a,const mesh_observation_address_key_t *b)
{ return a->mode==b->mode && a->value==b->value && same_pan(&a->pan,&b->pan); }
static bool matches_pan(const mesh_observation_pan_key_t *key,mesh_mac_pan_t pan,uint8_t channel)
{
    return key->channel==channel && key->known==(pan.origin!=MESH_MAC_PAN_UNKNOWN) &&
        key->value==pan.value;
}
static bool matches_address(const mesh_observation_address_key_t *key,mesh_mac_address_t address,
                            mesh_mac_pan_t pan,uint8_t channel)
{ return key->mode==address.mode && key->value==address.value && matches_pan(&key->pan,pan,channel); }
static bool reference(const mesh_observation_t *m,uint8_t index,uint32_t sequence)
{
    if (!sequence || sequence>m->last_sequence) return false;
    return sequence<=m->witness_count ? index==sequence-1U && m->witnesses[index].input.sequence==sequence :
        index==MESH_OBSERVATION_NO_WITNESS;
}
static bool seen_valid(const mesh_evidence_view_t *s,const mesh_observation_seen_t *seen)
{
    const mesh_observation_t *m=s->model;
    if (!seen->frames || seen->frames>m->counters.records || !seen->first_sequence ||
        seen->first_sequence>seen->last_sequence || seen->last_sequence>m->last_sequence ||
        seen->frames>seen->last_sequence-seen->first_sequence+1U ||
        !reference(m,seen->first_witness,seen->first_sequence) ||
        !reference(m,seen->last_witness,seen->last_sequence) ||
        !time_valid(s,seen->first_us,seen->first_time_valid) ||
        !time_valid(s,seen->last_us,seen->last_time_valid) ||
        (seen->first_time_valid && seen->last_time_valid && seen->first_us>seen->last_us)) return false;
    const uint8_t refs[]={seen->first_witness,seen->last_witness};
    const uint64_t times[]={seen->first_us,seen->last_us};
    const bool valid[]={seen->first_time_valid,seen->last_time_valid};
    for (unsigned i=0;i<2U;++i) if (refs[i]!=MESH_OBSERVATION_NO_WITNESS) {
        const mesh_observation_input_t *in=&m->witnesses[refs[i]].input;
        if (in->callback_us!=times[i] || ((in->flags&MESH_OBSERVATION_TIME_VALID)!=0U)!=valid[i]) return false;
    }
    return true;
}
static bool parsed_witness(const mesh_observation_t *m,uint8_t index,mesh_mac_observation_t *mac)
{
    if (index>=m->witness_count) return false;
    const mesh_observation_witness_t *w=&m->witnesses[index];
    return w->parse_result==MESH_MAC_HEADER_OK &&
        mesh_mac_parse(w->input.mac,w->input.length,mac)==MESH_MAC_HEADER_OK;
}
static bool witness_valid(const mesh_evidence_view_t *s,unsigned index,uint64_t *last,bool *timed)
{
    const mesh_observation_witness_t *w=&s->model->witnesses[index];
    const mesh_observation_input_t *in=&w->input;
    if (in->session!=s->model->session || in->sequence!=index+1U ||
        !channel_valid(in->channel) || !(s->observed_mask&(1U<<(in->channel-11U))) ||
        in->length<2U || in->length>MESH_MAC_MAX_LENGTH || (in->flags&~7U) ||
        !time_valid(s,in->callback_us,(in->flags&MESH_OBSERVATION_TIME_VALID)!=0U) ||
        (!(in->flags&MESH_OBSERVATION_RSSI_VALID) && in->rssi) ||
        (!(in->flags&MESH_OBSERVATION_LQI_VALID) && in->lqi)) return false;
    for (size_t i=0;i<sizeof(in->reserved);++i) if (in->reserved[i]) return false;
    for (size_t i=in->length;i<sizeof(in->mac);++i) if (in->mac[i]) return false;
    if (in->flags&MESH_OBSERVATION_TIME_VALID) {
        if (*timed && in->callback_us<*last) return false;
        *timed=true;*last=in->callback_us;
    }
    mesh_mac_observation_t mac;
    return w->parse_result==mesh_mac_parse(in->mac,in->length,&mac);
}

static bool summary_valid(const mesh_evidence_view_t *s)
{
    const cerberus_mesh_observation_summary_t *f=s->summary;
    if (!s->model->session || !f->requested_mask ||
        f->mode>CERBERUS_MESH_OBSERVATION_SWEEP || f->dwell_ms<100U || f->dwell_ms>2000U ||
        (f->mode==CERBERUS_MESH_OBSERVATION_FIXED &&
         (f->requested_mask&(uint16_t)(f->requested_mask-1U)))) return false;
    uint64_t total=(uint64_t)f->invalid_callbacks+f->queue_drops+f->enqueued;
    bool saturated=f->callbacks==UINT32_MAX || f->invalid_callbacks==UINT32_MAX ||
        f->queue_drops==UINT32_MAX || f->enqueued==UINT32_MAX || f->acked==UINT32_MAX;
    for (unsigned i=0;i<16U;++i) {
        if (f->completed_dwell_ms[i] && !(f->tuned_mask&(1U<<i))) return false;
        saturated|=f->completed_dwell_ms[i]==UINT32_MAX;
    }
    bool start=(f->flags&CERBERUS_MESH_OBSERVATION_START_TIME_VALID)!=0U;
    bool stop=(f->flags&CERBERUS_MESH_OBSERVATION_STOP_TIME_VALID)!=0U;
    bool copy=(f->flags&CERBERUS_MESH_OBSERVATION_COPY_TIME_VALID)!=0U;
    return f->version==1U && !f->reserved && !(f->flags&~0x7fU) && !f->channel &&
        (f->flags&CERBERUS_MESH_OBSERVATION_STOPPED) &&
        !(f->tuned_mask&(uint16_t)~f->requested_mask) && !(s->observed_mask&(uint16_t)~f->tuned_mask) &&
        (!f->callbacks || f->tuned_mask) && f->last_sequence<=MESH_OBSERVATION_MAX_SEQUENCE &&
        f->enqueued==f->acked && f->acked==f->last_sequence &&
        f->acked==s->model->last_sequence &&
        f->acked==s->model->counters.records &&
        f->callbacks==(total>UINT32_MAX?UINT32_MAX:(uint32_t)total) &&
        saturated==((f->flags&CERBERUS_MESH_OBSERVATION_COUNTERS_SATURATED)!=0U) &&
        (f->last_sequence!=MESH_OBSERVATION_MAX_SEQUENCE || (f->flags&CERBERUS_MESH_OBSERVATION_SEQUENCE_EXHAUSTED)) &&
        (start || !f->started_us) && (stop || !f->stopped_us) && (copy || !f->copied_us) &&
        (!start || !stop || f->started_us<=f->stopped_us) &&
        (!start || !copy || f->started_us<=f->copied_us) && (!stop || !copy || f->stopped_us<=f->copied_us);
}

bool mesh_evidence_valid(const mesh_evidence_view_t *s)
{
    if (!s || !s->model || !s->summary || !summary_valid(s)) return false;
    const mesh_observation_t *m=s->model;
    const mesh_observation_counters_t *c=&m->counters;
    if (m->pan_count>MESH_OBSERVATION_PANS || m->address_count>MESH_OBSERVATION_ADDRESSES ||
        m->edge_count>MESH_OBSERVATION_EDGES || m->witness_count>MESH_OBSERVATION_WITNESSES ||
        (uint64_t)c->headers+c->malformed+c->unsupported!=c->records || c->invalid_inputs ||
        c->invalid_clock || c->wrong_session || c->replayed || c->delivery_sequence_gaps ||
        m->witness_count!=(c->records<64U?c->records:64U) || c->witness_omissions!=c->records-m->witness_count ||
        (c->pan_omissions && m->pan_count!=MESH_OBSERVATION_PANS) ||
        (c->address_omissions && m->address_count!=MESH_OBSERVATION_ADDRESSES) ||
        m->pan_count>(uint64_t)c->headers*2U || m->address_count>(uint64_t)c->headers*2U ||
        m->edge_count>c->headers || m->time_seen!=s->callback_time_seen ||
        !time_valid(s,m->last_valid_us,m->time_seen) || m->last_valid_us!=s->last_callback_us ||
        !time_valid(s,s->first_callback_us,s->callback_time_seen) ||
        (s->callback_time_seen && s->first_callback_us>s->last_callback_us) ||
        (!c->records && s->observed_mask)) return false;
    bool saturated=c->pan_omissions==UINT32_MAX || c->address_omissions==UINT32_MAX ||
        c->edge_omissions==UINT32_MAX || c->witness_omissions==UINT32_MAX;
    if (c->saturated!=saturated) return false;
    uint64_t first=0U,last=0U;bool timed=false;
    unsigned parsed_counts[3]={0};
    for (unsigned i=0;i<m->witness_count;++i) {
        bool previously_timed=timed;
        if (!witness_valid(s,i,&last,&timed)) return false;
        if (!previously_timed && timed) first=last;
        mesh_mac_result_t result=m->witnesses[i].parse_result;
        if (result>MESH_MAC_UNSUPPORTED) return false;
        ++parsed_counts[result];
    }
    if (parsed_counts[MESH_MAC_HEADER_OK]>c->headers || parsed_counts[MESH_MAC_MALFORMED]>c->malformed ||
        parsed_counts[MESH_MAC_UNSUPPORTED]>c->unsupported ||
        (timed && (!m->time_seen || first!=s->first_callback_us || last>m->last_valid_us)) ||
        (c->records<=MESH_OBSERVATION_WITNESSES &&
         (s->callback_time_seen!=timed || s->last_callback_us!=(timed?last:0U)))) return false;
    for (unsigned i=0;i<m->pan_count;++i) {
        const mesh_observation_pan_t *p=&m->pans[i];
        if (!pan_valid(s,&p->key) || !origins_valid(&p->key,p->origin_bits) || !seen_valid(s,&p->seen)) return false;
        for (unsigned j=0;j<i;++j) if (same_pan(&p->key,&m->pans[j].key)) return false;
        const uint8_t refs[]={p->seen.first_witness,p->seen.last_witness};
        for (unsigned j=0;j<2U;++j) if (refs[j]!=MESH_OBSERVATION_NO_WITNESS) {
            mesh_mac_observation_t mac;
            if (!parsed_witness(m,refs[j],&mac)) return false;
            uint8_t channel=m->witnesses[refs[j]].input.channel;
            if (!matches_pan(&p->key,mac.source_pan,channel) && !matches_pan(&p->key,mac.destination_pan,channel)) return false;
        }
    }
    for (unsigned i=0;i<m->address_count;++i) {
        const mesh_observation_address_t *a=&m->addresses[i];
        if (!pan_valid(s,&a->key.pan) || !origins_valid(&a->key.pan,a->origin_bits) ||
            (a->key.mode!=MESH_MAC_ADDRESS_EXT64 && a->key.mode!=MESH_MAC_ADDRESS_SHORT16) ||
            (a->key.mode==MESH_MAC_ADDRESS_SHORT16 && a->key.value>=0xfffeU) || !seen_valid(s,&a->seen) ||
            a->source_frames>a->seen.frames || a->destination_frames>a->seen.frames ||
            (uint64_t)a->source_frames+a->destination_frames<a->seen.frames) return false;
        for (unsigned j=0;j<i;++j) if (same_address(&a->key,&m->addresses[j].key)) return false;
        if (!a->source_frames) {
            if (a->last_source_us || a->last_source_sequence || a->source_rssi || a->source_lqi || a->source_flags ||
                a->last_source_witness!=MESH_OBSERVATION_NO_WITNESS) return false;
        } else {
            if (a->last_source_sequence<a->seen.first_sequence || a->last_source_sequence>a->seen.last_sequence ||
                !reference(m,a->last_source_witness,a->last_source_sequence) || (a->source_flags&~7U) ||
                !time_valid(s,a->last_source_us,(a->source_flags&MESH_OBSERVATION_TIME_VALID)!=0U) ||
                (!(a->source_flags&MESH_OBSERVATION_RSSI_VALID) && a->source_rssi) ||
                (!(a->source_flags&MESH_OBSERVATION_LQI_VALID) && a->source_lqi)) return false;
            if (a->last_source_witness!=MESH_OBSERVATION_NO_WITNESS) {
                const mesh_observation_input_t *in=&m->witnesses[a->last_source_witness].input;
                mesh_mac_observation_t mac;
                if (!parsed_witness(m,a->last_source_witness,&mac) ||
                    !matches_address(&a->key,mac.source,mac.source_pan,in->channel) ||
                    in->callback_us!=a->last_source_us || in->flags!=a->source_flags ||
                    in->rssi!=a->source_rssi || in->lqi!=a->source_lqi) return false;
            }
        }
        const uint8_t refs[]={a->seen.first_witness,a->seen.last_witness};
        for (unsigned j=0;j<2U;++j) if (refs[j]!=MESH_OBSERVATION_NO_WITNESS) {
            mesh_mac_observation_t mac;
            if (!parsed_witness(m,refs[j],&mac)) return false;
            uint8_t channel=m->witnesses[refs[j]].input.channel;
            if (!matches_address(&a->key,mac.source,mac.source_pan,channel) &&
                !matches_address(&a->key,mac.destination,mac.destination_pan,channel)) return false;
        }
    }
    for (unsigned i=0;i<m->edge_count;++i) {
        const mesh_observation_edge_t *e=&m->edges[i];
        if (e->source>=m->address_count || e->destination>=m->address_count || !seen_valid(s,&e->seen) ||
            !e->frame_type_bits || (e->frame_type_bits&~0x0bU) || e->last_frame_type>3U ||
            e->last_frame_type==2U || !(e->frame_type_bits&(1U<<e->last_frame_type)) ||
            e->secured_frames>e->seen.frames || e->encrypted_frames>e->secured_frames ||
            (!e->last_sequence_present && e->last_sequence) || (e->last_signal_flags&~7U) ||
            (!(e->last_signal_flags&MESH_OBSERVATION_RSSI_VALID) && e->last_source_rssi) ||
            (!(e->last_signal_flags&MESH_OBSERVATION_LQI_VALID) && e->last_source_lqi)) return false;
        for (unsigned j=0;j<i;++j) if (e->source==m->edges[j].source && e->destination==m->edges[j].destination) return false;
        const uint8_t refs[]={e->seen.first_witness,e->seen.last_witness};
        for (unsigned j=0;j<2U;++j) if (refs[j]!=MESH_OBSERVATION_NO_WITNESS) {
            const mesh_observation_input_t *in=&m->witnesses[refs[j]].input;
            mesh_mac_observation_t mac;
            if (!parsed_witness(m,refs[j],&mac) || mac.frame_type==2U ||
                !matches_address(&m->addresses[e->source].key,mac.source,mac.source_pan,in->channel) ||
                !matches_address(&m->addresses[e->destination].key,mac.destination,mac.destination_pan,in->channel)) return false;
            if (j==1U && (mac.frame_type!=e->last_frame_type || mac.sequence_present!=e->last_sequence_present ||
                mac.sequence!=e->last_sequence || in->flags!=e->last_signal_flags ||
                in->rssi!=e->last_source_rssi || in->lqi!=e->last_source_lqi)) return false;
        }
    }
    return true;
}

typedef struct { char *out;size_t capacity,used;bool good; } writer_t;
static void bytes(writer_t *w,const char *text,size_t count)
{
    if (!w->good || count>MESH_REPORT_MAX_BYTES-w->used) { w->good=false;return; }
    if (w->out) {
        if (w->used>=w->capacity || count>=w->capacity-w->used) { w->good=false;return; }
        memcpy(w->out+w->used,text,count);w->out[w->used+count]='\0';
    }
    w->used+=count;
}
static void append(writer_t *w,const char *format,...)
{
    char fragment[320];
    va_list args;va_start(args,format);int n=vsnprintf(fragment,sizeof(fragment),format,args);va_end(args);
    if (n<0 || (size_t)n>=sizeof(fragment)) { w->good=false;return; }
    bytes(w,fragment,(size_t)n);
}
static void raw_hex(writer_t *w,const uint8_t *data,size_t count)
{
    static const char digits[]="0123456789ABCDEF";
    for (size_t i=0;i<count;++i) { char pair[2]={digits[data[i]>>4U],digits[data[i]&15U]};bytes(w,pair,2U); }
}
static void pan(writer_t *w,const mesh_observation_pan_key_t *p)
{ append(w,"%u,%s,",p->channel,p->known?"KNOWN":"UNKNOWN");if(p->known)append(w,"%04X",p->value); }
static void address(writer_t *w,const mesh_observation_address_key_t *a)
{
    append(w,"%s,",a->mode==MESH_MAC_ADDRESS_SHORT16?"SHORT16":"EXT64");
    if(a->mode==MESH_MAC_ADDRESS_SHORT16)append(w,"%04" PRIX64,a->value);
    else append(w,"%016" PRIX64,a->value);
}
static void seen(writer_t *w,const mesh_observation_seen_t *s)
{
    append(w,",%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%u,%" PRIu64 ",%u,%" PRIu64 ",%u,%u",
        s->frames,s->first_sequence,s->last_sequence,s->first_time_valid,s->first_us,
        s->last_time_valid,s->last_us,s->first_witness,s->last_witness);
}
static bool format(const mesh_evidence_view_t *s,char *output,size_t capacity,size_t *length)
{
    writer_t w={output,capacity,0U,true};
    const mesh_observation_t *m=s->model;const cerberus_mesh_observation_summary_t *f=s->summary;
    append(&w,"# CERBERUS_MESH_OBSERVATIONS_CSV,1\r\n");
    append(&w,"# Historical passive MAC addressing observations; not authenticated devices or topology.\r\n");
    append(&w,"# Destination means addressed endpoint; RSSI/LQI belong only to a received source frame.\r\n");
    append(&w,"# Times are C5 monotonic microseconds; validity flags distinguish unknown from zero.\r\n");
    append(&w,"# Counts are delivered callbacks/records, not all RF packets. Dwell is completed windows only.\r\n");
    append(&w,"# Witness255 means unavailable. Raw witnesses retain first64 records; aggregates continue.\r\n");
    append(&w,"# Origin bits:1 unknown PAN,2 explicit PAN,4 compressed-same PAN. Source flags:1 time,2 RSSI,4 LQI.\r\n");
    append(&w,"[SESSION]\r\nsession,mode,requested_mask,tuned_mask,dwell_ms,flags,source_error\r\n");
    append(&w,"%016" PRIX64 ",%s,%04X,%04X,%u,%u,%" PRId32 "\r\n",m->session,
        f->mode==CERBERUS_MESH_OBSERVATION_FIXED?"FIXED":"SWEEP",f->requested_mask,f->tuned_mask,f->dwell_ms,f->flags,f->error_code);
    append(&w,"started_valid,started_us,stopped_valid,stopped_us,copied_valid,copied_us\r\n");
    append(&w,"%u,%" PRIu64 ",%u,%" PRIu64 ",%u,%" PRIu64 "\r\n",
        !!(f->flags&CERBERUS_MESH_OBSERVATION_START_TIME_VALID),f->started_us,
        !!(f->flags&CERBERUS_MESH_OBSERVATION_STOP_TIME_VALID),f->stopped_us,
        !!(f->flags&CERBERUS_MESH_OBSERVATION_COPY_TIME_VALID),f->copied_us);
    append(&w,"[C5_COUNTERS]\r\ncallbacks,invalid_callbacks,queue_drops,enqueued,acked,last_sequence,saturated,sequence_exhausted\r\n");
    append(&w,"%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%u,%u\r\n",
        f->callbacks,f->invalid_callbacks,f->queue_drops,f->enqueued,f->acked,f->last_sequence,
        !!(f->flags&CERBERUS_MESH_OBSERVATION_COUNTERS_SATURATED),!!(f->flags&CERBERUS_MESH_OBSERVATION_SEQUENCE_EXHAUSTED));
    append(&w,"[CHANNEL_DWELL]\r\nchannel,requested,tuned,completed_dwell_ms\r\n");
    for(unsigned i=0;i<16U;++i)append(&w,"%u,%u,%u,%" PRIu32 "\r\n",i+11U,
        !!(f->requested_mask&(1U<<i)),!!(f->tuned_mask&(1U<<i)),f->completed_dwell_ms[i]);
    append(&w,"[S3_COUNTERS]\r\nrecords,headers,malformed,unsupported,invalid_inputs,invalid_clock,wrong_session,replayed,delivery_sequence_gaps\r\n");
    const mesh_observation_counters_t *c=&m->counters;
    append(&w,"%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32 "\r\n",
        c->records,c->headers,c->malformed,c->unsupported,c->invalid_inputs,c->invalid_clock,c->wrong_session,c->replayed,c->delivery_sequence_gaps);
    append(&w,"pan_omissions,address_omissions,edge_omissions,witness_omissions,saturated\r\n");
    append(&w,"%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%u\r\n",c->pan_omissions,c->address_omissions,c->edge_omissions,c->witness_omissions,c->saturated);
    append(&w,"pan_count,pan_capacity,address_count,address_capacity,edge_count,edge_capacity,witness_count,witness_capacity\r\n");
    append(&w,"%u,%u,%u,%u,%u,%u,%u,%u\r\n",m->pan_count,MESH_OBSERVATION_PANS,m->address_count,MESH_OBSERVATION_ADDRESSES,
        m->edge_count,MESH_OBSERVATION_EDGES,m->witness_count,MESH_OBSERVATION_WITNESSES);
    append(&w,"[PAN_BUCKETS]\r\nindex,channel,pan_state,pan_hex,origin_bits,frames,first_seq,last_seq,first_time_valid,first_us,last_time_valid,last_us,first_witness,last_witness\r\n");
    for(unsigned i=0;i<m->pan_count;++i){append(&w,"%u,",i);pan(&w,&m->pans[i].key);append(&w,",%u",m->pans[i].origin_bits);seen(&w,&m->pans[i].seen);append(&w,"\r\n");}
    append(&w,"[ADDRESSES]\r\nindex,type,address_hex,channel,pan_state,pan_hex,origin_bits,frames,first_seq,last_seq,first_time_valid,first_us,last_time_valid,last_us,first_witness,last_witness\r\n");
    for(unsigned i=0;i<m->address_count;++i){const mesh_observation_address_t *a=&m->addresses[i];
        append(&w,"%u,",i);address(&w,&a->key);append(&w,",");pan(&w,&a->key.pan);append(&w,",%u",a->origin_bits);seen(&w,&a->seen);append(&w,"\r\n");}
    append(&w,"[ADDRESS_SOURCE_SIGNALS]\r\naddress_index,source_frames,destination_frames,last_source_seq,source_flags,last_source_us,source_rssi_dbm,source_lqi,last_source_witness\r\n");
    for(unsigned i=0;i<m->address_count;++i){const mesh_observation_address_t *a=&m->addresses[i];
        append(&w,"%u,%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%u,%" PRIu64 ",%d,%u,%u\r\n",i,a->source_frames,a->destination_frames,
            a->last_source_sequence,a->source_flags,a->last_source_us,a->source_rssi,a->source_lqi,a->last_source_witness);}
    append(&w,"[DIRECTED_MAC_OBSERVATIONS]\r\nindex,source_index,destination_index,type_bits,last_type,sequence_present,sequence,secured_frames,encrypted_frames,source_flags,source_rssi_dbm,source_lqi\r\n");
    for(unsigned i=0;i<m->edge_count;++i){const mesh_observation_edge_t *e=&m->edges[i];
        append(&w,"%u,%u,%u,%u,%u,%u,%u,%" PRIu32 ",%" PRIu32 ",%u,%d,%u\r\n",i,e->source,e->destination,e->frame_type_bits,
            e->last_frame_type,e->last_sequence_present,e->last_sequence,e->secured_frames,e->encrypted_frames,e->last_signal_flags,e->last_source_rssi,e->last_source_lqi);}
    append(&w,"[DIRECTED_OBSERVATION_TIMES]\r\nindex,frames,first_seq,last_seq,first_time_valid,first_us,last_time_valid,last_us,first_witness,last_witness\r\n");
    for(unsigned i=0;i<m->edge_count;++i){append(&w,"%u",i);seen(&w,&m->edges[i].seen);append(&w,"\r\n");}
    append(&w,"[RAW_WITNESSES]\r\nindex,session,transport_sequence,callback_us,source_flags,channel,source_rssi_dbm,source_lqi,parse_result,mac_length,mac_hex\r\n");
    for(unsigned i=0;i<m->witness_count;++i){const mesh_observation_witness_t *r=&m->witnesses[i];const mesh_observation_input_t *in=&r->input;
        const char *parsed=r->parse_result==MESH_MAC_HEADER_OK?"HEADER_OK":r->parse_result==MESH_MAC_MALFORMED?"MALFORMED":"UNSUPPORTED";
        append(&w,"%u,%016" PRIX64 ",%" PRIu32 ",%" PRIu64 ",%u,%u,%d,%u,%s,%u,",i,in->session,in->sequence,in->callback_us,
            in->flags,in->channel,in->rssi,in->lqi,parsed,in->length);
        raw_hex(&w,in->mac,in->length);append(&w,"\r\n");}
    if(!w.good)return false;
    *length=w.used;
    return true;
}

static mesh_evidence_view_t live_view(const mesh_scan_t *s)
{
    return (mesh_evidence_view_t){&s->model,&s->final_summary,s->first_callback_us,
        s->last_callback_us,s->observed_mask,s->callback_time_seen};
}

bool mesh_report_valid(const mesh_scan_t *s)
{
    if (!s) return false;
    const cerberus_mesh_observation_summary_t *f=&s->final_summary;
    const cerberus_mesh_observation_control_t *r=&s->remote;
    if (!s->frozen || !s->final_valid || s->faulted || !mesh_scan_released(s) ||
        s->accepted || s->have_control || s->start_attempted || s->stop_requested || s->close_requested ||
        r->session_id!=s->model.session || r->sequence || r->error_code ||
        r->state || r->flags || r->count || r->capacity || r->channel || r->version ||
        s->sequence || s->part || s->fault_error || s->error!=f->error_code ||
        f->mode!=s->request.mode || f->requested_mask!=s->request.channel_mask ||
        f->dwell_ms!=s->request.dwell_ms || f->acked!=s->acked_sequence || f->acked!=s->last_enqueued)
        return false;
    mesh_evidence_view_t view=live_view(s);
    return mesh_evidence_valid(&view);
}

bool mesh_evidence_report_encode(const mesh_evidence_view_t *s,char *output,size_t capacity,size_t *length)
{
    if (!length || overlap(s,s?sizeof(*s):0U,length,sizeof(*length)) ||
        overlap(output,capacity,length,sizeof(*length)) || overlap(s,s?sizeof(*s):0U,output,capacity) ||
        (s && (overlap(s->model,s->model?sizeof(*s->model):0U,length,sizeof(*length)) ||
        overlap(s->model,s->model?sizeof(*s->model):0U,output,capacity) ||
        overlap(s->summary,s->summary?sizeof(*s->summary):0U,length,sizeof(*length)) ||
        overlap(s->summary,s->summary?sizeof(*s->summary):0U,output,capacity)))) return false;
    *length=0U;
    if ((!output && capacity) || !mesh_evidence_valid(s)) return false;
    size_t needed=0U;
    if (!format(s,NULL,0U,&needed)) return false;
    if (!output) { *length=needed;return true; }
    if (capacity<=needed) return false;
    return format(s,output,capacity,length);
}

bool mesh_report_encode(const mesh_scan_t *s,char *output,size_t capacity,size_t *length)
{
    if (!length || overlap(s,s?sizeof(*s):0U,length,sizeof(*length)) ||
        overlap(output,capacity,length,sizeof(*length)) || overlap(s,s?sizeof(*s):0U,output,capacity)) return false;
    *length=0U;
    if ((!output && capacity) || !mesh_report_valid(s)) return false;
    mesh_evidence_view_t view=live_view(s);
    return mesh_evidence_report_encode(&view,output,capacity,length);
}
