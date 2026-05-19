/*
 * yieldwise — optional higher-level driver layer.
 *
 * `yw_run` owns the chunk loop, the timer, the timeslice report, the
 * cross-yield variance bump, and the enif_schedule_nif reschedule, so a
 * client supplies three small callbacks instead of writing the skeleton
 * loop by hand.
 *
 * This file is purely additive over yieldwise.h — clients that prefer
 * direct control simply do not compile it in.
 */

#ifndef YIELDWISE_DRIVER_H
#define YIELDWISE_DRIVER_H

#include <stddef.h>
#include <erl_nif.h>

#include "yieldwise.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    YW_CONTINUE = 0,
    YW_FAILED   = 1
} yw_status;

/*
 * The work-shaped callback vtable. The driver only sees the work through
 * an opaque pointer; the client knows the concrete type.
 */
typedef struct {
    /*
     * Process up to `n` items, advance the client's work state, and write
     * the number actually processed into *processed. Return YW_CONTINUE
     * normally, or YW_FAILED to abort the run.
     *
     * IMPORTANT: do_chunk MUST loop n times INTERNALLY. The driver calls
     * this once per ~200 µs chunk, not per item — making it item-granular
     * would put an indirect call on the hot path.
     */
    yw_status    (*do_chunk)(void *work, size_t n, size_t *processed);

    /*
     * Items still to process. 0 means the work is complete.
     */
    size_t       (*remaining)(void *work);

    /*
     * Build the term to return. Called exactly once, at the end of the
     * run — after either normal completion or a YW_FAILED abort. The
     * callback inspects `work` (which the client populated with success
     * or error state) to decide what term to produce.
     */
    ERL_NIF_TERM (*finish)(ErlNifEnv *env, void *work);
} yw_work_vtable;

/*
 * Drive the work forward through one timeslice. On completion or abort,
 * returns vt->finish(env, work). On a yield, reschedules `nif_fp` via
 * enif_schedule_nif with argv = { input_term, state_term } and returns
 * its result.
 *
 *   env, vt, work        — what to run.
 *   est, cfg             — estimation state + tuning. `est` is read AND
 *                          written; it must live in the resource the
 *                          `state_term` refers to.
 *   nif_name, nif_fp     — reschedule target. `nif_name` is the name
 *                          shown to schedulers; `nif_fp` is the C entry
 *                          point invoked on resume.
 *   input_term           — carried through every reschedule as argv[0].
 *   state_term           — the resource term that carries `work` (and
 *                          the embedded `est`) across yields. The client
 *                          builds this on the first entry; the driver
 *                          just forwards it.
 *
 * Contract notes:
 *   - The client owns first-entry detection, enif_alloc_resource, field
 *     init, and yw_estimator_init. The driver has no resource type of
 *     its own.
 *   - Errors flow back through do_chunk's status return. They are NOT
 *     signalled by the C return path: the driver always ends in a call
 *     to vt->finish.
 *   - There is intentionally no "between-chunks" hook. Clients that need
 *     to poll a cancellation flag, send progress messages, or re-tune
 *     `cfg` between chunks should use the toolkit (yieldwise.h) directly.
 */
ERL_NIF_TERM
yw_run(ErlNifEnv *env,
       const yw_work_vtable *vt,
       void *work,
       yw_estimator *est,
       const yw_config *cfg,
       const char *nif_name,
       ERL_NIF_TERM (*nif_fp)(ErlNifEnv *, int, const ERL_NIF_TERM []),
       ERL_NIF_TERM input_term,
       ERL_NIF_TERM state_term);

#ifdef __cplusplus
}
#endif

#endif /* YIELDWISE_DRIVER_H */
