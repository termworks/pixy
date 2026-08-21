#!/usr/bin/env bash
set -euo pipefail

# Budgets are written for a quiet developer machine. A shared CI runner is
# slower by a constant factor rather than differently shaped, so it widens every
# time budget with PIXY_BENCH_SCALE instead of each one being relaxed for good.
scale=${PIXY_BENCH_SCALE:-1}
budget() { printf '%s' "$(( $1 * scale ))"; }

pixy=${PIXY_BIN:-build/pixy}
run_pixy() { env -u LD_LIBRARY_PATH "$pixy" "$@"; }
cold=$(run_pixy __bench cold 500)
query=$(run_pixy __bench query 10000)
provider=$(run_pixy __bench provider 100)
compat=$(run_pixy __bench compat 500)
printf '%s\n%s\n%s\n%s\n' "$cold" "$query" "$provider" "$compat"
cold_ns=$(printf '%s\n' "$cold" | sed -n 's/^cold_p95_ns=//p')
query_ns=$(printf '%s\n' "$query" | sed -n 's/^query_p95_ns=//p')
memory=$(printf '%s\n' "$query" | sed -n 's/^lua_memory_limit_bytes=//p')
test -n "$cold_ns" && test "$cold_ns" -le "$(budget 900000)"
test -n "$query_ns" && test "$query_ns" -le "$(budget 50000)"
test "$memory" -le 33554432
provider_ns=$(printf '%s\n' "$provider" | sed -n 's/^provider_exec_p95_ns=//p')
test -n "$provider_ns" && test "$provider_ns" -le "$(budget 8000000)"
compat_cold_ns=$(printf '%s\n' "$compat" | sed -n 's/^compat_cold_p95_ns=//p')
compat_query_ns=$(printf '%s\n' "$compat" | sed -n 's/^compat_query_p95_ns=//p')
compat_segment_ns=$(printf '%s\n' "$compat" | sed -n 's/^compat_segment_p95_ns=//p')
test -n "$compat_cold_ns" && test "$compat_cold_ns" -le "$(budget 1800000)"
test -n "$compat_query_ns" && test "$compat_query_ns" -le "$(budget 200000)"
test -n "$compat_segment_ns" && test "$compat_segment_ns" -le "$(budget 75000)"
binary_bytes=$(wc -c < "$pixy" | tr -d '[:space:]')
test "$binary_bytes" -le 4194304
printf 'performance budgets ok\n'
