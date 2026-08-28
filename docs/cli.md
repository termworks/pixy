# CLI

```text
pixy render <name[,name...]> [options]
pixy <name[,name...]> [options]
pixy list | check
pixy init bash|zsh|fish|oslo|hexe-oslo
pixy stream <names> [--fps N] [--duration MS]
pixy pack build|check|list ...
pixy names [<pack>]
pixy palette set|use|end|reset|ask [--slot N] [--wait] [key=colour ...]
pixy serve [--stdio]
pixy --help | --version
```

## Serving a host

`serve` answers length-prefixed JSON on stdin and stdout — a four-byte
big-endian length, then the request; the same, back. It loops until stdin
closes, so one child answers many requests and the Lua VM and the config are
paid for once rather than per frame. `--stdio` is accepted and is the default;
there is no other transport.

**There used to be a `--socket` server, and removing it was the point.** A
shared painter is one process every session on the machine talks to: a single
accept loop serialising them, one config to restart for all of them, a slow
render that is everybody's, and — since nothing owns it — one still running days
later from a build that is no longer installed. None of those are properties of
painting; they are properties of sharing. A host that spawns its own has none of
them, and needs no shutdown protocol: the pipe closing *is* the shutdown, so a
host that is killed cannot leave a painter behind.

A one-shot `pixy render` needs no server at all — right for a prompt, drawn a
few times a second. It costs a process start per render, so for anything
animating ask for a filmstrip (`--frames-ms`) and play it back, which is one
start per cycle rather than one per frame.

`<name>` is a zone such as `prompt.left` or a segment selector such as
`prompt.left.directory`. Pixy resolves an exact zone first; otherwise the final
dot separates the zone name from its segment name. Comma-separated selectors
retain their order and may repeat. `pixy list` prints every zone followed by
every callable `zone.segment` selector.

Render defaults to `--mode line --target ansi`. It writes only the payload and
does not append a newline unless `--newline` is passed. Diagnostics use stderr.
Exit codes are 1 for unsupported (the terminal answered nothing), 2 for usage,
3 for configuration, 4 for rendering, and 5 for transport or assets.

`--request -` reads a complete version-1 JSON render description directly from
stdin. JSON fields replace render flags.

Caller state reaches Lua only through repeatable `--set key=value`,
`--context-json`, and `--context-file`, all of which land in `ctx.values`.
There are no per-concept flags: Pixy has no opinion about what a prompt is made
of, so a new value needs a zone that reads it and nothing in the host. A `--set`
value is read as the number, boolean, or null it spells and as text otherwise;
an empty value is absent, so an unset shell variable reads as nil.

`--context-json` and `--context-file` carry a whole context and replace what
`--set` built, rather than merging into it: one argument describes the caller's
state completely. To override a single value from a fixture, edit the fixture or
drop the file argument — mixing the two means the file wins.

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

## Palette namespaces

A palette namespace is a private 256-colour table for one region of output. A
prompt claims a **slot**, prints, and releases it; every cell it wrote remembers
the slot, so repainting the slot later recolours exactly that prompt — on screen
and in scrollback — with nothing rendered again.

The protocol is hexe's, documented in its `docs/palette.md`. Pixy is a client of
it, and `pixy palette` is a front door onto the same sequences, so anything the
CLI does a configuration can do.

```sh
pixy palette set 1=#ff5555 bg=#0a0a0a   # define colours; does not select
pixy palette use --slot 4               # claim, until `end`
pixy palette end                        # release
pixy palette reset --slot 4             # forget its colours
pixy palette ask                        # capability query, as bytes
pixy palette ask --wait                 # ...and read the answer
```

A slot is `0`–`31`. Slot 0 is the ordinary palette and slot 1 the terminal's own
chrome: both can be themed, neither can be claimed, so `use` starts at 2 and
pixy's own default slot is **2**. `*` in `set` and `reset` addresses every slot
already in use. A key is an index `0`–`255` or `fg`, `bg`, `cursor`; a colour is
`#rrggbb`, `rrggbb`, or `rgb:rr/gg/bb`. `set` is a patch, so indexes left unnamed
keep passing through to the terminal's own theme.

With no `key=colour` pair, `pixy palette set` emits what the configuration
declared, which is where a prompt's colours belong:

```lua
palette = {slot = 2, [1] = "#f38ba8", [237] = "#313244", bg = "#11111b"}
```

Entries are emitted indexes-ascending then names, so the same configuration
always writes the same bytes. A configuration that declares no palette emits
nothing.

`--palette [N]` wraps the output in `use` and `end`, taking the slot from the
configuration unless one is given. It applies to anything that writes cells —
`render --mode line`, `--mode surface`, and every frame of `stream`, wrapped
frame by frame so a stream killed mid-flight leaves no slot claimed. For
`--target bash` and `--target zsh` the sequences are marked invisible, so the
shell does not count them as printable width. A bash prompt also ends its
sequences with `BEL` rather than `ST`: inside `\[ … \]` bash reads the backslash
of `ST` as an escape, which eats the closing marker and leaves a stray `]`
printing in the prompt. Both terminators are the protocol's, and zsh, ansi, and
plain keep `ST`.

The colours the configuration declared go out with the claim rather than from a
startup hook, so the output carries everything it needs. `set` is idempotent, a
configuration that declares none costs nothing, and a prompt written this way
survives a `clear`, a terminal reset, a reattach or a new pane — and works in a
shell whose configuration cannot run a command at all, which is why the
integrations need no startup line. Run mode is a description for a host that paints for itself and has
nowhere to carry a sequence, so `--mode run --palette` is a usage error rather
than a flag that quietly does nothing.

An unclaimable slot is refused before anything is written. Half-applying it —
emitting the release without the claim — would pop whatever namespace the
surrounding application was holding and mis-colour the rest of its output, so
`--palette 1` and `--palette 99` are usage errors, not silent no-ops.

Every failure is benign. A terminal without support discards the sequences and
the indexed colours render exactly as they did before, so pixy emits
optimistically and never waits for an answer. `PIXY_PALETTE_OSC`, then
`HEXE_PALETTE_OSC`, moves the sequences off OSC 1330 when the terminal says so.

### Asking first

Only worth it to *change behaviour* on the answer — choosing namespaced colours
over an `OSC 4` fallback, say. Everything else should just emit.

`pixy palette ask` writes the query and nothing else, like every other verb.
`pixy palette ask --wait` does the round trip: it writes to `/dev/tty` rather
than stdout, which for a prompt is usually a pipe, reads the reply with the
terminal briefly in raw mode, and prints the OSC number and the highest
addressable slot:

```sh
$ pixy palette ask --wait
1330 31
```

There is no negative reply in the protocol, so **silence is the answer** for
unsupported. `--wait` therefore always times out rather than blocking —
`--timeout-ms` sets how long, 100 by default — and exits **1** when nothing
came back. That is a result to branch on, not a failure:

```sh
if pixy palette ask --wait >/dev/null; then
  eval "$(pixy init bash)"      # namespaces work here
fi
```

A reply arriving after the deadline counts as silence: a prompt cannot wait on a
terminal. Interrupts are held off across the round trip, so the terminal cannot
be left in raw mode, and an unrelated reply already in the buffer is stepped
over rather than mistaken for an answer.

## Help and colour

`pixy --help` lists the commands, and `pixy <command> --help` describes one.
`pixy --version` prints the version. Colour is used only when stdout is a
terminal, and never when `NO_COLOR` is set or `TERM` is `dumb`.

Output closed early — `pixy names | head` — exits quietly rather than reporting
a broken pipe.
