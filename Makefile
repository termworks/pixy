# A pass-through. xmake.lua is the build; this exists so `make test` still means
# what it always did, and so muscle memory and CI do not have to change.
XMAKE ?= xmake

.DEFAULT_GOAL := build
.NOTPARALLEL:

.PHONY: build b compile c test t clean install help h

build:
	@$(XMAKE) pixy-build

b: build
compile: build
c: build

test:
	@$(XMAKE) pixy-test

t: test

clean:
	@$(XMAKE) clean-all

help:
	@$(XMAKE) --help

h: help

# Everything else -- release-build, release-musl, sanitize, fuzz, smoke,
# smoke-shell, bench, bench-compare, bench-phases, package-check, example-pack,
# docs-images, fmt, fmt-check, verify, install, release -- is an xmake task.
install:
	@$(XMAKE) pixy-install

%:
	@$(XMAKE) $@
