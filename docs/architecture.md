# Architecture

Pixy is a caller-neutral terminal painter. C owns the CLI, the bounded Lua
state, host primitives, and asset storage. Lua owns named zones, their ordered
named segments, nodes, layout, styling, animation frames, providers, sprites,
and all output encoding.

That split is why the host was rewritten from Rust to C without a configuration
changing: everything a config can see lives in `lua/`, which the rewrite did not
touch. `tests/parity.sh` holds the two builds against each other output by
output.

A render request selects whole zones or individual `zone.segment` values and
asks for one of three shapes:

- `line`: plain, ANSI, Bash, or Zsh prompt text without a newline;
- `run`: JSON-compatible text/style records without control bytes;
- `surface`: clipped multi-line ANSI in explicit geometry.

Sprite assets never bypass this model. Lua parses permitted SGR into styled
runs, rejects other controls, and the surface encoder emits fresh bounded ANSI.
The built-in Pokemon collection is packed into per-sprite gzip streams during
the build and embedded as a roughly 1.6-MiB archive. Generic version-2
packs use a compact hashed index and individually deflated payloads. Both paths
inflate only the selected item.

Whole-zone queries evaluate their segments in declaration order. Lower numeric
segment priorities survive width pressure before higher values. A segment query
evaluates only that segment. Every shape can also carry caller-neutral
interactive-region geometry and action identifiers.

Each CLI query loads the selected configuration, calls `Engine::render`, writes
one result, and exits. `pixy stream` keeps that engine only for the lifetime of
the foreground animation. Pixy has no background rendering service. A
100-millisecond fuel-sliced render deadline, 32-MiB Lua allocation limit, 1-MiB
result cap, bounded file reads, and argv-only process execution constrain
untrusted failure modes in trusted user configuration. The render deadline
bounds Lua execution alone; time a segment spends blocked in a host call is
charged instead against a separate 2-second per-render I/O budget, so a
provider slower than the deadline degrades rather than failing the render.
Loading a configuration is a once-per-process step with its own 250-millisecond
budget rather than a render's latency budget. The render ceiling is sized for
the heaviest supported render on a loaded machine, not for the typical prompt,
which measures three orders of magnitude below it.

## Source layout

- `src/engine.c` holds the Lua state: the bounding allocator, the deadline
  hook, the bundled modules, config validation, and the one call into
  `pixy._render`.
- `src/host.c` is `__pixy_host` — `env`, `read`, `exec`, `cell_width` and
  `asset` — with the trusted roots, the I/O budget, and the two-tier exec cache.
- `src/assets.c` reads and writes packs, including the embedded archive.
- `src/cli.c`, `src/serve.c` are the two front doors; `src/json.c`,
  `src/encode.c`, `src/width.c` and `src/util.c` are the plumbing.
- `vendor/lua` is Lua 5.4.7 unmodified; `vendor/miniz` provides deflate.
- `docs/assets/pokemon` is the sprite art the build packs and embeds.
- `lua/` is the part a configuration actually talks to, and is language-neutral.

The Lua modules and the shell integrations are turned into C string literals by
`scripts/embed_text.sh` at build time, and the sprite archive by
`scripts/pack_sprites.c`, so the binary carries everything it needs.

## Limits, and where they are enforced

The memory ceiling is the allocator handed to `lua_newstate`: past 32 MiB it
refuses, and Lua reports an ordinary allocation failure. The deadline is a count
hook that checks the clock every 4096 instructions, which is what stops a
configuration that loops forever. Both live in `src/engine.c` and are the same
numbers the Rust host used.

## Versioning

One line holds the version: `local PROJECT_VERSION` in `xmake.lua`. `veri` reads
and bumps it, the build compiles it in as `PIXY_VERSION_STRING`, and the
flake reads it for its package, so `make release TYPE=patch` moves a single
number and nothing drifts.
