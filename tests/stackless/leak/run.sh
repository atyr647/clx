#!/usr/bin/env bash
# A thread closed while suspended inside nested calls must give back every frame of the chain (the awaiter of a call owns the child frame it started). The program ends with such a thread; it is built
# with AddressSanitizer (leak detection on), and the run must report nothing.
# usage: run.sh <clx binary>   (the libraries are read from the binary's directory)
set -eu
CLX="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"; D="$(cd "$(dirname "$0")" && pwd)"; B="$(dirname "$CLX")"; T="$(mktemp -d)"; cd "$T"; cp "$D/suspended_nested.lua" .
"$CLX" suspended_nested.lua --cpp --stackless -o prog > /dev/null 2>&1 || { echo "FAIL: translating"; exit 1; }
g++ -std=c++20 -O1 -g -w -fsanitize=address -include climits -I"$D/../../../include" prog.cpp -o prog "$B/libclx.a" "$B"/libclx_{base,package,string,table,math,io,os,utf8,coroutine}.a -pthread > build.log 2>&1 || clang++ -std=c++20 -O1 -g -w -fsanitize=address -include climits -I"$D/../../../include" prog.cpp -o prog "$B/libclx.a" "$B"/libclx_{base,package,string,table,math,io,os,utf8,coroutine}.a -pthread > build.log 2>&1 || { echo "FAIL: build"; tail -5 build.log; exit 1; }
if ./prog > run.out 2>&1; then echo "stackless leak test: ok ($(head -1 run.out))"; rm -rf "$T"; else cat run.out | head -20; echo "FAIL: leaks"; exit 1; fi
