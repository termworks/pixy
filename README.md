# Pixy

Pixy is a terminal renderer implemented in C and configured through Lua.

Lua is the only configuration interface. A configuration declares zones,
segments, styles, layout, animation, and sprites through `require("pixy")`.
The module itself, every node constructor, the layout engine, encoders,
providers, animation, sprite handling, and asset loading are native C.

## Build

The repository uses Xmake. Its development recipes live in `.make.lua`:

```sh
oslo make build
oslo make test
oslo make verify
oslo make release-musl
```

Xmake may also be called directly:

```sh
xmake pixy-build
xmake pixy-test
xmake release-musl
```

The release build is a static Linux executable. `oslo make package-check`
checks the binary, the embedded Pokémon archive, and the example sprite pack.

## Configuration

Pixy loads Lua from the first available source:

1. `--config PATH`
2. `PIXY_CONFIG`
3. `$XDG_CONFIG_HOME/pixy/init.lua`
4. `$HOME/.config/pixy/init.lua`

There is no internal fallback configuration. Install the small starter config
with:

```sh
oslo make configs
```

The source installed by that recipe is [`config/init.lua`](config/init.lua).

```lua
local pixy = require("pixy")

pixy.zone("prompt.left", {
  pixy.segment("directory", pixy.renderers.directory),
  pixy.segment("git", pixy.renderers.git),
  pixy.segment("status", pixy.renderers.status),
})
```

Lua decides what to compose. The three render functions above are C functions
exposed through the API. User callbacks may also construct arbitrary node trees.

## Commands

```text
pixy render <zone[.segment][,...]> [options]
pixy stream <zone[.segment][,...]> [options]
pixy list [--config PATH]
pixy check [--config PATH]
pixy serve --stdio [--config PATH]
pixy init bash|zsh|fish
pixy names [pack]
pixy pack build|check|list
pixy palette set|use|end|reset|ask
```

Rendering supports plain text, ANSI, Bash prompt escaping, Zsh prompt escaping,
styled runs as JSON, and multi-line terminal surfaces.

## Assets

`assets/pokemon.hxsp` contains all regular and shiny Pokémon sprites in one
compressed deterministic archive. Xmake converts the archive into an object and
links it into the executable. Pixy inflates only the selected item at runtime.

```sh
pixy names pokemon
pixy render pokemon --mode surface --set pokemon_name=pikachu
pixy render pokemon --mode surface --set pokemon_name=pikachu --set sprite_shiny=true
```

The installed pack format is also available for custom sprites:

```sh
pixy pack build ./sprites --output custom.pixypack \
  --source project --license MIT --attribution author
pixy pack check custom.pixypack
```

## Documentation

- [`docs/architecture.md`](docs/architecture.md) — C core and Lua boundary
- [`docs/lua-api.md`](docs/lua-api.md) — configuration API
- [`docs/cli.md`](docs/cli.md) — command and output protocol
- [`docs/shell.md`](docs/shell.md) — Bash, Zsh, and Fish integration
- [`docs/sprites.md`](docs/sprites.md) — embedded and installed sprite packs
- [`docs/performance.md`](docs/performance.md) — limits and benchmarks

## License

Pixy is distributed under the [MIT License](LICENSE). Vendored dependency and
asset notices are recorded in [`vendor/THIRD_PARTY.md`](vendor/THIRD_PARTY.md)
and [`docs/assets/README.md`](docs/assets/README.md).
