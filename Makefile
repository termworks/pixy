SHELL := /bin/bash

PROJECT_NAME := $(shell if [ -f PROJECT ]; then sed -n '/^[[:space:]]*[^#\[[:space:]]/p' PROJECT | head -1 | tr -d '[:space:]'; else sed -n 's/^[[:space:]]*name[[:space:]]*=[[:space:]]*"\([^"]*\)".*/\1/p' Cargo.toml | head -1; fi)
PROJECT_VERSION := $(shell if [ -f PROJECT ]; then sed -n '/^[[:space:]]*[^#\[[:space:]]/p' PROJECT | sed -n '2p' | tr -d '[:space:]'; else sed -n 's/^[[:space:]]*version[[:space:]]*=[[:space:]]*"\([^"]*\)".*/\1/p' Cargo.toml | head -1; fi)
ifeq ($(PROJECT_NAME),)
    $(error Error: PROJECT file not found or invalid)
endif

TOP_DIR := $(CURDIR)
CARGO := cargo
TARGET ?=
MUSL_TARGET ?= $(shell uname -m)-unknown-linux-musl
CARGO_TARGET := $(if $(TARGET),--target $(TARGET),)
RELEASE_DIR := target/$(if $(TARGET),$(TARGET)/,)release
ARGS ?=
PREFIX ?= $(HOME)/.local

HAS_REL := $(shell command -v git-rel 2>/dev/null)

$(info ------------------------------------------)
$(info Project: $(PROJECT_NAME) v$(PROJECT_VERSION))
$(info ------------------------------------------)

.PHONY: build b compile c run r test t check check-all test-all clippy rustdoc fmt fmt-check clean verify smoke smoke-shell bench package-check example-pack release-build release-musl docs-images release help h

build:
	@$(CARGO) build --lib --bin pixy

b: build

compile:
	@$(CARGO) clean
	@$(MAKE) build

c: compile

run:
	@$(CARGO) run --bin pixy -- $(ARGS)

r: run

test:
	@$(CARGO) test --all-targets

t: test

check:
	@$(CARGO) check --all-targets

check-all:
	@$(CARGO) check --all-targets --all-features

fmt:
	@$(CARGO) fmt --all

fmt-check:
	@$(CARGO) fmt --all -- --check

clippy:
	@$(CARGO) clippy --all-targets --all-features -- -D warnings

rustdoc:
	@RUSTDOCFLAGS="-Dwarnings" $(CARGO) doc --all-features --no-deps

test-all:
	@$(CARGO) test --all-targets --all-features

clean:
	@$(CARGO) clean

verify: fmt-check check test check-all test-all clippy rustdoc

smoke: build
	@bash scripts/smoke.sh

smoke-shell: build
	@bash scripts/smoke_shell.sh

bench: release-build
	@bash scripts/bench.sh

package-check: release-build example-pack
	@RELEASE_DIR=$(RELEASE_DIR) bash scripts/package_check.sh

example-pack: release-build
	@$(RELEASE_DIR)/pixy pack build examples/pack --output $(RELEASE_DIR)/pixy-example.pixypack --source pixy --license MIT --attribution "Pixy contributors"

docs-images: release-build
	@bash scripts/docs_images.sh

release-musl:
	@$(MAKE) --no-print-directory release-build example-pack TARGET=$(MUSL_TARGET)

release-build:
	@$(CARGO) build --release --lib --bin pixy $(CARGO_TARGET)

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
	@echo "Available targets:"
	@echo "  build        Build the library and binary"
	@echo "  compile      Clean and rebuild"
	@echo "  run          Run Pixy with ARGS='...'"
	@echo "  test         Run all tests"
	@echo "  check        Run cargo check on all targets"
	@echo "  check-all    Run cargo check on all targets/all features"
	@echo "  test-all     Run cargo test on all targets/all features"
	@echo "  clippy       Run clippy with warnings denied"
	@echo "  rustdoc      Build docs with warnings denied"
	@echo "  fmt          Format the workspace"
	@echo "  fmt-check    Check formatting"
	@echo "  clean        Remove Cargo build artifacts"
	@echo "  verify       Run the full local gate"
	@echo "  smoke        Run CLI and renderer smoke tests"
	@echo "  smoke-shell  Check generated shell integrations"
	@echo "  bench        Run release performance checks"
	@echo "  package-check Check release artifact contents"
	@echo "  example-pack Build the original example sprite pack"
	@echo "  release-build Build release library and binary"
	@echo "  release-musl Build a static musl binary and pack for this arch"
	@echo "  docs-images  Regenerate the README frames from live output"
	@echo "  release      Release a new version"
	@echo

h: help
