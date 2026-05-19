defmodule AdaptiveFoldTest do
  use ExUnit.Case, async: false

  import Bitwise

  @mask 0xFFFFFFFFFFFFFFFF
  @prime 0x100000001B3
  @offset 0xCBF29CE484222325

  # Pure-Elixir reference implementation of the same mixing fold the NIF
  # performs. Used as the oracle in correctness tests.
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
        assert AdaptiveFold.fold(bin) == reference_fold(bin)
      end
    end

    test "matches the reference fold on a moderate binary" do
      # 256 KiB — small enough that the pure-Elixir oracle finishes in a
      # reasonable wall-clock, large enough to exercise multiple chunks.
      bin = :crypto.strong_rand_bytes(256 * 1024)
      assert AdaptiveFold.fold(bin) == reference_fold(bin)
    end

    test "is deterministic across invocations" do
      bin = :crypto.strong_rand_bytes(64 * 1024)
      a = AdaptiveFold.fold(bin)
      b = AdaptiveFold.fold(bin)
      assert a == b
    end
  end

  describe "scheduler friendliness" do
    test "no single invocation exceeds the 10ms long_schedule threshold" do
      # 32 MiB — large enough to force many reschedules on any modern CPU.
      bin = :binary.copy(<<0xAB>>, 32 * 1024 * 1024)

      # Drain any leftover long_schedule messages from earlier tests.
      flush_long_schedule()

      # Install the monitor, run, immediately uninstall.
      :erlang.system_monitor(self(), [{:long_schedule, 10}])
      try do
        _ = AdaptiveFold.fold(bin)
      after
        :erlang.system_monitor(self(), [])
      end

      offenders = collect_long_schedule([])
      assert offenders == [],
             "saw long_schedule messages (≥10 ms native execution): #{inspect(offenders)}"
    end
  end

  defp flush_long_schedule do
    receive do
      {:monitor, _pid, :long_schedule, _info} -> flush_long_schedule()
    after
      0 -> :ok
    end
  end

  defp collect_long_schedule(acc) do
    receive do
      {:monitor, _pid, :long_schedule, info} ->
        collect_long_schedule([info | acc])
    after
      0 -> Enum.reverse(acc)
    end
  end
end
