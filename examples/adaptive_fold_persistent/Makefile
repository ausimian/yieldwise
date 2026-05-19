# Unix Makefile — invoked by elixir_make as part of `mix compile`.
#
# Builds priv/adaptive_fold_persistent.{so,dylib}. C11 is required for
# _Thread_local, which this example uses to persist learnt estimator
# state across NIF invocations.

PRIV_DIR      ?= priv
C_SRC_DIR     ?= c_src
YIELDWISE_DIR ?= ../..
YIELDWISE_C   := $(YIELDWISE_DIR)/c_src

NIF_SOURCES := $(YIELDWISE_C)/yieldwise.c \
               $(C_SRC_DIR)/example_adaptive_fold_persistent.c

NIF_OUT     := $(PRIV_DIR)/adaptive_fold_persistent.so

# Discover the ERTS include directory from the running `erl`. We construct
# the path from code:root_dir/0 + the erts version, because the alternative
# (`code:lib_dir(erts, include)`) walks under `lib/erts-N.M/include`, which
# is wrong on some OTP installs where erts-N.M lives at the OTP root.
ERL_INCLUDE := $(shell erl -noshell -eval \
    'io:format("~s/erts-~s/include",[code:root_dir(),erlang:system_info(version)]),halt().')

CFLAGS  ?= -O3 -Wall -Wextra -std=c11 -fPIC
CFLAGS  += -I"$(ERL_INCLUDE)" -I$(YIELDWISE_C) -I$(C_SRC_DIR)

UNAME_S := $(shell uname -s)

ifeq ($(UNAME_S),Darwin)
    LDFLAGS ?= -undefined dynamic_lookup -shared
    LDLIBS  ?=
else
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
