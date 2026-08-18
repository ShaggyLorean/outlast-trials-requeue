#include "requeue_engine.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned assertions;

#define CHECK(expression)                                                       \
    do {                                                                        \
        ++assertions;                                                           \
        if (!(expression)) {                                                    \
            fprintf(stderr,                                                     \
                    "%s:%d: CHECK failed: %s\n",                              \
                    __FILE__,                                                   \
                    __LINE__,                                                   \
                    #expression);                                               \
            exit(EXIT_FAILURE);                                                 \
        }                                                                       \
    } while (0)

static rq_event line(rq_engine *engine,
                     const char *type,
                     const char *ticket,
                     uint64_t timestamp,
                     bool initial_scan,
                     uint64_t mono)
{
    char buffer[512];
    int length = snprintf(buffer,
                          sizeof(buffer),
                          "prefix {\"type\":\"%s\",\"context\":"
                          "\"invasion\",\"ticketId\":\"%s\","
                          "\"timestamp\":%" PRIu64 "} suffix",
                          type,
                          ticket,
                          timestamp);
    CHECK(length > 0 && (size_t)length < sizeof(buffer));
    return rq_engine_process_line(
        engine, buffer, initial_scan, mono, UINT64_C(999999));
}

static void test_matching_ticket_and_stale_timeout(void)
{
    rq_engine engine;
    rq_event event;

    rq_engine_init(&engine);
    event = line(&engine, "searching", "ticket-a", 1000, false, 20);
    CHECK(event.type == RQ_EVENT_SEARCHING);
    CHECK(strcmp(event.ticket, "ticket-a") == 0);
    CHECK(event.started_at_ms == 1000 && event.has_started_at);
    CHECK(!event.requeue_confirmed);
    CHECK(engine.has_armed_ticket);

    event = line(&engine, "timed_out", "stale", 6000, false, 30);
    CHECK(event.type == RQ_EVENT_NONE);
    CHECK(engine.has_armed_ticket);
    CHECK(strcmp(engine.armed_ticket, "ticket-a") == 0);
    CHECK(!engine.pending);

    event = line(&engine, "timed_out", "ticket-a", 6500, false, 100);
    CHECK(event.type == RQ_EVENT_TIMEOUT);
    CHECK(event.total_search_ms == 5500);
    CHECK(event.started_at_ms == 1000 && event.has_started_at);
    CHECK(!engine.has_armed_ticket);
    CHECK(engine.cumulative_search_ms == 5500);
    CHECK(engine.awaiting_requeue_search);
    CHECK(engine.pending);
    CHECK(engine.pending_phase == RQ_PENDING_DELAY);
    CHECK(engine.pending_due_mono_ms ==
          100 + RQ_DEFAULT_TIMEOUT_UI_DELAY_MS);
}

static void test_ticket_exact_terminal_and_duplicate_safety(void)
{
    rq_engine engine;
    rq_engine before;
    rq_event event;

    rq_engine_init(&engine);
    (void)line(&engine, "searching", "active", 1000, false, 0);

    before = engine;
    event = line(&engine, "succeeded", "stale", 1500, false, 1);
    CHECK(event.type == RQ_EVENT_NONE);
    CHECK(memcmp(&engine, &before, sizeof(engine)) == 0);

    event = line(&engine, "canceled", "stale", 1600, false, 2);
    CHECK(event.type == RQ_EVENT_NONE);
    CHECK(memcmp(&engine, &before, sizeof(engine)) == 0);

    event = line(&engine, "searching", "active", 9000, false, 3);
    CHECK(event.type == RQ_EVENT_NONE);
    CHECK(memcmp(&engine, &before, sizeof(engine)) == 0);

    event = line(&engine, "timed_out", "active", 4000, false, 100);
    CHECK(event.type == RQ_EVENT_TIMEOUT);
    CHECK(engine.awaiting_requeue_search);
    CHECK(strcmp(engine.awaiting_ticket, "active") == 0);
    CHECK(strcmp(engine.pending_ticket, "active") == 0);
    before = engine;

    /* A replayed search for the timed-out ticket is not a confirmation. */
    event = line(&engine, "searching", "active", 4500, false, 101);
    CHECK(event.type == RQ_EVENT_NONE);
    CHECK(memcmp(&engine, &before, sizeof(engine)) == 0);

    event = line(&engine, "succeeded", "active", 4600, false, 102);
    CHECK(event.type == RQ_EVENT_NONE);
    CHECK(memcmp(&engine, &before, sizeof(engine)) == 0);

    event = line(&engine, "cancelled", "stale", 4700, false, 103);
    CHECK(event.type == RQ_EVENT_NONE);
    CHECK(memcmp(&engine, &before, sizeof(engine)) == 0);

    event = line(&engine, "canceled", "active", 4800, false, 104);
    CHECK(event.type == RQ_EVENT_CANCELED);
    CHECK(event.total_search_ms == 3000);
    CHECK(!engine.has_armed_ticket && !engine.pending);
    CHECK(!engine.awaiting_requeue_search);
    CHECK(engine.awaiting_ticket[0] == '\0');
    CHECK(engine.cumulative_search_ms == 0);
}

static void test_initial_scan_awaiting_ticket_cancel_safety(void)
{
    rq_engine engine;
    rq_engine before;
    rq_event event;

    rq_engine_init(&engine);
    (void)line(&engine, "searching", "scan-a", 1000, true, 0);
    (void)line(&engine, "timed_out", "scan-a", 4000, true, 0);
    CHECK(!engine.pending && engine.awaiting_requeue_search);
    CHECK(strcmp(engine.awaiting_ticket, "scan-a") == 0);
    before = engine;

    event = line(&engine, "canceled", "old", 4500, true, 0);
    CHECK(event.type == RQ_EVENT_NONE);
    CHECK(memcmp(&engine, &before, sizeof(engine)) == 0);

    event = line(&engine, "searching", "scan-a", 4600, true, 0);
    CHECK(event.type == RQ_EVENT_NONE);
    CHECK(memcmp(&engine, &before, sizeof(engine)) == 0);

    event = line(&engine, "cancelled", "scan-a", 4700, true, 0);
    CHECK(event.type == RQ_EVENT_NONE);
    CHECK(!engine.pending && !engine.awaiting_requeue_search);
    CHECK(engine.awaiting_ticket[0] == '\0');
    CHECK(engine.cumulative_search_ms == 0);
}

static void test_cumulative_verified_requeues_and_success(void)
{
    rq_engine engine;
    rq_event event;

    rq_engine_init(&engine);
    (void)line(&engine, "searching", "a", 1000, false, 0);
    (void)line(&engine, "timed_out", "a", 4000, false, 10);

    event = line(&engine, "searching", "b", 5000, false, 20);
    CHECK(event.type == RQ_EVENT_SEARCHING);
    CHECK(event.requeue_confirmed);
    CHECK(event.cumulative_before_ms == 3000);
    CHECK(event.requeue_count == 1);
    CHECK(!engine.pending);
    CHECK(!engine.awaiting_requeue_search);

    event = line(&engine, "timed_out", "b", 9000, false, 30);
    CHECK(event.type == RQ_EVENT_TIMEOUT);
    CHECK(event.total_search_ms == 7000);
    CHECK(engine.requeue_count == 1);

    event = line(&engine, "searching", "c", 10000, false, 40);
    CHECK(event.requeue_confirmed);
    CHECK(event.requeue_count == 2);
    CHECK(event.cumulative_before_ms == 7000);

    event = line(&engine, "succeeded", "c", 12500, false, 50);
    CHECK(event.type == RQ_EVENT_SUCCEEDED);
    CHECK(event.total_search_ms == 9500);
    CHECK(event.requeue_count == 2);
    CHECK(engine.finished);
    CHECK(!engine.has_armed_ticket && !engine.pending);
    CHECK(engine.cumulative_search_ms == 0);
    CHECK(engine.requeue_count == 0);
}

static void test_pending_lifecycle(void)
{
    rq_engine engine;
    rq_event event;

    rq_engine_init(&engine);
    engine.timeout_ui_delay_ms = 80;
    engine.verify_timeout_ms = 200;
    engine.max_requeue_attempts = 0;
    (void)line(&engine, "searching", "a", 100, false, 0);
    (void)line(&engine, "timed_out", "a", 200, false, 1000);

    event = rq_engine_tick(&engine, 1079);
    CHECK(event.type == RQ_EVENT_NONE);
    event = rq_engine_tick(&engine, 1080);
    CHECK(event.type == RQ_EVENT_REQUEUE_DUE);
    CHECK(engine.pending_phase == RQ_PENDING_ACTION_CLAIMED);
    event = rq_engine_tick(&engine, 1090);
    CHECK(event.type == RQ_EVENT_NONE);

    event = rq_engine_mark_requeue_posted(&engine, 1100, 200);
    CHECK(event.type == RQ_EVENT_REQUEUE_POSTED);
    CHECK(engine.pending_phase == RQ_PENDING_VERIFYING);
    CHECK(engine.pending_verify_until_mono_ms == 1300);
    event = rq_engine_tick(&engine, 1299);
    CHECK(event.type == RQ_EVENT_NONE);
    event = rq_engine_tick(&engine, 1300);
    CHECK(event.type == RQ_EVENT_REQUEUE_UNCONFIRMED);
    CHECK(strcmp(event.ticket, "a") == 0);
    CHECK(!engine.pending && !engine.awaiting_requeue_search);

    (void)line(&engine, "searching", "fresh", 500, false, 1400);
    (void)line(&engine, "timed_out", "fresh", 600, false, 1500);
    (void)rq_engine_tick(&engine, 1580);
    event = rq_engine_mark_requeue_failed(&engine);
    CHECK(event.type == RQ_EVENT_HELPER_FAILED);
    CHECK(!engine.pending && !engine.awaiting_requeue_search);
    CHECK(rq_engine_mark_helper_failed(&engine).type == RQ_EVENT_NONE);
}

static void test_stale_due_action_expires_without_becoming_claimable(void)
{
    rq_engine engine;
    rq_event event;

    rq_engine_init(&engine);
    engine.timeout_ui_delay_ms = 80;
    engine.action_max_lateness_ms = 100;
    (void)line(&engine, "searching", "sleep-ticket", 100, false, 0);
    (void)line(&engine, "timed_out", "sleep-ticket", 200, false, 1000);

    CHECK(rq_engine_tick(&engine, 1179).type == RQ_EVENT_REQUEUE_DUE);

    rq_engine_init(&engine);
    engine.timeout_ui_delay_ms = 80;
    engine.action_max_lateness_ms = 100;
    (void)line(&engine, "searching", "sleep-ticket", 100, false, 0);
    (void)line(&engine, "timed_out", "sleep-ticket", 200, false, 1000);
    event = rq_engine_tick(&engine, 1181);
    CHECK(event.type == RQ_EVENT_REQUEUE_EXPIRED);
    CHECK(strcmp(event.ticket, "sleep-ticket") == 0);
    CHECK(!engine.pending);
    CHECK(!engine.awaiting_requeue_search);
    CHECK(engine.pending_phase == RQ_PENDING_NONE);
    CHECK(rq_engine_tick(&engine, 999999).type == RQ_EVENT_NONE);

    rq_engine_init(&engine);
    engine.timeout_ui_delay_ms = 0;
    (void)line(&engine, "searching", "stage-ticket", 300, false, 0);
    (void)line(&engine, "timed_out", "stage-ticket", 400, false, 2000);
    CHECK(rq_engine_tick(&engine, 2000).type == RQ_EVENT_REQUEUE_DUE);
    event = rq_engine_mark_requeue_expired(&engine);
    CHECK(event.type == RQ_EVENT_REQUEUE_EXPIRED);
    CHECK(strcmp(event.ticket, "stage-ticket") == 0);
    CHECK(!engine.pending && !engine.awaiting_requeue_search);
}

static void test_cancel_and_disconnect(void)
{
    rq_engine engine;
    rq_event event;

    rq_engine_init(&engine);
    (void)line(&engine, "searching", "a", 1000, false, 0);
    event = line(&engine, "cancelled", "a", 1750, false, 1);
    CHECK(event.type == RQ_EVENT_CANCELED);
    CHECK(event.total_search_ms == 750);
    CHECK(!engine.has_armed_ticket);
    CHECK(engine.cumulative_search_ms == 0);

    (void)line(&engine, "searching", "b", 2000, false, 2);
    (void)line(&engine, "timed_out", "b", 3000, false, 3);
    (void)line(&engine, "searching", "c", 3100, false, 3);
    event = rq_engine_process_line(
        &engine,
        "UEngine::BroadcastNetworkFailure {\"timestamp\":3600}",
        false,
        4,
        9999);
    CHECK(event.type == RQ_EVENT_DISCONNECTED);
    CHECK(event.total_search_ms == 1500);
    CHECK(event.requeue_count == 1);
    CHECK(!engine.pending && !engine.awaiting_requeue_search);
    CHECK(engine.cumulative_search_ms == 0 && engine.requeue_count == 0);

    event = rq_engine_process_line(&engine,
                                   "Connection failed; returning to Entry",
                                   false,
                                   5,
                                   4000);
    CHECK(event.type == RQ_EVENT_NONE);
}

static void test_initial_scan_recovery(void)
{
    rq_engine engine;
    rq_event event;

    rq_engine_init(&engine);
    CHECK(line(&engine, "searching", "a", 1000, true, 0).type ==
          RQ_EVENT_NONE);
    CHECK(line(&engine, "timed_out", "a", 4000, true, 0).type ==
          RQ_EVENT_NONE);
    CHECK(engine.awaiting_requeue_search && !engine.pending);
    CHECK(line(&engine, "searching", "b", 5000, true, 0).type ==
          RQ_EVENT_NONE);
    CHECK(line(&engine, "timed_out", "old", 7000, true, 0).type ==
          RQ_EVENT_NONE);
    CHECK(engine.has_armed_ticket);
    CHECK(strcmp(engine.armed_ticket, "b") == 0);
    CHECK(engine.cumulative_search_ms == 3000);
    CHECK(engine.requeue_count == 1);

    event = rq_engine_recovery_event(&engine);
    CHECK(event.type == RQ_EVENT_SEARCHING);
    CHECK(event.recovered);
    CHECK(strcmp(event.ticket, "b") == 0);
    CHECK(event.started_at_ms == 5000);
    CHECK(event.cumulative_before_ms == 3000);
    CHECK(event.requeue_count == 1);

    CHECK(line(&engine, "timed_out", "b", 9000, true, 0).type ==
          RQ_EVENT_NONE);
    CHECK(!engine.has_armed_ticket && !engine.pending);
    CHECK(engine.awaiting_requeue_search);
    CHECK(engine.cumulative_search_ms == 7000);
    CHECK(rq_engine_recovery_event(&engine).type == RQ_EVENT_NONE);

    event = line(&engine, "searching", "c", 10000, false, 100);
    CHECK(event.type == RQ_EVENT_SEARCHING);
    CHECK(event.requeue_confirmed);
    CHECK(event.requeue_count == 2);
    CHECK(event.cumulative_before_ms == 7000);
}

static void test_initial_completed_sessions_reset(void)
{
    rq_engine engine;

    rq_engine_init(&engine);
    (void)line(&engine, "searching", "old", 100, true, 0);
    (void)line(&engine, "succeeded", "old", 200, true, 0);
    CHECK(!engine.finished);
    CHECK(!engine.has_armed_ticket);
    CHECK(engine.cumulative_search_ms == 0);
    (void)line(&engine, "searching", "current", 300, true, 0);
    CHECK(engine.has_armed_ticket);
    CHECK(strcmp(engine.armed_ticket, "current") == 0);
    CHECK(engine.requeue_count == 0);
}

static void test_fallback_timestamp_and_non_invasion_noise(void)
{
    rq_engine engine;
    rq_event event;

    rq_engine_init(&engine);
    event = rq_engine_process_line(
        &engine,
        "{\"type\":\"searching\",\"context\":\"invasion\","
        "\"ticketId\":\"fallback\"}",
        false,
        0,
        5000);
    CHECK(event.started_at_ms == 5000);
    event = rq_engine_process_line(
        &engine,
        "{\"type\":\"timed_out\",\"context\":\"invasion\","
        "\"ticketId\":\"fallback\",\"timestamp\":4500}",
        false,
        10,
        9999);
    CHECK(event.type == RQ_EVENT_TIMEOUT);
    CHECK(event.total_search_ms == 0);

    rq_engine_init(&engine);
    event = rq_engine_process_line(
        &engine,
        "{\"type\":\"searching\",\"context\":\"trial\","
        "\"ticketId\":\"wrong\",\"timestamp\":1}",
        false,
        0,
        0);
    CHECK(event.type == RQ_EVENT_NONE);
    CHECK(!engine.has_armed_ticket);
}

static void test_unknown_ticket_matches_python_semantics(void)
{
    rq_engine engine;
    rq_event event;

    rq_engine_init(&engine);
    event = rq_engine_process_line(
        &engine,
        "{\"type\":\"searching\",\"context\":\"invasion\","
        "\"timestamp\":100}",
        false,
        0,
        0);
    CHECK(event.type == RQ_EVENT_SEARCHING);
    CHECK(strcmp(engine.armed_ticket, "unknown") == 0);
    event = rq_engine_process_line(
        &engine,
        "{\"type\":\"timed_out\",\"context\":\"invasion\","
        "\"timestamp\":200}",
        false,
        1,
        0);
    CHECK(event.type == RQ_EVENT_TIMEOUT);
    CHECK(event.total_search_ms == 100);
}

static void test_event_names_and_disarm(void)
{
    rq_engine engine;

    CHECK(strcmp(rq_event_type_name(RQ_EVENT_SEARCHING), "searching") == 0);
    CHECK(strcmp(rq_event_type_name(RQ_EVENT_REQUEUE_EXPIRED),
                 "requeue_expired") == 0);
    CHECK(strcmp(rq_event_type_name((rq_event_type)999), "unknown") == 0);
    rq_engine_init(&engine);
    (void)line(&engine, "searching", "a", 100, false, 0);
    (void)line(&engine, "timed_out", "a", 200, false, 0);
    rq_engine_disarm_pending(&engine);
    CHECK(!engine.pending && !engine.awaiting_requeue_search);
}

static void test_observed_delayed_ticket_confirmation(void)
{
    rq_engine engine;
    rq_event event;

    /*
     * A captured live cycle produced its replacement ticket 10.679 seconds
     * after timeout.  The confirmation window must preserve that valid requeue
     * chain, and the default pre-action delay now clears the client-side
     * timeout handling instead of racing it.
     */
    rq_engine_init(&engine);
    (void)line(&engine, "searching", "slow-a", 1000, false, 0);
    (void)line(&engine, "timed_out", "slow-a", 301000, false, 0);
    CHECK(rq_engine_tick(&engine, 2499).type == RQ_EVENT_NONE);
    CHECK(rq_engine_tick(&engine, 2500).type == RQ_EVENT_REQUEUE_DUE);
    CHECK(rq_engine_mark_requeue_posted(
              &engine, 4400, RQ_DEFAULT_VERIFY_TIMEOUT_MS)
              .type == RQ_EVENT_REQUEUE_POSTED);
    CHECK(rq_engine_tick(&engine, 10679).type == RQ_EVENT_NONE);

    event = line(&engine, "searching", "slow-b", 311679, false, 10679);
    CHECK(event.type == RQ_EVENT_SEARCHING);
    CHECK(event.requeue_confirmed);
    CHECK(event.cumulative_before_ms == 300000);
    CHECK(event.requeue_count == 1);
}

static void test_requeue_retries_before_reporting_a_miss(void)
{
    rq_engine engine;
    rq_event event;

    /*
     * A missed press used to end the cycle outright, which left the player
     * with nothing in the queue while the tool still looked armed.  That is
     * worse than one more targeted attempt on the same timed-out ticket.
     */
    rq_engine_init(&engine);
    engine.timeout_ui_delay_ms = 100;
    engine.verify_timeout_ms = 1000;
    engine.max_requeue_attempts = 2;
    engine.requeue_retry_delay_ms = 500;
    (void)line(&engine, "searching", "a", 100, false, 0);
    (void)line(&engine, "timed_out", "a", 200, false, 1000);

    CHECK(rq_engine_tick(&engine, 1100).type == RQ_EVENT_REQUEUE_DUE);
    CHECK(rq_engine_mark_requeue_posted(&engine, 1100, 1000).type ==
          RQ_EVENT_REQUEUE_POSTED);
    CHECK(engine.requeue_attempt == 1);

    event = rq_engine_tick(&engine, 2100);
    CHECK(event.type == RQ_EVENT_REQUEUE_RETRY);
    CHECK(strcmp(event.ticket, "a") == 0);
    CHECK(engine.pending && engine.awaiting_requeue_search);
    CHECK(engine.pending_phase == RQ_PENDING_DELAY);
    CHECK(engine.pending_due_mono_ms == 2600);

    CHECK(rq_engine_tick(&engine, 2600).type == RQ_EVENT_REQUEUE_DUE);
    CHECK(rq_engine_mark_requeue_posted(&engine, 2600, 1000).type ==
          RQ_EVENT_REQUEUE_POSTED);
    CHECK(engine.requeue_attempt == 2);

    event = rq_engine_tick(&engine, 3600);
    CHECK(event.type == RQ_EVENT_REQUEUE_UNCONFIRMED);
    CHECK(!engine.pending && !engine.awaiting_requeue_search);

    /* A ticket that arrives after a retry is still one requeue, not two. */
    rq_engine_init(&engine);
    engine.timeout_ui_delay_ms = 100;
    engine.verify_timeout_ms = 1000;
    engine.requeue_retry_delay_ms = 500;
    (void)line(&engine, "searching", "a", 100, false, 0);
    (void)line(&engine, "timed_out", "a", 200, false, 1000);
    (void)rq_engine_tick(&engine, 1100);
    (void)rq_engine_mark_requeue_posted(&engine, 1100, 1000);
    CHECK(rq_engine_tick(&engine, 2100).type == RQ_EVENT_REQUEUE_RETRY);
    CHECK(rq_engine_tick(&engine, 2600).type == RQ_EVENT_REQUEUE_DUE);
    (void)rq_engine_mark_requeue_posted(&engine, 2600, 1000);
    event = line(&engine, "searching", "b", 300, false, 3000);
    CHECK(event.type == RQ_EVENT_SEARCHING);
    CHECK(event.requeue_confirmed);
    CHECK(event.requeue_count == 1);
    CHECK(strcmp(rq_event_type_name(RQ_EVENT_REQUEUE_RETRY),
                 "requeue_retry") == 0);
}

int main(void)
{
    test_matching_ticket_and_stale_timeout();
    test_ticket_exact_terminal_and_duplicate_safety();
    test_initial_scan_awaiting_ticket_cancel_safety();
    test_cumulative_verified_requeues_and_success();
    test_pending_lifecycle();
    test_stale_due_action_expires_without_becoming_claimable();
    test_cancel_and_disconnect();
    test_initial_scan_recovery();
    test_initial_completed_sessions_reset();
    test_fallback_timestamp_and_non_invasion_noise();
    test_unknown_ticket_matches_python_semantics();
    test_event_names_and_disarm();
    test_observed_delayed_ticket_confirmation();
    test_requeue_retries_before_reporting_a_miss();
    printf("requeue_engine: %u assertions passed\n", assertions);
    return EXIT_SUCCESS;
}
