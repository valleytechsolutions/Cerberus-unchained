/* Adversarial checks of the production pure parser/table, included by the
 * actual native owner fixture; no second implementation of the algorithms. */
static void pure_parser_edges(void)
{
    uint8_t bytes[2400]={0}, before[2400];
    wifi_promiscuous_pkt_t p=frame((const uint8_t *)"A",1,1);
    cerberus_probe_sniff_row_t row,zero={0};
    memcpy(bytes,p.payload,27);
    for(unsigned n=0;n<26;++n){
        memset(&row,0xa5,sizeof(row));
        CHECK(cerberus_probe_sniff_decode(bytes,n,1,-127,0,&row)==CERBERUS_PROBE_SNIFF_DECODE_MALFORMED);
        CHECK(!memcmp(&row,&zero,sizeof(row)));
    }
    for(unsigned fc=0;fc<256;++fc){
        bytes[0]=(uint8_t)fc;
        CHECK(cerberus_probe_sniff_decode(bytes,27,1,-127,0,&row)==
            (fc==0x40?CERBERUS_PROBE_SNIFF_DECODE_VALID:CERBERUS_PROBE_SNIFF_DECODE_IGNORE));
    }
    bytes[0]=0x40;
    for(unsigned flags=0;flags<256;++flags){
        bytes[1]=(uint8_t)flags;
        CHECK(cerberus_probe_sniff_decode(bytes,27,177,0,INT64_MAX,&row)==
            (flags&0xc7?CERBERUS_PROBE_SNIFF_DECODE_UNSUPPORTED:CERBERUS_PROBE_SNIFF_DECODE_VALID));
    }
    bytes[1]=0;
    for(unsigned fragment=1;fragment<16;++fragment){
        bytes[22]=(uint8_t)fragment;
        CHECK(cerberus_probe_sniff_decode(bytes,27,1,-40,0,&row)==CERBERUS_PROBE_SNIFF_DECODE_UNSUPPORTED);
    }
    bytes[22]=0;
    for(unsigned channel=0;channel<256;++channel){
        bool known=false;
        for(unsigned i=0;i<CERBERUS_PROBE_SNIFF_CHANNEL_COUNT;++i)
            if(cerberus_probe_sniff_channel_at((uint8_t)i)==channel)known=true;
        CHECK(cerberus_probe_sniff_channel_valid((uint8_t)channel)==known);
        CHECK(cerberus_probe_sniff_decode(bytes,27,(uint8_t)channel,-40,0,&row)==
            (known?CERBERUS_PROBE_SNIFF_DECODE_VALID:CERBERUS_PROBE_SNIFF_DECODE_UNSUPPORTED));
    }
    CHECK(!cerberus_probe_sniff_channel_at(CERBERUS_PROBE_SNIFF_CHANNEL_COUNT));
    CHECK(cerberus_probe_sniff_decode(bytes,27,1,-128,0,&row)==CERBERUS_PROBE_SNIFF_DECODE_UNSUPPORTED);
    CHECK(cerberus_probe_sniff_decode(bytes,27,1,1,0,&row)==CERBERUS_PROBE_SNIFF_DECODE_UNSUPPORTED);
    CHECK(cerberus_probe_sniff_decode(bytes,27,1,-40,UINT64_MAX,&row)==CERBERUS_PROBE_SNIFF_DECODE_UNSUPPORTED);
    memset(bytes+10,0,6);
    CHECK(cerberus_probe_sniff_decode(bytes,27,1,-40,0,&row)==CERBERUS_PROBE_SNIFF_DECODE_MALFORMED);
    memcpy(bytes,p.payload,27);bytes[10]|=1;
    CHECK(cerberus_probe_sniff_decode(bytes,27,1,-40,0,&row)==CERBERUS_PROBE_SNIFF_DECODE_MALFORMED);
    memcpy(bytes,p.payload,27);bytes[24]=1;
    CHECK(cerberus_probe_sniff_decode(bytes,27,1,-40,0,&row)==CERBERUS_PROBE_SNIFF_DECODE_MALFORMED);
    bytes[24]=0;bytes[25]=33;memset(bytes+26,0,33);
    CHECK(cerberus_probe_sniff_decode(bytes,59,1,-40,0,&row)==CERBERUS_PROBE_SNIFF_DECODE_MALFORMED);
    memcpy(bytes,p.payload,27);bytes[27]=0;bytes[28]=0;
    CHECK(cerberus_probe_sniff_decode(bytes,29,1,-40,0,&row)==CERBERUS_PROBE_SNIFF_DECODE_MALFORMED);
    bytes[27]=221;bytes[28]=255;
    CHECK(cerberus_probe_sniff_decode(bytes,29,1,-40,0,&row)==CERBERUS_PROBE_SNIFF_DECODE_MALFORMED);
    CHECK(cerberus_probe_sniff_decode(bytes,28,1,-40,0,&row)==CERBERUS_PROBE_SNIFF_DECODE_MALFORMED);
    /* Every byte of a maximum accepted frame belongs to a complete IE. */
    memcpy(bytes,p.payload,26);bytes[25]=0;
    for(unsigned at=26;at<2304;at+=2){bytes[at]=221;bytes[at+1]=0;}
    CHECK(cerberus_probe_sniff_decode(bytes,2304,1,-40,0,&row)==CERBERUS_PROBE_SNIFF_DECODE_VALID);
    CHECK(row.ssid_length==0&&row.first_callback_us==0&&cerberus_probe_sniff_row_valid(&row));
    bytes[2303]=1;
    CHECK(cerberus_probe_sniff_decode(bytes,2304,1,-40,0,&row)==CERBERUS_PROBE_SNIFF_DECODE_MALFORMED);
    CHECK(!memcmp(&row,&zero,sizeof(row)));
    CHECK(cerberus_probe_sniff_decode(bytes,2305,1,-40,0,&row)==CERBERUS_PROBE_SNIFF_DECODE_UNSUPPORTED);
    memcpy(before,bytes,sizeof(bytes));
    CHECK(cerberus_probe_sniff_decode(bytes,2304,1,-40,0,(void *)(bytes+24))==CERBERUS_PROBE_SNIFF_DECODE_MALFORMED);
    CHECK(!memcmp(before,bytes,sizeof(bytes)));
}

static void pure_table_edges(void)
{
    static cerberus_probe_sniff_table_t t,saved;
    wifi_promiscuous_pkt_t p=frame((const uint8_t *)"X",1,1);
    cerberus_probe_sniff_row_t row;
    cerberus_probe_sniff_projection_t projection;
    cerberus_probe_sniff_summary_t summary;
    cerberus_probe_sniff_table_begin(&t,UINT64_C(0x100000001),0);
    CHECK(cerberus_probe_sniff_decode(p.payload,27,1,-40,0,&row)==CERBERUS_PROBE_SNIFF_DECODE_VALID);
    CHECK(cerberus_probe_sniff_table_commit(&t,t.session_id,&row,0));
    CHECK(cerberus_probe_sniff_table_copy_row(&t,t.session_id,0,0,&projection));
    CHECK(projection.row.flags&CERBERUS_PROBE_SNIFF_ROW_COPY_TIME_VALID);
    CHECK(cerberus_probe_sniff_projection_valid(&projection)&&projection.copied_source_us==0);
    saved=t;
    CHECK(!cerberus_probe_sniff_table_commit(&t,1,&row,0)&&!memcmp(&t,&saved,sizeof(t)));
    cerberus_probe_sniff_table_rejected(&t,1,CERBERUS_PROBE_SNIFF_DECODE_MALFORMED,0);
    cerberus_probe_sniff_table_finish(&t,1,0);CHECK(!memcmp(&t,&saved,sizeof(t)));
    CHECK(!cerberus_probe_sniff_table_copy_summary(&t,t.session_id,0,&t.summary)&&!memcmp(&t,&saved,sizeof(t)));
    row.first_callback_us=row.last_callback_us=10;
    CHECK(cerberus_probe_sniff_table_commit(&t,t.session_id,&row,10));
    row.first_callback_us=row.last_callback_us=9;
    CHECK(!cerberus_probe_sniff_table_commit(&t,t.session_id,&row,9));
    CHECK(t.rows[0].frames==2&&t.rows[0].last_callback_us==10&&t.summary.clock_dropped_frames==1);
    CHECK(cerberus_probe_sniff_table_copy_row(&t,t.session_id,0,9,&projection));
    CHECK(!(projection.row.flags&CERBERUS_PROBE_SNIFF_ROW_COPY_TIME_VALID)&&!projection.copied_source_us);
    CHECK(cerberus_probe_sniff_projection_valid(&projection));
    t.rows[0].frames=t.summary.valid_frames=t.summary.retained_frames=UINT32_MAX-1;
    row.first_callback_us=row.last_callback_us=11;
    CHECK(cerberus_probe_sniff_table_commit(&t,t.session_id,&row,11));
    CHECK(t.rows[0].frames==UINT32_MAX&&(t.rows[0].flags&CERBERUS_PROBE_SNIFF_ROW_SATURATED));
    CHECK(t.summary.flags&CERBERUS_PROBE_SNIFF_COUNTERS_SATURATED);
    CHECK(cerberus_probe_sniff_summary_valid(&t.summary));
    summary=t.summary;summary.flags&=~CERBERUS_PROBE_SNIFF_COUNTERS_SATURATED;
    CHECK(!cerberus_probe_sniff_summary_valid(&summary));
    row.first_callback_us=row.last_callback_us=12;
    CHECK(cerberus_probe_sniff_table_commit(&t,t.session_id,&row,12));
    CHECK(t.rows[0].frames==UINT32_MAX&&t.summary.valid_frames==UINT32_MAX);
    t.summary.revision=UINT32_MAX-1;
    row.first_callback_us=row.last_callback_us=13;
    CHECK(cerberus_probe_sniff_table_commit(&t,t.session_id,&row,13));
    CHECK(!t.accepting&&t.summary.revision==UINT32_MAX&&(t.summary.flags&CERBERUS_PROBE_SNIFF_REVISION_EXHAUSTED));
    saved=t;row.first_callback_us=row.last_callback_us=14;
    CHECK(!cerberus_probe_sniff_table_commit(&t,t.session_id,&row,14)&&!memcmp(&t,&saved,sizeof(t)));
    cerberus_probe_sniff_table_finish(&t,t.session_id,12);
    CHECK((t.summary.flags&CERBERUS_PROBE_SNIFF_STOPPED)&&!(t.summary.flags&CERBERUS_PROBE_SNIFF_STOP_TIME_VALID));
    saved=t;cerberus_probe_sniff_table_finish(&t,t.session_id,20);CHECK(!memcmp(&t,&saved,sizeof(t)));
    CHECK(cerberus_probe_sniff_table_copy_summary(&t,t.session_id,-1,&summary));
    CHECK(!(summary.flags&CERBERUS_PROBE_SNIFF_COPY_TIME_VALID)&&cerberus_probe_sniff_summary_valid(&summary));
    summary.attempted_channels=UINT64_C(1)<<CERBERUS_PROBE_SNIFF_CHANNEL_COUNT;
    CHECK(!cerberus_probe_sniff_summary_valid(&summary));
    cerberus_probe_sniff_table_begin(&t,9,0);summary=t.summary;
    summary.flags|=CERBERUS_PROBE_SNIFF_COUNTERS_SATURATED;
    CHECK(!cerberus_probe_sniff_summary_valid(&summary));
    for(unsigned field=0;field<8;++field){
        summary=t.summary;
        if(field==0)summary.valid_frames=summary.retained_frames=UINT32_MAX,summary.count=1;
        if(field==1)summary.valid_frames=summary.retained_frames=summary.retry_frames=UINT32_MAX,summary.count=1;
        if(field==2)summary.valid_frames=summary.capacity_dropped_frames=UINT32_MAX,summary.retained_frames=32,summary.count=32;
        if(field==3)summary.malformed_frames=UINT32_MAX;
        if(field==4)summary.unsupported_frames=UINT32_MAX;
        if(field==5)summary.clock_dropped_frames=UINT32_MAX;
        if(field==6)summary.channel_failures=UINT32_MAX;
        if(field==7)summary.valid_frames=summary.retained_frames=UINT32_MAX,summary.count=32;
        CHECK(!cerberus_probe_sniff_summary_valid(&summary));
        summary.flags|=CERBERUS_PROBE_SNIFF_COUNTERS_SATURATED;
        CHECK(cerberus_probe_sniff_summary_valid(&summary));
    }
}
