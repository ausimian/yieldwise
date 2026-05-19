defmodule AdaptiveFoldPersistent do
  @moduledoc """
  Worked example for the **yieldwise** toolkit — variant that persists
  the learnt per-item-cost estimator across NIF invocations.

  Same fold semantics as `AdaptiveFold` (FNV-1a-style mixing hash over
  every byte of a binary), but the Kalman-filtered estimator is
  bootstrapped from — and conditionally committed back to — a
  `_Thread_local` slot on the scheduler thread the fold *started*
  on.

  The result: the first call seeds the slot from a deliberately
  pessimistic 0.05 µs/byte and converges over its chunk loop; later
  calls on the same scheduler open with the converged estimate and
  pick a much larger first chunk, skipping the warm-up cost.

  Pattern is *snapshot in / commit on same-thread completion*:

    * on first entry the NIF captures the current thread's TLS
      pointer and copies its `yw_estimator` into the per-fold
      resource;
    * the resource's copy is what the chunk loop mutates (so a
      reschedule that resumes on a different scheduler thread keeps
      using the same estimator, exactly as in the basic example);
    * at fold completion the NIF re-resolves the current thread's
      TLS pointer and compares it against the captured one. If they
      match (the fold finished where it started), the final state
      is committed back. If they differ (mid-fold scheduler
      migration), the update is silently dropped — measurements
      taken across multiple threads would skew either thread's
      future estimates more than dropping the data point hurts.

  See `fold_with_stats/1` for an instrumented form that returns the
  first-chunk size and the post-fold estimator state — useful for
  observing convergence across calls.
  """

  @on_load :load_nif

  @doc false
  def load_nif do
    path = :filename.join(:code.priv_dir(:adaptive_fold_persistent), ~c"adaptive_fold_persistent")
    :erlang.load_nif(path, 0)
  end

  @doc """
  Fold a mixing hash over every byte of `bin`. Returns a 64-bit integer.
  """
  @spec fold(binary()) :: non_neg_integer()
  def fold(bin) when is_binary(bin) do
    {hash, _stats} = fold_nif(bin)
    hash
  end

  @doc """
  Fold and also return the instrumentation map:

      %{
        first_chunk_size: non_neg_integer(),
        item_cost_us:     float(),
        variance:         float()
      }

  `first_chunk_size` is the chunk size yieldwise picked for the very
  first chunk of this fold — a useful proxy for "how warm was the
  estimator when this call started". `item_cost_us` and `variance`
  are the post-fold estimator state, which is what subsequent calls
  on the same scheduler thread will see as their seed.
  """
  @spec fold_with_stats(binary()) :: {non_neg_integer(), map()}
  def fold_with_stats(bin) when is_binary(bin), do: fold_nif(bin)

  # NIF stub — replaced when the shared library loads.
  defp fold_nif(_bin), do: :erlang.nif_error(:nif_not_loaded)
end
