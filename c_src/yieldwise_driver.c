/*
 * yieldwise_driver — sugar over yieldwise.h.
 *
 * Owns the chunk-loop, the timer, the timeslice bookkeeping, the cross-
 * yield variance bump, and the enif_schedule_nif reschedule. Everything
 * else (resource type, first-entry init, term construction) is the
 * client's job, as documented in yieldwise_driver.h.
 */

#include "yieldwise_driver.h"

ERL_NIF_TERM
yw_run(ErlNifEnv *env,
       const yw_work_vtable *vt,
       void *work,
       yw_estimator *est,
       const yw_config *cfg,
       const char *nif_name,
       ERL_NIF_TERM (*nif_fp)(ErlNifEnv *, int, const ERL_NIF_TERM []),
       ERL_NIF_TERM input_term,
       ERL_NIF_TERM state_term)
{
    for (;;) {
        size_t    remaining = vt->remaining(work);
        size_t    n;
        size_t    processed = 0;
        yw_timer  t;
        double    elapsed;
        yw_status st;
        int       slice_spent;

        if (remaining == 0) {
            return vt->finish(env, work);
        }

        n = yw_next_chunk(est, cfg, remaining);

        yw_timer_start(&t);
        st = vt->do_chunk(work, n, &processed);
        elapsed = yw_timer_elapsed_us(&t);

        /*
         * Update the estimator + report to the VM using the number of
         * items actually processed. If do_chunk reports fewer than `n`
         * (e.g. it aborted mid-chunk), the timer reading still reflects
         * the wall-clock spent and `processed` weights the per-item
         * cost correctly.
         */
        slice_spent = yw_chunk_done(env, est, cfg, processed, elapsed);

        if (st == YW_FAILED) {
            return vt->finish(env, work);
        }

        if (slice_spent) {
            yw_estimator_cross_yield(est, cfg);
            {
                ERL_NIF_TERM next[2] = { input_term, state_term };
                return enif_schedule_nif(env, nif_name, 0,
                                         nif_fp, 2, next);
            }
        }
    }
}
