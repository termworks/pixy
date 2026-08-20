# Lua API

A config defines zones containing ordered named segments:

```lua
local pixy = require("pixy")

return pixy.config({
  zones = {
    ["prompt.left"] = pixy.zone({
      pixy.segment("directory", function(ctx)
        return pixy.text(ctx.values.cwd or "?", {fg = 14})
      end, {priority = 2}),
      pixy.segment("status", function(ctx)
        if (ctx.values.status or 0) == 0 then return nil end
        return pixy.text(tostring(ctx.values.status), {fg = 9})
      end, {priority = 1}),
    }),
  },
})
```

`pixy render prompt.left` evaluates and composes the complete zone.
`pixy render prompt.left.status` evaluates only the named segment. Zone names
may contain dots; segment names may contain letters, digits, `_`, and `-`, but
not dots. Segment names must be unique inside a zone.

`pixy.zone` requires a dense, non-empty segment array. `pixy.segment` requires
a name and render function. The function receives `ctx` and returns a string,
node, or `nil`.

`ctx` carries `width`, `height`, `now_ms`, the `env` map, and `values`. The host
names no other field: everything a caller supplies arrives through `--set
key=value`, `--context-json`, or `--context-file` and appears under
`ctx.values`, so what a prompt or statusbar is made of is decided by the zones
in this file. A `--set` value is read as the number, boolean, or null it spells
and as text otherwise, and an empty one is absent. The shell integrations
`pixy init` prints send `status`, `duration_ms`, `jobs`, `language`, and
`vimode`; those names are the integration's convention, not Pixy's vocabulary,
and a configuration is free to expect different ones. Its optional table accepts `priority`, `id`, `actions`,
`hover_style`, and `press_styles`. Lower priority numbers survive whole-zone
width pruning first. Interaction options attach bounded geometry and action
identifiers to that segment's result.

Node constructors are `text`, `row`, `column`, `segments`, `regions`, `region`,
`spacer`, `pad`, `when`, `priority`, `truncate`, `style`, `palette`, `spinner`,
`animate`, `sprite`, and `surface`.

Styles accept `fg`, `bg`, `bold`, `dim`, `italic`, `underline`, and `reverse`.
Colors are palette indexes 0-255, RGB triples, or one of the basic named
colors. Layout uses terminal cell width rather than UTF-8 byte length.

`spinner` accepts frame lists or `kind="knight_rider"`. The named spinner
supports `width`, `step`, `hold`, `trail`, `colors`, `placeholder`, `bg`,
`prefix`, and `suffix`, and reports its next deterministic frame deadline.
`sprite` accepts plain or SGR truecolor frames, recursive pack item names,
transparent spaces, five pane positions, and an optional `fallback_name`.

`spacer` is a flexible gap: it measures zero, and after pruning it absorbs the
width left over between the rendered content and the requested width. Several
spacers split that slack in proportion to their weight, `pixy.spacer(3)` taking
three times what `pixy.spacer()` takes. Where the spacers sit is what makes a
zone left, centered, right, or anything between, so a status bar needs no
alignment vocabulary in the host. A spacer is never pruned, and collapses to zero
when the content already fills the width.

`segments` is a lower-level responsive layout node; prefer config-level zones
for independently queryable content. `regions` remains a node for positioning
left, center, and right children inside one result. `region` adds interaction
metadata to an arbitrary node. Callers select hover and pressed appearance
through `ctx.values.hover_region`, `press_region`, and `press_button`.

## Palette

A config may declare the colours its indexes should resolve to, alongside the
slot the prompt claims:

```lua
palette = {slot = 2, [1] = "#f38ba8", [237] = "#313244", bg = "#11111b"},
```

Keys are whole numbers `0`–`255` or `fg`, `bg`, `cursor`; colours are strings
spelling `#rrggbb`, `rrggbb`, or `rgb:rr/gg/bb`; the slot is a whole number
`2`–`31`, since 0 is the ordinary palette and 1 the terminal's own chrome.
Anything else is a config error naming the key that caused it, rather than a
value quietly reinterpreted — `[1.5]` is not index 1, and `16711680` is not a
colour.

`pixy check` reports the palette it accepts and refuses a broken one. A render
does not: a prompt that stops drawing is worse than a prompt wearing the
terminal's own colours, so `render --palette` falls back to the default slot and
draws anyway. `check` is therefore where a mistake surfaces.

`pixy palette set` emits the table and `render --palette` claims the slot, so a
zone keeps naming plain indexes while the exact shades stay pixy's rather than
the terminal theme's — and stay repaintable afterwards without rendering again.
Declaring no palette is normal and costs nothing. See [the CLI](cli.md#palette-namespaces).

## Run style grammar

Each run and interaction style is a space-separated list of `fg:<color>`,
`bg:<color>`, `bold`, `dim`, `italic`, and `underline`. A color is a palette
index from `0` through `255` or a lowercase 24-bit `#rrggbb` value. Named Lua
colors are normalized to palette indexes.

Run mode has no portable `reverse` token. When foreground and background are
both explicit, Pixy swaps them. Other reverse cases are omitted from the run
description. ANSI line and surface output still use SGR reverse video.
`fg:default` and `bg:default` are omitted; callers that need terminal reset
semantics should use explicit colors or surface mode. Run text contains no
terminal control characters.

Host calls are `pixy.host.env(name)`, `read(path)`, `exec(argv, options)`,
`cell_width(text)`, and `asset(pack, name)`. Reads are limited to trusted Pixy,
`/proc`, and `/sys` roots. Execution is argv-only, limited to two seconds and
64 KiB, and optionally cached with `ttl_ms`.

`require` resolves bundled modules first, then bounded Lua files beside the
selected config or below its `lua/` directory. Module names cannot contain path
components, and symlinks cannot escape the trusted config directory.

## Progress and spinners

`require("pixy.segments.progress")` turns what a host reports about progress
into a node. The host supplies `progress_state` — one of `inactive`,
`in_progress`, `error`, `indeterminate`, `paused` — and `progress_pct`; hexe
fills both from a pane's OSC `9;4`.

```lua
local progress = require("pixy.segments.progress")

pixy.segment("progress", function(ctx)
  return progress.segment({width = 12}, ctx)      -- nil when nothing is running
end)
```

`segment` draws a bar when a percentage is known and a sweeping block when the
state is `indeterminate`, colours it by state, and returns `nil` for `inactive`
or an unrecognised state, so a config never has to special-case "no progress".
The pieces are available on their own: `bar{width, percent}`, `sweep{width,
block, interval_ms}`, `state(ctx)` and `percent(ctx)`.

A sweep sets `next_frame_ms` to the moment its picture next changes, so a host
polls then rather than on a timer.

`progress.spinner(name, options, ctx)` returns one frame of a named spinner —
`dots`, `line`, `bounce`, `arc`, `circle`, `square`, `triangle`, `clock` — with
the same deadline behaviour. `progress.SPINNERS` holds the frame lists, and
`pixy.spinner{frames = ...}` still takes any list of your own. For the sweeping
scanner, `pixy.spinner{kind = "knight_rider"}` remains the richer one, with
`width`, `step`, `hold`, `trail`, `colors`, `prefix` and `suffix`.
