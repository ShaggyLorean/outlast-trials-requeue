#include "requeue_engine.h"

#include <ctype.h>
#include <limits.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

static const char TIMEOUT_MARKER[] =
    "\"type\":\"timed_out\",\"context\":\"invasion\"";
static const char SEARCH_MARKER[] =
    "\"type\":\"searching\",\"context\":\"invasion\"";
static const char SUCCESS_MARKER[] =
    "\"type\":\"succeeded\",\"context\":\"invasion\"";
static const char CANCEL_MARKER_US[] =
    "\"type\":\"canceled\",\"context\":\"invasion\"";
static const char CANCEL_MARKER_UK[] =
    "\"type\":\"cancelled\",\"context\":\"invasion\"";
static const char SESSION_LOSS_NETWORK_FAILURE[] =
    "UEngine::BroadcastNetworkFailure";
static const char SESSION_LOSS_RETURNING[] =
    "Connection failed; returning to Entry";
static const char TICKET_PREFIX[] = "\"ticketId\":\"";
static const char TIMESTAMP_PREFIX[] = "\"timestamp\":";

static rq_event empty_event(void)
{
    rq_event event;
    memset(&event, 0, sizeof(event));
    event.type = RQ_EVENT_NONE;
    return event;
}

static void copy_string(char *destination, size_t capacity, const char *source)
{
    size_t length;

    if (capacity == 0) {
        return;
    }
    if (source == NULL) {
        destination[0] = '\0';
        return;
    }
    length = strlen(source);
    if (length >= capacity) {
        length = capacity - 1;
    }
    memcpy(destination, source, length);
    destination[length] = '\0';
}

static void event_set_ticket(rq_event *event, const char *ticket)
{
    copy_string(event->ticket, sizeof(event->ticket), ticket);
}

static bool contains(const char *line, const char *marker)
{
    return line != NULL && strstr(line, marker) != NULL;
}

static void parse_ticket(const char *line, char output[RQ_TICKET_CAPACITY])
{
    const char *begin;
    const char *end;
    size_t length;

    copy_string(output, RQ_TICKET_CAPACITY, "unknown");
    if (line == NULL) {
        return;
    }
    begin = strstr(line, TICKET_PREFIX);
    if (begin == NULL) {
        return;
    }
    begin += sizeof(TICKET_PREFIX) - 1;
    end = strchr(begin, '"');
    if (end == NULL || end == begin) {
        return;
    }
    length = (size_t)(end - begin);
    if (length >= RQ_TICKET_CAPACITY) {
        length = RQ_TICKET_CAPACITY - 1;
    }
    memcpy(output, begin, length);
    output[length] = '\0';
}

static uint64_t parse_timestamp_ms(const char *line, uint64_t fallback_wall_ms)
{
    const char *digits;
    char *end;
    unsigned long long value;

    if (line == NULL) {
        return fallback_wall_ms;
    }
    digits = strstr(line, TIMESTAMP_PREFIX);
    if (digits == NULL) {
        return fallback_wall_ms;
    }
    digits += sizeof(TIMESTAMP_PREFIX) - 1;
    if (!isdigit((unsigned char)*digits)) {
        return fallback_wall_ms;
    }
    value = strtoull(digits, &end, 10);
    if (end == digits) {
        return fallback_wall_ms;
    }
    return (uint64_t)value;
}

static uint64_t positive_delta(uint64_t end_ms, uint64_t start_ms)
{
    return end_ms >= start_ms ? end_ms - start_ms : UINT64_C(0);
}

static uint64_t saturating_add(uint64_t left, uint64_t right)
{
    return UINT64_MAX - left < right ? UINT64_MAX : left + right;
}

static uint32_t increment_count(uint32_t value)
{
    return value == UINT32_MAX ? UINT32_MAX : value + UINT32_C(1);
}

static void clear_pending(rq_engine *engine)
{
    engine->pending = false;
    engine->pending_phase = RQ_PENDING_NONE;
    engine->pending_ticket[0] = '\0';
    engine->pending_due_mono_ms = 0;
    engine->pending_verify_until_mono_ms = 0;
}

static void clear_awaiting_requeue(rq_engine *engine)
{
    engine->awaiting_requeue_search = false;
    engine->awaiting_ticket[0] = '\0';
}

static void set_awaiting_requeue(rq_engine *engine, const char *ticket)
{
    engine->awaiting_requeue_search = true;
    copy_string(
        engine->awaiting_ticket, sizeof(engine->awaiting_ticket), ticket);
}

static void clear_active_search(rq_engine *engine)
{
    engine->armed_ticket[0] = '\0';
    engine->has_armed_ticket = false;
    engine->search_started_ms = 0;
    engine->has_search_started = false;
}

static uint64_t active_total_at(const rq_engine *engine, uint64_t event_ms)
{
    uint64_t total = engine->cumulative_search_ms;

    if (engine->has_search_started) {
        total = saturating_add(
            total, positive_delta(event_ms, engine->search_started_ms));
    }
    return total;
}

static rq_event base_state_event(const rq_engine *engine, rq_event_type type)
{
    rq_event event = empty_event();
    event.type = type;
    event.total_search_ms = engine->cumulative_search_ms;
    event.cumulative_before_ms = engine->cumulative_search_ms;
    event.requeue_count = engine->requeue_count;
    return event;
}

void rq_engine_init(rq_engine *engine)
{
    if (engine == NULL) {
        return;
    }
    memset(engine, 0, sizeof(*engine));
    engine->timeout_ui_delay_ms = RQ_DEFAULT_TIMEOUT_UI_DELAY_MS;
    engine->verify_timeout_ms = RQ_DEFAULT_VERIFY_TIMEOUT_MS;
    engine->action_max_lateness_ms = RQ_DEFAULT_ACTION_MAX_LATENESS_MS;
    engine->max_requeue_attempts = RQ_DEFAULT_MAX_REQUEUE_ATTEMPTS;
    engine->requeue_retry_delay_ms = RQ_DEFAULT_REQUEUE_RETRY_DELAY_MS;
}

rq_event rq_engine_process_line(rq_engine *engine,
                                const char *line,
                                bool initial_scan,
                                uint64_t now_mono_ms,
                                uint64_t fallback_wall_ms)
{
    rq_event event = empty_event();
    char ticket[RQ_TICKET_CAPACITY];
    uint64_t event_ms;

    if (engine == NULL || line == NULL) {
        return event;
    }

    if (contains(line, SESSION_LOSS_NETWORK_FAILURE) ||
        contains(line, SESSION_LOSS_RETURNING)) {
        bool had_active_state = engine->has_armed_ticket || engine->pending ||
                                engine->awaiting_requeue_search;
        uint64_t total;
        uint32_t requeue_count = engine->requeue_count;

        event_ms = parse_timestamp_ms(line, fallback_wall_ms);
        total = active_total_at(engine, event_ms);
        clear_active_search(engine);
        clear_pending(engine);
        clear_awaiting_requeue(engine);
        engine->cumulative_search_ms = 0;
        engine->requeue_count = 0;
        if (!initial_scan && had_active_state) {
            event.type = RQ_EVENT_DISCONNECTED;
            event.total_search_ms = total;
            event.requeue_count = requeue_count;
        }
        return event;
    }

    if (contains(line, SEARCH_MARKER)) {
        bool requeue_confirmed;

        parse_ticket(line, ticket);
        /* Duplicate log records must not restart or shorten an active timer. */
        if (engine->has_armed_ticket &&
            strcmp(ticket, engine->armed_ticket) == 0) {
            return event;
        }
        /*
         * A replay of the ticket which just timed out is not proof that the
         * targeted action created a replacement search.  Keep waiting for a
         * distinct server-issued ticket instead of falsely confirming it.
         */
        if ((engine->awaiting_requeue_search &&
             strcmp(ticket, engine->awaiting_ticket) == 0) ||
            (engine->pending &&
             strcmp(ticket, engine->pending_ticket) == 0)) {
            return event;
        }
        requeue_confirmed =
            engine->pending || engine->awaiting_requeue_search;
        event_ms = parse_timestamp_ms(line, fallback_wall_ms);
        if (requeue_confirmed) {
            engine->requeue_count = increment_count(engine->requeue_count);
        } else {
            engine->cumulative_search_ms = 0;
            engine->requeue_count = 0;
        }
        if (requeue_confirmed) {
            clear_pending(engine);
        }
        copy_string(engine->armed_ticket, sizeof(engine->armed_ticket), ticket);
        engine->has_armed_ticket = true;
        engine->search_started_ms = event_ms;
        engine->has_search_started = true;
        clear_awaiting_requeue(engine);

        if (!initial_scan) {
            event = base_state_event(engine, RQ_EVENT_SEARCHING);
            event_set_ticket(&event, ticket);
            event.started_at_ms = event_ms;
            event.has_started_at = true;
            event.requeue_confirmed = requeue_confirmed;
        }
        return event;
    }

    if (contains(line, TIMEOUT_MARKER)) {
        uint64_t started_at_ms = engine->search_started_ms;
        bool had_started_at = engine->has_search_started;

        parse_ticket(line, ticket);
        if (!engine->has_armed_ticket ||
            strcmp(ticket, engine->armed_ticket) != 0) {
            return event;
        }
        clear_active_search(engine);
        event_ms = parse_timestamp_ms(line, fallback_wall_ms);
        if (had_started_at) {
            engine->cumulative_search_ms = saturating_add(
                engine->cumulative_search_ms,
                positive_delta(event_ms, started_at_ms));
        }
        set_awaiting_requeue(engine, ticket);
        if (initial_scan) {
            return event;
        }

        engine->pending = true;
        engine->pending_phase = RQ_PENDING_DELAY;
        engine->requeue_attempt = 0;
        copy_string(
            engine->pending_ticket, sizeof(engine->pending_ticket), ticket);
        engine->pending_due_mono_ms =
            saturating_add(now_mono_ms, engine->timeout_ui_delay_ms);
        engine->pending_verify_until_mono_ms = 0;

        event = base_state_event(engine, RQ_EVENT_TIMEOUT);
        event_set_ticket(&event, ticket);
        event.started_at_ms = started_at_ms;
        event.has_started_at = had_started_at;
        return event;
    }

    if (contains(line, SUCCESS_MARKER)) {
        uint64_t total;

        parse_ticket(line, ticket);
        if (!engine->has_armed_ticket ||
            strcmp(ticket, engine->armed_ticket) != 0) {
            return event;
        }
        event_ms = parse_timestamp_ms(line, fallback_wall_ms);
        total = active_total_at(engine, event_ms);
        clear_active_search(engine);
        clear_pending(engine);
        clear_awaiting_requeue(engine);

        if (!initial_scan) {
            event.type = RQ_EVENT_SUCCEEDED;
            event_set_ticket(&event, ticket);
            event.total_search_ms = total;
            event.requeue_count = engine->requeue_count;
            engine->finished = true;
        }
        engine->cumulative_search_ms = 0;
        engine->requeue_count = 0;
        return event;
    }

    if (contains(line, CANCEL_MARKER_US) || contains(line, CANCEL_MARKER_UK)) {
        bool matches_active;
        bool matches_pending;
        bool matches_awaiting;
        uint64_t total;

        parse_ticket(line, ticket);
        matches_active = engine->has_armed_ticket &&
                         strcmp(ticket, engine->armed_ticket) == 0;
        matches_pending = engine->pending &&
                          strcmp(ticket, engine->pending_ticket) == 0;
        matches_awaiting = engine->awaiting_requeue_search &&
                           strcmp(ticket, engine->awaiting_ticket) == 0;
        if (!matches_active && !matches_pending && !matches_awaiting) {
            return event;
        }

        total = engine->cumulative_search_ms;
        event_ms = parse_timestamp_ms(line, fallback_wall_ms);
        if (matches_active) {
            if (engine->has_search_started) {
                total = saturating_add(
                    total,
                    positive_delta(event_ms, engine->search_started_ms));
            }
        }
        clear_active_search(engine);
        clear_pending(engine);
        clear_awaiting_requeue(engine);
        if (!initial_scan) {
            event.type = RQ_EVENT_CANCELED;
            event_set_ticket(&event, ticket);
            event.total_search_ms = total;
            event.requeue_count = engine->requeue_count;
        }
        engine->cumulative_search_ms = 0;
        engine->requeue_count = 0;
        return event;
    }

    return event;
}

rq_event rq_engine_recovery_event(const rq_engine *engine)
{
    rq_event event = empty_event();

    if (engine == NULL || !engine->has_armed_ticket) {
        return event;
    }
    event = base_state_event(engine, RQ_EVENT_SEARCHING);
    event_set_ticket(&event, engine->armed_ticket);
    event.started_at_ms = engine->search_started_ms;
    event.has_started_at = engine->has_search_started;
    event.recovered = true;
    return event;
}

rq_event rq_engine_tick(rq_engine *engine, uint64_t now_mono_ms)
{
    rq_event event = empty_event();

    if (engine == NULL || !engine->pending) {
        return event;
    }
    if (engine->pending_phase == RQ_PENDING_VERIFYING) {
        if (now_mono_ms < engine->pending_verify_until_mono_ms) {
            return event;
        }
        /*
         * The posted sequence produced no replacement ticket.  Giving up here
         * leaves the player with nothing in the queue at all, which is worse
         * than one more targeted attempt on the same timed-out ticket.
         */
        if (engine->requeue_attempt < engine->max_requeue_attempts) {
            engine->pending_phase = RQ_PENDING_DELAY;
            engine->pending_due_mono_ms = saturating_add(
                now_mono_ms, engine->requeue_retry_delay_ms);
            engine->pending_verify_until_mono_ms = 0;
            event = base_state_event(engine, RQ_EVENT_REQUEUE_RETRY);
            event_set_ticket(&event, engine->pending_ticket);
            return event;
        }
        event = base_state_event(engine, RQ_EVENT_REQUEUE_UNCONFIRMED);
        event_set_ticket(&event, engine->pending_ticket);
        clear_pending(engine);
        clear_awaiting_requeue(engine);
        return event;
    }
    if (engine->pending_phase != RQ_PENDING_DELAY ||
        now_mono_ms < engine->pending_due_mono_ms) {
        return event;
    }
    if (now_mono_ms > saturating_add(engine->pending_due_mono_ms,
                                     engine->action_max_lateness_ms)) {
        event = base_state_event(engine, RQ_EVENT_REQUEUE_EXPIRED);
        event_set_ticket(&event, engine->pending_ticket);
        clear_pending(engine);
        clear_awaiting_requeue(engine);
        return event;
    }

    engine->pending_phase = RQ_PENDING_ACTION_CLAIMED;
    event = base_state_event(engine, RQ_EVENT_REQUEUE_DUE);
    event_set_ticket(&event, engine->pending_ticket);
    return event;
}

rq_event rq_engine_mark_requeue_posted(rq_engine *engine,
                                       uint64_t now_mono_ms,
                                       uint64_t verify_timeout_ms)
{
    rq_event event = empty_event();

    if (engine == NULL || !engine->pending) {
        return event;
    }
    event = base_state_event(engine, RQ_EVENT_REQUEUE_POSTED);
    event_set_ticket(&event, engine->pending_ticket);
    engine->requeue_attempt = increment_count(engine->requeue_attempt);
    engine->pending_phase = RQ_PENDING_VERIFYING;
    engine->verify_timeout_ms = verify_timeout_ms;
    engine->pending_verify_until_mono_ms =
        saturating_add(now_mono_ms, verify_timeout_ms);
    return event;
}

rq_event rq_engine_mark_requeue_expired(rq_engine *engine)
{
    rq_event event = empty_event();

    if (engine == NULL || !engine->pending) {
        return event;
    }
    event = base_state_event(engine, RQ_EVENT_REQUEUE_EXPIRED);
    event_set_ticket(&event, engine->pending_ticket);
    clear_pending(engine);
    clear_awaiting_requeue(engine);
    return event;
}

rq_event rq_engine_mark_requeue_failed(rq_engine *engine)
{
    rq_event event = empty_event();

    if (engine == NULL || !engine->pending) {
        return event;
    }
    event = base_state_event(engine, RQ_EVENT_HELPER_FAILED);
    event_set_ticket(&event, engine->pending_ticket);
    clear_pending(engine);
    clear_awaiting_requeue(engine);
    return event;
}

rq_event rq_engine_mark_helper_failed(rq_engine *engine)
{
    return rq_engine_mark_requeue_failed(engine);
}

void rq_engine_disarm_pending(rq_engine *engine)
{
    if (engine == NULL) {
        return;
    }
    clear_pending(engine);
    clear_awaiting_requeue(engine);
}

const char *rq_event_type_name(rq_event_type type)
{
    switch (type) {
    case RQ_EVENT_NONE:
        return "none";
    case RQ_EVENT_SEARCHING:
        return "searching";
    case RQ_EVENT_TIMEOUT:
        return "timeout";
    case RQ_EVENT_SUCCEEDED:
        return "succeeded";
    case RQ_EVENT_CANCELED:
        return "canceled";
    case RQ_EVENT_DISCONNECTED:
        return "disconnected";
    case RQ_EVENT_REQUEUE_DUE:
        return "requeue_due";
    case RQ_EVENT_REQUEUE_POSTED:
        return "requeue_posted";
    case RQ_EVENT_REQUEUE_RETRY:
        return "requeue_retry";
    case RQ_EVENT_REQUEUE_UNCONFIRMED:
        return "requeue_unconfirmed";
    case RQ_EVENT_REQUEUE_EXPIRED:
        return "requeue_expired";
    case RQ_EVENT_HELPER_FAILED:
        return "helper_failed";
    default:
        return "unknown";
    }
}
