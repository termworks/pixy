SHELL := /bin/bash

PROJECT_NAME := pixy
PROJECT_VERSION := $(shell sed -nE 's/^local PROJECT_VERSION = "([0-9.]+)".*/\1/p' xmake.lua)

TOP_DIR := $(CURDIR)
CC ?= cc
# The precompiler runs here, so it is built for this machine even when the
# binary is not.
HOSTCC ?= $(CC)
TARGET ?=
MUSL_TARGET ?= $(shell uname -m)-unknown-linux-musl
BUILD := build
ARGS ?=
ROUNDS ?= 1000
PREFIX ?= $(HOME)/.local

HAS_REL := $(shell command -v git-rel 2>/dev/null)

$(info ------------------------------------------)
$(info Project: $(PROJECT_NAME) v$(PROJECT_VERSION))
$(info ------------------------------------------)

WARNINGS := -Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes -Wno-unused-parameter
INCLUDES := -Isrc -Ivendor/lua/src -Ivendor/miniz
DEFINES  := -DLUA_USE_POSIX -D_GNU_SOURCE -DPIXY_VERSION_STRING=\"$(PROJECT_VERSION)\"
CFLAGS_COMMON := $(WARNINGS) $(INCLUDES) $(DEFINES) -std=c11
CFLAGS_DEBUG := $(CFLAGS_COMMON) -g -O0
CFLAGS_RELEASE := $(CFLAGS_COMMON) -O2 -DNDEBUG
CFLAGS_SANITIZE := $(CFLAGS_COMMON) -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer
LDLIBS := -lm
# musl-gcc links dynamically by default; a release binary should need nothing
# on the target machine at all.
LDFLAGS ?=

ifneq ($(TARGET),)
    CC_TARGET := -target $(TARGET)
else
    CC_TARGET :=
endif

PIXY_SRC := $(wildcard src/*.c)
LUA_SRC := $(wildcard vendor/lua/src/*.c)
MINIZ_SRC := vendor/miniz/miniz.c vendor/miniz/miniz_tdef.c vendor/miniz/miniz_tinfl.c
GENERATED := $(BUILD)/lua_modules.c $(BUILD)/texts.c
LUA_MODULES := lua/pixy/style.lua lua/pixy/nodes.lua lua/pixy/layout.lua lua/pixy/ansi.lua \
               lua/pixy/encode.lua lua/pixy/animate.lua lua/pixy/sprite.lua \
               lua/pixy/segments/shell.lua lua/pixy/segments/git.lua \
               lua/pixy/segments/system.lua lua/pixy/segments/progress.lua lua/pixy/init.lua

.PHONY: build b compile c run r test t check fmt fmt-check clean verify smoke smoke-shell bench \
        package-check example-pack release-build release-musl sanitize fuzz bench-compare bench-phases \
        docs-images install release help h

build: $(BUILD)/pixy
b: build
compile: build
c: compile

$(BUILD)/pokemon.pack: scripts/pack_sprites.c $(wildcard docs/assets/pokemon/regular/*) $(wildcard docs/assets/pokemon/shiny/*)
	@mkdir -p $(BUILD)
	@$(CC) -O2 -Ivendor/miniz -o $(BUILD)/pack_sprites scripts/pack_sprites.c $(MINIZ_SRC) -lm
	@$(BUILD)/pack_sprites docs/assets/pokemon $(BUILD)/pokemon.pack

# Compiled here rather than at every prompt: parsing the modules was half the
# cost of starting up, to reach the same functions each time.
$(BUILD)/lua_precompile: scripts/lua_precompile.c $(LUA_SRC)
	@mkdir -p $(BUILD)
	@$(HOSTCC) -O2 -Ivendor/lua/src -Isrc -o $@ $< $(LUA_SRC) $(DEFINES) $(LDLIBS)

$(BUILD)/lua_modules.c: $(LUA_MODULES) $(BUILD)/lua_precompile
	@mkdir -p $(BUILD)
	@$(BUILD)/lua_precompile $@ $(LUA_MODULES)

$(BUILD)/texts.c: lua/pixy/default.lua examples/hexe-oslo.lua examples/shell/init.bash examples/shell/init.zsh \
                  examples/shell/init.fish examples/shell/init.oslo scripts/embed_text.sh
	@mkdir -p $(BUILD)
	@bash scripts/embed_text.sh text $@ \
		PIXY_DEFAULT_CONFIG=lua/pixy/default.lua \
		PIXY_HEXE_OSLO_CONFIG=examples/hexe-oslo.lua \
		PIXY_BASH_INIT=examples/shell/init.bash \
		PIXY_ZSH_INIT=examples/shell/init.zsh \
		PIXY_FISH_INIT=examples/shell/init.fish \
		PIXY_OSLO_INIT=examples/shell/init.oslo

$(BUILD)/pixy: $(PIXY_SRC) $(LUA_SRC) $(MINIZ_SRC) $(GENERATED) $(BUILD)/pokemon.pack
	@mkdir -p $(BUILD)
	@$(CC) $(CC_TARGET) $(CFLAGS_DEBUG) -o $@ $(PIXY_SRC) $(GENERATED) $(LUA_SRC) $(MINIZ_SRC) $(LDLIBS)

release-build: $(PIXY_SRC) $(LUA_SRC) $(MINIZ_SRC) $(GENERATED) $(BUILD)/pokemon.pack
	@mkdir -p $(BUILD)
	@$(CC) $(CC_TARGET) $(CFLAGS_RELEASE) $(LDFLAGS) -o $(BUILD)/pixy $(PIXY_SRC) $(GENERATED) $(LUA_SRC) $(MINIZ_SRC) $(LDLIBS)
	@strip $(BUILD)/pixy 2>/dev/null || true

release-musl:
	@$(MAKE) --no-print-directory release-build TARGET=$(MUSL_TARGET) LDFLAGS="-static $(LDFLAGS)"

# The suite again, against a binary that traps what a release build carries
# silently. Slower, so it is its own target rather than part of `test`.
$(BUILD)/pixy-sanitize: $(PIXY_SRC) $(LUA_SRC) $(MINIZ_SRC) $(GENERATED) $(BUILD)/pokemon.pack
	@mkdir -p $(BUILD)
	@$(CC) $(CC_TARGET) $(CFLAGS_SANITIZE) -o $@ $(PIXY_SRC) $(GENERATED) $(LUA_SRC) $(MINIZ_SRC) $(LDLIBS)

sanitize: $(BUILD)/pixy-sanitize
	@PIXY=$(BUILD)/pixy-sanitize PIXY_BIN=$(BUILD)/pixy-sanitize \
	  ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
	  bash tests/run.sh

fuzz: $(BUILD)/pixy-sanitize
	@ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 \
	  bash tests/fuzz.sh $(BUILD)/pixy-sanitize $(ROUNDS)

run: build
	@$(BUILD)/pixy $(ARGS)
r: run

# Against the optimized binary, because the suite asserts production guarantees:
# the largest sprite renders in 10ms at -O2 and 110ms at -O0, either side of the
# 100ms render deadline, so a debug build fails a bound it was never held to.
# `make build` still produces the debug binary for a debugger.
test: release-build
	@bash tests/run.sh
t: test

check: build
	@$(BUILD)/pixy check

fmt:
	@command -v clang-format >/dev/null && clang-format -i src/*.c src/*.h scripts/*.c || true

fmt-check:
	@command -v clang-format >/dev/null && clang-format --dry-run --Werror src/*.c src/*.h scripts/*.c || true

verify: fmt-check test

smoke: release-build
	@bash scripts/smoke.sh

smoke-shell: release-build
	@bash scripts/smoke_shell.sh

bench: release-build
	@bash scripts/bench.sh

# Against starship, at what a prompt actually costs. Needs the dev shell, which
# pins both it and hyperfine.
bench-compare: release-build
	@bash scripts/bench_compare.sh

# Where a prompt's time goes, so an optimisation can be aimed.
bench-phases: release-build
	@$(BUILD)/pixy __bench phases 400
	@$(BUILD)/pixy __bench compat-phases 400

package-check: release-build example-pack
	@RELEASE_DIR=$(BUILD) bash scripts/package_check.sh

example-pack: release-build
	@$(BUILD)/pixy pack build examples/pack --output $(BUILD)/pixy-example.pixypack --source pixy --license MIT --attribution "Pixy contributors"

docs-images: release-build
	@PIXY=$(BUILD)/pixy bash scripts/docs_images.sh

install: release-build
	@install -Dm755 $(BUILD)/pixy $(PREFIX)/bin/pixy
	@echo "installed $(PREFIX)/bin/pixy"

clean:
	@rm -rf $(BUILD)

release:
	@if [ -z "$(HAS_REL)" ]; then \
		echo "git-rel is not installed. Please install it first."; \
		exit 1; \
	fi
	@if [ -z "$(TYPE)" ]; then \
		echo "Release type not specified. Use 'make release TYPE=[patch|minor|major|M.m.p]'"; \
		exit 1; \
	fi
	@git rel $(TYPE)

help:
	@echo
	@echo "Usage: make [target]"
	@echo
	@echo "  build        Build the debug binary"
	@echo "  release-build Build the optimized binary"
	@echo "  release-musl Build a static musl binary for this arch"
	@echo "  test         Run the test suite"
	@echo "  verify       Format check plus tests"
	@echo "  sanitize     The suite under address and UB sanitizers"
	@echo "  fuzz         Random input at the palette surface"
	@echo "  smoke        Run the CLI smoke"
	@echo "  smoke-shell  Run the shell integration smoke"
	@echo "  bench        Run release performance checks"
	@echo "  bench-compare Compare against starship (needs nix develop)"
	@echo "  bench-phases Where a prompt spends its time"
	@echo "  package-check Check release artifact contents"
	@echo "  example-pack Build the example sprite pack"
	@echo "  docs-images  Regenerate the README frames from live output"
	@echo "  install      Install into $(PREFIX)/bin"
	@echo "  release      Release a new version"
	@echo
h: help
