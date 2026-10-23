// ┌─────────────────────────────────────────────┐
// │  clx — Lua to C++ Native Compiler           │
// │  Copyright (c) 2026 Tine Samir. MIT License.│
// ├─────────────────────────────────────────────┤
// │  dump_builtin.h · string.dump via the VM    │
// └─────────────────────────────────────────────┘

#ifndef CLX_VM_DUMP_BUILTIN_H
#define CLX_VM_DUMP_BUILTIN_H

#include <clx_runtime.h>

namespace clx {

//------------------ clx_string_dump_dynamic - takes over string.dump when linked with --dynamic:
//------------------ VM functions dump to stock bytecode strings, native closures come back as
//------------------ callable VMFunction boxes. Assigned to clx_string_dump_hook at startup.
MultiValue clx_string_dump_dynamic(LState *L, const LValue *args, size_t count);

}

#endif
