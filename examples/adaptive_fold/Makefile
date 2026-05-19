# Unix Makefile — invoked by elixir_make as part of `mix compile`.
#
# Discovers the ERTS include directory from the running `erl`, picks
# per-OS link flags (no source-level #ifdef in the C sources), and
# produces priv/adaptive_fold.{so,dylib-extension-is-still-so}.
#
# YIELDWISE_DIR is passed in by the example's mix.exs and points at the
# yieldwise dep's source tree (path dep in this repo, deps/yieldwise/ for
# a real Hex client). The example compiles its own copy of the yieldwise
# C sources alongside its own NIF — that's the intended usage pattern.

PRIV_DIR      ?= priv
C_SRC_DIR     ?= c_src
YIELDWISE_DIR ?= ../..
YIELDWISE_C   := $(YIELDWISE_DIR)/c_src

NIF_SOURCES := $(YIELDWISE_C)/yieldwise.c \
               $(YIELDWISE_C)/yieldwise_driver.c \
               $(C_SRC_DIR)/example_adaptive_fold.c

NIF_OUT     := $(PRIV_DIR)/adaptive_fold.so

# Discover the ERTS include directory from the running `erl`. We construct
# the path from code:root_dir/0 + the erts version, because the alternative
# (`code:lib_dir(erts, include)`) walks under `lib/erts-N.M/include`, which
# is wrong on some OTP installs where erts-N.M lives at the OTP root.
ERL_INCLUDE := $(shell erl -noshell -eval \
    'io:format("~s/erts-~s/include",[code:root_dir(),erlang:system_info(version)]),halt().')

CFLAGS  ?= -O3 -Wall -Wextra -std=c99 -fPIC
CFLAGS  += -I"$(ERL_INCLUDE)" -I$(YIELDWISE_C) -I$(C_SRC_DIR)

UNAME_S := $(shell uname -s)

ifeq ($(UNAME_S),Darwin)
    # macOS: NIFs ship as .so (not .dylib). enif_* symbols are filled in
    # by the host VM at load time — leave them undefined here.
    LDFLAGS ?= -undefined dynamic_lookup -shared
    LDLIBS  ?=
else
    # Linux / other ELF unices.
    LDFLAGS ?= -shared
    LDLIBS  ?= -lm
endif

.PHONY: all clean

all: $(NIF_OUT)

$(NIF_OUT): $(NIF_SOURCES) | $(PRIV_DIR)
	$(CC) $(CFLAGS) $(LDFLAGS) $(NIF_SOURCES) -o $@ $(LDLIBS)

$(PRIV_DIR):
	mkdir -p $(PRIV_DIR)

clean:
	rm -f $(NIF_OUT)
