# clx Architecture

## Overview

clx is a Lua-to-C++ compiler that transpiles Lua source code into optimized C++ code, which is then compiled using the system's C++ compiler to produce native machine code. The normal execution path is entirely AOT. An optional `--dynamic` link path embeds a separate Lua VM for loading and executing Lua source at runtime.

The AOT runtime and the embedded VM are separate runtimes. They have different value representations, heaps, garbage collectors, standard-library implementations, and coroutine systems. The bridge converts primitive values and wraps supported tables, userdata, and functions; it does not make the two runtimes share native objects.

## System Architecture

The following figure shows both paths. The compiler pipeline is shared; `--dynamic` changes the final link and generated runtime registration, not the compilation of the input source file.

```
                         Lua source file
                                │
                                ▼
┌──────────────────────────────────────────────────────────┐
│                      clx compiler                        │
│  CLI → Lexer → Parser → AST → Optimizer → Code generator │
│                                             │             │
│                                             ▼             │
│                                       generated C++       │
└─────────────────────────────────────────┬────────────────┘
                                          │
                                          ▼
                                  system C++ compiler
                                          │
                 ┌────────────────────────┴────────────────────────┐
                 │                                                 │
              normal                                             --dynamic
                 │                                                 │
                 ▼                                                 ▼
       ┌────────────────────┐                         ┌────────────────────┐
       │ libclx / clx.lib   │                         │ libclx / clx.lib   │
       │ clx AOT runtime    │                         │ clx AOT runtime    │
       └─────────┬──────────┘                         └─────────┬──────────┘
                 │                                             │
                 │                                  libclx_lua.a / clx_lua.lib
                 │                                  embedded Lua VM + bridge
                 │                                             │
                 └──────────────────┬──────────────────────────┘
                                    ▼
                         generated native executable

       AOT code ───────────────▶ clx runtime
          │                          │
          │ load/loadfile/dofile     │ --dynamic bridge calls
          │                          ▼
          └──────────────────▶ embedded Lua VM
                               VM libraries, heap,
                               GC, and coroutines
```

In a normal build, no embedded VM archive is linked and the dynamic loader functions are not registered. In a `--dynamic` build, the generated program creates a `DynamicVM` lazily when a loader or bridge operation first needs it. Runtime-loaded chunks execute in that VM, while the original clx program continues to execute as native code.

## Project layout

```
clx/
├── CMakeLists.txt              # Build configuration
├── build.sh / build.bat        # Convenience build scripts
├── include/
│   ├── clx.h                   # Public C++ API (value ctors, type queries, helpers)
│   ├── clx_runtime.h           # Internal runtime (types, GC, tables, inline ops)
│   ├── clx_simd.h              # Cross-platform SIMD helpers for type-array scans
│   ├── lua.h / lauxlib.h / lualib.h  # Lua 5.5 C API headers (for --modules C modules)
├── src/
│   ├── clx.cpp                  # Compiler driver / CLI
│   ├── syntax/
│   │   ├── lexer.h/cpp          # Tokenizer/scanner
│   │   ├── parser.h/cpp         # Recursive descent parser
│   │   └── nodes.h              # AST node definitions
│   ├── optimizer/
│   │   ├── optimizer.h/cpp      # Optimizer driver (thin orchestrator over the passes)
│   │   ├── passes.h             # PassScratch + pass declarations, traverse_node
│   │   ├── analysis_state.h     # AnalysisState shared across passes
│   │   ├── owners.cpp           # Ownership, goto/labels, name scans, hygiene
│   │   ├── numbers.cpp          # Constants/bounds, numeric params, int returns, nogc locals
│   │   ├── functions.cpp        # Callables, multi-native tuples
│   │   ├── tables.cpp           # Pure arrays, array safety, table fields, arenas
│   │   └── structs.cpp          # Struct-local optimization
│   ├── codegen/
│   │   ├── codegen.h/cpp        # Code generator driver (calls optimizer internally)
│   │   ├── helpers.h/cpp        # Shared free helpers for emission
│   │   ├── expressions.cpp      # Native ops, calls, literals, table access
│   │   ├── statements.cpp       # Blocks, functions, returns, control flow
│   │   ├── loops.cpp            # for/generic-for, loop-table hoists
│   │   └── assignments.cpp      # Assignments, table constructors, snapshots
│   └── runtime/                  # Runtime library (libclx.a)
│       ├── state.cpp             # LState lifecycle, globals, open/close
│       ├── values.cpp            # LValue conversions, comparisons, metamethods
│       ├── tables.cpp            # LTable core (array/hash, get/set, inline cache)
│       ├── threads.cpp           # LThread fibers, root_stash/root_load, coro switching
│       ├── gc.cpp                # Generational GC (mark/sweep, barriers, finalizers)
│       ├── calls.cpp             # call_function / pcall / call_direct, lazy registry
│       ├── internal.h            # Shared internal declarations
│       ├── base.cpp              # Base module (print, error, type, pcall, etc.)
│       ├── table.cpp             # Table module (insert, remove, concat, sort, etc.)
│       ├── math.cpp              # Math module
│       ├── strings.cpp           # String module
│       ├── coroutine.cpp         # Coroutine module
│       ├── io.cpp                # I/O module
│       ├── os.cpp                # OS module
│       ├── utf8.cpp              # UTF-8 module
│       ├── package.cpp           # AOT package/module system
│       ├── fulllibs.cpp          # openlibs() — opens the std modules
│       ├── openlibs.cpp          # AOT standard modules loader helpers
│       ├── luaapi.cpp            # Lua C API compatibility shim (libclx_capi*.a)
│       ├── coro_switch_aarch64.S # ARM64 fiber switch (Apple + Linux)
│       ├── coro_switch_x86_64.s  # x86_64 fiber switch (Linux)
│       └── vm/                   # Optional embedded Lua VM and bridge
│           ├── dynamic_vm.cpp    # VM lifetime, environment, and proxy roots
│           ├── load_builtin.cpp  # load/loadfile/dofile registration
│           ├── vm_convert.cpp    # clx ↔ VM value conversion
│           ├── vm_function_object.cpp # VM function wrapper
│           ├── vm_native_bridge.cpp   # VM-to-clx callable dispatch
│           └── lua/              # Vendored Lua VM sources
├── tests/                       # End-to-end test suite
├── examples/                    # Example clx projects using the C++ embedding API
│   ├── hello/                   # Minimal hello-world
│   ├── mandelbrot/              # Mandelbrot viewer
│   ├── pong/                    # Pong game
│   ├── sokol/                   # Sokol graphics module for clx
│   └── weather/                 # Weather app using a Lua C module
├── benchmarks/                  # Performance benchmarks with comparisons (lua / luajit / clx)
└── doc/                         # Comprehensive documentation
    ├── index.md
    ├── api.md
    ├── cli.md
    ├── modules.md
    ├── luacapi.md
    ├── benchmarks.md
    ├── getting-started.md
    ├── install.md
    ├── compatibility.md
    ├── dynamic-lua.md
    ├── migration-guide.md
    └── internals/               # Internal design docs
        ├── architecture.md
        ├── optimizations.md
        └── runtime.md
```

## Components

### 1. CLI (src/clx.cpp)

The command-line interface handles:
- Argument parsing (`--executable`, `--object`, `--static`, `--debug`, `--size`, `--fast`, `--cpp`, `--modules`, `--dynamic`, `--output`/`-o`, `--minimal`, `--version`, `--cxx`, `--help`)
- File I/O and multiple lua files compilation
- Invoking the C++ compiler (fixed at build time via CMake)
- Locating its own runtime libraries, headers, and native modules via a unified search: the in-tree `build/` dir and executable-relative `lib`/`lib64` dirs first, then the install prefix/libdir embedded from CMake/GNUInstallDirs (`CLX_INSTALL_PREFIX`/`CLX_INSTALL_LIBDIR`/`CLX_INSTALL_INCLUDEDIR`). Default install prefix: `/usr/local` on POSIX, `%ProgramFiles%\clx` on Windows.
- Selecting the optional dynamic link path: `--dynamic` links `libclx_lua.a` on POSIX or `clx_lua.lib` on Windows and emits registration for `load`, `loadfile`, and `dofile`
- Output file management and temp file cleanup
- Dead code elimination by default via `-ffunction-sections` + `-Wl,--gc-sections` (Linux gcc/clang), `-Wl,-dead_strip` (macOS), or `/Gy /link /OPT:REF /OPT:ICF` (MSVC)
- Default optimization profile is `--size`: `-O1 -foptimize-sibling-calls -fno-inline-functions -fvisibility=hidden` (gcc/clang) or `/O1 /Ob0 /GL /GR- /MD /EHsc /GS- /fp:fast /Gw /Gy /utf-8` (MSVC); `--fast` switches to `-O3 -flto=auto` / `/O2 /Ot /GL`

### 2. Lexer (src/syntax/lexer.cpp)

The tokenizer converts raw source code into a stream of tokens:
- Keywords (`if`, `while`, `function`, etc.)
- Identifiers
- Literals (numbers, strings)
- Operators (+, -, *, /, etc.)
- Delimiters (parentheses, braces, brackets)

### 3. Parser (src/syntax/parser.cpp)

The recursive descent parser builds an Abstract Syntax Tree (AST):
- `parse_statement()` - parses statements
- `parse_expression()` - parses expressions
- `parse_block()` - parses statement blocks
- `parse_funcbody()` - parses function definitions

### 4. AST Nodes (src/syntax/nodes.h)

Core AST node types:
- `Block` - statement block
- `Identifier` - variable reference
- `BinaryOp` / `UnaryOp` - operations
- `FunctionDef` - function definition
- `TableConstructor` - table literals
- `ForStatement` / `WhileStatement` - loops
- `IfStatement` - conditional
- `CallExpression` - function calls

### 5. Optimizer (src/optimizer/)

`Optimizer::run()` is a thin orchestrator over 15 pass functions (declared in `passes.h`,
implemented across `owners.cpp`, `numbers.cpp`, `functions.cpp`, `tables.cpp`,
`structs.cpp`), sharing run-scoped state via `PassScratch`/`AnalysisState`. Passes analyze
the AST and annotate nodes with optimization hints:
- Numeric expression detection (Integer and Number fast paths)
- Variable scope resolution
- Table access purity analysis
- Constant folding preparation
- `yields_number` analysis for numeric for loops
- Non-fast function parameter numeric detection (marks params used in arithmetic as native doubles)
- Function parameter numeric-record array inference (traces `local bi = bodies[i]` + field accesses to prove numeric fields)

(Inline-cache invalidation uses `hash_version`, which is *runtime* table state, not an
optimizer pass.)

### 6. Code Generator (src/codegen/codegen.cpp)

The code generator produces C++ code:
- Fast-path code for numeric operations (Integer and Number)
- Slow-path code for dynamic operations
- Loop transformation (numeric for, generic for with direct `LCFunction` pointer calls)
- Expression emission with `[[likely]]` branch prediction hints
- Per-LTable `InlineCache` (8 entries, `LTABLE_IC_SIZE`) for string-keyed table access, checked inside `LTable::gettable()`/`LTable::settable()`
- StringBuilder-based string concatenation
- Wyhash-based string hashing

### 7. Runtime Library (src/runtime/)

The runtime library implements Lua semantics:
 - `state.cpp` - LState lifecycle, slab allocation, globals, open/close
 - `values.cpp` - LValue conversions/comparisons, metamethod dispatch, arithmetic
 - `tables.cpp` - LTable core (array/hash storage, get/set, inline cache)
 - `threads.cpp` - LThread fibers, root_stash/root_load, coro switching
 - `gc.cpp` - Generational GC (mark/sweep, barriers, finalizers, GCStack)
 - `calls.cpp` - call_function/pcall/call_direct, lazy function registry, `set_lazy_funcs`
 - `base.cpp` - Base library (print, error, type, rawequal, rawget, rawset, rawlen, warn, _VERSION)
 - `table.cpp` - Table library (insert, remove, concat, sort, unpack, pack, move)
 - `math.cpp` - Math library
 - `strings.cpp` - String library (len, sub, reverse, lower, upper, rep, byte, char, format, find, match, gmatch, gsub)
 - `coroutine.cpp` - Coroutine support (OS-level fibers; hand-written assembly on ARM64 for Apple+Linux and x86_64 Linux, ucontext fallback elsewhere)
 - `io.cpp` - I/O library
 - `os.cpp` - OS library (clock, time, date, difftime, execute, tmpname, getenv)
 - `utf8.cpp` - UTF-8 library
 - `package.cpp` - AOT package/module system
 - `luaapi.cpp` - Lua C API compatibility shim (built as `libclx_capi*.a`)

The optional dynamic path is kept separate from these AOT modules:

- `runtime/vm/lua/` - Vendored Lua VM sources and its standard libraries
- `runtime/vm/dynamic_vm.cpp` - Creates and owns one embedded VM per clx `LState`, including its VM environment and proxy roots
- `runtime/vm/load_builtin.cpp` - Registers `load`, `loadfile`, and `dofile` when the generated program was linked with `--dynamic`
- `runtime/vm/vm_convert.cpp` - Converts primitive values and creates proxies for supported clx tables and userdata
- `runtime/vm/vm_function_object.cpp` - Represents a VM function as a callable clx value
- `runtime/vm/vm_native_bridge.cpp` - Exposes clx callables to the VM and dispatches calls back into the AOT runtime
- `runtime/vm/vm_table_proxy.cpp` - Implements VM-side access to proxied clx tables

`--dynamic` does not replace clx's AOT `string`, `math`, `table`, `io`, `os`, `coroutine`, or `package` modules. Dynamically loaded chunks use the embedded VM's own libraries and `require` implementation. The two package systems and coroutine systems remain runtime-local.

## Data Flow

### Key Runtime Components

#### StringPool / StringArena

Open-addressed hash map for string interning, backed by a block-based `StringArena` (64 KB
blocks, bump-allocated). Each slot owns a baked allocation with a **16-byte header**
`[uint32 hash_low @0][pad @4][uint32 len @8][pad @12][char data...\0 @16]`. LValue stores a
pointer to the char data (16 bytes past alloc start). One probe on hit, no `std::string`, no
side map, no double lookup. Supports `intern_preallocated()` for zero-allocation string
concatenation.

#### wyhash

Fast, high-quality 64-bit hash function used for table keys and string interning. Uses
compile-time constant secrets with 128-bit multiply (`clx_umul128`) for excellent avalanche.
For interned strings, the low 32 bits of the hash are baked into the 16-byte allocation
header, making `lvalue_hash()` a single 4-byte load.

For strings ≤8 bytes, `swar_hash_8()` replaces `wyhash_str` — loads all bytes into one register
with a single `memcpy` and mixes via `wyhash64`. Used consistently for both TAG_ISTR inline
strings and short interned strings so cross-type hash compatibility is maintained.

#### Per-LTable Inline Cache

Each `LTable`'s lazily-allocated `LTableExt` holds an 8-entry inline cache that accelerates
repeated string-keyed reads. Each entry caches the key payload, entry index, and the table's
`hash_version` at the time of the last successful probe — no values are stored, so there is
nothing to dangle after collection. On the next read, the cache is checked first — if key and
version match, the cached entry index is used directly, skipping the full hash-probe path.
Cache invalidation is structural: `hash_version` bumps only on hash **rehash** (the sole
invalidator that relocates entries), plus metatable set and table recycle — plain inserts and
deletes never move entries and intentionally leave the version alone, and value updates never
bump it, so read-then-write patterns still hit the cache.

The cache lives in `LTableExt` (lazily allocated), so pure array-part tables never pay for it.
The hot `table_get` fast path (array indexing + inline cache) stays inline; misses fall through
to an out-of-line `table_get_slow` for the hash probe and `__index` dispatch.

#### StringBuilder

O(n) string concatenation that avoids the O(n²) quadratic blow-up of repeated `s = s .. part`
patterns. Uses inline storage for up to 8 parts, grows to heap allocation when needed. Produces
a single interned string with baked hash on `to_string()`.

#### Table Version Tracking

Tables track `hash_version`, which bumps on hash **rehash** (relocating entries), metatable
set, and table recycle. The per-LTable inline cache checks `hash_version` to detect stale
entries after those structural changes. Plain inserts/deletes and value updates do not bump
the version, allowing efficient read-then-write patterns.

## Data Flow

### Compilation Process

1. **Input**: Lua source file
2. **Lexing**: Source → Token stream
3. **Parsing**: Token stream → AST
4. **Optimization**: AST → Annotated AST
5. **Codegen**: Annotated AST → C++ source
6. **Compilation**: C++ source → Native binary

### Runtime Process

1. **Initialization**: Create the clx `LState` and register the AOT standard libraries
2. **Execution**: Run compiled code using native arithmetic where possible
3. **Fallback**: Use the clx `LValue` representation when types are unknown
4. **Cleanup**: Garbage collection of unused clx objects

### Dynamic Runtime Process (`--dynamic`)

1. **Linking**: Add the optional embedded VM archive and dynamic bridge registration to the generated executable
2. **Initialization**: Keep the embedded VM dormant until `load`, `loadfile`, `dofile`, or another VM bridge path is used
3. **VM setup**: Create one `DynamicVM` for the clx `LState`, open the VM's own standard libraries, and build the default VM-side environment
4. **Loading**: Compile source or binary chunks with the VM's loader; `load` accepts the current bridge's string form and uses the VM's `loadbufferx` path
5. **Calling**: Wrap loaded VM functions as clx-callable values, convert arguments into VM values, and convert returned values back into clx values
6. **Reverse calls**: Expose clx functions to VM code through `NativeBridge`, with table proxies and tracked table-argument write-back where supported
7. **Cleanup**: Destroy VM-owned values with the VM collector and release clx-side proxy roots with the clx collector

The dynamic path is a boundary, not an alternate code generator. A loaded chunk is interpreted by the embedded VM; it is never turned into AOT C++ during program execution.

## Memory Management

### Value Representation

clx uses a 16-byte `LValue` (8-byte payload + separate `ValueType` tag) to store Lua values:
- Numbers: Direct double representation (IEEE 754)
- Integers: 64-bit signed integer in TValue payload
 - Strings: Either TAG_ISTR inline (≤6 bytes, stored directly in `val`) or pointer to heap-allocated interned string (via StringPool)
- Tables: Pointer to heap-allocated table
- Functions: Pointer to function closure
- Nil/Boolean: Special sentinel values
- Threads: Pointer to LThread (coroutine/fiber)
- Userdata: Pointer to user-defined data

### Runtime Ownership and Bridge Objects

The clx runtime owns `LValue`, `LTable`, clx functions, clx threads, and clx userdata. The embedded VM owns `lua_State`, Lua tables, Lua closures, Lua threads, and VM userdata. A value crossing the boundary is either copied, wrapped in bridge userdata, or rejected:

- nil, booleans, numbers, and strings are copied;
- clx tables become `clx_proxy` userdata in the VM, with bridge metamethods for supported access;
- clx userdata becomes `clx_userdata` proxy userdata; it is not exposed as a raw native pointer;
- clx functions become VM-side callable bridge closures;
- VM tables passed to a clx function are converted into clx tables, with write-back for tracked table arguments;
- ordinary VM-created userdata and lightuserdata are VM-local and convert to `nil` when passed to clx; actual VM threads are rejected, while only recognized internal bridge proxies have special handling;
- clx and VM coroutines cannot be resumed across the boundary, and a yield cannot cross it.

Proxy nodes keep the referenced clx object alive from the VM side. The two collectors still run independently; the bridge explicitly marks proxy targets when the clx collector builds its root set.

### Garbage Collection

Generational collector (default; `CLX_GC_MODE=incremental` opts out):
- **Minor collections**: trace roots (the root-frame chain, threads, permanent roots) plus the remembered set, mark reachable young objects, promote survivors (young to survivor to old), and free unreachable survivor garbage — old objects are never traced or freed here. See [Runtime](./runtime.md) for the `RootRange`/`Frame`/`FrameGuard` rooting model, which replaced the old shadow stack and its `ScopeGuard` push/restore guards
- **Write barriers**: storing a young value into an old table, closure, coroutine or upvalue cell records the owner, so the next minor sees the edge
- **Major collections**: full mark-and-sweep fallback when the old generation outgrows its budget; the incremental collector remains available as the opt-out path

Tables whose metatables are set are linked into a `meta_next` metatable list, so the mark
phase scans only metatabled tables for finalizer references instead of every table. Permanent
roots (a small set of long-lived objects, including the string/io metatables and default io
handles) are marked directly each cycle so they are never collected. The mark phase validates
GC-header fields before writing and skips opaque/external userdata.

#### Per-function table arenas

The optimizer performs escape analysis on table constructors. Tables proven not to escape
their function (not returned, not stored in globals/upvalues/fields, not captured, not used
in growing loops) are allocated from a per-function bump-pointer arena (`FuncArena`) instead
of the collector heap. `arena_create_table` placement-news the whole table — `LTable` header,
array part, type part, hash part, and `LTableExt` — into one arena blob, freed wholesale at
function exit. This removes per-table `new`/`free` for the common temporary-table case and is
excluded from GC accounting.