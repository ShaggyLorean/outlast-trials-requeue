#ifndef OUTLAST_REQUEUE_ENGINE_H
#define OUTLAST_REQUEUE_ENGINE_H

/*
 * Platform-neutral Outlast Trials Invasion queue state machine.
 *
 * The engine only parses log lines and tracks state.  It never opens a
 * window, sends input, sleeps, or reads a file, so it stays independent of the
 * native Win32 front end that drives it.
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RQ_TICKET_CAPACITY 128
#define RQ_DEFAULT_TIMEOUT_UI_DELAY_MS UINT64_C(800)
#define RQ_DEFAULT_VERIFY_TIMEOUT_MS UINT64_C(15000)
#define RQ_DEFAULT_ACTION_MAX_LATENESS_MS UINT64_C(3000)

typedef enum rq_event_type {
    RQ_EVENT_NONE = 0,
    RQ_EVENT_SEARCHING,
    RQ_EVENT_TIMEOUT,
    RQ_EVENT_SUCCEEDED,
    RQ_EVENT_CANCELED,
    RQ_EVENT_DISCONNECTED,
    RQ_EVENT_REQUEUE_DUE,
    RQ_EVENT_REQUEUE_POSTED,
    RQ_EVENT_REQUEUE_UNCONFIRMED,
    RQ_EVENT_REQUEUE_EXPIRED,
    RQ_EVENT_HELPER_FAILED
} rq_event_type;

typedef enum rq_pending_phase {
    RQ_PENDING_NONE = 0,
    RQ_PENDING_DELAY,
    RQ_PENDING_ACTION_CLAIMED,
    RQ_PENDING_VERIFYING
} rq_pending_phase;

typedef struct rq_event {
    rq_event_type type;
    char ticket[RQ_TICKET_CAPACITY];
    uint64_t started_at_ms;
    bool has_started_at;
    uint64_t total_search_ms;
    uint64_t cumulative_before_ms;
    uint32_t requeue_count;
    bool requeue_confirmed;
    bool recovered;
} rq_event;

/*
 * State is intentionally public/read-only-by-convention.  Front ends can
 * render status without maintaining a second, subtly different state model.
 */
typedef struct rq_engine {
    char armed_ticket[RQ_TICKET_CAPACITY];
    bool has_armed_ticket;
    uint64_t search_started_ms;
    bool has_search_started;
    uint64_t cumulative_search_ms;
    uint32_t requeue_count;

    bool pending;
    rq_pending_phase pending_phase;
    char pending_ticket[RQ_TICKET_CAPACITY];
    uint64_t pending_due_mono_ms;
    uint64_t pending_verify_until_mono_ms;
    bool awaiting_requeue_search;
    char awaiting_ticket[RQ_TICKET_CAPACITY];
    bool finished;

    uint64_t timeout_ui_delay_ms;
    uint64_t verify_timeout_ms;
    uint64_t action_max_lateness_ms;
} rq_engine;

void rq_engine_init(rq_engine *engine);

/*
 * Process one complete UTF-8/ASCII-compatible OPP.log line.
 *
 * initial_scan suppresses externally visible events and pending actions while
 * still replaying the state transitions needed to recover the current search.
 * fallback_wall_ms is used when the line has no server timestamp.
 */
rq_event rq_engine_process_line(rq_engine *engine,
                                const char *line,
                                bool initial_scan,
                                uint64_t now_mono_ms,
                                uint64_t fallback_wall_ms);

/* Return the recovered SEARCHING event after an initial scan, if applicable. */
rq_event rq_engine_recovery_event(const rq_engine *engine);

/*
 * Claim a due action once, or expire a posted action which never produced a
 * new searching ticket.  The caller performs the platform-specific sequence.
 */
rq_event rq_engine_tick(rq_engine *engine, uint64_t now_mono_ms);

/* Report the result of the platform-specific targeted action sequence. */
rq_event rq_engine_mark_requeue_posted(rq_engine *engine,
                                       uint64_t now_mono_ms,
                                       uint64_t verify_timeout_ms);
rq_event rq_engine_mark_requeue_expired(rq_engine *engine);
rq_event rq_engine_mark_requeue_failed(rq_engine *engine);

/* Backwards-readable alias: the failure is from the targeted helper/action. */
rq_event rq_engine_mark_helper_failed(rq_engine *engine);

/* Disarm an in-flight action, for example when automation is switched off. */
void rq_engine_disarm_pending(rq_engine *engine);

const char *rq_event_type_name(rq_event_type type);

#ifdef __cplusplus
}
#endif

#endif
