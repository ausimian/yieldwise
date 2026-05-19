/*
 * example_adaptive_fold — a worked-example NIF that demonstrates how a
 * client uses yieldwise.
 *
 * Public behaviour: fold a mixing hash over every byte of an input
 * binary, in scheduler-friendly chunks. To the caller it is an ordinary
 * synchronous call — `AdaptiveFold.fold(bin)` — that returns a uint64
 * result. Internally the NIF reschedules itself transparently whenever
 * its BEAM timeslice is spent.
 *
 * The first entry receives the binary as argv[0] and arity 1; resumed
 * entries are arity 2, with argv[1] being the resource that carries
 * the work state (cursor, accumulator, estimator).
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <erl_nif.h>

#include "yieldwise.h"

/* ------------------------------------------------------------------------- */
/* Work state — carried across every reschedule.                             */
/* ------------------------------------------------------------------------- */

typedef struct {
    size_t       pos;     /* next byte index to process    */
    uint64_t     acc;     /* hash accumulator              */
    yw_estimator est;     /* embedded estimator state      */
} fold_state;

static ErlNifResourceType *FOLD_STATE = NULL;

/*
 * No destructor: fold_state owns no pointers — only POD fields. The
 * resource is freed by the runtime when refcount hits zero.
 */
static int
load(ErlNifEnv *env, void **priv_data, ERL_NIF_TERM info)
{
    (void)priv_data;
    (void)info;

    FOLD_STATE = enif_open_resource_type(
        env, NULL, "fold_state", NULL,
        ERL_NIF_RT_CREATE | ERL_NIF_RT_TAKEOVER, NULL);
    return FOLD_STATE ? 0 : 1;
}

/* ------------------------------------------------------------------------- */
/* Hash step — a small, branch-free mixing fold.                             */
/* ------------------------------------------------------------------------- */

static inline uint64_t
fold_step(uint64_t acc, uint8_t byte)
{
    /* FNV-1a-ish mix, fine for the example's purposes. */
    acc ^= (uint64_t)byte;
    acc *= 0x100000001b3ULL;
    return acc;
}

/* ------------------------------------------------------------------------- */
/* The NIF body.                                                             */
/* ------------------------------------------------------------------------- */

static ERL_NIF_TERM fold_nif(ErlNifEnv *env, int argc,
                             const ERL_NIF_TERM argv[]);

static ERL_NIF_TERM
fold_nif(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[])
{
    ErlNifBinary bin;
    fold_state  *st;
    yw_config    cfg = yw_config_default(200.0);
    int          first_entry;

    if (argc < 1 || argc > 2)
        return enif_make_badarg(env);

    if (!enif_inspect_binary(env, argv[0], &bin))
        return enif_make_badarg(env);

    first_entry = (argc == 1);

    if (first_entry) {
        st = (fold_state *)enif_alloc_resource(FOLD_STATE, sizeof *st);
        if (st == NULL)
            return enif_make_badarg(env);
        st->pos = 0;
        st->acc = 0xcbf29ce484222325ULL;          /* FNV offset basis */
        /*
         * Seed the estimator deliberately high. The folder is a few
         * arithmetic ops per byte — well under 1 µs/item — so 0.05 µs
         * gives the filter plenty of room to converge downward.
         */
        yw_estimator_init(&st->est, &cfg, 0.05);
    } else {
        if (!enif_get_resource(env, argv[1], FOLD_STATE, (void **)&st))
            return enif_make_badarg(env);
    }

    /* The chunk loop. */
    while (st->pos < bin.size) {
        size_t   remaining = bin.size - st->pos;
        size_t   n         = yw_next_chunk(&st->est, &cfg, remaining);
        yw_timer t;
        double   elapsed;
        size_t   i;
        uint64_t acc;
        const unsigned char *p;

        yw_timer_start(&t);

        /* Hot loop — strictly process exactly n items. */
        acc = st->acc;
        p   = bin.data + st->pos;
        for (i = 0; i < n; ++i) {
            acc = fold_step(acc, p[i]);
        }
        st->acc  = acc;
        st->pos += n;

        elapsed = yw_timer_elapsed_us(&t);

        if (yw_chunk_done(env, &st->est, &cfg, n, elapsed)) {
            ERL_NIF_TERM state_term;

            yw_estimator_cross_yield(&st->est, &cfg);

            if (first_entry) {
                state_term = enif_make_resource(env, st);
                enif_release_resource(st);          /* term owns it now */
            } else {
                state_term = argv[1];               /* reuse the carried */
            }

            {
                ERL_NIF_TERM next[2] = { argv[0], state_term };
                return enif_schedule_nif(env, "fold_nif", 0,
                                         fold_nif, 2, next);
            }
        }
    }

    /* Done — build the result. */
    {
        ERL_NIF_TERM result = enif_make_uint64(env, st->acc);
        if (first_entry) {
            /* Allocated this call, never made a term for it. */
            enif_release_resource(st);
        }
        return result;
    }
}

/* ------------------------------------------------------------------------- */
/* NIF registration.                                                         */
/* ------------------------------------------------------------------------- */

static ErlNifFunc nif_funcs[] = {
    {"fold_nif", 1, fold_nif, 0}
};

/*
 * AdaptiveFold is an Elixir module — at the BEAM level its name is
 * `Elixir.AdaptiveFold`. ERL_NIF_INIT stringifies its first argument
 * but never pastes it into a C identifier, so the dot is fine.
 */
ERL_NIF_INIT(Elixir.AdaptiveFold, nif_funcs, load, NULL, NULL, NULL)
