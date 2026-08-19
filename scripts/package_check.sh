#!/usr/bin/env bash
set -euo pipefail

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
archive="$tmp/pixy.tar.gz"
tar -czf "$archive" target/release/pixy README.md LICENSE THIRD_PARTY.md LICENSES assets lua examples docs
tar -tzf "$archive" >"$tmp/contents"
for path in target/release/pixy README.md LICENSE THIRD_PARTY.md LICENSES/GPL-3.0-only.txt assets/README.md assets/pokemon/regular/pikachu assets/pokemon/shiny/pikachu lua/pixy/init.lua lua/pixy/default.lua examples/init.lua examples/oslo.lua examples/hexe-oslo.lua docs/architecture.md docs/hexe-oslo.md; do
  grep -qx "$path" "$tmp/contents"
done
if grep -q 'pixy-example.pixypack' "$tmp/contents"; then exit 1; fi
target/release/pixy pack check target/release/pixy-example.pixypack >/dev/null
target/release/pixy pack list | grep -q $'^pokemon\t2034\t.*(embedded)$'
tar -czf "$tmp/pixy-example-pack.tar.gz" target/release/pixy-example.pixypack
tar -tzf "$tmp/pixy-example-pack.tar.gz" | grep -qx 'target/release/pixy-example.pixypack'
printf 'package ok\n'
