/* Deterministic compact Locate transport beside the existing survey peer.
 * Both use one common epoch and reject overlap. The real controller validates
 * every encoded projection and source witness before the UI sees it. */
#include "mesh_locate.h"
static struct {
    cerberus_mesh_locate_start_t target;
    cerberus_mesh_locate_projection_t current, pinned;
    uint32_t pin;
    unsigned starts, stops, releases, reads, stop_hold, release_hold;
    bool bound, running, lose_start, bad_part, invalid_clock;
} locate_peer;
static void locate_peer_control(cerberus_mesh_locate_control_t *r, uint64_t session)
{
    memset(r,0,sizeof(*r));r->session_id=session;
    if(!locate_peer.bound||session!=locate_peer.target.session_id){r->error_code=ESP_ERR_NOT_FOUND;return;}
    r->sequence=locate_peer.current.match_sequence;r->version=1U;
    r->projection_bytes=(uint8_t)(72U+locate_peer.current.mac_length);
    r->flags=CERBERUS_MESH_LOCATE_BOUND|CERBERUS_MESH_LOCATE_HELD|CERBERUS_MESH_LOCATE_DATA_READY;
    r->state=locate_peer.running?CERBERUS_MESH_LOCATE_RECEIVING:
        locate_peer.stop_hold?CERBERUS_MESH_LOCATE_STOPPING:CERBERUS_MESH_LOCATE_COMPLETE;
    r->channel=locate_peer.running?locate_peer.target.channel:0U;
    if(locate_peer.running)r->flags|=CERBERUS_MESH_LOCATE_RX_ACTIVE;
    if(locate_peer.invalid_clock)r->flags|=CERBERUS_MESH_LOCATE_CLOCK_INVALID;
}
static void locate_peer_sample(int8_t rssi, unsigned delta)
{
    cerberus_mesh_locate_projection_t *p=&locate_peer.current;
    CHECK(locate_peer.bound&&locate_peer.running&&delta);
    p->matches+=delta;p->match_sequence=p->matches;p->callbacks+=delta;
    p->flags=CERBERUS_MESH_LOCATE_COPY_TIME_VALID|CERBERUS_MESH_LOCATE_MATCH_TIME_VALID|
        CERBERUS_MESH_LOCATE_RSSI_VALID|CERBERUS_MESH_LOCATE_LQI_VALID;
    p->last_match_us=(uint64_t)peer.clock+UINT64_C(1000000);p->rssi=rssi;p->lqi=190U;
    memset(p->mac,0,sizeof(p->mac));p->mac[0]=0x41U;p->mac[2]=7U;
    bool extended=p->address_mode==MESH_MAC_ADDRESS_EXT64;
    if(p->pan_known){
        p->mac[1]=extended?0xc8U:0x88U;le(p->mac+3U,p->pan,2U);le(p->mac+5U,0x3344U,2U);
        le(p->mac+7U,p->address,extended?8U:2U);p->mac_length=extended?15U:9U;
    }else{
        p->mac[1]=extended?0xe0U:0xa0U;le(p->mac+3U,p->address,extended?8U:2U);p->mac_length=extended?11U:5U;
    }
}
static esp_err_t locate_peer_exchange(uint8_t command,const void *request,size_t request_size,void *reply,size_t reply_size)
{
    CHECK(reply_size==22U);peer.wire_calls+=(command==CERBERUS_MESH_LOCATE_COMMAND_INFO||
        command==CERBERUS_MESH_LOCATE_COMMAND_START)?2U:1U;
    peer.clock+=peer.step_us;
    memset(reply,0,reply_size);cerberus_mesh_locate_control_t *c=reply;
    if(command==CERBERUS_MESH_LOCATE_COMMAND_INFO){
        CHECK(!request&&!request_size);c->session_id=peer.epoch;c->version=1U;c->projection_bytes=72U;
        if(peer.bound||locate_peer.bound){c->flags=CERBERUS_MESH_LOCATE_HELD;c->state=CERBERUS_MESH_LOCATE_QUEUED;}
        return ESP_OK;
    }
    uint64_t session=0U;CHECK(request&&request_size>=8U);memcpy(&session,request,8U);
    if(command==CERBERUS_MESH_LOCATE_COMMAND_START){
        ++locate_peer.starts;CHECK(request_size==22U&&!peer.bound&&!locate_peer.bound&&session==peer.epoch+1U);
        peer.epoch=session;locate_peer.target=*(const cerberus_mesh_locate_start_t*)request;
        locate_peer.bound=locate_peer.running=true;memset(&locate_peer.current,0,sizeof(locate_peer.current));
        locate_peer.current.version=1U;locate_peer.current.address=locate_peer.target.address;
        locate_peer.current.address_mode=locate_peer.target.address_mode;locate_peer.current.pan=locate_peer.target.pan;
        locate_peer.current.pan_known=locate_peer.target.pan_known;
        locate_peer_control(c,session);
        if(locate_peer.lose_start){locate_peer.lose_start=false;return ESP_ERR_TIMEOUT;}return ESP_OK;
    }
    if(command==CERBERUS_MESH_LOCATE_COMMAND_READ){
        CHECK(request_size==13U);++locate_peer.reads;
        const cerberus_mesh_locate_read_t *q=request;cerberus_mesh_locate_chunk_t *r=reply;
        r->session_id=session;r->part=q->part;
        if(!locate_peer.bound||session!=locate_peer.target.session_id||q->pin!=locate_peer.pin){le(r->data,ESP_ERR_NOT_FOUND,4U);return ESP_OK;}
        r->pin=q->pin;size_t n=72U+locate_peer.pinned.mac_length,offset=q->part*9U;CHECK(offset<n);
        size_t amount=n-offset;if(amount>9U)amount=9U;memcpy(r->data,(const uint8_t*)&locate_peer.pinned+offset,amount);
        if(locate_peer.bad_part)++r->part;return ESP_OK;
    }
    CHECK(request_size==8U);locate_peer_control(c,session);
    if(command==CERBERUS_MESH_LOCATE_COMMAND_RELEASE){
        ++locate_peer.releases;
        if(locate_peer.release_hold){--locate_peer.release_hold;return ESP_OK;}
        locate_peer.bound=locate_peer.running=false;memset(c,0,sizeof(*c));c->session_id=session;return ESP_OK;
    }
    if(!locate_peer.bound||session!=locate_peer.target.session_id)return ESP_OK;
    if(command==CERBERUS_MESH_LOCATE_COMMAND_STOP){
        ++locate_peer.stops;locate_peer.running=false;if(locate_peer.stop_hold)--locate_peer.stop_hold;
        locate_peer_control(c,session);return ESP_OK;
    }
    if(command==CERBERUS_MESH_LOCATE_COMMAND_STATUS)return ESP_OK;
    CHECK(command==CERBERUS_MESH_LOCATE_COMMAND_PEEK);
    locate_peer.pinned=locate_peer.current;locate_peer.pinned.state=c->state;locate_peer.pinned.channel=c->channel;
    locate_peer.pinned.copied_us=(uint64_t)peer.clock+UINT64_C(1000000);
    locate_peer.pinned.flags|=CERBERUS_MESH_LOCATE_COPY_TIME_VALID;
    if(!locate_peer.running&&!locate_peer.stop_hold)locate_peer.pinned.flags|=CERBERUS_MESH_LOCATE_STOPPED;
    if(locate_peer.invalid_clock)locate_peer.pinned.flags|=CERBERUS_MESH_LOCATE_PROJECTION_CLOCK_INVALID;
    locate_peer.pin=locate_peer.pin?locate_peer.pin+1U:UINT32_C(0x80000001);c->sequence=locate_peer.pin;
    return ESP_OK;
}
