# yieldwise

Adaptive work-chunking for Erlang NIFs.

A long-running NIF that processes its input in a single call will block its
scheduler thread, hurt fairness for every other process on that scheduler, and
trip BEAM's `long_schedule` monitor. The well-known fix is to split the work
into chunks and `enif_schedule_nif` between them — but picking the chunk size
is awkward: too small and the per-chunk overhead dominates, too large and you
overshoot the timeslice.

yieldwise picks the chunk size for you. It is a small C library — no globals,
no resource type of its own, no load-time hooks — that runs a Kalman filter
over the per-item cost of your work and sizes each chunk against a wall-clock
budget. You embed two doubles in your own resource and it does the rest.

Two layers are provided:

- **Toolkit** (`yieldwise.h`) — six small functions you call by hand around
  your own chunk loop. Full control; you own the `enif_schedule_nif`.
- **Driver** (`yieldwise_driver.h`) — one entry point, `yw_run`, that owns
  the loop, the timer, the timeslice report, and the reschedule. You supply
  three callbacks.

## Installation

Add yieldwise to your `mix.exs` deps:

```elixir
def deps do
  [
    {:yieldwise, "~> 0.1"},
    {:elixir_make, "~> 0.8", runtime: false}
  ]
end
```

yieldwise ships C source only — there is no precompiled artifact and no
Elixir code. Your NIF compiles the yieldwise sources alongside its own. Wire
`elixir_make` into your `project/0` and use `make_env:` to pass the dep's
location to your Makefile:

```elixir
def project do
  [
    app: :your_app,
    # ...
    compilers: [:elixir_make | Mix.compilers()],
    make_env: &make_env/0,
    deps: deps()
  ]
end

defp make_env do
  %{"YIELDWISE_DIR" => Mix.Project.deps_paths()[:yieldwise]}
end
```

`make_env:` accepts a 0-arity function so the path is resolved at build time
(after deps are fetched). Then in your `Makefile`:

```make
YIELDWISE_C := $(YIELDWISE_DIR)/c_src
NIF_SOURCES := $(YIELDWISE_C)/yieldwise.c \
               $(YIELDWISE_C)/yieldwise_driver.c \
               $(C_SRC_DIR)/your_nif.c
CFLAGS += -I$(YIELDWISE_C)
```

`yieldwise_driver.c` is only needed if you use `yw_run`; if you call the
toolkit directly, omit it.

See `examples/adaptive_fold/` in the repository for a complete worked example
(an FNV-1a-style mixing fold over a binary). `examples/adaptive_fold_persistent/`
is a sibling that demonstrates persisting the learnt estimator across NIF calls
via origin-pinned thread-local storage — useful when the same NIF is invoked
repeatedly and you want the second-call onward to skip the convergence cost.

## Toolkit (`yieldwise.h`)

The toolkit is three POD types and six functions.

### `yw_config`

```c
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
```

Tuning knobs. Stateless POD — rebuild it from constants on each NIF entry,
never round-trip it through a resource. Get defaults from
`yw_config_default(target_us)`.

| Field             | Default                     | Meaning                                                                                                                                                                                                       |
| ----------------- | --------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `target_us`       | 200.0 (or your override)    | Wall-clock budget per chunk, in µs. Lower → more reschedules and lower tail latency, more per-chunk overhead. Higher → fewer reschedules and higher throughput, more risk of tripping `long_schedule`.        |
| `q_drift`         | 0.05                        | Kalman process noise: how much per-item cost is allowed to drift between chunks. Higher → filter tracks shifts (e.g. cache regime changes) faster, but is noisier. Lower → smoother but slower to adapt.      |
| `sigma_timer_us`  | 2.0                         | Stddev of timer jitter on the whole-chunk timing, in µs. Contributes `σ²/n²` to measurement noise. Lower → trust the timer more, accept the measurement quickly. Reasonable values track your platform timer. |
| `sigma_item_us`   | 0.0                         | Stddev of genuine per-item cost variability (heterogeneous work), in µs. Contributes `σ²/n` to measurement noise. Default `0.0` assumes uniform items; raise it if individual items vary in cost.             |
| `beta`            | 1.5                         | Pessimism factor in chunk sizing. The size is computed against `cost + beta·sd` rather than `cost` alone. Higher → smaller, more conservative chunks. Lower → bigger chunks, more chance of overshoot.        |
| `outlier_sigma`   | 3.0                         | One-sided gate for slow-chunk outliers, in sigmas. A chunk whose innovation is positive and exceeds `outlier_sigma · sqrt(S)` is rejected (variance widens, mean held). Lower → reject more; higher → fewer.  |
| `cross_yield_q`   | 10 · `q_drift` (i.e. 0.5)   | Variance bump applied at each yield by `yw_estimator_cross_yield`. Higher → more cautious first chunk after resume (helpful when the scheduler thread changes). Lower → trust pre-yield state more.           |
| `min_chunk`       | 1                           | Hard floor on chunk size. Raise if your per-chunk overhead dominates at tiny sizes.                                                                                                                           |
| `max_chunk`       | 16 Mi (1 << 24)             | Hard ceiling on chunk size. Lower if your items are very cheap and the filter could otherwise pick a chunk that exceeds the timeslice budget by a wide margin.                                                |

The defaults are tuned for "many cheap items, ~µs each" workloads, which is
the case yieldwise was designed for. For heavier per-item work (e.g. hashes,
parsing) the defaults still work — the filter converges in two or three
chunks regardless — but you may want to raise `min_chunk` and/or shrink
`max_chunk` to bracket the steady state.

### `yw_estimator`

```c
typedef struct {
    double item_cost_us;   /* x: estimated cost per item, in µs  */
    double variance;       /* P: uncertainty of that estimate    */
} yw_estimator;
```

The Kalman filter state. Two doubles. **Embed this in your resource** so it
survives reschedules — everything else (config, timer) is rebuilt fresh on
each NIF entry, but the estimator must carry over.

- `item_cost_us` — running mean estimate of per-item cost. After
  `yw_estimator_init(est, cfg, cost_guess_us)` it is `max(cost_guess_us,
  1 ns)`. `yw_chunk_done` moves it toward the observed `elapsed/n` at each
  non-rejected chunk; the gain depends on the relative size of the prior
  variance and the chunk's measurement noise.
- `variance` — uncertainty of the estimate. After `yw_estimator_init` it is
  `(0.5 · cost_guess_us)²` — i.e. ~50% relative stddev. Each chunk it
  shrinks by `(1 - K)·(P + q_drift)` on accept, or widens by `q_drift` on
  reject. `yw_estimator_cross_yield` adds `cfg->cross_yield_q`.

Direct manipulation of these fields is not part of the API. The only
supported entry points are `yw_estimator_init` and the loop helpers below.

### `yw_timer`

```c
typedef struct {
    ErlNifTime start;
} yw_timer;
```

Transient µs timer over `enif_monotonic_time`. Stack-local only — never
store one in a resource.

### `yw_config_default`

```c
yw_config yw_config_default(double target_us);
```

Build a config populated with sensible defaults. Pass `target_us <= 0` to get
the default 200 µs per-chunk budget; otherwise the value you pass is used.
The config is stateless: rebuild it from constants on each NIF entry — never
round-trip it through a resource.

### `yw_estimator_init`

```c
void yw_estimator_init(yw_estimator *est,
                       const yw_config *cfg,
                       double cost_guess_us);
```

Seed the Kalman filter. `cost_guess_us` must be deliberately **pessimistic**
(high) — the filter converges downward toward the true cost. The initial
variance is set wide (~50%-relative stddev) so the first few chunks stay
conservative until evidence narrows it.

The estimator is two doubles and **must survive reschedules**: embed it in
your resource alongside the rest of your work state.

### `yw_estimator_cross_yield`

```c
void yw_estimator_cross_yield(yw_estimator *est, const yw_config *cfg);
```

Widen the estimator's variance by `cfg->cross_yield_q` to reflect the
staleness of resuming on a (possibly different) scheduler thread with cold
caches. Call once at each yield, just before `enif_schedule_nif`.

### `yw_timer_start`

```c
void yw_timer_start(yw_timer *t);
```

Start a stack-local µs timer over `enif_monotonic_time`. Never store a timer
in a resource — it is transient by design.

### `yw_timer_elapsed_us`

```c
double yw_timer_elapsed_us(const yw_timer *t);
```

Return the µs elapsed since `yw_timer_start`.

### `yw_next_chunk`

```c
size_t yw_next_chunk(const yw_estimator *est,
                     const yw_config *cfg,
                     size_t items_remaining);
```

Return the size of the next chunk. Internally:

    pess = est->item_cost_us + cfg->beta * sqrt(est->variance)
    n    = clamp(cfg->target_us / pess, cfg->min_chunk, cfg->max_chunk)
    n    = min(n, items_remaining)

Returns 0 only when `items_remaining` is 0.

### `yw_chunk_done`

```c
int yw_chunk_done(ErlNifEnv *env,
                  yw_estimator *est,
                  const yw_config *cfg,
                  size_t n,
                  double elapsed_us);
```

Call once after each chunk. Performs two things:

1. **Kalman update** of `est` using the per-item cost implied by the chunk
   (`elapsed_us / n`). A one-sided outlier gate (`cfg->outlier_sigma`) drops
   suspiciously slow chunks — those almost certainly mean the OS preempted
   us, not that the work got harder. Rejected chunks still widen the
   variance.
2. **Timeslice report**: `enif_consume_timeslice(env, percent)` where
   `percent = round(elapsed_us / 10)` clamped to `[1, 100]` (1 ms ≈ 100%).

Returns non-zero iff the timeslice is spent and the caller should yield via
`enif_schedule_nif` as soon as it can save its state.

### The hand-rolled chunk loop

Putting it together:

```c
ERL_NIF_TERM my_nif(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[])
{
    yw_config cfg = yw_config_default(200.0);
    my_state *st = /* alloc-or-fetch from resource, init est on first entry */;

    while (st->pos < st->total) {
        size_t   n = yw_next_chunk(&st->est, &cfg, st->total - st->pos);
        yw_timer t;
        yw_timer_start(&t);

        /* process exactly n items into st */
        do_work(st, n);

        if (yw_chunk_done(env, &st->est, &cfg, n, yw_timer_elapsed_us(&t))) {
            yw_estimator_cross_yield(&st->est, &cfg);
            ERL_NIF_TERM next[2] = { argv[0], state_term };
            return enif_schedule_nif(env, "my_nif", 0, my_nif, 2, next);
        }
    }
    return /* build result from st */;
}
```

## Driver (`yieldwise_driver.h`)

If you do not need to do anything between chunks (poll a cancel flag, send
progress messages, re-tune `cfg`), the driver removes the entire skeleton.
It adds one enum, one struct, and one function.

### `yw_status`

```c
typedef enum {
    YW_CONTINUE = 0,
    YW_FAILED   = 1
} yw_status;
```

Return code from the `do_chunk` callback. `YW_CONTINUE` means "more to do,
or done cleanly"; `YW_FAILED` aborts the run and jumps straight to `finish`.

### `yw_work_vtable`

```c
typedef struct {
    yw_status    (*do_chunk)(void *work, size_t n, size_t *processed);
    size_t       (*remaining)(void *work);
    ERL_NIF_TERM (*finish)(ErlNifEnv *env, void *work);
} yw_work_vtable;
```

The work-shaped callback vtable. The driver only sees the work through an
opaque `void *`; the client knows the concrete type.

- `do_chunk` processes up to `n` items, advances `work`, and writes the count
  actually processed into `*processed`. Returns `YW_CONTINUE` normally, or
  `YW_FAILED` to abort. **It must loop `n` times internally** — the driver
  calls it once per chunk, not once per item, to keep the indirect call off
  the hot path.
- `remaining` returns the items still to process; 0 means done.
- `finish` builds the result term. Called exactly once at the end of the run,
  whether completion or abort. It inspects `work` (which the client populates
  with success or error state) to decide what to produce.

The driver owns no resource type. The client still allocates the resource,
embeds `est`, initialises it on first entry, and builds `state_term` — these
are not on the per-yield hot path and they need the client's concrete type
anyway. There is intentionally no "between chunks" hook; if you need one,
use the toolkit directly.

### `yw_run`

```c
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
```

Drives the work forward through one timeslice. On completion or abort it
returns `vt->finish(env, work)`. On a yield it widens the estimator's
variance, reschedules `nif_fp` via `enif_schedule_nif` with
`argv = {input_term, state_term}`, and returns its result.

## License

MIT — see `LICENSE`. Copyright © 2026 Nick Gunn.
