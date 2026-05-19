defmodule AdaptiveFoldPersistent.MixProject do
  use Mix.Project

  @version "0.1.0"

  def project do
    [
      app: :adaptive_fold_persistent,
      version: @version,
      elixir: "~> 1.18",
      compilers: [:elixir_make | Mix.compilers()],
      make_targets: ["all"],
      make_clean: ["clean"],
      make_makefile: makefile_for_os(),
      make_env: &make_env/0,
      start_permanent: Mix.env() == :prod,
      deps: deps()
    ]
  end

  def application do
    [extra_applications: [:logger]]
  end

  defp deps do
    [
      {:yieldwise, path: "../.."},
      {:elixir_make, "~> 0.8", runtime: false},
      # Sibling example, pulled in only for bench/compare.exs so it
      # can put the two NIFs side-by-side on the same input. Not
      # needed at test or runtime.
      {:adaptive_fold, path: "../adaptive_fold", only: :dev}
    ]
  end

  # Expose the yieldwise dep's source directory to the Makefile, so the
  # example compiles its own copy of yieldwise.c alongside its own NIF.
  defp make_env do
    yieldwise_dir =
      case Mix.Project.deps_paths() do
        %{yieldwise: path} -> path
        _ -> "../.."
      end

    %{"YIELDWISE_DIR" => yieldwise_dir}
  end

  defp makefile_for_os do
    case :os.type() do
      {:win32, _} -> "Makefile.win"
      _ -> "Makefile"
    end
  end
end
