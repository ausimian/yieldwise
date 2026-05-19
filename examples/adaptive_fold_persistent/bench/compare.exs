# Two-section illustrative bench for AdaptiveFoldPersistent.
#
# Section 1 — chunk-size guesstimate quality. Within
# AdaptiveFoldPersistent, the very first call lands on a scheduler
# whose TLS slot is cold (seeded at 0.05 µs/byte → first chunk
# ~2 300 bytes), while subsequent calls on the same scheduler open
# with the converged estimate (~0.001 µs/byte → first chunk
# ~150 000 bytes). That ratio *is* the guesstimate improvement
# from persistence.
#
# Section 2 — wall-clock impact across input sizes. Compares the
# basic adaptive_fold example (re-converges from cold every call)
# against the persistent one (warm seed) on a few input sizes. The
# fold itself is very cheap (~1 ns/byte), so wall-clock wins from
# the bigger chunks tend to sit inside timing noise here — the
# meaningful improvement is the chunk-size guesstimate itself, in
# Section 1. NIFs with heavier per-chunk overhead or per-byte cost
# would see proportionally larger wall-clock wins from the warm
# seed.
#
# Run with:
#
#   cd examples/adaptive_fold_persistent
#   mix run bench/compare.exs
#
# Single-shot readings; run a few times for variance.

defmodule Compare do
  @input_sizes [
    {"4 KiB", 4 * 1024},
    {"64 KiB", 64 * 1024},
    {"1 MiB", 1 * 1024 * 1024},
    {"4 MiB", 4 * 1024 * 1024},
    {"32 MiB", 32 * 1024 * 1024}
  ]

  def run do
    section1()
    section2()
  end

  defp section1 do
    IO.puts("")
    IO.puts("Section 1 — chunk-size guesstimate quality")
    IO.puts("(AdaptiveFoldPersistent, 1 MiB binary)")
    IO.puts("")
    IO.puts("  call         first_chunk_size  total_chunks")
    IO.puts("  ───────────  ────────────────  ────────────")

    bin = :binary.copy(<<0xAB>>, 1 * 1024 * 1024)

    # First call lands on a cold scheduler slot (assuming this is
    # the first NIF call on this scheduler in this BEAM instance).
    {_, cold} = AdaptiveFoldPersistent.fold_with_stats(bin)
    IO.puts(row1("cold (1st)", cold.first_chunk_size, cold.total_chunks))

    # Subsequent call reads the converged seed.
    {_, warm} = AdaptiveFoldPersistent.fold_with_stats(bin)
    IO.puts(row1("warm (2nd)", warm.first_chunk_size, warm.total_chunks))

    IO.puts("")
  end

  defp section2 do
    IO.puts("Section 2 — wall-clock per call (basic vs persistent)")
    IO.puts("")
    IO.puts("  input     basic_µs    pers_µs   speedup")
    IO.puts("  ────────  ────────   ────────   ───────")

    # Make sure both NIFs are loaded and the persistent variant's
    # TLS is warm before timing. The very first call to a NIF in a
    # BEAM instance can pay a one-shot load cost that would skew the
    # smallest-input row.
    warm = :binary.copy(<<0xAB>>, 1 * 1024 * 1024)
    _ = AdaptiveFold.fold(warm)
    _ = AdaptiveFoldPersistent.fold(warm)

    Enum.each(@input_sizes, fn {label, n} ->
      bin = :binary.copy(<<0xAB>>, n)

      {basic_us, _} = :timer.tc(fn -> AdaptiveFold.fold(bin) end)
      {pers_us, _} = :timer.tc(fn -> AdaptiveFoldPersistent.fold(bin) end)

      speedup_str =
        cond do
          pers_us <= 0 -> "   —   "
          basic_us / pers_us >= 100 -> ">100x  "
          true -> :io_lib.format("~5.2fx", [basic_us / pers_us]) |> IO.iodata_to_binary()
        end

      IO.puts(row2(label, basic_us, pers_us, speedup_str))
    end)

    IO.puts("")
  end

  defp row1(label, fcs, tc) do
    [
      "  ",
      String.pad_trailing(label, 11),
      "  ",
      String.pad_leading(format_int(fcs), 16),
      "  ",
      String.pad_leading(format_int(tc), 12)
    ]
  end

  defp row2(label, basic, pers, speedup) do
    [
      "  ",
      String.pad_trailing(label, 8),
      "  ",
      String.pad_leading(format_int(basic), 8),
      "   ",
      String.pad_leading(format_int(pers), 8),
      "   ",
      speedup
    ]
  end

  # Group digits with thin separators so 150_000 reads at a glance.
  defp format_int(n) when is_integer(n) do
    n
    |> Integer.to_string()
    |> String.reverse()
    |> String.graphemes()
    |> Enum.chunk_every(3)
    |> Enum.map_join(",", &Enum.join/1)
    |> String.reverse()
  end
end

Compare.run()
