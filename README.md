<h1 align="center">Pixy</h1>

<p align="center">
  <em>Lua decides what your terminal says. Rust makes it fast and keeps it bounded.</em>
</p>

<p align="center">
  <a href="https://github.com/termworks/pixy/actions/workflows/tests.yml"><img alt="tests" src="https://github.com/termworks/pixy/actions/workflows/tests.yml/badge.svg"></a>
  <img alt="license" src="https://img.shields.io/badge/license-MIT-blue">
  <img alt="static musl" src="https://img.shields.io/badge/linux-static%20musl-informational">
</p>

![a pixy prompt](docs/images/prompt.png)

Pixy is a standalone terminal painter. A Lua config defines named **zones** made of
ordered named **segments**; a caller asks for a whole zone or a single
`zone.segment` and gets back plain text, an ANSI prompt, styled runs, or a
bounded terminal surface. Rust hosts the Lua, enforces the limits and exits.
Nothing about what a prompt or a status bar *is* lives in the Rust.

There is no daemon to babysit, no config of yours that gets rewritten, and no
second repository to install.

## What it draws

A prompt is one zone. So is a status bar — the same machinery, a different width:

![a status bar](docs/images/status.png)

Give that zone less room and it does not wrap or truncate mid-word. Segments
carry priorities and drop in order, and `pixy.spacer()` absorbs whatever width
is left over, which is what puts the clock left and the battery right:

![the same zone at four widths](docs/images/responsive.png)

Surfaces are the other output shape — raw ANSI clipped to a rectangle you name,
which is how sprites and popups get drawn. The binary embeds 1,017 regular and
1,017 shiny Pokémon sprites, so this needs no asset pack:

![a truecolor sprite](docs/images/sprite.png)

## Install

Grab a release build — the Linux artifacts are fully static musl binaries with
no runtime dependencies at all:

```sh
tar -xzf pixy-linux-amd64.tar.gz
install -m 755 pixy ~/.local/bin/pixy
```

Or build it:

```sh
make build          # debug
make release-build  # optimized
make release-musl   # static, for this arch
```

## Your own prompt in five minutes

Write `~/.config/pixy/init.lua`:

```lua
local pixy = require("pixy")

return pixy.config({
  zones = {
    ["prompt.left"] = pixy.zone({
      pixy.segment("directory", function(ctx)
        return pixy.text(" " .. (ctx.values.cwd or "?") .. " ", {bg = 237, fg = 250})
      end, {priority = 1}),
      pixy.segment("status", function(ctx)
        if (ctx.values.status or 0) == 0 then return nil end
        return pixy.text(" " .. ctx.values.status .. " ", {bg = 1, fg = 15, bold = true})
      end, {priority = 5}),
    }),
  },
})
```

Check it, look at it, wire it into your shell:

```sh
pixy check
pixy render prompt.left --target ansi --set status=7
pixy init bash >> ~/.bashrc     # or zsh, fish, oslo
```

`pixy render prompt.left` composes the zone; `pixy render prompt.left.status`
renders that one segment; `pixy list` prints every address. Segment names are
yours — `status`, `duration_ms` and friends are the shell integration's
vocabulary, not Pixy's, and a config is free to expect entirely different ones.

Prefer to start from something complete? `pixy init hexe-oslo` prints a full
profile: prompt, status bar, knight-rider spinner, sprite overlay, float titles
and popups.

## What Rust guarantees

Every render is a fresh query that renders and exits, inside hard limits:

| bound | value |
|---|---|
| Lua memory | 32 MiB |
| render deadline | 100 ms |
| config load deadline | 250 ms |
| background services | none |

Measured on one machine with `make bench` (p95):

| | cold | warm |
|---|---|---|
| a whole prompt | 3.1 ms | 0.17 ms |
| a single segment | — | 25 µs |

A config that loops forever, allocates without end, or shells out to something
slow gets stopped rather than hanging your shell.

## Painting a multiplexer

`pixy serve` speaks [hexe](https://github.com/termworks/hexe)'s painter protocol
on a Unix socket, so the same zones that draw your prompt can draw the
multiplexer's chrome — status bar, pane and float titles, per-pane sprites:

```lua
-- ~/.config/hexe/init.lua
status = { enabled = true, view = "status", command = "pixy serve" }
```

hexe asks for a view at a width, Pixy answers with styled runs plus the
clickable regions inside them, and hexe never restyles what comes back. The
config reloads on write — save the file and the bar repaints.

## Commands

| | |
|---|---|
| `pixy render <zone[.segment]>` | render once and exit |
| `pixy stream <zone>` | bounded animation, `--fps`, `--duration` |
| `pixy serve` | painter socket for hexe |
| `pixy list` / `pixy check` | inventory and config validation |
| `pixy init <shell>` | shell integration text |
| `pixy pack build\|check\|list` | sprite packs |

XDG all the way down: config in `$XDG_CONFIG_HOME/pixy`, provider cache in
`$XDG_CACHE_HOME/pixy`, packs in `$XDG_DATA_HOME/pixy/packs`, each overridable
with `PIXY_CONFIG`, `PIXY_CACHE_DIR` and `PIXY_DATA_DIR`.

## Documentation

- [`docs/lua-api.md`](docs/lua-api.md) — zones, segments, nodes, styles
- [`docs/architecture.md`](docs/architecture.md) — how the host is bounded
- [`docs/cli.md`](docs/cli.md) — every command and flag
- [`docs/hexe-oslo.md`](docs/hexe-oslo.md) — the bundled compatibility profile
- [`docs/sprites.md`](docs/sprites.md) — sprite packs and surfaces

The zone schema is deliberately breaking between versions. After upgrading,
regenerate a generated config before testing the new binary:

```sh
pixy init hexe-oslo > ~/.config/pixy/init.lua && pixy check
```

## License

MIT, in [`LICENSE`](LICENSE). Sprite provenance and third-party notices are in
[`THIRD_PARTY.md`](THIRD_PARTY.md) and [`LICENSES/`](LICENSES).

<sub>Frames in this README are generated from live output by
<code>scripts/docs_images.sh</code>.</sub>
