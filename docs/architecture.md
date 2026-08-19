# Architecture

Pixy is a caller-neutral terminal painter. Rust owns the CLI, bounded Lua VM,
host primitives, scheduling, and asset storage. Lua owns named zones, their
ordered named segments, nodes, layout, styling, animation frames, providers,
sprites, and all output encoding.

A render request selects whole zones or individual `zone.segment` values and
asks for one of three shapes:

- `line`: plain, ANSI, Bash, or Zsh prompt text without a newline;
- `run`: JSON-compatible text/style records without control bytes;
- `surface`: clipped multi-line ANSI in explicit geometry.

Sprite assets never bypass this model. Lua parses permitted SGR into styled
runs, rejects other controls, and the surface encoder emits fresh bounded ANSI.
The built-in Pokemon collection is packed into per-sprite gzip streams during
the Cargo build and embedded as a roughly 1.6-MiB archive. Generic version-2
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

## Rust source layout

Rust modules are grouped by dependency direction:

- `src/model/` defines render context, output types, and errors;
- `src/runtime/` provides assets, configuration, host operations, and
  scheduling;
- `src/application/` composes those layers into the engine and CLI.

`src/lib.rs` exposes the grouped modules and their direct root re-exports such
as `pixy::config`.
