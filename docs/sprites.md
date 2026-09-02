# Sprites

Sprites are rendered by the C core from inline frames, installed `.pixypack`
files, or the embedded Pokémon archive.

```lua
return pixy.sprite({
  pack = "pokemon",
  name = "regular/pikachu",
  format = "ansi",
  transparent = true,
})
```

The embedded pack contains `regular/<name>` and `shiny/<name>` members. Use
`pixy names pokemon` to list its names. The compressed archive is linked into
the executable, but members are inflated individually when requested.

Custom packs are built and checked with:

```sh
pixy pack build ./sprites --output custom.pixypack \
  --source project --license MIT --attribution author
pixy pack check custom.pixypack
pixy pack list custom.pixypack
```

Installed packs live below `PIXY_DATA_DIR` or the normal Pixy data directory.
The parser validates bounds, sizes, checksums, and compressed members before
returning data to the renderer.

`frames = {...}` creates an inline animated sprite. `interval_ms` and
`started_at_ms` determine the current frame and next deadline.

Plain frames are split into terminal lines. ANSI frames accept SGR styling and
reject unrelated control sequences. With `transparent = true`, runs of spaces
become cursor-forward operations in surface mode.
