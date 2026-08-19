# Pixy

Pixy is a standalone, fast Lua-powered terminal painter. Lua defines named
zones containing ordered named segments. Callers query a whole zone or one
`zone.segment` and request plain text, an ANSI prompt, styled runs, or a bounded
terminal surface. Rust hosts and bounds execution; Lua owns the result.

## Commands

```sh
make build
make run ARGS='--help'
make run ARGS='render prompt.left --target plain'
make run ARGS='render prompt.left.directory --target plain'
make run ARGS='init hexe-oslo'
make run ARGS='demo --target ansi --set status=7'
make test
make verify
make smoke
make smoke-shell
make bench
make package-check
make release TYPE=patch
```

The default config is `~/.config/pixy/init.lua`, or `$PIXY_CONFIG`. If it does
not exist, Pixy uses its bundled prompt and demo zones.

```lua
local pixy = require("pixy")

return pixy.config({
  zones = {
    prompt = pixy.zone({
      pixy.segment("label", function()
        return pixy.text(" hello ", {fg = 15, bg = 24, bold = true})
      end),
      pixy.segment("directory", function(ctx)
        return pixy.text(ctx.values.cwd or "?", {fg = 14})
      end),
    }),
  },
})
```

`pixy render prompt` composes both segments. `pixy render prompt.directory`
queries only the named segment. `pixy list` prints both forms.

The zone schema is intentionally breaking. Regenerate an earlier generated
compatibility config before testing the new binary:

```sh
pixy init hexe-oslo > ~/.config/pixy/init.lua
pixy check
```

Each `pixy render` invocation is a direct query that renders and exits. Use
`pixy stream` for bounded animations and `pixy init bash|zsh|fish|oslo` to
print integration text. Pixy has no background service, does not edit external
configuration, and does not require another repository.

`pixy init hexe-oslo` prints a Pixy-only profile matching the prompt, status,
knight-rider spinner, truecolor Pokemon overlay, float-title, and popup
rendering used by the reference Hexe and Oslo setup. The profile accepts mux
state through `context.values` and reports interactive recording regions in
structured render output. The binary embeds 1,017 regular and 1,017
shiny Pokemon sprites; no external sprite pack is required.

Pixy follows XDG paths. Configuration lives under `$XDG_CONFIG_HOME/pixy`,
provider cache under `$XDG_CACHE_HOME/pixy`, and packs under
`$XDG_DATA_HOME/pixy/packs`. Override them with `PIXY_CONFIG`,
`PIXY_CACHE_DIR`, and `PIXY_DATA_DIR`.

See [`docs/architecture.md`](docs/architecture.md),
[`docs/lua-api.md`](docs/lua-api.md), and [`docs/cli.md`](docs/cli.md).
The exact compatibility profile and caller context are documented in
[`docs/hexe-oslo.md`](docs/hexe-oslo.md).

Third-party asset provenance and distribution notices are recorded in
[`THIRD_PARTY.md`](THIRD_PARTY.md).
