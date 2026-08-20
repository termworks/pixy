#!/usr/bin/env bash
set -euo pipefail

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
archive="$tmp/pixy.tar.gz"
release_dir="${RELEASE_DIR:-build}"
tar -czf "$archive" "$release_dir/pixy" README.md LICENSE THIRD_PARTY.md lua examples docs
tar -tzf "$archive" >"$tmp/contents"
for path in "$release_dir/pixy" README.md LICENSE THIRD_PARTY.md docs/assets/README.md docs/assets/pokemon/regular/pikachu docs/assets/pokemon/shiny/pikachu lua/pixy/init.lua lua/pixy/default.lua examples/init.lua examples/oslo.lua examples/hexe-oslo.lua docs/architecture.md docs/hexe-oslo.md; do
  grep -qx "$path" "$tmp/contents"
done
if grep -q 'pixy-example.pixypack' "$tmp/contents"; then exit 1; fi
"$release_dir/pixy" pack check "$release_dir/pixy-example.pixypack" >/dev/null
"$release_dir/pixy" pack list | grep -q $'^pokemon\t2034\t.*(embedded)$'
tar -czf "$tmp/pixy-example-pack.tar.gz" "$release_dir/pixy-example.pixypack"
tar -tzf "$tmp/pixy-example-pack.tar.gz" | grep -qx "$release_dir/pixy-example.pixypack"
printf 'package ok\n'
