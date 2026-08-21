# Performance

A prompt is a process. Whatever it costs is paid on every command, before the
shell gives the terminal back, so the number that matters is exec to bytes and
not what happens once a runtime is warm.

## Against starship

`make bench-compare` runs both under hyperfine. The dev shell pins starship and
hyperfine, so a comparison is against a known build rather than whatever is on
the machine. Two scenarios: `bare` is one directory segment, which isolates
runtime overhead; `git` is a real repository with both tools at their defaults.

| | pixy | starship | |
|---|---|---|---|
| bare | **0.9 ms** | 3.1 ms | 3.5× |
| git | **1.5 ms** | 6.0 ms | 4.2× |

Measured on one developer machine; the ratio travels better than the absolute
numbers.

## Where the time goes

`make bench-phases` splits a cold render into what it actually pays for, so an
optimisation can be aimed rather than guessed at. On the bundled profile:

```
phase_discover_ns=1339      finding the config
phase_read_config_ns=4911   reading it
phase_engine_load_ns=341708 building the Lua state
phase_render_ns=165386      evaluating the zones
```

Building the state dominates, which is why the work below went there.

### What was done about it

**The bundled modules are compiled at build time.** Parsing 64 KB of Lua to
reach the same twelve functions cost ~1 ms of a ~2 ms prompt, every prompt.
`scripts/lua_precompile.c` compiles them once and the binary carries the
bytecode: 1014 us of parsing became 108 us of loading. The source ships too, and
the engine falls back to it if the bytecode is not loadable, so a build for a
machine the precompiler cannot run on starts slower rather than not at all.

**Modules load only when required.** They sit in `package.preload` behind a C
closure, so a configuration that uses two of them does not pay for the other
ten.

**A compiled configuration is cached.** A real config is tens of kilobytes and
was parsed at every prompt. The compiled form is kept beside the provider cache,
keyed by a hash of the *contents* — `st_mtime` counts whole seconds, so two
edits a moment apart that leave the file the same length would be
indistinguishable, and the second would be served the first one's compile. Each
config keeps exactly one compiled copy; a new revision deletes the ones it
replaces.

**Only the libraries a configuration needs are opened.** `luaL_openlibs` also
hands it `io`, `debug` and `os.execute`, which is both work at startup and a way
straight past the trusted roots. `os` is opened for the clock with the parts
that act on the machine withheld.

Together these took a cold load-and-render p95 from 1.15 ms to 0.19 ms.

## Budgets

`make bench` measures 500 complete 80-column CLI queries plus 10,000 direct
render calls, and the same against the Hexe/Oslo compatibility profile.
Explicit geometry keeps the process measurement focused on Pixy rather than
controlling-terminal lookup. Benchmarks remove `LD_LIBRARY_PATH` so unrelated
development libraries cannot dominate dynamic-loader time. It enforces:

- complete CLI query p95 at most 900 us;
- core render query p95 at most 50 us;
- configured Lua memory limit at most 32 MiB.
- compatibility-profile cold load-and-render p95 at most 1.8 ms;
- compatibility-profile whole-zone query p95 at most 200 us;
- compatibility-profile single-segment query p95 at most 75 us;
- uncached provider process p95 at most 8 ms;
- release binary size at most 4 MiB.

`PIXY_BENCH_SCALE` widens every time budget by a constant factor, which is how a
shared CI runner is accommodated without relaxing each one permanently.
Provider work is excluded from the literal core thresholds because process and
filesystem latency is provider-specific. The gate reports and bounds
`provider_exec_p95_ns` separately over 100 uncached bounded process calls.
Provider execution has independent timeout, output, and TTL bounds.
The gate also floods the latest-value queue with 100,000 updates and requires
`pending_outputs=1`.

Compatibility measurements use deterministic provider inputs. The complete
query budget matches the Oslo prompt's asynchronous 10 ms deadline; the core
budget isolates Lua evaluation from process startup and configuration loading.

The hot path pre-indexes zone segments so a `zone.segment` selector does not
walk the list, moves request context into host state, converts Lua values
directly, and skips clipping when measured content already fits. Sprite code
loads only when a sprite is asked for. `-O2` and symbol stripping keep the
release artifact compact; a static musl binary halves process startup against a
dynamically linked one, 0.4 ms to 0.2 ms, because there is no loader to run.

Sprite packs use a compact binary index so a render does not parse or inflate
the whole collection. Pack integrity checks remain a separate explicit CLI
operation; rendering validates the index and selected item's bounds/checksum.
