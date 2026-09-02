# Architecture

Pixy has one implementation boundary: Lua describes a rendering configuration;
C performs the rendering.

## Lua interface

Every usable configuration is a Lua file. The engine creates a bounded Lua
state, registers the native `pixy` module, executes the selected configuration,
and validates its zones and segments.

Configuration code may:

- register zones and segments;
- construct native node descriptions;
- call native host providers;
- use callbacks to choose content from the supplied context;
- load adjacent user modules through the restricted module searcher.

The repository ships one starter configuration for `make configs`. It does not
ship a Lua implementation of Pixy and the executable contains no bundled Lua
module source or bytecode.

## C core

The native core owns:

- the `require("pixy")` module and its constructors;
- configuration validation and resource limits;
- node flattening, terminal-cell measurement, layout, pruning, and truncation;
- style normalization and plain, ANSI, Bash, Zsh, run, and surface encoding;
- deterministic animation and sprite rendering;
- Git, shell, and starter render functions;
- restricted environment, file, process, and asset access;
- compressed sprite-pack parsing and on-demand decompression;
- CLI parsing and the length-prefixed stdio server.

`src/lua_api.c` registers the Lua-facing C functions. `src/render.c` consumes
the node tables returned by configuration callbacks. `src/engine.c` owns the
Lua state and request lifecycle. The other `src/` files provide the CLI, host,
JSON, palette, width, and asset subsystems.

## Limits

The Lua state is limited to 32 MiB. Configuration loading has a 250 ms CPU
deadline and each render has a 100 ms CPU deadline. Host execution is argv-only,
limited to two seconds and 64 KiB, and shares a bounded I/O budget per render.
Configuration reads stay inside the configuration directory, Pixy data roots,
`/proc`, and `/sys`.

## Assets

`assets/pokemon.hxsp` is linked into the executable as binary data. Its index is
read directly from the embedded bytes and only the requested compressed member
is inflated. Installed `.pixypack` files use the same bounded parser.

## Build boundary

`xmake.lua` is the build definition. `.make.lua` and `.env.lua` are intentionally
small development orchestration files. Their Lua is build tooling, not part of
Pixy's runtime implementation or configuration API.
