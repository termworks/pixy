# Performance

Pixy keeps the Lua interface bounded while performing layout and encoding in C.

## Limits

- Lua state: 32 MiB
- configuration source: 1 MiB
- configuration CPU deadline: 250 ms
- render CPU deadline: 100 ms
- render output: 1 MiB
- provider execution: 2 seconds and 64 KiB per stream
- configuration read: 64 KiB

Provider waiting does not consume the Lua CPU deadline. It does consume the
separate render I/O budget.

## Benchmarks

```sh
oslo make bench
oslo make bench-phases
oslo make bench-compare
```

The benchmark command measures cold configuration loading, repeated rendering,
and a native host execution provider. `bench-phases` separates path discovery,
configuration reading, engine creation, and rendering. `bench-compare` uses the
pinned Hyperfine and Starship packages from `flake.nix` to compare complete
process startup for matched directory and Git-branch providers. Pass `--runs
N` to change its default 200 samples.

## Release checks

```sh
oslo make release-musl
oslo make package-check
```

The release task builds an optimized static Linux executable. Package checking
verifies the binary, required notices, compressed Pokémon archive, and example
sprite pack.
