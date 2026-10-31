#!/usr/bin/env bash
# Oracle for the stackless lowering: every program is compiled twice, as before (a fiber per coroutine on targets with stacks) and with
# -DCLX_STACKLESS_ONLY (no fibers: a coroutine body runs as the C++20 coroutine the compiler lowered, or fails with "attempt to yield across a
# C-call boundary"), and run. The outputs must be identical; a program whose output differs is listed with the compiler's reasons
# (CLX_STACKLESS_DIAG). usage: oracle.sh <clx binary> <program.lua>...
set -u
CLX="$1"; shift
T="$(mktemp -d)"; same=0; diff_n=0
for f in "$@"; do
    n="$(basename "$f" .lua)"
    "$CLX" "$f" -o "$T/$n.fib" > "$T/$n.fib.log" 2>&1 || { echo "COMPILE-FAIL(fibers) $f"; continue; }
    CLX_STACKLESS_DIAG=1 "$CLX" "$f" --stackless -o "$T/$n.sl" -DCLX_STACKLESS_ONLY > "$T/$n.sl.log" 2>&1 || { echo "COMPILE-FAIL(stackless) $f"; tail -5 "$T/$n.sl.log"; continue; }
    ( cd "$(dirname "$f")" && timeout 300 "$T/$n.fib" > "$T/$n.fib.out" 2>&1 ); ( cd "$(dirname "$f")" && timeout 300 "$T/$n.sl" > "$T/$n.sl.out" 2>&1 )
    if diff -q <(sed -E "s/0x[0-9a-f]+/0xADDR/g" "$T/$n.fib.out") <(sed -E "s/0x[0-9a-f]+/0xADDR/g" "$T/$n.sl.out") >/dev/null; then echo "SAME   $f"; same=$((same+1))
    else echo "DIFF   $f"; diff_n=$((diff_n+1)); diff <(sed -E "s/0x[0-9a-f]+/0xADDR/g" "$T/$n.fib.out") <(sed -E "s/0x[0-9a-f]+/0xADDR/g" "$T/$n.sl.out") | head -8; grep "stackless:" "$T/$n.sl.log" | head -5; fi
done
echo "oracle: $same same, $diff_n different"; rm -rf "$T"; [ "$diff_n" = 0 ]
