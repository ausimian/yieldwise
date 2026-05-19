defmodule Yieldwise.MixProject do
  use Mix.Project

  @version "0.1.0"
  @source_url "https://github.com/ausimian/yieldwise"

  def project do
    [
      app: :yieldwise,
      version: @version,
      elixir: "~> 1.18",
      start_permanent: Mix.env() == :prod,
      deps: deps(),
      description: description(),
      package: package(),
      source_url: @source_url
    ]
  end

  def application do
    [extra_applications: [:logger]]
  end

  defp deps, do: []

  defp description do
    "yieldwise — adaptive work-chunking for Erlang NIFs (C library; bring your own NIF)."
  end

  defp package do
    [
      licenses: ["MIT"],
      links: %{"GitHub" => @source_url},
      files: ~w(c_src mix.exs README.md LICENSE)
    ]
  end
end
