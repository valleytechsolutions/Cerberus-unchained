/* Included after the raw copier; all pointer access is scanner-mutex owned.
 * Parsing uses callback-local storage outside the mutex; never a saved pointer
 * into this optional allocation. Stop/Release may free it during parsing. */
static inline void response_free_locked(void)
{
    if (!dual_attached && !scan_radio_owned_locked()) {
        free(dual_response); dual_response=NULL;
    }
}

static inline void response_error_locked(void)
{
    /* Keep the bound error/status lease for its caller, but no RF or callback
     * can use this context after a confirmed quiescent ERROR boundary. */
    if (!scan_radio_owned_locked() && scan_status.state==CERBERUS_BLE_SCAN_ERROR) {
        free(dual_response);dual_response=NULL;dual_snapshot.valid=false;
    }
}

static bool response_record_matches(const cerberus_ble_record_t *r, const struct ble_gap_disc_desc *d)
{ return r->address_type==d->addr.type && !memcmp(r->address,d->addr.val,6U); }

static void response_support_original_locked(const struct ble_gap_disc_desc *d,
    bool complete, int64_t callback_us)
{
    if (d->length_data && !d->data) return;
    cerberus_ble_activity_observe(&activity_capture,d->addr.val,d->addr.type,d->data,d->length_data,complete,
        callback_us>=0?(uint32_t)((uint64_t)callback_us/1000U):0U);
    sentinel_watch_protocol_record_t protocol;
    bool matched=complete && d->addr.type<=3U && sentinel_watch_protocol_decode_ble(d->data,d->length_data,&protocol);
    if (complete && callback_us>=0)
        cerberus_watch_capture_observe_timed(&watch_capture,matched?&protocol:NULL,d->addr.val,d->addr.type,d->rssi,0U,(uint64_t)callback_us);
    else cerberus_watch_capture_observe(&watch_capture,NULL,d->addr.val,d->addr.type,d->rssi,0U);
    if (activity_capture.status.identities>scan_status.total_count) scan_status.total_count=activity_capture.status.identities;
}

static void response_none(c5_response_live_t *live,uint64_t sequence,uint64_t original_us)
{
    memset(live,0,sizeof(*live)); live->wire.version=CERBERUS_DUAL_BLE_RESPONSE_VERSION;
    live->wire.state=CERBERUS_DUAL_BLE_RESPONSE_NONE; live->wire.original_sequence=sequence;
    live->original_us=original_us;
}

static bool __attribute__((noinline)) response_receive_event(const struct ble_gap_disc_desc *d,
    uint32_t epoch,int64_t callback_us,const cerberus_dual_ble_receive_metadata_t *receive,
    bool complete,bool response)
{
    uint64_t sequence=0U,session=0U;
    uint8_t family_code=0U;
    xSemaphoreTake(scanner_mutex,portMAX_DELAY);
    const bool active=dual_attached && cerberus_dual_ble_response_mode(dual_mode);
    if (!active || !dual_response || !scan_epoch_current_locked(epoch) ||
        scan_status.state!=CERBERUS_BLE_SCAN_RUNNING) { xSemaphoreGive(scanner_mutex); return active; }
    session=dual_epoch; family_code=dual_family;
    if (!response) {
        /* Clear matching state even when this newer original cannot replace
         * the historical raw capture. No delayed RSP can bind the old ADV. */
        for (uint8_t i=0;i<scan_status.stored_count;++i) if (response_record_matches(&scan_results[i],d)) {
            memset(&dual_response->live[i].wire,0,sizeof(dual_response->live[i].wire));
            dual_response->live[i].response_us=0U;
            if (family_code) scan_inventory.rank[i]=0U;
        }
        for (uint8_t i=0;i<8U;++i) if (dual_response->candidates[i].sequence &&
                response_record_matches(&dual_response->candidates[i].original,d))
            memset(&dual_response->candidates[i],0,sizeof(dual_response->candidates[i]));
        if (d->event_type<=3U) response_support_original_locked(d,complete,callback_us);
    }
    if (d->addr.type>3U || callback_us<0 ||
        (dual_response->clock_valid && (uint64_t)callback_us<dual_response->clock_highwater) ||
        dual_response->sequence==UINT64_MAX) { xSemaphoreGive(scanner_mutex); return true; }
    dual_response->clock_valid=true; dual_response->clock_highwater=(uint64_t)callback_us;
    sequence=++dual_response->sequence;
    xSemaphoreGive(scanner_mutex);
    if (!complete || (!response && d->event_type>3U) || (d->length_data && !d->data)) return true;

    struct ble_hs_adv_fields original_fields={0};
    const bool original_parsed=!response && (!d->length_data ||
        ble_hs_adv_parse_fields(&original_fields,d->data,d->length_data)==0);
    cerberus_dual_ble_family_evidence_t facts;
    const bool valid=cerberus_dual_ble_family_parse_ad(d->data,d->length_data,d->addr.val,d->addr.type,&facts);
    cerberus_dual_ble_receive_metadata_t normalized=*receive;
    normalized.ad_validation=valid?CERBERUS_DUAL_BLE_AD_VALID:CERBERUS_DUAL_BLE_AD_INVALID;
    const uint8_t length=d->length_data<31U?d->length_data:31U;
    const uint8_t raw_flags=(d->length_data>31U?1U:0U)|((response?valid:original_parsed)?0U:2U);
    cerberus_dual_ble_family_wire_t family={0};
    if (response) {
        if (!valid || !cerberus_dual_ble_response_receive_valid(&normalized,length,d->event_type,raw_flags) ||
            !cerberus_dual_ble_response_family_encode(&facts,family_code,&family)) return true;
    } else {
        if (!cerberus_dual_ble_receive_metadata_valid(&normalized,length,d->event_type,raw_flags)) memset(&normalized,0,sizeof(normalized));
        if (normalized.source_kind==CERBERUS_DUAL_BLE_RECEIVE_UNKNOWN)
            cerberus_dual_ble_family_unavailable(&facts,CERBERUS_DUAL_BLE_FAMILY_RECEIVE_UNKNOWN);
        if (family_code) (void)cerberus_dual_ble_family_wire_encode(&facts,family_code,&family);
    }
    cerberus_ble_ad_hints_t parsed;
    bool parsed_ok=valid && cerberus_ble_ad_hints_parse(d->data,d->length_data,&parsed);
    xSemaphoreTake(scanner_mutex,portMAX_DELAY);
    if (!dual_response || !dual_attached || dual_epoch!=session || !scan_epoch_current_locked(epoch) ||
        scan_status.state!=CERBERUS_BLE_SCAN_RUNNING || dual_response->sequence!=sequence) {
        xSemaphoreGive(scanner_mutex); return true;
    }
    int index=find_record(&d->addr);
    if (!response) {
        bool match=!family_code || (facts.available && (facts.categories & (1U<<(family_code-1U))));
        bool eligible=valid && cerberus_dual_ble_response_original_eligible(&normalized,length,d->event_type,raw_flags);
        if (index<0 && !match) {
            if (eligible) {
                uint8_t slot=0U;
                for (uint8_t i=0;i<8U;++i) {
                    if (!dual_response->candidates[i].sequence) { slot=i; break; }
                    if (dual_response->candidates[i].sequence<dual_response->candidates[slot].sequence) slot=i;
                }
                c5_response_candidate_t *c=&dual_response->candidates[slot];
                memset(c,0,sizeof(*c)); c->sequence=sequence; c->original_us=(uint64_t)callback_us; c->family=family;
                memcpy(c->original.address,d->addr.val,6U); c->original.address_type=d->addr.type;
                c->original.rssi=d->rssi; c->original.latest_rssi=d->rssi;
                c->original.manufacturer_id=CERBERUS_BLE_MANUFACTURER_UNKNOWN;
                if (d->event_type==0U || d->event_type==1U) c->original.flags=CERBERUS_BLE_RESULT_CONNECTABLE;
                if (original_parsed) copy_advertisement_fields(&c->original,&original_fields);
                copy_raw_advertisement(&c->original,d,original_parsed); c->original.receive=normalized;
                c->original.original_observed_ms=(uint32_t)((uint64_t)callback_us/1000U); c->original.original_time_valid=true;
            }
            xSemaphoreGive(scanner_mutex); return true;
        }
        int slot=cerberus_ble_inventory_admit(&scan_inventory,scan_status.stored_count,index,valid&&match,callback_us);
        if (slot<0) { if (scan_status.dropped_reports<UINT16_MAX) ++scan_status.dropped_reports; xSemaphoreGive(scanner_mutex); return true; }
        cerberus_ble_record_t *r=&scan_results[slot];
        if (index<0) {
            memset(r,0,sizeof(*r)); memset(&dual_response->live[slot],0,sizeof(dual_response->live[slot]));
            if (slot==scan_status.stored_count) ++scan_status.stored_count;
            memcpy(r->address,d->addr.val,6U);r->address_type=d->addr.type;r->rssi=d->rssi;r->manufacturer_id=CERBERUS_BLE_MANUFACTURER_UNKNOWN;
        } else if (d->rssi>r->rssi) r->rssi=d->rssi;
        if (d->event_type==0U || d->event_type==1U) r->flags|=CERBERUS_BLE_RESULT_CONNECTABLE;
        if (original_parsed) copy_advertisement_fields(r,&original_fields);
        copy_raw_advertisement(r,d,original_parsed); r->receive=normalized;r->latest_rssi=d->rssi;
        r->original_observed_ms=(uint32_t)((uint64_t)callback_us/1000U);r->original_time_valid=true;
        dual_response->live[slot].original_us=(uint64_t)callback_us;
        if (family_code) scan_family[slot]=family;
        if (eligible) response_none(&dual_response->live[slot],sequence,(uint64_t)callback_us);
    } else {
        c5_response_candidate_t *candidate=NULL;
        c5_response_live_t *live=index>=0?&dual_response->live[index]:NULL;
        const cerberus_ble_record_t *original=index>=0?&scan_results[index]:NULL;
        uint64_t original_sequence=live?live->wire.original_sequence:0U,original_us=live?live->original_us:0U;
        if (!original) for (uint8_t i=0;i<8U;++i) if (dual_response->candidates[i].sequence &&
                response_record_matches(&dual_response->candidates[i].original,d)) {
            candidate=&dual_response->candidates[i]; original=&candidate->original;
            original_sequence=candidate->sequence;original_us=candidate->original_us;break;
        }
        if (!original || !original_sequence || sequence<=original_sequence || (uint64_t)callback_us<original_us ||
            (uint64_t)callback_us-original_us>CERBERUS_DUAL_BLE_RESPONSE_INTERVAL_US ||
            !cerberus_dual_ble_response_original_eligible(&original->receive,original->advertisement_length,
                original->advertisement_event_type,original->advertisement_flags) ||
            !cerberus_dual_ble_response_context_matches(&original->receive,&normalized)) {
            xSemaphoreGive(scanner_mutex); return true;
        }
        if (candidate) {
            if (!(facts.categories & (1U<<(family_code-1U)))) { xSemaphoreGive(scanner_mutex); return true; }
            index=cerberus_ble_inventory_admit(&scan_inventory,scan_status.stored_count,-1,true,callback_us);
            if (index<0) { xSemaphoreGive(scanner_mutex); return true; }
            if (index==scan_status.stored_count) ++scan_status.stored_count;
            scan_results[index]=candidate->original;scan_family[index]=candidate->family;
            live=&dual_response->live[index];response_none(live,original_sequence,original_us);
            memset(candidate,0,sizeof(*candidate));
        }
        cerberus_dual_ble_response_wire_t *r=&live->wire;
        memset(r,0,sizeof(*r));r->version=CERBERUS_DUAL_BLE_RESPONSE_VERSION;r->state=CERBERUS_DUAL_BLE_RESPONSE_PRESENT;
        r->original_sequence=original_sequence;r->response_sequence=sequence;r->interval_us=(uint32_t)((uint64_t)callback_us-original_us);
        r->age_ms=UINT32_MAX;r->rssi=d->rssi>=-127&&d->rssi<=20?d->rssi:127;
        if (r->rssi!=127) r->flags|=CERBERUS_DUAL_BLE_RESPONSE_RSSI_VALID;
        r->raw_length=length;r->event_type=d->event_type;r->raw_flags=d->length_data>31U?1U:0U;
        memcpy(r->raw,d->data,length);r->receive=normalized;r->family=family;
        if (parsed_ok && parsed.name_length) {r->name_length=parsed.name_length;memcpy(r->name,parsed.name,parsed.name_length);
            if(parsed.complete_name)r->flags|=CERBERUS_DUAL_BLE_RESPONSE_NAME_COMPLETE;
            if(parsed.name_truncated)r->flags|=CERBERUS_DUAL_BLE_RESPONSE_NAME_TRUNCATED;}
        live->response_us=(uint64_t)callback_us;
        /* A later accepted response may dequalify a response-only family row.
         * Original source evidence is independent and never overwritten. */
        if (family_code && !(scan_family[index].categories & (1U<<(family_code-1U))) &&
            !(facts.categories & (1U<<(family_code-1U)))) scan_inventory.rank[index]=0U;
        else if (family_code && !candidate && (facts.categories & (1U<<(family_code-1U))))
            (void)cerberus_ble_inventory_admit(&scan_inventory,scan_status.stored_count,index,true,callback_us);
    }
    xSemaphoreGive(scanner_mutex);return true;
}

static inline void response_freeze_locked(uint8_t index,int64_t freeze_us)
{
    c5_response_live_t *live=&dual_response->live[index];
    /* Temporarily compute only the frozen age, serialize, then restore live
     * state. The mutex excludes callbacks and no second full-row copy exists. */
    uint8_t flags=live->wire.flags;uint32_t age=live->wire.age_ms;
    if (live->wire.state==CERBERUS_DUAL_BLE_RESPONSE_PRESENT) {
        live->wire.flags&=(uint8_t)~CERBERUS_DUAL_BLE_RESPONSE_AGE_VALID;live->wire.age_ms=UINT32_MAX;
        if (freeze_us>=0 && (uint64_t)freeze_us>=live->response_us &&
            (uint64_t)freeze_us/1000U-live->response_us/1000U<UINT32_MAX) {
            live->wire.age_ms=(uint32_t)((uint64_t)freeze_us/1000U-live->response_us/1000U);
            live->wire.flags|=CERBERUS_DUAL_BLE_RESPONSE_AGE_VALID;
        }
    }
    cerberus_dual_ble_response_encode_bytes(&live->wire,dual_response->frozen[index]);
    live->wire.flags=flags;live->wire.age_ms=age;
}
