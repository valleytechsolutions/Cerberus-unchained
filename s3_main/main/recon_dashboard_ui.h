/* Presentation over the existing UI-owned inventory. No RF/storage/clock work
 * here: caller supplies one full-width age sample and cached source status. */
static const char *watch_dashboard_category(uint8_t category)
{
    static const char *const labels[]={"UNCLASSIFIED","AXON","FLOCK","WELLNESS",
        "FIND MY","TRACKER","CAMERA","META HINT","FLIPPER",
        "MESHTASTIC","SAMSUNG","BEACON","PWNAGOTCHI","DRONE ID","BLE ACTIVITY"};
    return category<sizeof(labels)/sizeof(labels[0])?labels[category]:"UNCLASSIFIED";
}
static bool watch_dashboard_recognized(const cerberus_recon_record_t *r)
{
    /* A presentation predicate over committed classifications, not a detector.
     * Demo scenes never enter this model. Meta requires the separate bound
     * existing-evidence adapter before it can become a committed record. */
    return r->classified && r->category > SENTINEL_WATCH_NONE &&
        r->category < SENTINEL_WATCH_CATEGORY_COUNT;
}
static bool watch_dashboard_before(const cerberus_recon_inventory_t *model, uint8_t a, uint8_t b)
{
    const cerberus_recon_inventory_entry_t *left=&model->entries[a], *right=&model->entries[b];
    const bool left_hint=watch_dashboard_recognized(&left->record), right_hint=watch_dashboard_recognized(&right->record);
    if(left_hint!=right_hint)return left_hint;
    if(left_hint&&left->record.confidence!=right->record.confidence)
        return left->record.confidence>right->record.confidence;
    /* Refreshing an existing identity must not shuffle it between sources.
     * New retained identities sort first within their priority/confidence tier. */
    if(left->first_consumed_us!=right->first_consumed_us)return left->first_consumed_us>right->first_consumed_us;
    return a<b;
}
static void watch_dashboard_prepare(uint64_t now_us)
{
    recon_inventory_ui_store_t *store=recon_inventory_ui;
    watch_view.radar=watch_radar;
    if(!store)return;
    store->paint_valid=false;store->painted_count=0;
    sentinel_watch_dashboard_t *d=&store->dashboard;
    memset(d,0,sizeof(*d));watch_view.dashboard=d;
    uint8_t indexes[CERBERUS_RECON_INVENTORY_CAPACITY];
    size_t total=cerberus_recon_inventory_indexes(&store->model,CERBERUS_RECON_INVENTORY_ALL,indexes,sizeof(indexes));
    for(size_t i=1;i<total;++i){
        uint8_t index=indexes[i];size_t at=i;
        while(at&&watch_dashboard_before(&store->model,index,indexes[at-1])){
            indexes[at]=indexes[at-1];--at;
        }
        indexes[at]=index;
    }
    if((size_t)store->dashboard_page*4>=total)store->dashboard_page=0;
    d->total=(uint8_t)total;d->page=store->dashboard_page;d->pages=(uint8_t)(total?(total+3)/4:1);
    store->painted_total=total;
    /* Read the final32 existing chronological buckets directly. No64-bucket
     * automatic copy, invented samples or changes to model timing. */
    d->buckets=store->model.history_count<32?store->model.history_count:32;
    for(unsigned i=0;i<d->buckets;++i){
        unsigned slot=(store->model.history_head+CERBERUS_RECON_INVENTORY_BUCKETS+1-d->buckets+i)%CERBERUS_RECON_INVENTORY_BUCKETS;
        d->added[i]=store->model.history[slot].added;d->expired[i]=store->model.history[slot].expired;
    }
    for(unsigned i=0;i<4&&(size_t)store->dashboard_page*4+i<total;++i){
        cerberus_recon_inventory_entry_t *e=&store->painted[i];
        *e=store->model.entries[indexes[(size_t)store->dashboard_page*4+i]];
        const cerberus_recon_record_t *r=&e->record;
        sentinel_watch_dashboard_row_t *row=&d->rows[i];
        char name[33],address[18];recon_inventory_name(r,name);recon_inventory_address(r,address);
        snprintf(row->name,sizeof(row->name),"%s %.30s",r->radio==CERBERUS_RECON_RADIO_BLE?"S3 BLE":"C5 WiFi",name[0]?name:"[NO NAME]");
        char signal[12];if(r->rssi_valid)snprintf(signal,sizeof(signal),"%d dBm",r->rssi);
        else snprintf(signal,sizeof(signal),"RSSI ?");
        if(r->radio==CERBERUS_RECON_RADIO_BLE)snprintf(row->identity,sizeof(row->identity),"%.17s T%u %.8s",address,r->address_type,signal);
        else if(r->channel_valid)snprintf(row->identity,sizeof(row->identity),"%.17s CH%u %.8s",address,r->channel,signal);
        else snprintf(row->identity,sizeof(row->identity),"%.17s CH? %.8s",address,signal);
        const char *confidence=r->confidence==SENTINEL_WATCH_CONFIDENCE_HIGH?"HIGH":
            r->confidence==SENTINEL_WATCH_CONFIDENCE_MEDIUM?"MED":"LOW";
        bool recognized=watch_dashboard_recognized(r);
        if(recognized)snprintf(row->hint,sizeof(row->hint),"%s / %s",watch_dashboard_category(r->category),confidence);
        else snprintf(row->hint,sizeof(row->hint),"OTHER OBSERVATION");
        row->scene=recognized?watch_scene((sentinel_watch_category_t)r->category):SENTINEL_WATCH_SCENE_GUARDIAN;
        row->rssi=r->rssi;row->rssi_valid=r->rssi_valid;
        uint64_t age=0;
        if(!cerberus_recon_inventory_entry_age(e,now_us,&age))snprintf(row->age,sizeof(row->age),"AGE UNKNOWN");
        else {
            bool upper=e->age_basis==CERBERUS_RECON_INVENTORY_AGE_SOURCE&&e->age_source_kind==CERBERUS_RECON_SOURCE_C5_AGE_MS;
            uint64_t seconds=age/UINT64_C(1000000)+(upper&&age%UINT64_C(1000000)!=0);
            const char *basis=upper?"<=":e->age_basis==CERBERUS_RECON_INVENTORY_AGE_SWEEP?"COPY ":
                e->age_source_kind==CERBERUS_RECON_SOURCE_S3_RESPONSE_MS32?"RSP ":
                e->age_source_kind==CERBERUS_RECON_SOURCE_S3_ADV_MS32?"ADV ":"RX ";
            if(seconds>999999)snprintf(row->age,sizeof(row->age),"%sLARGE",basis);
            else snprintf(row->age,sizeof(row->age),"%s%lus",basis,(unsigned long)seconds);
        }
        ++store->painted_count;++d->count;
    }
}
static bool watch_dashboard_tap(sentinel_watch_action_t action)
{
    recon_inventory_ui_store_t *store=recon_inventory_ui;
    if(!store)return false;
    if(action==SENTINEL_WATCH_ACTION_PREV||action==SENTINEL_WATCH_ACTION_NEXT){
        if(action==SENTINEL_WATCH_ACTION_PREV&&store->dashboard_page)--store->dashboard_page;
        else if(action==SENTINEL_WATCH_ACTION_NEXT&&((size_t)store->dashboard_page+1)*4<store->painted_total)++store->dashboard_page;
        else return false;
        store->paint_valid=false;return true;
    }
    if(action<SENTINEL_WATCH_ACTION_ROW_0||action>SENTINEL_WATCH_ACTION_ROW_3)return false;
    unsigned row=(unsigned)(action-SENTINEL_WATCH_ACTION_ROW_0);
    if(!store->paint_valid||row>=store->painted_count)return false;
    store->selected=store->painted[row];store->selected_valid=true;store->detail_from_dashboard=true;
    store->paint_valid=false;watch_page=WATCH_INVENTORY_DETAIL;return true;
}
