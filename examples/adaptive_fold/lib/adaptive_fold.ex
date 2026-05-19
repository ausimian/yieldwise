defmodule AdaptiveFold do
  @moduledoc """
  Worked example for the **yieldwise** toolkit.

  `fold/1` folds an FNV-1a-style mixing hash over every byte of a
  binary, in scheduler-friendly chunks. To callers it looks like an
  ordinary synchronous NIF call:

      iex> AdaptiveFold.fold(<<"hello">>)
      11831194018420276491

  Internally the NIF reschedules itself transparently whenever its
  BEAM timeslice is spent — large inputs do not block a scheduler.
  """

  @on_load :load_nif

  @doc false
  def load_nif do
    # Load by path *without* a file extension — ERTS appends .so / .dll
    # per platform. `:code.priv_dir/1` resolves to the app's priv/ dir.
    path = :filename.join(:code.priv_dir(:adaptive_fold), ~c"adaptive_fold")
    :erlang.load_nif(path, 0)
  end

  @doc """
  Fold a mixing hash over every byte of `bin`. Returns a 64-bit integer.
  """
  @spec fold(binary()) :: non_neg_integer()
  def fold(bin) when is_binary(bin), do: fold_nif(bin)

  # NIF stub — replaced when the shared library loads.
  defp fold_nif(_bin), do: :erlang.nif_error(:nif_not_loaded)
end
