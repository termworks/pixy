# A pass-through. xmake.lua is the build; this exists so `make test` still means
# what it always did, and so muscle memory and CI do not have to change.
#
# xmake is not something to have installed before a clone will build, so if it
# is missing and nix is here, borrow it from the dev shell. `make test` on a
# fresh checkout then works without being told to enter anything first.
ifeq ($(origin XMAKE), undefined)
  ifneq ($(shell command -v xmake 2>/dev/null),)
    XMAKE := xmake
  else ifneq ($(shell command -v nix 2>/dev/null),)
    XMAKE := nix develop --command xmake
  else
    $(warning pixy builds with xmake: install it, or install nix and use `nix develop`)
    XMAKE := xmake
  endif
endif

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
