# Lua API

Lua is Pixy's required configuration and extension interface. The `pixy` module
is registered by the executable and implemented in C.

## Configuration

```lua
local pixy = require("pixy")

pixy.zone("prompt.left", {
  pixy.segment("directory", function(ctx)
    return pixy.text(ctx.values.cwd or "?", {fg = 14})
  end, {priority = 1}),
})
```

`pixy.zone(name, segments)` registers a zone. Registering the same name again
replaces it.

`pixy.segment(name, callback, options)` constructs a segment. A callback receives
the render context and returns a string, number, node, or `nil`.

Configurations register zones and an optional palette, then return nothing.

Zone names may contain letters, numbers, `_`, `-`, and `.`. Segment names use
the same characters except `.`. `zone.segment` selects one segment while `zone`
renders all segments in order.

## Context

```lua
function(ctx)
  return ctx.values.label
end
```

The context contains:

- `ctx.width` and `ctx.height`
- `ctx.now_ms`
- `ctx.values`, populated by `--set` and request context
- `ctx.env`, populated by request context

Pixy does not assign application meaning to custom values.

## Nodes

The native constructors are:

- `text(value, style)`
- `row(children)` and `column(children)`
- `segments(children)`
- `regions({left = ..., center = ..., right = ...})`
- `surface(lines)`
- `pad(value, padding)`
- `style(value, style)`
- `truncate(value, width, marker)`
- `priority(value, number)`
- `when(condition, value)`
- `spacer(weight)`
- `transparent(width)`
- `region(value, options)`
- `spinner(options)`
- `animate(callback, interval_ms)`
- `sprite(options)`

Styles accept `fg`, `bg`, `bold`, `dim`, `italic`, `underline`, and `reverse`.
A color is an index from 0 through 255, one of the eight basic color names,
`"default"`, or an RGB triple such as `{244, 63, 94}`.

`spacer` absorbs remaining horizontal width. `segments` and configuration zones
remove higher-priority-numbered items first when width is insufficient.

## Animation

```lua
pixy.spinner({
  frames = {"⠋", "⠙", "⠹", "⠸"},
  interval_ms = 80,
  started_at_ms = ctx.values.started_at_ms,
})
```

Spinners and animated nodes return a relative `next_frame_ms`. Filmstrip and
stream callers use it to schedule the next distinct frame.

## Sprites

```lua
pixy.sprite({
  pack = "pokemon",
  name = "regular/pikachu",
  fallback_name = "regular/pikachu",
  format = "ansi",
  transparent = true,
})
```

`frames` may replace `pack` and `name` for inline animation. Plain and SGR ANSI
frames are parsed by C. Spaces can remain transparent in surface output.

## Native render functions

`pixy.renderers` contains C callbacks usable directly as segment callbacks:

- `directory`
- `git`
- `status`
- `spinner`
- `pokemon`

The starter configuration uses these functions, keeping its Lua limited to
composition.

## Providers

`pixy.git.branch(ctx)` and `pixy.git.status(ctx)` are native providers.

`pixy.shell.directory(ctx)`, `status(ctx)`, and `character(ctx)` are also native.

`pixy.progress` provides `state`, `percent`, `bar`, `sweep`, and `segment`.

`pixy.system` provides native clock, identity, uptime, memory, battery, sudo,
parsing, and stable-choice helpers.

## Host

`pixy.host` exposes bounded native operations:

- `env(name)`
- `read(path)`
- `exec(argv, options)`
- `cell_width(text)`
- `asset(pack, name)`

`exec` accepts `cwd`, `env`, `timeout_ms`, and `ttl_ms`. It never invokes a
shell. Reads and module loading are restricted to trusted roots.

## Palette

A configuration may register a palette:

```lua
pixy.palette({slot = 2, [1] = "#cc241d", bg = "#282828"})
```

Keys are indexes 0-255 or `fg`, `bg`, and `cursor`. Slots 2-31 are claimable.
