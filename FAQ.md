# FAQ

## What is clx?

clx is a cross-platform ahead-of-time (AOT) compiler for Lua 5.5.

It compiles Lua source code to C++, which is then compiled to native machine code using Clang, GCC, or MSVC.

The result is a standalone executable, static library or object file.

---

## Is clx related to LuaRT?

No.

Although both projects are authored by Samir Tine, clx is a completely separate project with its own compiler, runtime, architecture, and codebase.

LuaRT focuses on Windows application development and deployment.

clx focuses on native compilation of Lua programs.

---

## Why generate C++?

clx uses C++ as an intermediate representation because mature C++ toolchains already provide:

* Excellent optimizers
* Multiple target architectures
* Cross-platform support
* Link-time optimization (LTO)
* Profile-guided optimization (PGO)

This allows clx to benefit from decades of compiler development without maintaining architecture-specific code generators.

---

## Why C++20, and why not C (e.g. C99)?

C++ is clx's implementation language in two senses: the runtime library is written in it, and the generated code is C++. Both choices are load-bearing, not incidental.

* **Errors are C++ exceptions.** A Lua `error()` becomes a thrown `clx::LRuntimeException` that unwinds through native frames. C has no exceptions, so a C backend would need `setjmp`/`longjmp`, which skips cleanup code. That directly conflicts with clx's garbage-collector rooting model, where generated code holds values alive through RAII guards (`FrameGuard`, `RootRange`). With exceptions, unwinding runs those destructors and unroots values automatically; with `longjmp`, every error path would need manual unrooting, which is more generated code and a larger bug surface.
* **RAII keeps generated code small and correct.** The same guards that make unwinding safe also make the non-error path shorter: acquire rooted storage on scope entry, release on exit, no explicit cleanup at every return. C would spell all of that out by hand at every call site.
* **Overloads and templates keep one runtime API.** The runtime serves `int64`, `double`, and boxed `LValue` through overloaded functions, so generated code reads as plain arithmetic instead of a family of name-mangled variants or `_Generic` dispatch. It also keeps `--cpp` output human-debuggable.
* **C99 would not actually be more portable.** MSVC's C conformance historically lagged far behind its C++ (VLAs, compound literals, and designated initializers were missing or late), while its C++ support is first class. C++20, by contrast, is a stable, years-old baseline fully supported by every toolchain clx drives: GCC, Clang, Apple Clang, MSVC, clang-cl, and MinGW. Nothing in the generated code needs newer-than-20 features.
* **The backend is the point.** Emitting C++ buys decades of optimizer work for free: inlining, auto-vectorization, link-time optimization, profile-guided optimization. A C backend would demand the same codegen effort while giving up the language features above, for no measurable gain in portability or binary size.

In short: C++20 is not a dependency indulged for convenience. Exceptions, destructors, and overloads are the mechanisms the runtime's correctness and performance arguments rest on, and every supported compiler already implements the standard completely.

---

## Why not LLVM?

LLVM is an excellent infrastructure, but integrating it would require maintaining a dedicated backend.

clx instead leverages existing C++ compilers as its backend while remaining portable and lightweight.

---

## Why not LuaJIT?

LuaJIT and clx solve different problems.

LuaJIT focuses on runtime JIT compilation.

clx focuses on:

* Ahead-of-time compilation
* Standalone deployment
* Fast startup times
* Predictable performance
* Native toolchain integration

Some workloads may perform better under LuaJIT, while others will benefit from clx's AOT model.

---

## Does clx support Lua 5.5?

Yes, Lua 5.5 compatibility is the project's target.

See [`doc/compatibility.md`](doc/compatibility.md) for detailed support status.

---

## How do load(), loadfile() and dofile() work?

These functions are available when the generated program is compiled with `--dynamic`:

```bash
clx script.lua --dynamic --output script
```

The generated program then links the embedded Lua VM. Source passed to `load`, `loadfile`, or `dofile` executes in that VM and crosses the clx/VM bridge when it calls AOT functions or returns values. Without `--dynamic`—and in `--minimal` builds—the loader functions are not registered.

The clx AOT runtime registers `string.dump()` but it always throws `string.dump not supported`, and it does not register the `debug` module at all. Dynamic code runs in the embedded VM and uses its own standard libraries; see [`doc/dynamic-lua.md`](doc/dynamic-lua.md) for the environment and bridge boundaries.

---

## Does dynamic Lua provide string.dump() and debug?

The clx AOT runtime does not expose a working `string.dump()` (it exists but always throws) or a `debug` global. When compiled with `--dynamic`, however, loaded code executes in the embedded Lua VM, which contains Lua’s own standard libraries: the default bridge environment copies selected VM globals (including `string`, which has a real `string.dump` that works on VM closures) but does not copy `debug`. Consult [Dynamic Lua](doc/dynamic-lua.md) for the boundary details.

---

## Which platforms are supported?

Any platform supported by Clang, GCC, or MSVC.

Examples include:

* Linux
* Windows
* macOS

---

## Which CPU architectures are supported?

Any architecture supported by the underlying C++ compiler, including:

* x86-64
* ARM64
* RISC-V (not tested)

---

## Can I write native modules?

Yes.

clx provides a C++ API for creating native modules that can be linked statically.

See [`doc/modules.md`](doc/modules.md).

---

## Is clx a transpiler?

clx translates Lua to C++ as an intermediate representation.

The generated C++ is then compiled to native machine code.

In practice, clx behaves as an ahead-of-time native compiler.

---

## How small are generated executables?

Executable size depends on:

* Optimization profile
* Linked runtime modules
* Target platform

Typical examples (measured on macOS ARM64 with Apple clang; other platforms vary):

| Program                        | Size    |
| ------------------------------ | ------- |
| Minimal runtime (`--minimal`)  | ~110 KB |
| Default runtime (`--size`)     | ~250 KB |
| Pong + Sokol (`--size`)        | ~500 KB |
| Pong + Sokol (`--fast`)        | ~700 KB |

Actual results depend on platform and compiler.
