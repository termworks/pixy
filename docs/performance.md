# Performance

`make bench` builds release mode and measures 500 complete 80-column CLI
queries plus 10,000 direct `Engine::render` calls. It also measures whole-zone
and single-segment queries against the Hexe/Oslo compatibility profile.
Explicit geometry keeps the process measurement focused on Pixy rather than
controlling-terminal lookup. Benchmarks remove `LD_LIBRARY_PATH` so unrelated
development libraries cannot dominate dynamic-loader time. The process lane
uses 100 unsampled launches before collecting its 500 samples. It enforces:

- complete CLI query p95 at most 8 ms;
- core render query p95 at most 50 us;
- configured Lua memory limit at most 32 MiB.
- compatibility-profile cold load-and-render p95 at most 3.5 ms;
- compatibility-profile whole-zone query p95 at most 200 us;
- compatibility-profile single-segment query p95 at most 75 us;
- uncached provider process p95 at most 8 ms;
- release binary size at most 4 MiB.

Provider work is excluded from the literal core thresholds because process and
filesystem latency is provider-specific. The gate reports and bounds
`provider_exec_p95_ns` separately over 100 uncached bounded process calls.
Provider execution has independent timeout, output, and TTL bounds.
The gate also floods the latest-value queue with 100,000 updates and requires
`pending_outputs=1`.

Compatibility measurements use deterministic provider inputs. The complete
query budget matches the Oslo prompt's asynchronous 10 ms deadline; the core
budget isolates Lua evaluation from process startup and configuration loading.

The hot path pre-indexes zone segments, moves request context into host state,
converts Lua values directly instead of round-tripping through serde, reuses one
executor across renders rather than allocating a thread per call, and skips
clipping when measured content already fits. Bundled Lua sources stay static and sprite
code loads only when requested. Fat LTO, one codegen unit, symbol stripping, and
abort-on-panic keep the release artifact compact and reduce startup work.

Sprite packs use a compact binary index so a render does not parse or inflate
the whole collection. Pack integrity checks remain a separate explicit CLI
operation; rendering validates the index and selected item's bounds/checksum.
