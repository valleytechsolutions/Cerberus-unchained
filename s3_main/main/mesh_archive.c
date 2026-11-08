#include "mesh_archive.h"
#include <string.h>

/* Original fieldwise format. Offsets/sizes below are wire values, never native
 * sizeof values. CRC32/ISO-HDLC covers the entire file with bytes24..27 zero. */
static const uint8_t magic[8]={'C','B','M','S','H',13U,10U,26U};
enum { CAPTURE_BYTES=24, SUMMARY_BYTES=128, MODEL_BYTES=84,
       PAN_BYTES=37, ADDRESS_BYTES=70, EDGE_BYTES=49, WITNESS_BYTES=154 };
_Static_assert(MESH_ARCHIVE_MIN_BYTES==MESH_ARCHIVE_HEADER_BYTES+CAPTURE_BYTES+SUMMARY_BYTES+MODEL_BYTES,"archive minimum");
_Static_assert(MESH_ARCHIVE_MAX_BYTES==MESH_ARCHIVE_MIN_BYTES+
    MESH_OBSERVATION_PANS*PAN_BYTES+MESH_OBSERVATION_ADDRESSES*ADDRESS_BYTES+
    MESH_OBSERVATION_EDGES*EDGE_BYTES+MESH_OBSERVATION_WITNESSES*WITNESS_BYTES,"archive maximum");

static bool overlap(const void *a,size_t an,const void *b,size_t bn)
{
    if (!a || !b || !an || !bn) return false;
    uintptr_t x=(uintptr_t)a,y=(uintptr_t)b;
    return x<=y?y-x<an:x-y<bn;
}
static mesh_evidence_view_t view(const mesh_archive_t *d)
{
    return (mesh_evidence_view_t){&d->model,&d->final_summary,d->first_callback_us,
        d->last_callback_us,d->observed_mask,d->callback_time_seen};
}
bool mesh_archive_valid(const mesh_archive_t *d)
{
    if (!d) return false;
    mesh_evidence_view_t v=view(d);
    return mesh_evidence_valid(&v);
}
bool mesh_archive_from_scan(const mesh_scan_t *s,mesh_archive_t *out)
{
    if (!s || !out || overlap(s,sizeof(*s),out,sizeof(*out)) || !mesh_report_valid(s)) return false;
    memset(out,0,sizeof(*out));
    out->model=s->model;out->final_summary=s->final_summary;
    out->first_callback_us=s->first_callback_us;out->last_callback_us=s->last_callback_us;
    out->observed_mask=s->observed_mask;out->callback_time_seen=s->callback_time_seen;
    return true;
}
static size_t wire_size(const mesh_observation_t *m)
{
    return MESH_ARCHIVE_MIN_BYTES+(size_t)m->pan_count*PAN_BYTES+
        (size_t)m->address_count*ADDRESS_BYTES+(size_t)m->edge_count*EDGE_BYTES+
        (size_t)m->witness_count*WITNESS_BYTES;
}
static uint64_t load(const uint8_t *p,unsigned n)
{ uint64_t v=0U;for(unsigned i=0;i<n;++i)v|=(uint64_t)p[i]<<(i*8U);return v; }
static void store(uint8_t *p,uint64_t v,unsigned n)
{ for(unsigned i=0;i<n;++i)p[i]=(uint8_t)(v>>(i*8U)); }
static uint32_t checksum(const uint8_t *p,size_t n)
{
    uint32_t crc=UINT32_MAX;
    for(size_t i=0;i<n;++i){
        crc^=(i>=24U && i<28U)?0U:p[i];
        for(unsigned bit=0;bit<8U;++bit)crc=(crc>>1U)^((crc&1U)?UINT32_C(0xedb88320):0U);
    }
    return ~crc;
}
typedef struct { const uint8_t *input;uint8_t *output;size_t size,offset;bool good; } codec_t;
static uint64_t number(codec_t *c,uint64_t value,unsigned width)
{
    if(!c->good || c->offset>c->size || width>c->size-c->offset){c->good=false;return 0U;}
    if(c->input)value=load(c->input+c->offset,width);
    else store(c->output+c->offset,value,width);
    c->offset+=width;return value;
}
static void reserved(codec_t *c,unsigned count)
{ for(unsigned i=0;i<count;++i)if(number(c,0U,1U))c->good=false; }
static int8_t signed8(uint64_t n)
{ return (int8_t)(n<=INT8_MAX?(int)n:(int)n-256); }
static int32_t signed32(uint64_t n)
{ return n<=INT32_MAX?(int32_t)n:-1-(int32_t)(UINT32_MAX-(uint32_t)n); }
/* These transfers never cast away const: exactly one source/destination is
 * supplied, and scalar conversion is explicit even for signed wire values. */
#define VALUE(field,type,width) do { uint64_t value=number(c,src?(uint64_t)src->field:0U,width); if(dst)dst->field=(type)value; } while(0)
#define BOOLEAN(field) do { uint64_t value=number(c,src?(uint64_t)src->field:0U,1U); if(value>1U)c->good=false; if(dst)dst->field=value==1U; } while(0)
#define SIGNED8(field) do { uint64_t value=number(c,src?(uint64_t)src->field:0U,1U); if(dst)dst->field=signed8(value); } while(0)
#define SIGNED32(field) do { uint64_t value=number(c,src?(uint64_t)src->field:0U,4U); if(dst)dst->field=signed32(value); } while(0)

static void transfer_pan_key(codec_t *c,const mesh_observation_pan_key_t *src,mesh_observation_pan_key_t *dst)
{
    VALUE(value,uint16_t,2U);VALUE(channel,uint8_t,1U);BOOLEAN(known);
}
static void transfer_seen(codec_t *c,const mesh_observation_seen_t *src,mesh_observation_seen_t *dst)
{
    VALUE(first_us,uint64_t,8U);VALUE(last_us,uint64_t,8U);
    VALUE(first_sequence,uint32_t,4U);VALUE(last_sequence,uint32_t,4U);VALUE(frames,uint32_t,4U);
    VALUE(first_witness,uint8_t,1U);VALUE(last_witness,uint8_t,1U);
    BOOLEAN(first_time_valid);BOOLEAN(last_time_valid);
}
static void transfer_summary(codec_t *c,const cerberus_mesh_observation_summary_t *src,
                             cerberus_mesh_observation_summary_t *dst)
{
    VALUE(version,uint8_t,1U);VALUE(flags,uint8_t,1U);VALUE(mode,uint8_t,1U);VALUE(channel,uint8_t,1U);
    VALUE(requested_mask,uint16_t,2U);VALUE(tuned_mask,uint16_t,2U);VALUE(dwell_ms,uint16_t,2U);
    reserved(c,2U);
    VALUE(started_us,uint64_t,8U);VALUE(stopped_us,uint64_t,8U);VALUE(copied_us,uint64_t,8U);
    VALUE(callbacks,uint32_t,4U);VALUE(invalid_callbacks,uint32_t,4U);VALUE(queue_drops,uint32_t,4U);
    VALUE(enqueued,uint32_t,4U);VALUE(acked,uint32_t,4U);VALUE(last_sequence,uint32_t,4U);
    for(unsigned i=0;i<16U;++i){VALUE(completed_dwell_ms[i],uint32_t,4U);}
    SIGNED32(error_code);
}
static void transfer_counters(codec_t *c,const mesh_observation_counters_t *src,mesh_observation_counters_t *dst)
{
    VALUE(records,uint32_t,4U);VALUE(headers,uint32_t,4U);VALUE(malformed,uint32_t,4U);
    VALUE(unsupported,uint32_t,4U);VALUE(invalid_inputs,uint32_t,4U);VALUE(invalid_clock,uint32_t,4U);
    VALUE(wrong_session,uint32_t,4U);VALUE(replayed,uint32_t,4U);VALUE(delivery_sequence_gaps,uint32_t,4U);
    VALUE(pan_omissions,uint32_t,4U);VALUE(address_omissions,uint32_t,4U);
    VALUE(edge_omissions,uint32_t,4U);VALUE(witness_omissions,uint32_t,4U);
    BOOLEAN(saturated);reserved(c,3U);
}
static void transfer_pan(codec_t *c,const mesh_observation_pan_t *src,mesh_observation_pan_t *dst)
{
    transfer_pan_key(c,src?&src->key:NULL,dst?&dst->key:NULL);
    transfer_seen(c,src?&src->seen:NULL,dst?&dst->seen:NULL);VALUE(origin_bits,uint8_t,1U);
}
static void transfer_address(codec_t *c,const mesh_observation_address_t *src,mesh_observation_address_t *dst)
{
    VALUE(key.value,uint64_t,8U);
    transfer_pan_key(c,src?&src->key.pan:NULL,dst?&dst->key.pan:NULL);VALUE(key.mode,uint8_t,1U);
    transfer_seen(c,src?&src->seen:NULL,dst?&dst->seen:NULL);
    VALUE(last_source_us,uint64_t,8U);VALUE(source_frames,uint32_t,4U);
    VALUE(destination_frames,uint32_t,4U);VALUE(last_source_sequence,uint32_t,4U);
    SIGNED8(source_rssi);VALUE(source_lqi,uint8_t,1U);VALUE(source_flags,uint8_t,1U);
    VALUE(origin_bits,uint8_t,1U);VALUE(last_source_witness,uint8_t,1U);
}
static void transfer_edge(codec_t *c,const mesh_observation_edge_t *src,mesh_observation_edge_t *dst)
{
    transfer_seen(c,src?&src->seen:NULL,dst?&dst->seen:NULL);
    VALUE(source,uint8_t,1U);VALUE(destination,uint8_t,1U);VALUE(frame_type_bits,uint8_t,1U);
    VALUE(last_frame_type,uint8_t,1U);VALUE(last_sequence,uint8_t,1U);VALUE(last_signal_flags,uint8_t,1U);
    SIGNED8(last_source_rssi);VALUE(last_source_lqi,uint8_t,1U);
    VALUE(secured_frames,uint32_t,4U);VALUE(encrypted_frames,uint32_t,4U);BOOLEAN(last_sequence_present);
}
static void transfer_witness(codec_t *c,const mesh_observation_witness_t *src,mesh_observation_witness_t *dst)
{
    VALUE(input.session,uint64_t,8U);VALUE(input.callback_us,uint64_t,8U);VALUE(input.sequence,uint32_t,4U);
    VALUE(input.channel,uint8_t,1U);VALUE(input.flags,uint8_t,1U);VALUE(input.length,uint8_t,1U);
    SIGNED8(input.rssi);VALUE(input.lqi,uint8_t,1U);reserved(c,3U);
    for(unsigned i=0;i<MESH_MAC_MAX_LENGTH;++i){VALUE(input.mac[i],uint8_t,1U);}
    VALUE(parse_result,mesh_mac_result_t,1U);
}
static bool transfer(codec_t *c,const mesh_archive_t *src,mesh_archive_t *dst)
{
    VALUE(first_callback_us,uint64_t,8U);VALUE(last_callback_us,uint64_t,8U);
    VALUE(observed_mask,uint16_t,2U);BOOLEAN(callback_time_seen);reserved(c,5U);
    transfer_summary(c,src?&src->final_summary:NULL,dst?&dst->final_summary:NULL);
    VALUE(model.session,uint64_t,8U);VALUE(model.last_valid_us,uint64_t,8U);VALUE(model.last_sequence,uint32_t,4U);
    VALUE(model.pan_count,uint8_t,1U);VALUE(model.address_count,uint8_t,1U);
    VALUE(model.edge_count,uint8_t,1U);VALUE(model.witness_count,uint8_t,1U);BOOLEAN(model.time_seen);reserved(c,3U);
    transfer_counters(c,src?&src->model.counters:NULL,dst?&dst->model.counters:NULL);
    const mesh_observation_t *m=src?&src->model:&dst->model;
    if(!c->good || m->pan_count>MESH_OBSERVATION_PANS || m->address_count>MESH_OBSERVATION_ADDRESSES ||
        m->edge_count>MESH_OBSERVATION_EDGES || m->witness_count>MESH_OBSERVATION_WITNESSES ||
        wire_size(m)!=c->size)return false;
    for(unsigned i=0;i<m->pan_count;++i)transfer_pan(c,src?&src->model.pans[i]:NULL,dst?&dst->model.pans[i]:NULL);
    for(unsigned i=0;i<m->address_count;++i)transfer_address(c,src?&src->model.addresses[i]:NULL,dst?&dst->model.addresses[i]:NULL);
    for(unsigned i=0;i<m->edge_count;++i)transfer_edge(c,src?&src->model.edges[i]:NULL,dst?&dst->model.edges[i]:NULL);
    for(unsigned i=0;i<m->witness_count;++i)transfer_witness(c,src?&src->model.witnesses[i]:NULL,dst?&dst->model.witnesses[i]:NULL);
    return c->good && c->offset==c->size;
}
#undef VALUE
#undef BOOLEAN
#undef SIGNED8
#undef SIGNED32

bool mesh_archive_encode(const mesh_archive_t *d,uint8_t *output,size_t capacity,size_t *length)
{
    if(!length || overlap(d,d?sizeof(*d):0U,length,sizeof(*length)) ||
        overlap(output,capacity,length,sizeof(*length)) || overlap(d,d?sizeof(*d):0U,output,capacity))return false;
    *length=0U;
    if((!output && capacity) || !mesh_archive_valid(d))return false;
    size_t needed=wire_size(&d->model);
    if(!output){*length=needed;return true;}
    if(capacity<needed)return false;
    memcpy(output,magic,sizeof(magic));store(output+8U,MESH_ARCHIVE_VERSION,2U);
    store(output+10U,MESH_ARCHIVE_HEADER_BYTES,2U);store(output+12U,MESH_ARCHIVE_PARSER_VERSION,2U);
    store(output+14U,0U,2U);store(output+16U,needed,4U);store(output+20U,needed-MESH_ARCHIVE_HEADER_BYTES,4U);
    store(output+24U,0U,4U);store(output+28U,0U,4U);
    codec_t c={NULL,output,needed,MESH_ARCHIVE_HEADER_BYTES,true};
    /* Validated bounds determine the exact transfer size before output changes. */
    if(!transfer(&c,d,NULL))return false;
    store(output+24U,checksum(output,needed),4U);*length=needed;return true;
}
bool mesh_archive_decode(const uint8_t *bytes,size_t length,mesh_archive_t *out,mesh_archive_t *workspace)
{
    if(!bytes || !out || !workspace || length<MESH_ARCHIVE_MIN_BYTES || length>MESH_ARCHIVE_MAX_BYTES ||
        overlap(bytes,length,out,sizeof(*out)) || overlap(bytes,length,workspace,sizeof(*workspace)) ||
        overlap(out,sizeof(*out),workspace,sizeof(*workspace)))return false;
    if(memcmp(bytes,magic,sizeof(magic)) || load(bytes+8U,2U)!=MESH_ARCHIVE_VERSION ||
        load(bytes+10U,2U)!=MESH_ARCHIVE_HEADER_BYTES || load(bytes+12U,2U)!=MESH_ARCHIVE_PARSER_VERSION ||
        load(bytes+14U,2U) || load(bytes+16U,4U)!=length ||
        load(bytes+20U,4U)!=length-MESH_ARCHIVE_HEADER_BYTES || load(bytes+28U,4U) ||
        load(bytes+24U,4U)!=checksum(bytes,length))return false;
    memset(workspace,0,sizeof(*workspace));
    codec_t c={bytes,NULL,length,MESH_ARCHIVE_HEADER_BYTES,true};
    if(!transfer(&c,NULL,workspace) || !mesh_archive_valid(workspace))return false;
    *out=*workspace;return true;
}
bool mesh_archive_report_encode(const mesh_archive_t *d,char *output,size_t capacity,size_t *length)
{
    if(!length || overlap(d,d?sizeof(*d):0U,length,sizeof(*length)) ||
        overlap(output,capacity,length,sizeof(*length)) || overlap(d,d?sizeof(*d):0U,output,capacity))return false;
    *length=0U;if(!d)return false;
    mesh_evidence_view_t v=view(d);
    return mesh_evidence_report_encode(&v,output,capacity,length);
}
