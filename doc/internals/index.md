# clx Internals — start here

This section is for contributors who want to understand how clx works under the hood.
You do not need it to *use* clx. If you are new to the codebase, read the three pages
in this order:

1. **[Architecture](./architecture.md)** — the pipeline (Lua → C++ → binary) and where
   each source file fits. Read this first.
2. **[Optimizations](./optimizations.md)** — how clx makes Lua fast: prove types at
   compile time, emit plain C++ arithmetic. The examples are small; run them.
3. **[Runtime](./runtime.md)** — the C++ library every compiled program links against
   (values, tables, GC, strings, modules). Reference material — dip in as needed.

## The one idea behind clx

Lua values are *dynamic*: a variable can hold a number, then a string, then a table.
A normal interpreter checks the type at every operation. clx instead tries to **prove**
the types while compiling:

- If the compiler can prove `a` and `b` are numbers, `a + b` becomes plain C++
  (`l_a + l_b`) — no type checks, no function calls.
- If it cannot prove it, it emits the safe generic path (`clx::add(L, a, b)`), which
  checks types at runtime exactly like the interpreter would.

Everything in these docs is a variation on that idea: smarter proofs (optimizer),
simpler emitted code (codegen), and a runtime that makes the generic path fast.

## Words you will see everywhere

| Term | What it means |
|---|---|
| `LValue` | The 16-byte box that holds any Lua value (a type tag + 8 bytes of payload). |
| Native / unboxed | A plain C++ `double` or `int64_t` — no `LValue` box, no runtime checks. |
| Yielding a number | Optimizer shorthand: "this expression is provably numeric". |
| Fast function | A function whose parameters are all numeric, compiled to raw `double`s. |
| Root / root frame | How the garbage collector finds live values: every function pins its locals in a frame (`_fr`) so the GC never frees something still in use. |
| Arena | A bump-allocator region for short-lived tables that never escape their function — freed all at once, never garbage-collected. |
| Interning | Storing one shared copy of each distinct string and comparing pointers. |
| Hot path | The code that runs most often (e.g. a loop body). Optimizations target these. |

## How to explore

- Compile anything with `clx prog.lua --cpp` and read the generated C++ next to the
  Lua source. Every claim in these docs can be checked that way.
- The optimizer lives in `src/optimizer/` (one file per topic: `numbers.cpp`,
  `tables.cpp`, `structs.cpp`, …). The emitter lives in `src/codegen/`.
- The test suite (`tests/`) pins down behavior: if you change an optimization and a
  test fails, the test is usually right.
