<div align="center">

# Pixy

**Lua decides what your terminal says. Rust hosts it, bounds it, and gets out of the way.**

[![tests](https://github.com/termworks/pixy/actions/workflows/tests.yml/badge.svg)](https://github.com/termworks/pixy/actions/workflows/tests.yml)
[![license](https://img.shields.io/badge/license-MIT-blue)](LICENSE)
![static musl](https://img.shields.io/badge/linux-static%20musl-informational)
![no daemon](https://img.shields.io/badge/daemon-none-success)

<img src="docs/images/responsive.svg" alt="one zone rendered at every terminal width" width="100%">

*One zone. Every width. Segments drop by priority, spacers take up the slack.*

</div>

---

Pixy paints prompts, status bars, titles and sprites. A Lua config defines named
**zones** made of ordered named **segments**; a caller asks for a whole zone or a
single `zone.segment`, and gets plain text, an ANSI prompt, styled runs, or a
bounded terminal surface.

What a prompt *is* lives entirely in your Lua. Rust never learns the word
"branch", "battery" or "hostname" — it loads your config, enforces the limits,
prints the answer and exits.

## ✨ Why

- **🧩 Zones and segments, not flags.** Address `prompt.left` or
  `prompt.left.directory`. Add a segment by writing one, not by waiting for a
  release.
- **📐 Width is an input.** Segments carry priorities and drop in order;
  `pixy.spacer()` absorbs the leftovers. One zone is a prompt at 40 columns and a
  status bar at 200.
- **🎨 Four output shapes.** Plain text, ANSI, styled runs as JSON, or a surface
  clipped to a rectangle — the same zone through any of them.
- **⛓️ Bounded on purpose.** 32 MiB of Lua, a 100 ms render deadline, a 250 ms
  load deadline. A bad config gets stopped, not your shell.
- **👻 No daemon, no rewrite.** Every render is one process that renders and
  exits. Pixy never edits your shell config or needs a second repository.
- **🐭 Sprites included.** 1,017 regular and 1,017 shiny Pokémon are embedded in
  the binary.

## 📸 What it looks like

A prompt — left zone, right zone, both from the same config:

![a pixy prompt](docs/images/prompt.png)

The same machinery as a full-width status bar, spacers doing the alignment:

![a status bar](docs/images/status.png)

A surface: raw ANSI, clipped to a rectangle you name:

<img src="docs/images/sprite.png" alt="a truecolor sprite" width="380">

## 📦 Install

<details open>
<summary><b>Release binary</b> — Linux builds are fully static musl, no runtime deps</summary>

```sh
tar -xzf pixy-linux-amd64.tar.gz
install -m 755 pixy ~/.local/bin/pixy
pixy --help
```

</details>

<details>
<summary><b>From source</b></summary>

```sh
make build          # debug
make release-build  # optimized
make release-musl   # static, this arch
```

</details>

<details>
<summary><b>Shell integration</b></summary>

```sh
pixy init bash >> ~/.bashrc
pixy init zsh  >> ~/.zshrc
pixy init fish >> ~/.config/fish/config.fish
pixy init oslo             # prints the assignments, edits nothing
```

The integrations pass `status`, `duration_ms`, `jobs`, `language` and `vimode`
through `--set`. Those names are the integration's vocabulary, not Pixy's — a
config is free to expect entirely different ones.

</details>

## 🔧 Configure

`~/.config/pixy/init.lua`, or `$PIXY_CONFIG`:

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

```sh
pixy check                                            # validate
pixy render prompt.left --target ansi --set status=7  # look at it
pixy render prompt.left.status --target plain         # one segment alone
pixy list                                             # every address
```

Prefer to start from something finished? `pixy init hexe-oslo` prints a complete
profile — prompt, status bar, knight-rider spinner, sprite overlay, float titles,
popups.

<details>
<summary><b>Building a bar with spacers</b></summary>

```lua
status = pixy.zone({
  pixy.segment("clock",   clock),
  pixy.segment("session", session),
  pixy.segment("gap1",    function() return pixy.spacer() end),
  pixy.segment("tabs",    tabs),
  pixy.segment("gap2",    function() return pixy.spacer() end),
  pixy.segment("battery", battery),
})
```

A spacer measures zero and absorbs whatever width is left after pruning. Two of
them centre the tabs. `pixy.spacer(3)` takes three times the share of a plain
one. Where the spacers sit is what makes a zone left, centred or right — there is
no alignment vocabulary in the Rust.

</details>

## ⚡ Speed, honestly

The engine is not where prompts spend their time:

| | |
|---|---|
| one segment, warm, in process | **25 µs** |
| a whole prompt, warm, in process | **0.17 ms** |
| a whole prompt as a process, providers cached | **5 ms** |
| the same prompt with every cache entry expired | **~26 ms** |

What costs milliseconds is whatever your segments *call*. The bundled
compatibility profile shells out for the distro logo, the sudo ticket, the
container kind and the scratch count; those subprocesses dwarf everything above,
so `pixy.host.exec` caches each result by `ttl_ms` on disk, and a prompt whose
providers are all still fresh never leaves the process.

Give static things a long life (`ttl_ms = 86400000` for a distro logo that will
not change today), hand Pixy the values your shell already knows through `--set`,
and let the cache absorb the rest. `make bench` prints the numbers for your
machine.

## 🧵 Painting a multiplexer

`pixy serve` speaks [hexe](https://github.com/termworks/hexe)'s painter protocol
over a Unix socket, so the zones that draw your prompt can draw the mux chrome —
status bar, pane and float titles, per-pane sprites:

```lua
-- ~/.config/hexe/init.lua
status = { enabled = true, view = "status", command = "pixy serve" }
```

hexe asks for a view at a width; Pixy answers with styled runs plus the clickable
regions inside them, and hexe never restyles what comes back. Save the config and
the bar repaints — `serve` reloads on write.

## 🗂 Commands

| | |
|---|---|
| `pixy render <zone[.segment]>` | render once and exit |
| `pixy stream <zone>` | bounded animation, `--fps`, `--duration` |
| `pixy serve` | painter socket for hexe |
| `pixy list` · `pixy check` | inventory · config validation |
| `pixy init <shell>` | shell integration text |
| `pixy pack build\|check\|list` | sprite packs |

XDG throughout: config in `$XDG_CONFIG_HOME/pixy`, provider cache in
`$XDG_CACHE_HOME/pixy`, packs in `$XDG_DATA_HOME/pixy/packs` — each overridable
with `PIXY_CONFIG`, `PIXY_CACHE_DIR`, `PIXY_DATA_DIR`.

## 📖 Documentation

| | |
|---|---|
| [`docs/lua-api.md`](docs/lua-api.md) | zones, segments, nodes, styles |
| [`docs/architecture.md`](docs/architecture.md) | how the host is bounded |
| [`docs/cli.md`](docs/cli.md) | every command and flag |
| [`docs/hexe-oslo.md`](docs/hexe-oslo.md) | the bundled compatibility profile |
| [`docs/sprites.md`](docs/sprites.md) | sprite packs and surfaces |

The zone schema breaks between versions on purpose. After upgrading, regenerate a
generated config before testing the new binary:

```sh
pixy init hexe-oslo > ~/.config/pixy/init.lua && pixy check
```

## 📄 License

MIT — see [`LICENSE`](LICENSE). Sprite provenance and third-party notices live in
[`THIRD_PARTY.md`](THIRD_PARTY.md) and [`LICENSES/`](LICENSES).

<sub>Every frame in this README is generated from live output by
<code>make docs-images</code> — no mockups.</sub>
