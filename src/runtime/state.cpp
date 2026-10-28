// ┌─────────────────────────────────────────────┐
// │  clx — Lua to C++ Native Compiler           │
// │  Copyright (c) 2026 Tine Samir. MIT License.│
// ├─────────────────────────────────────────────┤
// │  state.cpp · LState, allocators & modules   │
// └─────────────────────────────────────────────┘

#include "clx.h"
#include "clx_runtime.h"
#include "internal.h"
#include "vm/vm_convert.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace clx {

//------------------ LState::LState — state constructor
LState::LState()
    : allocated_objects(nullptr)
    , free_tables(nullptr)
    , free_tables_big(nullptr)
    , free_functions(nullptr)
    , metatabled_ud(nullptr)
    , current_file("")
    , current_line(0)
    , object_count(0)
    , gc_bytes_threshold(2 * 1024 * 1024)
    , string_metatable(nullptr) {
    _G = slab_alloc_table();
    allocated_bytes += sizeof(LTable);
    _G->next = allocated_objects;
    allocated_objects = _G;
    object_count++;
    _G->settable(LValue(intern_string("_G")), LValue(Table, _G));

    str_index = LValue(intern_string("__index"));
    str_newindex = LValue(intern_string("__newindex"));
    str_gc = LValue(intern_string("__gc"));
    str_call = LValue(intern_string("__call"));
    str_close = LValue(intern_string("__close"));
    str_pairs = LValue(intern_string("__pairs"));
    str_tostring = LValue(intern_string("__tostring"));
    str_len = LValue(intern_string("__len"));
    str_eq = LValue(intern_string("__eq"));
    str_lt = LValue(intern_string("__lt"));
    str_le = LValue(intern_string("__le"));
    str_concat = LValue(intern_string("__concat"));
    str_add = LValue(intern_string("__add"));
    str_sub = LValue(intern_string("__sub"));
    str_mul = LValue(intern_string("__mul"));
    str_div = LValue(intern_string("__div"));
    str_mod = LValue(intern_string("__mod"));
    str_idiv = LValue(intern_string("__idiv"));
    str_pow = LValue(intern_string("__pow"));
    str_unm = LValue(intern_string("__unm"));
    str_band = LValue(intern_string("__band"));
    str_bor = LValue(intern_string("__bor"));
    str_bxor = LValue(intern_string("__bxor"));
    str_shl = LValue(intern_string("__shl"));
    str_shr = LValue(intern_string("__shr"));
    str_bnot = LValue(intern_string("__bnot"));
    str_metatable = LValue(intern_string("__metatable"));
    str_name = LValue(intern_string("__name"));
    str_ipairs = LValue(intern_string("__ipairs"));

    //------------------ GC work-vector pre-sizing (callgrind: vector growth is

    gc_worklist.reserve(1 << 18);
    gc_recent.reserve(1 << 18);
    gc_remembered.reserve(1 << 16);
    gc_pinned.reserve(1 << 16);

    //------------------ GC mode / tuning knobs (environment override)

    if (const char *e = getenv("CLX_GC_MODE")) {
        if (strcmp(e, "incremental") == 0 || strcmp(e, "incr") == 0) {
            gc_mode = GCMode::Incremental;
        }
    }
    if (const char *e = getenv("CLX_GC_MINOR_KB")) {
        long long v = atoll(e);
        if (v > 0) {
            gc_minor_threshold = size_t(v) * 1024;
            gc_minor_threshold_env = true;
        }
    }
    if (const char *e = getenv("CLX_GC_MAJOR_KB")) {
        long long v = atoll(e);
        if (v > 0)
            gc_major_threshold = size_t(v) * 1024;
    }
}

static void dtor_free_table(LTable *t) {
    table_free_array(t->array, t->array_types, t->small_array);
    if (t->ext) {
        table_free_hash(t->ext->entries, t->ext->hash_bitmap);
        if (t->ext->ic) {
            delete[] t->ext->ic;
            t->ext->ic = nullptr;
        }
        delete t->ext;
        t->ext = nullptr;
    }
    t->array_size = t->array_cap = 0;
    if (t->flags & LFLAG_SLAB) {
        //------------------ slab object: storage lives in a slab, released whole by slab_release()
        t->flags &= ~LFLAG_SLAB;
        return;
    }
    delete t;
}

//------------------ LState::slab_alloc_table — bump-allocate an LTable from the current slab
LTable *LState::slab_alloc_table() {
    static_assert(alignof(LTable) <= 16, "slab bump assumes 16-byte-aligned table slots");
    if (slab_current + sizeof(LTable) > slab_end) {
        constexpr size_t kAlign = 16;

        size_t slab_bytes = sizeof(TableSlab) + kAlign + SLAB_TABLE_COUNT * sizeof(LTable);
        char *mem = static_cast<char *>(std::malloc(slab_bytes));
        if (!mem)
            throw std::bad_alloc();
        TableSlab *slab = reinterpret_cast<TableSlab *>(mem);
        slab->next = slab_blocks;
        slab_blocks = slab;
        uintptr_t data = reinterpret_cast<uintptr_t>(mem) + sizeof(TableSlab);
        data = (data + (kAlign - 1)) & ~static_cast<uintptr_t>(kAlign - 1);
        slab_current = reinterpret_cast<char *>(data);
        slab_end = mem + slab_bytes;
    }
    LTable *t = new (slab_current) LTable();
    slab_current += sizeof(LTable);
    t->flags |= LFLAG_SLAB;
    return t;
}

//------------------ LState::slab_release — free all table slabs (state close only)
void LState::slab_release() {
    TableSlab *s = slab_blocks;
    while (s) {
        TableSlab *n = s->next;
        std::free(s);
        s = n;
    }
    slab_blocks = nullptr;
    slab_current = slab_end = nullptr;
}

//------------------ LState::invoke_gc_finalizer — call __gc metamethod on userdata
void LState::invoke_gc_finalizer(LUserdata *ud, const char *tag) {
    if (!ud->metatable)
        return;
    LValue gc_func = ud->metatable->gettable(this->str_gc);
    if (gc_func.type != Function)
        return;
    LValue args[1] = { LValue(UserData, ud) };
    RootRange rr(this, args, 1);
    try {
        call_function(this, gc_func, args, 1, tag, 0);
    } catch (const LRuntimeException &e) {
        std::cerr << "error in __gc metamethod: " << e.what() << "\n";
    } catch (std::exception &e) {
        std::cerr << "error in __gc metamethod: " << e.what() << "\n";
    } catch (...) {
        std::cerr << "error in __gc metamethod\n";
    }
}

void (*clx_io_flush_all_ptr)() = nullptr;
void (*clx_io_write_ptr)(FILE *fp, const char *p, size_t n) = nullptr;
MultiValue (*clx_string_dump_hook)(LState *L, const LValue *args, size_t count) = nullptr;

//------------------ LState::~LState — state destructor
LState::~LState() {
    if (clx_io_flush_all_ptr)
        clx_io_flush_all_ptr();
    if (luaapi_cleanup)
        luaapi_cleanup(this);
    if (gc_phase == GCPhase::Sweeping) {
        while (gc_phase == GCPhase::Sweeping)
            gc_step();
    }

    for (int pass = 0; pass < 2; ++pass) {
        for (LHeader *h = (pass == 0) ? allocated_objects : old_objects; h; h = h->next) {
            if (h->type == static_cast<uint8_t>(UserData)) {
                LUserdata *ud = static_cast<LUserdata *>(h);
                invoke_gc_finalizer(ud, "StateClose_Finalizer");
            }
        }
    }

    for (int pass = 0; pass < 2; ++pass) {
        LHeader *curr = (pass == 0) ? allocated_objects : old_objects;
        while (curr) {
            LHeader *next = curr->next;
            if (curr->flags & LFLAG_VM_PROXY) {
                if (clx_free_vm_proxy_ptr)
                    clx_free_vm_proxy_ptr(this, curr);
            } else if (curr->type == static_cast<uint8_t>(Table))
                dtor_free_table(static_cast<LTable *>(curr));
            else if (curr->type == static_cast<uint8_t>(Function))
                delete static_cast<LCFunction *>(curr);
            else if (curr->type == static_cast<uint8_t>(UserData))
                delete[] reinterpret_cast<char *>(curr);
            else if (curr->type == static_cast<uint8_t>(Thread))
                delete static_cast<LThread *>(curr);
            curr = next;
        }
    }
    allocated_objects = nullptr;
    old_objects = nullptr;

    {
        LTable *t = static_cast<LTable *>(gc_finalizable);
        while (t) {
            LTable *nxt = static_cast<LTable *>(t->next);
            dtor_free_table(t);
            t = nxt;
        }
    }
    gc_finalizable = nullptr;

    {
        LUserdata *ud = static_cast<LUserdata *>(gc_finalizable_ud);
        while (ud) {
            LUserdata *nxt = static_cast<LUserdata *>(ud->next);
            invoke_gc_finalizer(ud, "GC_Finalizer");
            delete[] reinterpret_cast<char *>(ud);
            ud = nxt;
        }
    }
    gc_finalizable_ud = nullptr;

    LTable *ft = free_tables;
    while (ft) {
        LTable *next = static_cast<LTable *>(ft->next);
        dtor_free_table(ft);
        ft = next;
    }
    free_tables = nullptr;
    ft = free_tables_big;
    while (ft) {
        LTable *next = static_cast<LTable *>(ft->next);
        dtor_free_table(ft);
        ft = next;
    }
    free_tables_big = nullptr;

    slab_release();

    LCFunction *ff = free_functions;
    while (ff) {
        LCFunction *next = static_cast<LCFunction *>(ff->next);
        delete ff;
        ff = next;
    }
    free_functions = nullptr;

    LThread *fth = free_threads;
    while (fth) {
        LThread *nxt0 = static_cast<LThread *>(fth->next);
        delete fth;
        fth = nxt0;
    }
    free_threads = nullptr;
    std::free(overflow_heap);
    gc_worklist.free();
    gc_recent.free();
    gc_pinned.free();
}

//------------------ CloseGuard::~CloseGuard — close guard destructor
CloseGuard::~CloseGuard() {
    if (val.type != Table)
        return;
    LTable *t = static_cast<LTable *>(val.as_pointer());
    LTable *mt = tbl_metatable(t);
    if (!mt)
        return;
    LValue close_func = mt->gettable(L->str_close);
    if (close_func.type == Nil)
        return;

    LValue args[2] = { val, LValue() };
    try {
        call_function_rooted(L, close_func, args, 2, "CloseGuard", 0);
    } catch (const LRuntimeException &e) {
        std::cerr << "error in __close metamethod: " << e.what() << "\n";
    } catch (std::exception &e) {
        std::cerr << "error in __close metamethod: " << e.what() << "\n";
    } catch (...) {
        std::cerr << "error in __close metamethod\n";
    }
}

//------------------ LState::register_module — register module loader
void LState::register_module(const std::string &name, LValue (*func)(LState *)) {
    register_static_preload(this, name.c_str(), func);
}

//------------------ LState::register_loaded_module — seed package.loaded[name] with an already-built module value
void LState::register_loaded_module(const std::string &name, const LValue &module) {
    LValue pack_val = get_global(this, "package");
    if (pack_val.type != ValueType::Table)
        return;
    LValue loaded = static_cast<LTable *>(pack_val.as_pointer())->gettable(LValue(intern_string("loaded")));
    if (loaded.type != ValueType::Table)
        return;
    static_cast<LTable *>(loaded.as_pointer())->settable(LValue(intern_string(name)), module);
}

//------------------ LState::create_table — allocate a table
LValue LState::create_table(size_t asize, size_t hsize) {
    gc_maybe_collect();

    LTable *t;
    //------------------ Size-segregated pool pop: a sized creation reuses a big-buffer table (same-shape
    //------------------ waves land head-first), while a size-unknown creation ({}) takes a small one — a
    //------------------ 90KB array must never park, counted, on a table that will never use it while its
    //------------------ rightful user allocates another (linear pool growth wave-over-wave).
    bool want_big = (asize > 0);
    LTable **pool = want_big ? &free_tables_big : &free_tables;
    if (!*pool)
        pool = want_big ? &free_tables : &free_tables_big;
    bool recycled_tbl = (*pool != nullptr);
    if (recycled_tbl)
        g_alloc_census.table_recycled++;
    else
        g_alloc_census.table_new++;
    uint8_t prev_age_tbl = recycled_tbl ? (*pool)->age : AGE_YOUNG;
    if (*pool) {
        t = *pool;
        *pool = static_cast<LTable *>((*pool)->next);
        if (!want_big && t->array && t->array != t->small_array) {
            //------------------ small creation drew big (small list drained): free the array it will never
            //------------------ use instead of parking it. No accounting: dispose already subtracted it.
            table_free_array(t->array, t->array_types, t->small_array);
            t->array_cap = 0;
        }
        if (t->ext) {
            if (!t->ext->entries)
                t->ext->hash_size = 0;
            t->ext->hash_count = 0;
            t->ext->hash_tombs = 0;
            t->ext->hash_version++;
            t->ext->metatable = nullptr;
            if (t->ext->entries) {
                for (size_t i = 0; i < t->ext->hash_size; ++i) {
                    t->ext->entries[i].key.payload.u64 = HASH_EMPTY;
                    t->ext->entries[i].ktype = Nil;
                }
                if (t->ext->hash_bitmap)
                    std::memset(t->ext->hash_bitmap, 0, ((t->ext->hash_size + 63) / 64) * sizeof(uint64_t));
            }
        }
        allocated_bytes += sizeof(LTable);
    } else {
        t = slab_alloc_table();
        allocated_bytes += sizeof(LTable);
    }

    if (asize > 0) {
        if (asize <= 2) {

            table_free_array(t->array, t->array_types, t->small_array);
            t->array = t->small_array;
            t->array_types = t->small_array_types;
            t->array_size = asize;
            t->array_cap = asize;
            t->array_types[0] = ValueType::Nil;
            t->array_types[1] = ValueType::Nil;
        } else if (t->array_cap >= asize) {
            t->array_size = asize;
            std::fill(t->array_types, t->array_types + asize, ValueType::Nil);
            t->array_cap = asize;
        } else {
            table_free_array(t->array, t->array_types, t->small_array);
            table_alloc_array(t->array, t->array_types, asize);
            t->array_size = asize;
            t->array_cap = asize;
        }
    } else {
        t->array_size = 0;
    }
    t->array_lim = 0;

    allocated_bytes += table_heap_bytes(t);

    if (hsize > 0)
        t->presize_hash(hsize);

    t->type = static_cast<uint8_t>(Table);
    t->marked = 0;
    t->age = AGE_YOUNG;
    if (recycled_tbl && prev_age_tbl != AGE_YOUNG && (gc_phase == GCPhase::Sweeping || gc_minor_active)) {

        t->flags |= LFLAG_GC_PIN;
        gc_pinned.push(t);
    }

    t->next = allocated_objects;
    allocated_objects = t;
    gc_recent.push(t);

    object_count++;
    return LValue(Table, t);
}

//------------------ LState::create_closure — allocate a closure
clx::LValue clx::LState::create_closure(CFunctionType func, LTable *env, std::vector<LUpValue> gc_cells) {
    gc_maybe_collect();

    LCFunction *f;
    bool recycled_fn = (free_functions != nullptr);
    if (recycled_fn)
        g_alloc_census.closure_recycled++;
    else
        g_alloc_census.closure_new++;
    uint8_t prev_age_fn = recycled_fn ? free_functions->age : AGE_YOUNG;
    if (free_functions) {
        f = free_functions;
        free_functions = static_cast<LCFunction *>(free_functions->next);
        f->func = std::move(func);
        f->env = env ? env : _G;
        f->marked = 0;
        f->gc_cells = std::move(gc_cells);
    } else {
        f = new LCFunction(std::move(func));
        f->env = env ? env : _G;
        f->gc_cells = std::move(gc_cells);
        allocated_bytes += sizeof(LCFunction);
    }

    auto *fnptr = f->func.target<MultiValue (*)(LState *, const LValue *, size_t)>();
    f->direct = fnptr ? *fnptr : nullptr;
    f->self_ref = LValue();
    f->type = static_cast<uint8_t>(Function);
    f->marked = 0;
    f->age = AGE_YOUNG;
    if (recycled_fn && prev_age_fn != AGE_YOUNG && (gc_phase == GCPhase::Sweeping || gc_minor_active)) {

        f->flags |= LFLAG_GC_PIN;
        gc_pinned.push(f);
    }
    f->next = allocated_objects;
    allocated_objects = f;
    gc_recent.push(f);
    object_count++;
    return clx::LValue(Function, f);
}

//------------------ newuserdata — allocate userdata
LValue newuserdata(LState *L, size_t size) {
    L->gc_maybe_collect();

    char *mem = new char[sizeof(LUserdata) + size];
    LUserdata *ud = reinterpret_cast<LUserdata *>(mem);
    ud->type = static_cast<uint8_t>(UserData);
    ud->marked = 0;
    ud->flags = 0;
    ud->age = AGE_YOUNG;
    ud->metatable = nullptr;
    ud->meta_next = nullptr;
    ud->size = size;

    ud->next = L->allocated_objects;
    L->allocated_objects = ud;
    L->gc_recent.push(ud);
    L->object_count++;
    L->allocated_bytes += sizeof(LUserdata) + size;
    return LValue(UserData, ud);
}

//------------------ get_global — get global variable
LValue get_global(LState *L, const char *name) {
    LValue val = L->_G->gettable(LValue(L->intern_string(name)));
    return val.type != Nil ? val : LValue();
}

//------------------ set_global — set global variable
void set_global(LState *L, const char *name, const LValue &val) {
    LValue key = LValue(L->intern_string(name));
    gc_barrier_table(L, L->_G, val);
    if (key.is_gc_obj())
        gc_barrier_table(L, L->_G, key);
    L->_G->settable(key, val);
}

//------------------ open — create Lua state
extern thread_local LState *clx_current_L;
thread_local LState *clx_current_L = nullptr;

//------------------ open_bare — create Lua state without registering any std library. Generated
//------------------ executables use this plus a filtered registration sequence so the linker strips
//------------------ unreferenced base functions; open() below preserves the legacy all-registered behavior.
LState *open_bare(int argc, char *argv[]) {
    LState *L = new LState();
    clx_current_L = L;

    LThread *main_th = new LThread();
    main_th->state = L;
    main_th->is_main = true;
    main_th->status = THREAD_RUNNING;
#if defined(_WIN32)
    main_th->fiber = ConvertThreadToFiber(nullptr);
#elif (defined(__APPLE__) || defined(__linux__)) && (defined(__aarch64__) || defined(__x86_64__))
    clx_coro_save(&main_th->ctx);
#elif defined(__wasi__)
    // the main thread is the WASI guest's own thread: nothing to capture (see CoroutineContext in clx_runtime.h)
#else
    getcontext(&main_th->ctx);
#endif
    L->main_thread = main_th;
    L->running_thread = main_th;

    if (argc > 0 && argv) {
        LValue arg_table = L->create_table(argc);
        LTable *t = static_cast<LTable *>(arg_table.as_pointer());
        for (int i = 0; i < argc; ++i) {
            t->settable(LValue(static_cast<int64_t>(i)), LValue(L->intern_string(argv[i])));
        }
        L->_G->settable(LValue(L->intern_string("arg")), arg_table);
    }

    return L;
}

//------------------ open — create Lua state with the full minimal std set (legacy behavior)
LState *open(int argc, char *argv[]) {
    LState *L = open_bare(argc, argv);
    luastd_base(L);
    luastd_package(L);
    return L;
}

//------------------ close — close Lua state
void close(LState *L) {
    if (std::getenv("CLX_ALLOC_STATS"))
        dump_alloc_census();
    if (clx_current_L == L)
        clx_current_L = nullptr;
#if defined(_WIN32)
    ConvertFiberToThread();
#endif
    delete L->main_thread;
    delete L;
}

}
