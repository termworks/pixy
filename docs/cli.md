# CLI

```text
pixy render <name[,name...]> [options]
pixy <name[,name...]> [options]
pixy list | check
pixy init bash|zsh|fish|oslo|hexe-oslo
pixy stream <names> [--fps N] [--duration MS]
pixy pack build|check|list ...
pixy names [<pack>]
pixy serve [--socket PATH]
pixy --help | --version
```

`<name>` is a zone such as `prompt.left` or a segment selector such as
`prompt.left.directory`. Pixy resolves an exact zone first; otherwise the final
dot separates the zone name from its segment name. Comma-separated selectors
retain their order and may repeat. `pixy list` prints every zone followed by
every callable `zone.segment` selector.

Render defaults to `--mode line --target ansi`. It writes only the payload and
does not append a newline unless `--newline` is passed. Diagnostics use stderr.
Exit codes are 2 for usage, 3 for configuration, 4 for rendering, and 5 for
transport or assets.

`--request -` reads a complete version-1 JSON render description directly from
stdin. JSON fields replace render flags.

Caller state reaches Lua only through repeatable `--set key=value`,
`--context-json`, and `--context-file`, all of which land in `ctx.values`.
There are no per-concept flags: Pixy has no opinion about what a prompt is made
of, so a new value needs a zone that reads it and nothing in Rust. A `--set`
value is read as the number, boolean, or null it spells and as text otherwise;
an empty value is absent, so an unset shell variable reads as nil.

`--context-json` and `--context-file` carry a whole context and replace what
`--set` built, rather than merging into it: one argument describes the caller's
state completely. To override a single value from a fixture, edit the fixture or
drop the file argument � mixing the two means the file wins.

When `--width` is absent, Pixy uses a positive inherited `COLUMNS`, then the
controlling terminal geometry, then 80 columns.

Every render loads the selected configuration, evaluates the requested zones,
writes the result, and exits. Pixy does not start or connect to a background
service.

`pixy pack build` walks its input directory recursively, rejects symlinks, and
writes deterministic version-2 binary packs. `pack check` verifies every
item; `pack list` prints the recorded source, license, item paths, sizes, and
checksums. With no file argument, `pack list` includes the embedded `pokemon`
pack before user-installed packs. Rendering validates the compact index and
inflates only the named item.

## Names

`pixy names [<pack>]` prints every distinct id a pack can draw, one per line,
sorted, with the variant prefixes collapsed — the embedded `pokemon` pack lists
1,017 ids rather than its 2,034 regular and shiny items. The pack defaults to
`pokemon`; an unknown one is a usage error.

This is for a host that names things after a pack. A multiplexer naming its
panes reads the list once at startup, hands out ids from it, and then asks for
`overlay.sprite` with the id it chose. Because both came from the same pack, a
name it hands out always has a picture behind it.

```sh
pixy names                    # 1017 ids
pixy names | shuf -n 1        # one, at random
pixy names nope               # exit 2, names what is installed
```

## Help and colour

`pixy --help` lists the commands, and `pixy <command> --help` describes one.
`pixy --version` prints the version. Colour is used only when stdout is a
terminal, and never when `NO_COLOR` is set or `TERM` is `dumb`.

Output closed early — `pixy names | head` — exits quietly rather than reporting
a broken pipe.
