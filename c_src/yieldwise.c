/*
 * yieldwise — implementation.
 *
 * Pure functions only. No globals, no static state, no resource type, no
 * load hook. Everything that must persist across a reschedule lives in
 * yw_estimator (two doubles), embedded by the caller in its own resource.
 */

#include "yieldwise.h"

#include <math.h>
#include <stddef.h>

/*
 * ε guards against division by zero and keeps pessimistic chunk math
 * monotone. Anything below 1 nanosecond/item is, for our purposes, zero.
 */
#define YW_EPS_US 1e-3

/* ------------------------------------------------------------------------- */
/* Config                                                                    */
/* ------------------------------------------------------------------------- */

yw_config
yw_config_default(double target_us)
{
    yw_config c;
    c.target_us      = (target_us > 0.0) ? target_us : 200.0;
    c.q_drift        = 0.05;
    c.sigma_timer_us = 2.0;
    c.sigma_item_us  = 0.0;
    c.beta           = 1.5;
    c.outlier_sigma  = 3.0;
    c.cross_yield_q  = 10.0 * c.q_drift;
    c.min_chunk      = 1;
    c.max_chunk      = (size_t)1 << 24;   /* 16 Mi items */
    return c;
}

/* ------------------------------------------------------------------------- */
/* Estimator                                                                 */
/* ------------------------------------------------------------------------- */

void
yw_estimator_init(yw_estimator *est,
                  const yw_config *cfg,
                  double cost_guess_us)
{
    double x = (cost_guess_us > YW_EPS_US) ? cost_guess_us : YW_EPS_US;
    double sd = 0.5 * x;                    /* ~50%-relative initial stddev */

    (void)cfg;                              /* reserved for future use      */

    est->item_cost_us = x;
    est->variance     = sd * sd;
}

void
yw_estimator_cross_yield(yw_estimator *est, const yw_config *cfg)
{
    est->variance += cfg->cross_yield_q;
}

/* ------------------------------------------------------------------------- */
/* Timer                                                                     */
/* ------------------------------------------------------------------------- */

void
yw_timer_start(yw_timer *t)
{
    t->start = enif_monotonic_time(ERL_NIF_USEC);
}

double
yw_timer_elapsed_us(const yw_timer *t)
{
    ErlNifTime now = enif_monotonic_time(ERL_NIF_USEC);
    ErlNifTime d   = now - t->start;
    if (d < 0) d = 0;                       /* monotonic, but be defensive */
    return (double)d;
}

/* ------------------------------------------------------------------------- */
/* Loop helpers                                                              */
/* ------------------------------------------------------------------------- */

static size_t
yw__clamp_size(double x, size_t lo, size_t hi)
{
    /* Compare in double-space so the size_t cast can never wrap. */
    if (!(x > 0.0))      return lo;         /* covers NaN and negatives    */
    if (x < (double)lo)  return lo;
    if (x > (double)hi)  return hi;
    return (size_t)x;
}

size_t
yw_next_chunk(const yw_estimator *est,
              const yw_config *cfg,
              size_t items_remaining)
{
    double pess, n_d;
    size_t n;

    if (items_remaining == 0) return 0;

    pess = est->item_cost_us + cfg->beta * sqrt(est->variance);
    if (pess < YW_EPS_US) pess = YW_EPS_US;

    n_d = cfg->target_us / pess;

    n = yw__clamp_size(n_d, cfg->min_chunk, cfg->max_chunk);
    if (n > items_remaining) n = items_remaining;
    return n;
}

int
yw_chunk_done(ErlNifEnv *env,
              yw_estimator *est,
              const yw_config *cfg,
              size_t n,
              double elapsed_us)
{
    /*
     * Step 1 — Kalman update. Skipped entirely when n == 0 (no items
     * means no per-item measurement; the timeslice report still runs).
     */
    if (n > 0) {
        double n_d        = (double)n;
        double z          = elapsed_us / n_d;
        double P          = est->variance + cfg->q_drift;
        double R          = (cfg->sigma_timer_us * cfg->sigma_timer_us) / (n_d * n_d)
                          + (cfg->sigma_item_us  * cfg->sigma_item_us)  / n_d;
        double innovation = z - est->item_cost_us;
        double S          = P + R;

        int reject = (innovation > 0.0)
                  && (innovation * innovation
                      > cfg->outlier_sigma * cfg->outlier_sigma * S);

        if (reject) {
            /* Keep the widened prediction; refuse to learn from the spike. */
            est->variance = P;
        } else {
            double K = (S > 0.0) ? (P / S) : 0.0;
            double x = est->item_cost_us + K * innovation;
            if (x < YW_EPS_US) x = YW_EPS_US;
            est->item_cost_us = x;
            est->variance     = (1.0 - K) * P;
        }
    }

    /*
     * Step 2 — VM hint. Always runs, with the raw elapsed time (including
     * any preemption). enif_consume_timeslice treats a 1 ms slice as 100%,
     * so percent = µs / 10. The +0.5 before the truncating cast rounds
     * half-up (elapsed_us is non-negative); clamp to the [1, 100] range
     * the API accepts.
     */
    {
        double percent_d = elapsed_us * 0.1 + 0.5;
        if (percent_d < 1.0)   percent_d = 1.0;
        if (percent_d > 100.0) percent_d = 100.0;
        return enif_consume_timeslice(env, (int)percent_d);
    }
}
