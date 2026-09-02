# CLI

```text
pixy render <zone[.segment][,...]> [options]
pixy stream <zone[.segment][,...]> [options]
pixy list [--config PATH]
pixy check [--config PATH]
pixy init bash|zsh|fish
pixy names [pack]
pixy pack build|check|list
pixy palette set|use|end|reset|ask
pixy serve --stdio [--config PATH]
```

## Configuration

`--config PATH` overrides `PIXY_CONFIG` and the discovered
`$XDG_CONFIG_HOME/pixy/init.lua`. Pixy requires a Lua configuration and reports
exit code 3 when none can be loaded.

## Rendering

An exact zone name is resolved before treating the final dot as a segment
separator. Comma-separated selectors preserve order and may repeat.

Common options:

- `--mode line|run|surface`
- `--target plain|ansi|bash|zsh`
- `--width N` and `--height N`
- `--set key=value`
- `--context-json JSON` or `--context-file PATH`
- `--now-ms N`
- `--frames-ms N`
- `--palette [SLOT]`
- `--newline`

Line mode writes only the rendered bytes. Run mode emits styled runs as JSON.
Surface mode emits a multi-line ANSI payload. With `--frames-ms`, run and
surface modes return a JSON filmstrip covering the requested horizon.

## Streaming

`stream` redraws until its duration ends. Native animation nodes supply
`next_frame_ms`, allowing the CLI to wait for the next distinct frame rather
than polling continuously.

## Stdio server

`serve --stdio` accepts a four-byte big-endian length followed by a JSON request
and replies using the same framing. The caller owns the process and closing
stdin terminates it.

## Sprite packs

`pack build` creates a deterministic compressed pack from a directory. `check`
validates one and `list` shows its members. With no path, `pack list` shows the
embedded and installed packs.

## Palette namespaces

```sh
pixy palette set 1=#ff5555 bg=#0a0a0a
pixy palette use --slot 4
pixy palette end
pixy palette reset --slot 4
pixy palette ask
```

`pixy palette set --config PATH` with no explicit color entries emits the
palette declared by that Lua configuration. `PIXY_PALETTE_OSC` changes the OSC
number when a terminal implements the namespace protocol on another number.

## Exit codes

- 0: success
- 1: terminal capability unsupported
- 2: usage error
- 3: configuration error
- 4: rendering error
- 5: transport or host error
