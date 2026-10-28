#!/usr/bin/env bash
# Builds and runs the stackless coroutine runtime test against a clx build directory (default: ../../build).
# usage: run.sh [build dir]
set -eu
D="$(cd "$(dirname "$0")" && pwd)"; B="${1:-$D/../../build}"; B="$(cd "$B" && pwd)"; OUT="${TMPDIR:-/tmp}/clx-co-test-$$"
g++ -std=c++20 -O2 -fno-ipa-icf -include climits -w -I"$D/../../include" "$D/co_runtime_test.cpp" -o "$OUT" \
    "$B/libclx.a" "$B"/libclx_{base,package,string,table,math,io,os,utf8,coroutine}.a -pthread
"$OUT"; rc=$?; rm -f "$OUT"; exit $rc
