/*
 * yieldwise — adaptive work-chunking for Erlang NIFs.
 *
 * Public API. See ../README/yieldwise_design_brief.md for the design.
 *
 * The library is a stateless toolkit:
 *   - it owns no resources, no globals, no load-time hooks;
 *   - the caller drives its own chunk loop and its own enif_schedule_nif;
 *   - the only state that must survive a reschedule is the two-double
 *     yw_estimator, which the caller embeds in its own resource object.
 *
 * Targets Linux, macOS, and Windows. There is no platform-specific source.
 */

#ifndef YIELDWISE_H
#define YIELDWISE_H

#include <stddef.h>
#include <erl_nif.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------- */
/* Config — stateless POD, fill once per NIF entry.                          */
/* ------------------------------------------------------------------------- */

typedef struct {
    double target_us;       /* wall-clock a chunk should aim for          */
    double q_drift;         /* process noise: drift in per-item cost      */
    double sigma_timer_us;  /* stddev of timer jitter on whole-chunk time */
    double sigma_item_us;   /* stddev of genuine per-item cost            */
    double beta;            /* pessimism: size against cost + beta*sd     */
    double outlier_sigma;   /* slow-side outlier gate, in sigmas          */
    double cross_yield_q;   /* extra variance added when resumed          */
    size_t min_chunk;       /* hard floor on chunk size                   */
    size_t max_chunk;       /* hard ceiling on chunk size                 */
} yw_config;

/*
 * Build a config populated with sensible defaults. `target_us` is the
 * wall-clock budget for a single chunk; pass ≤ 0 to get the default 200 µs.
 * The config is stateless: rebuild it from constants on each NIF entry,
 * never round-trip it through a resource.
 */
yw_config yw_config_default(double target_us);

/* ------------------------------------------------------------------------- */
/* Estimator — POD; MUST survive reschedules (embed in your resource).       */
/* ------------------------------------------------------------------------- */

typedef struct {
    double item_cost_us;   /* x: estimated cost per item, in µs  */
    double variance;       /* P: uncertainty of that estimate    */
} yw_estimator;

/*
 * Seed the filter. `cost_guess_us` must be a deliberately *pessimistic*
 * (high) per-item cost — the filter converges downward toward the truth.
 * The initial variance is set wide (~50%-relative stddev) so the first
 * few chunks stay conservative until evidence narrows it.
 */
void yw_estimator_init(yw_estimator *est,
                       const yw_config *cfg,
                       double cost_guess_us);

/*
 * Widen the estimator's variance to reflect the staleness of resuming on
 * a (possibly different) scheduler thread with cold caches. Call once at
 * each yield, just before enif_schedule_nif.
 */
void yw_estimator_cross_yield(yw_estimator *est, const yw_config *cfg);

/* ------------------------------------------------------------------------- */
/* Timer — transient, stack-local only. Never store one in a resource.       */
/* ------------------------------------------------------------------------- */

typedef struct {
    ErlNifTime start;      /* enif_monotonic_time(ERL_NIF_USEC) */
} yw_timer;

void   yw_timer_start(yw_timer *t);
double yw_timer_elapsed_us(const yw_timer *t);

/* ------------------------------------------------------------------------- */
/* Loop helpers — the two calls that bracket each chunk.                     */
/* ------------------------------------------------------------------------- */

/*
 * Pick the next chunk size, given the current estimator state, the config,
 * and how many items are left. Returns a size clamped to
 * [cfg->min_chunk, cfg->max_chunk] and never exceeding `items_remaining`.
 * Returns 0 only when `items_remaining` is 0.
 */
size_t yw_next_chunk(const yw_estimator *est,
                     const yw_config *cfg,
                     size_t items_remaining);

/*
 * Call once after each chunk completes.
 *
 *   `n`           — the chunk size you actually processed.
 *   `elapsed_us`  — wall-clock duration of the chunk, INCLUDING any
 *                   preemption (i.e. the raw timer reading).
 *
 * Performs two things in one call:
 *   (1) update the Kalman estimator with the per-item cost implied by the
 *       chunk (with a one-sided outlier gate that drops only suspiciously
 *       slow chunks — those almost certainly mean the OS preempted us);
 *   (2) report the same raw elapsed time to enif_consume_timeslice so the
 *       BEAM scheduler accounts for our reductions.
 *
 * Returns the result of enif_consume_timeslice: non-zero means the slice is
 * spent and the caller should yield (via enif_schedule_nif) as soon as it
 * can save its state.
 */
int yw_chunk_done(ErlNifEnv *env,
                  yw_estimator *est,
                  const yw_config *cfg,
                  size_t n,
                  double elapsed_us);

#ifdef __cplusplus
}
#endif

#endif /* YIELDWISE_H */
