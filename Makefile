SHELL := /bin/bash

PROJECT_NAME := pixy
PROJECT_VERSION := $(shell cat VERSION 2>/dev/null || echo 0.0.0)

TOP_DIR := $(CURDIR)
CC ?= cc
TARGET ?=
MUSL_TARGET ?= $(shell uname -m)-unknown-linux-musl
BUILD := build
ARGS ?=
PREFIX ?= $(HOME)/.local

HAS_REL := $(shell command -v git-rel 2>/dev/null)

$(info ------------------------------------------)
$(info Project: $(PROJECT_NAME) v$(PROJECT_VERSION))
$(info ------------------------------------------)

WARNINGS := -Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes -Wno-unused-parameter
INCLUDES := -Icsrc -Ivendor/lua/src -Ivendor/miniz
DEFINES  := -DLUA_USE_POSIX -D_GNU_SOURCE -DPIXY_VERSION_STRING=\"$(PROJECT_VERSION)\"
CFLAGS_COMMON := $(WARNINGS) $(INCLUDES) $(DEFINES) -std=c11
CFLAGS_DEBUG := $(CFLAGS_COMMON) -g -O0
CFLAGS_RELEASE := $(CFLAGS_COMMON) -O2 -DNDEBUG
LDLIBS := -lm
# musl-gcc links dynamically by default; a release binary should need nothing
# on the target machine at all.
LDFLAGS ?=

ifneq ($(TARGET),)
    CC_TARGET := -target $(TARGET)
else
    CC_TARGET :=
endif

PIXY_SRC := $(wildcard csrc/*.c)
LUA_SRC := $(wildcard vendor/lua/src/*.c)
MINIZ_SRC := vendor/miniz/miniz.c vendor/miniz/miniz_tdef.c vendor/miniz/miniz_tinfl.c
GENERATED := $(BUILD)/lua_modules.c $(BUILD)/texts.c
LUA_MODULES := lua/pixy/style.lua lua/pixy/nodes.lua lua/pixy/layout.lua lua/pixy/ansi.lua \
               lua/pixy/encode.lua lua/pixy/animate.lua lua/pixy/sprite.lua \
               lua/pixy/segments/shell.lua lua/pixy/segments/git.lua \
               lua/pixy/segments/system.lua lua/pixy/segments/progress.lua lua/pixy/init.lua

.PHONY: build b compile c run r test t check fmt fmt-check clean verify smoke smoke-shell bench \
        package-check example-pack release-build release-musl docs-images install release help h

build: $(BUILD)/pixy
b: build
compile: build
c: compile

$(BUILD)/pokemon.pack: tools/pack_sprites.c $(wildcard assets/pokemon/regular/*) $(wildcard assets/pokemon/shiny/*)
	@mkdir -p $(BUILD)
	@$(CC) -O2 -Ivendor/miniz -o $(BUILD)/pack_sprites tools/pack_sprites.c $(MINIZ_SRC) -lm
	@$(BUILD)/pack_sprites assets/pokemon $(BUILD)/pokemon.pack

$(BUILD)/lua_modules.c: $(LUA_MODULES) tools/embed_text.sh
	@mkdir -p $(BUILD)
	@bash tools/embed_text.sh modules $@ $(LUA_MODULES)

$(BUILD)/texts.c: lua/pixy/default.lua examples/hexe-oslo.lua shell/init.bash shell/init.zsh \
                  shell/init.fish shell/init.oslo tools/embed_text.sh
	@mkdir -p $(BUILD)
	@bash tools/embed_text.sh text $@ \
		PIXY_DEFAULT_CONFIG=lua/pixy/default.lua \
		PIXY_HEXE_OSLO_CONFIG=examples/hexe-oslo.lua \
		PIXY_BASH_INIT=shell/init.bash \
		PIXY_ZSH_INIT=shell/init.zsh \
		PIXY_FISH_INIT=shell/init.fish \
		PIXY_OSLO_INIT=shell/init.oslo

$(BUILD)/pixy: $(PIXY_SRC) $(LUA_SRC) $(MINIZ_SRC) $(GENERATED) $(BUILD)/pokemon.pack
	@mkdir -p $(BUILD)
	@$(CC) $(CC_TARGET) $(CFLAGS_DEBUG) -o $@ $(PIXY_SRC) $(GENERATED) $(LUA_SRC) $(MINIZ_SRC) $(LDLIBS)

release-build: $(PIXY_SRC) $(LUA_SRC) $(MINIZ_SRC) $(GENERATED) $(BUILD)/pokemon.pack
	@mkdir -p $(BUILD)
	@$(CC) $(CC_TARGET) $(CFLAGS_RELEASE) $(LDFLAGS) -o $(BUILD)/pixy $(PIXY_SRC) $(GENERATED) $(LUA_SRC) $(MINIZ_SRC) $(LDLIBS)
	@strip $(BUILD)/pixy 2>/dev/null || true

release-musl:
	@$(MAKE) --no-print-directory release-build TARGET=$(MUSL_TARGET) LDFLAGS="-static $(LDFLAGS)"

run: build
	@$(BUILD)/pixy $(ARGS)
r: run

test: build
	@bash tests/run.sh
t: test

check: build
	@$(BUILD)/pixy check

fmt:
	@command -v clang-format >/dev/null && clang-format -i csrc/*.c csrc/*.h tools/*.c || true

fmt-check:
	@command -v clang-format >/dev/null && clang-format --dry-run --Werror csrc/*.c csrc/*.h tools/*.c || true

verify: fmt-check build test

smoke: build
	@bash scripts/smoke.sh

smoke-shell: build
	@bash scripts/smoke_shell.sh

bench: release-build
	@bash scripts/bench.sh

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
	@echo "  smoke        Run the CLI smoke"
	@echo "  smoke-shell  Run the shell integration smoke"
	@echo "  bench        Run release performance checks"
	@echo "  package-check Check release artifact contents"
	@echo "  example-pack Build the example sprite pack"
	@echo "  docs-images  Regenerate the README frames from live output"
	@echo "  install      Install into $(PREFIX)/bin"
	@echo "  release      Release a new version"
	@echo
h: help
