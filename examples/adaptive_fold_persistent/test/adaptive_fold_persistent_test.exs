defmodule AdaptiveFoldPersistentTest do
  use ExUnit.Case, async: false

  import Bitwise

  @mask 0xFFFFFFFFFFFFFFFF
  @prime 0x100000001B3
  @offset 0xCBF29CE484222325

  # Pure-Elixir reference implementation of the same mixing fold the
  # NIF performs. Used as the oracle in correctness tests.
  defp reference_fold(bin) when is_binary(bin) do
    do_ref(bin, @offset)
  end

  defp do_ref(<<>>, acc), do: acc

  defp do_ref(<<b, rest::binary>>, acc) do
    acc = bxor(acc, b)
    acc = acc * @prime &&& @mask
    do_ref(rest, acc)
  end

  describe "correctness" do
    test "matches the reference fold on small inputs" do
      for bin <- [<<>>, <<0>>, <<"hello">>, <<"the quick brown fox">>] do
        assert AdaptiveFoldPersistent.fold(bin) == reference_fold(bin)
      end
    end

    test "matches the reference fold on a moderate binary" do
      bin = :crypto.strong_rand_bytes(256 * 1024)
      assert AdaptiveFoldPersistent.fold(bin) == reference_fold(bin)
    end

    test "fold_with_stats returns the same hash plus a stats map" do
      bin = <<"hello world">>
      {hash, stats} = AdaptiveFoldPersistent.fold_with_stats(bin)
      assert hash == reference_fold(bin)
      assert is_map(stats)
      assert Map.has_key?(stats, :first_chunk_size)
      assert Map.has_key?(stats, :item_cost_us)
      assert Map.has_key?(stats, :variance)
    end
  end

  describe "TLS persistence" do
    test "snapshot-out commits learnt state to the origin slot" do
      bin = :binary.copy(<<0xAB>>, 1 * 1024 * 1024)

      {_hash, %{item_cost_us: cost, variance: var}} =
        AdaptiveFoldPersistent.fold_with_stats(bin)

      # Cold seed is 0.05 µs/byte with initial stddev = 0.5 * x → variance
      # ~0.000625. After a megabyte's worth of chunks the filter has
      # plenty of evidence to converge downward and narrow uncertainty,
      # so both quantities should be meaningfully smaller than their
      # initial values.
      assert cost < 0.05,
             "expected item_cost_us to converge below the cold seed (0.05); got #{cost}"

      assert var < 0.000625,
             "expected variance to narrow below the cold initial (~0.000625); got #{var}"
    end

    test "at least one call observes a TLS-warmed first chunk" do
      bin = :binary.copy(<<0xAB>>, 1 * 1024 * 1024)

      sizes =
        for _ <- 1..5 do
          {_hash, %{first_chunk_size: size}} =
            AdaptiveFoldPersistent.fold_with_stats(bin)

          size
        end

      # The cold seed (0.05 µs/byte) gives a first chunk around 2300;
      # a TLS-bootstrapped converged estimator (~0.001 µs/byte) gives
      # one north of 100 k. We assert that *at least one* call lands
      # in the warm range. We can't insist all five do, because BEAM
      # may work-steal the test process between calls onto a
      # scheduler thread whose TLS was seeded by an earlier
      # tiny-input test (whose measurements were dominated by per-NIF
      # overhead and converged to a pessimistic cost). The persistence
      # claim is satisfied by *any* call seeing the warm seed — that's
      # impossible without origin-pinned snapshot in/out working.
      assert Enum.any?(sizes, fn s -> s > 10_000 end),
             "expected TLS persistence to produce at least one warm first chunk " <>
               "(cold seed ~2300, converged ≫ 100k); got #{inspect(sizes)}"
    end
  end
end
