/*
 * example_adaptive_fold_persistent — a worked-example NIF that
 * persists the yieldwise estimator's learnt state across calls.
 *
 * Same fold semantics as example_adaptive_fold, but the Kalman
 * estimator is bootstrapped from (and committed back to) a thread-
 * local slot on the scheduler thread the fold *started* on. So the
 * first call seeds the slot from a deliberately pessimistic
 * 0.05 µs/byte and converges over its own chunk loop; later calls
 * open with the converged estimate and pick a much larger first
 * chunk, skipping the warm-up cost.
 *
 * Pattern is "origin-pinned snapshot in / snapshot out":
 *
 *   - on first entry, capture &TLS_EST and copy its yw_estimator
 *     into the per-fold resource;
 *   - the resource's copy is what the chunk loop mutates (so a
 *     reschedule that resumes on a different scheduler thread keeps
 *     using the same estimator, exactly as in the basic example);
 *   - at fold completion, write the resource's final state back
 *     through the captured pointer — landing in the originating
 *     thread's TLS slot regardless of which thread finished.
 *
 * fold_nif/1 returns {hash, stats_map}; the Elixir wrapper splits
 * this into fold/1 (just the hash) and fold_with_stats/1 (the
 * tuple, for tests / benchmarks observing convergence).
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <erl_nif.h>

#include "yieldwise.h"

/* ------------------------------------------------------------------------- */
/* TLS — one yw_estimator per scheduler thread, lazily initialised.          */
/* ------------------------------------------------------------------------- */

typedef struct {
    yw_estimator est;
    int          inited;
} tls_slot;

static _Thread_local tls_slot TLS_EST;

/* ------------------------------------------------------------------------- */
/* Work state — carried across every reschedule.                             */
/* ------------------------------------------------------------------------- */

typedef struct {
    size_t        pos;
    uint64_t      acc;
    yw_estimator  est;                /* live state during this fold        */
    tls_slot     *origin_tls;         /* fold-origin TLS slot, write-back   */
    size_t        first_chunk_size;   /* recorded on first entry, for stats */
} fold_state;

static ErlNifResourceType *FOLD_STATE = NULL;

/* Atoms used in the stats map. Cached at load time. */
static ERL_NIF_TERM ATOM_FIRST_CHUNK_SIZE;
static ERL_NIF_TERM ATOM_ITEM_COST_US;
static ERL_NIF_TERM ATOM_VARIANCE;

static int
load(ErlNifEnv *env, void **priv_data, ERL_NIF_TERM info)
{
    (void)priv_data;
    (void)info;

    FOLD_STATE = enif_open_resource_type(
        env, NULL, "fold_state_persistent", NULL,
        ERL_NIF_RT_CREATE | ERL_NIF_RT_TAKEOVER, NULL);
    if (FOLD_STATE == NULL) return 1;

    ATOM_FIRST_CHUNK_SIZE = enif_make_atom(env, "first_chunk_size");
    ATOM_ITEM_COST_US     = enif_make_atom(env, "item_cost_us");
    ATOM_VARIANCE         = enif_make_atom(env, "variance");

    return 0;
}

/* ------------------------------------------------------------------------- */
/* Hash step — same branch-free mixer as the basic example.                  */
/* ------------------------------------------------------------------------- */

static inline uint64_t
fold_step(uint64_t acc, uint8_t byte)
{
    acc ^= (uint64_t)byte;
    acc *= 0x100000001b3ULL;
    return acc;
}

static ERL_NIF_TERM
build_stats(ErlNifEnv *env, const fold_state *st)
{
    ERL_NIF_TERM map = enif_make_new_map(env);
    enif_make_map_put(env, map, ATOM_FIRST_CHUNK_SIZE,
                      enif_make_uint64(env, (ErlNifUInt64)st->first_chunk_size),
                      &map);
    enif_make_map_put(env, map, ATOM_ITEM_COST_US,
                      enif_make_double(env, st->est.item_cost_us), &map);
    enif_make_map_put(env, map, ATOM_VARIANCE,
                      enif_make_double(env, st->est.variance), &map);
    return map;
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
            return enif_raise_exception(env, enif_make_atom(env, "enomem"));

        /*
         * Lazy per-thread bootstrap. The seed is the same deliberately
         * pessimistic 0.05 µs/byte the basic example uses, so the
         * filter converges downward over the cold thread's first fold.
         */
        if (!TLS_EST.inited) {
            yw_estimator_init(&TLS_EST.est, &cfg, 0.05);
            TLS_EST.inited = 1;
        }

        st->pos              = 0;
        st->acc              = 0xcbf29ce484222325ULL;     /* FNV offset */
        st->origin_tls       = &TLS_EST;                  /* origin-pinned */
        st->est              = TLS_EST.est;               /* snapshot in */
        st->first_chunk_size = yw_next_chunk(&st->est, &cfg, bin.size);
    } else {
        if (!enif_get_resource(env, argv[1], FOLD_STATE, (void **)&st))
            return enif_make_badarg(env);
    }

    /* The chunk loop — identical in shape to the basic example. */
    while (st->pos < bin.size) {
        size_t   remaining = bin.size - st->pos;
        size_t   n         = yw_next_chunk(&st->est, &cfg, remaining);
        yw_timer t;
        double   elapsed;
        size_t   i;
        uint64_t acc;
        const unsigned char *p;

        yw_timer_start(&t);

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
                enif_release_resource(st);
            } else {
                state_term = argv[1];
            }

            {
                ERL_NIF_TERM next[2] = { argv[0], state_term };
                return enif_schedule_nif(env, "fold_nif", 0,
                                         fold_nif, 2, next);
            }
        }
    }

    /*
     * Fold complete on this entry — snapshot out before returning.
     * The captured origin pointer commits the final state back to the
     * originating thread's TLS, regardless of which thread we finished
     * on (see file header for the design note).
     */
    st->origin_tls->est = st->est;

    {
        ERL_NIF_TERM hash   = enif_make_uint64(env, st->acc);
        ERL_NIF_TERM stats  = build_stats(env, st);
        ERL_NIF_TERM result = enif_make_tuple2(env, hash, stats);
        if (first_entry)
            enif_release_resource(st);
        return result;
    }
}

/* ------------------------------------------------------------------------- */
/* NIF registration.                                                         */
/* ------------------------------------------------------------------------- */

static ErlNifFunc nif_funcs[] = {
    {"fold_nif", 1, fold_nif, 0}
};

ERL_NIF_INIT(Elixir.AdaptiveFoldPersistent, nif_funcs, load, NULL, NULL, NULL)
