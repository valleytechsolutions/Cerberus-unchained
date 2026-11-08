#include "sentinel_scan_session.h"
#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <string.h>

static unsigned local_checks;
#define CHECK_LOCAL(test) do { ++local_checks; assert(test); } while(0)

static sentinel_scan_observation_t observation(sentinel_scan_tool_t tool,
    sentinel_scan_state_t state, bool busy)
{
    return (sentinel_scan_observation_t){.valid = true, .radio_busy = busy,
        .tool = tool, .state = state};
}

static void exact_local_start_ack(void)
{
    /* Definite local rejection has no lease to reconcile. It preserves the
     * retry policy without interpreting the next foreign RUNNING as ours. */
    for(unsigned result=SENTINEL_SCAN_BUSY;result<=SENTINEL_SCAN_FAILED;++result){
        sentinel_scan_session_t local,remote;
        sentinel_scan_session_init(&local);sentinel_scan_session_select(&local,SENTINEL_SCAN_BLE);
        sentinel_scan_command_t start=sentinel_scan_session_step(&local,100U,
            observation(SENTINEL_SCAN_NONE,SENTINEL_SCAN_IDLE,false));remote=local;
        CHECK_LOCAL(sentinel_scan_session_ack_local(&local,start,(sentinel_scan_result_t)result,100U));
        CHECK_LOCAL(sentinel_scan_session_ack(&remote,start,(sentinel_scan_result_t)result,100U));
        CHECK_LOCAL(local.active==SENTINEL_SCAN_NONE&&!local.command_failed&&!local.stop_requested&&
            !local.active_arm_generation&&local.waiting);
        CHECK_LOCAL(local.failure_count==remote.failure_count&&local.faulted==remote.faulted&&
            local.wait_since_ms==remote.wait_since_ms&&local.wait_duration_ms==remote.wait_duration_ms);
        CHECK_LOCAL(sentinel_scan_session_step(&local,200U,
            observation(SENTINEL_SCAN_NONE,SENTINEL_SCAN_RUNNING,true)).kind==SENTINEL_SCAN_COMMAND_NONE);
        CHECK_LOCAL(local.active==SENTINEL_SCAN_NONE&&local.waiting);
        /* Existing transport semantics deliberately retain ambiguity. */
        CHECK_LOCAL(sentinel_scan_session_step(&remote,200U,
            observation(SENTINEL_SCAN_BLE,SENTINEL_SCAN_RUNNING,false)).kind==SENTINEL_SCAN_COMMAND_NONE);
        CHECK_LOCAL(remote.active==SENTINEL_SCAN_BLE&&!remote.command_failed&&!remote.waiting);
        CHECK_LOCAL(sentinel_scan_session_step(&local,1100U,
            observation(SENTINEL_SCAN_NONE,SENTINEL_SCAN_RUNNING,true)).kind==SENTINEL_SCAN_COMMAND_NONE);
        start=sentinel_scan_session_step(&local,1200U,
            observation(SENTINEL_SCAN_NONE,SENTINEL_SCAN_IDLE,false));
        CHECK_LOCAL(start.kind==SENTINEL_SCAN_COMMAND_START);
    }
    /* Accepted local starts and every Stop result are exactly equivalent to
     * existing acknowledgment, including the bounded Stop retry fields. */
    for(unsigned kind=SENTINEL_SCAN_COMMAND_START;kind<=SENTINEL_SCAN_COMMAND_STOP;++kind)
        for(unsigned result=SENTINEL_SCAN_ACCEPTED;result<=SENTINEL_SCAN_FAILED;++result){
            sentinel_scan_session_t local,remote;
            sentinel_scan_session_init(&local);sentinel_scan_session_select(&local,SENTINEL_SCAN_BLE);
            sentinel_scan_command_t command=sentinel_scan_session_step(&local,100U,
                observation(SENTINEL_SCAN_NONE,SENTINEL_SCAN_IDLE,false));
            if(kind==SENTINEL_SCAN_COMMAND_STOP){
                CHECK_LOCAL(sentinel_scan_session_ack_local(&local,command,SENTINEL_SCAN_ACCEPTED,100U));
                sentinel_scan_session_pause(&local,true);
                command=sentinel_scan_session_step(&local,200U,
                    observation(SENTINEL_SCAN_BLE,SENTINEL_SCAN_RUNNING,false));
                CHECK_LOCAL(command.kind==SENTINEL_SCAN_COMMAND_STOP);
            }
            remote=local;sentinel_scan_session_t before=local;
            sentinel_scan_command_t stale=command;++stale.token;
            CHECK_LOCAL(!sentinel_scan_session_ack_local(&local,stale,(sentinel_scan_result_t)result,200U));
            CHECK_LOCAL(!memcmp(&local,&before,sizeof(local)));
            CHECK_LOCAL(!sentinel_scan_session_ack_local(&local,command,(sentinel_scan_result_t)99,200U));
            CHECK_LOCAL(!memcmp(&local,&before,sizeof(local)));
            CHECK_LOCAL(sentinel_scan_session_ack_local(&local,command,(sentinel_scan_result_t)result,200U));
            CHECK_LOCAL(sentinel_scan_session_ack(&remote,command,(sentinel_scan_result_t)result,200U));
            if(kind==SENTINEL_SCAN_COMMAND_STOP||result==SENTINEL_SCAN_ACCEPTED)
                CHECK_LOCAL(!memcmp(&local,&remote,sizeof(local)));
            before=local;
            CHECK_LOCAL(!sentinel_scan_session_ack_local(&local,command,(sentinel_scan_result_t)result,201U));
            CHECK_LOCAL(!memcmp(&local,&before,sizeof(local)));
        }
    sentinel_scan_session_t session;sentinel_scan_session_init(&session);
    sentinel_scan_session_select(&session,SENTINEL_SCAN_BLE);
    uint32_t now=100U;
    for(unsigned i=0;i<SENTINEL_SCAN_FAILURE_LIMIT;++i){
        sentinel_scan_command_t command=sentinel_scan_session_step(&session,now,
            observation(SENTINEL_SCAN_NONE,SENTINEL_SCAN_IDLE,false));
        CHECK_LOCAL(command.kind==SENTINEL_SCAN_COMMAND_START);
        CHECK_LOCAL(sentinel_scan_session_ack_local(&session,command,SENTINEL_SCAN_FAILED,now));
        CHECK_LOCAL(session.failure_count==i+1U&&session.active==SENTINEL_SCAN_NONE);
        now+=session.wait_duration_ms;
    }
    CHECK_LOCAL(session.faulted&&sentinel_scan_session_step(&session,now,
        observation(SENTINEL_SCAN_NONE,SENTINEL_SCAN_IDLE,false)).kind==SENTINEL_SCAN_COMMAND_NONE);
    sentinel_scan_session_pause(&session,false);
    CHECK_LOCAL(sentinel_scan_session_step(&session,now,
        observation(SENTINEL_SCAN_NONE,SENTINEL_SCAN_IDLE,false)).kind==SENTINEL_SCAN_COMMAND_START);
    printf("PASS: %u exact local rejection/acknowledgment checks; remote ambiguity unchanged\n",local_checks);
}

int main(void)
{
    exact_local_start_ack();
    sentinel_scan_session_t wifi, ble;
    sentinel_scan_session_init(&wifi);
    sentinel_scan_session_init(&ble);
    sentinel_scan_session_select(&wifi, SENTINEL_SCAN_WIFI);
    sentinel_scan_session_select(&ble, SENTINEL_SCAN_BLE);

    sentinel_scan_command_t c5 = sentinel_scan_session_step(&wifi, 0,
        observation(SENTINEL_SCAN_NONE, SENTINEL_SCAN_IDLE, false));
    sentinel_scan_command_t s3 = sentinel_scan_session_step(&ble, 0,
        observation(SENTINEL_SCAN_NONE, SENTINEL_SCAN_IDLE, false));
    assert(c5.kind == SENTINEL_SCAN_COMMAND_START && c5.tool == SENTINEL_SCAN_WIFI);
    assert(s3.kind == SENTINEL_SCAN_COMMAND_START && s3.tool == SENTINEL_SCAN_BLE);
    assert(sentinel_scan_session_ack(&wifi, c5, SENTINEL_SCAN_ACCEPTED, 0));
    assert(sentinel_scan_session_ack(&ble, s3, SENTINEL_SCAN_ACCEPTED, 0));

    /* One radio can finish and rearm while the other keeps listening. */
    assert(sentinel_scan_session_step(&wifi, 8000,
        observation(SENTINEL_SCAN_WIFI, SENTINEL_SCAN_COMPLETE, false)).kind ==
        SENTINEL_SCAN_COMMAND_NONE);
    assert(wifi.completed && wifi.active == SENTINEL_SCAN_NONE);
    assert(sentinel_scan_session_step(&ble, 8000,
        observation(SENTINEL_SCAN_BLE, SENTINEL_SCAN_RUNNING, false)).kind ==
        SENTINEL_SCAN_COMMAND_NONE);
    assert(ble.active == SENTINEL_SCAN_BLE && !ble.completed);
    sentinel_scan_session_pause(&wifi, false); /* Watch consumed C5 generation. */
    c5 = sentinel_scan_session_step(&wifi, 8100,
        observation(SENTINEL_SCAN_NONE, SENTINEL_SCAN_IDLE, false));
    assert(c5.kind == SENTINEL_SCAN_COMMAND_START && c5.tool == SENTINEL_SCAN_WIFI);
    assert(sentinel_scan_session_ack(&wifi, c5, SENTINEL_SCAN_ACCEPTED, 8100));

    /* A C5-local busy owner does not prevent the independent S3 observer. */
    sentinel_scan_session_pause(&ble, true);
    s3 = sentinel_scan_session_step(&ble, 8200,
        observation(SENTINEL_SCAN_BLE, SENTINEL_SCAN_RUNNING, false));
    assert(s3.kind == SENTINEL_SCAN_COMMAND_STOP && s3.tool == SENTINEL_SCAN_BLE);
    assert(sentinel_scan_session_ack(&ble, s3, SENTINEL_SCAN_ACCEPTED, 8200));
    assert(sentinel_scan_session_step(&ble, 8300,
        observation(SENTINEL_SCAN_BLE, SENTINEL_SCAN_IDLE, false)).kind ==
        SENTINEL_SCAN_COMMAND_NONE);
    sentinel_scan_session_pause(&ble, false);
    s3 = sentinel_scan_session_step(&ble, 8400,
        observation(SENTINEL_SCAN_NONE, SENTINEL_SCAN_IDLE, false));
    assert(s3.kind == SENTINEL_SCAN_COMMAND_START && s3.tool == SENTINEL_SCAN_BLE);
    assert(sentinel_scan_session_ack(&ble, s3, SENTINEL_SCAN_ACCEPTED, 8400));

    sentinel_scan_session_t blocked_wifi;
    sentinel_scan_session_init(&blocked_wifi);
    sentinel_scan_session_select(&blocked_wifi, SENTINEL_SCAN_WIFI);
    assert(sentinel_scan_session_step(&blocked_wifi, 8500,
        observation(SENTINEL_SCAN_NONE, SENTINEL_SCAN_IDLE, true)).kind ==
        SENTINEL_SCAN_COMMAND_NONE);
    assert(ble.active == SENTINEL_SCAN_BLE);

    puts("PASS: independent C5 Wi-Fi and S3 BLE start, rearm, stop and busy gating");
    return 0;
}
