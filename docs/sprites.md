# Sprites and packs

`pixy.sprite` accepts inline `frames` or a `pack` and item `name`. Frame choice
is derived from `ctx.now_ms`, `started_at_ms`, and `interval_ms`; Lua never
sleeps. Surface output clips rows and terminal cells to requested geometry.
Anchors combine `top|center|bottom` with `left|center|right`; `x` and `y`
offsets apply afterward. Spaces are transparent unless `transparent=false`.

Frames containing SGR escapes are detected automatically, or selected with
`format="ansi"`. Pixy parses basic, 256-color, and 24-bit foreground and
background SGR into styled cells before composing the surface. Non-SGR escape
sequences and other control bytes are rejected, so an asset cannot move the
cursor or paint outside its requested rectangle. This supports truecolor
Unicode half-block sprites without allowing raw asset bytes into the output.

`position` accepts `topleft`, `topright`, `bottomleft`, `bottomright`, or
`center` with the same pane-relative insets used by the Hexe Pokemon overlay.
`fallback_name` selects another packed item when `name` is absent.

Pixy embeds a built-in `pokemon` pack containing 1,017 regular and 1,017 shiny
sprites. The build deterministically converts `docs/assets/pokemon/{regular,shiny}` into
an HXSP archive of individually gzipped entries and embeds the roughly 1.6-MiB
result in the executable. Lookup scans the bounded index and inflates only the
selected sprite. No asset files are read or downloaded at runtime.

The imported artwork, source chain, copyright notice, and applicable license
are recorded in `docs/assets/README.md`, `vendor/THIRD_PARTY.md`, and
`LICENSES/GPL-3.0-only.txt`.

The bundled config exposes regular and shiny Pikachu directly:

```sh
pixy render pokemon --mode surface --width 80 --height 40
pixy render pokemon.shiny --mode surface --width 80 --height 40
```

Pass `sprite_name`, `sprite_shiny`, or `sprite_position` through context values
to select another entry or placement.

Pixy pack version 2 remains available for custom assets. It is a deterministic
binary index followed by individually deflated items and records raw/stored
sizes, per-item FNV-1a checksums, index and content hashes, source, license, and
attribution. The reader also accepts legacy version-1 JSON packs.

Pack builds recurse into directories. An installed `pokemon.pixypack` overrides
matching built-in entries, so custom artwork can preserve the same
`regular/<name>` and `shiny/<name>` paths:

```sh
mkdir -p "${XDG_DATA_HOME:-$HOME/.local/share}/pixy/packs"
pixy pack build /path/to/sprites \
  --output "${XDG_DATA_HOME:-$HOME/.local/share}/pixy/packs/pokemon.pixypack" \
  --source 'krabby / PokeSprite' \
  --license '<asset license>' \
  --attribution '<asset attribution>'
pixy pack check "${XDG_DATA_HOME:-$HOME/.local/share}/pixy/packs/pokemon.pixypack"
```

`make example-pack` builds the original tiny fixture as
`target/release/pixy-example.pixypack`. Release automation publishes it as a
separate artifact rather than embedding it in the base binary archive.

Install that file as `$PIXY_DATA_DIR/pixy-example.pixypack` (or under Pixy's
default data directory), then render its item from Lua:

```lua
pixy.sprite({pack = "pixy-example", name = "mascot.txt", anchor = "center"})
```

The Hexe/Oslo profile exposes the Pokemon surface as `overlay.sprite`. Its
default item is `regular/pikachu`, while `sprite_name`, `sprite_shiny`,
`sprite_position`, and `sprite_pack` come from `context.values`.
