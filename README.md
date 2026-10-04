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

### Nix binary cache

Each package and architecture uses one stable pin, such as
`pixy-x86_64-linux`, with `--keep-revisions 5`. The five newest pin revisions
are protected from cache cleanup; each binary retains its actual package version.
Older revisions become eligible for garbage collection and may need rebuilding.

Tagged releases are cached for `x86_64-linux` and `aarch64-linux`:

```sh
cachix use termworks
nix build --accept-flake-config github:termworks/pixy/v0.3.2
nix run --accept-flake-config github:termworks/pixy/v0.3.2 -- --version
```

The cache is `https://termworks.cachix.org`, with public signing key
`termworks.cachix.org-1:Ty7sSVALfD5ajbcWBIdaNHcaEx3fEmVrOo+rSzy0mvE=`.
Only pushed `v*` tags publish to it; branch revisions may need compilation.
Use `oslo make nix-build` and `oslo make nix-check` for local package checks.
The Nix package includes starter configuration and examples under `share/pixy`;
unlike the standalone release archive, its runtime dependencies live in the Nix store.

From another flake, set `inputs.pixy.url = "github:termworks/pixy/v0.3.2"` and
use `pixy.packages.${system}.default`. Enable the cache on the consuming machine
with `cachix use termworks`; input flakes do not apply their `nixConfig`
automatically.

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
