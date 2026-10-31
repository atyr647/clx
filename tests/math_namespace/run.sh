#!/usr/bin/env bash
# CLX_MATH_NS: with -DCLX_MATH_NS=<namespace> the generated code's math intrinsics and the ^ operator call that namespace instead of std (programs that need the same results on every machine
# supply their own implementations). The program is built twice: the output with the marker namespace must show every function came from it, the output without must not.
# usage: run.sh <clx binary>
set -eu
CLX="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"; D="$(cd "$(dirname "$0")" && pwd)"; T="$(mktemp -d)"; cd "$T"; cp "$D/prog.lua" .
"$CLX" prog.lua --cpp -o prog > /dev/null 2>&1 || { echo "FAIL: translating"; exit 1; }
B="$(dirname "$CLX")"; INC="$D/../../include"
LIBS=("$B/libclx.a" "$B"/libclx_{base,package,string,table,math,io,os,utf8,coroutine}.a -pthread)
g++ -std=c++20 -O1 -w -include climits -I"$INC" prog.cpp -o std_build "${LIBS[@]}" > std.log 2>&1 || { echo "FAIL: build"; tail -5 std.log; exit 1; }
NSF=(-std=c++20 -O1 -w -include climits -I"$INC" -DCLX_MATH_NS=marker_math -include "$D/ns.hpp")
g++ "${NSF[@]}" -c "$D/../../src/runtime/math.cpp" -o math_ns.o > ns.log 2>&1 || { echo "FAIL: building the runtime's math library with CLX_MATH_NS"; tail -5 ns.log; exit 1; }
g++ "${NSF[@]}" prog.cpp math_ns.o -o ns_build "${LIBS[@]}" > ns.log 2>&1 || { echo "FAIL: build with CLX_MATH_NS"; tail -5 ns.log; exit 1; }
./std_build > std.out; ./ns_build > ns.out
cat ns.out
fail=0
for k in 1002 2002 3002 4000.5 5000.5 6002 7032 8002 9002 10002 11002 "log	12002" 0.9999166 13203; do grep -q "$k" ns.out || { echo "FAIL: $k not in the output built with CLX_MATH_NS"; fail=1; }; done
for k in 1002 2002 3002 13203; do grep -q "$k" std.out && { echo "FAIL: $k in the output built without it"; fail=1; }; done
grep -q "sqrt	4.0	floor	2" ns.out || { echo "FAIL: exact functions changed"; fail=1; }
rm -rf "$T"; [ "$fail" = 0 ] && echo "math namespace: ok"; exit $fail
