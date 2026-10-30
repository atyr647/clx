// ┌─────────────────────────────────────────────┐
// │  clx — Lua to C++ Native Compiler           │
// │  Copyright (c) 2026 Tine Samir. MIT License.│
// ├─────────────────────────────────────────────┤
// │  gc.cpp · Generational garbage collector    │
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

void (*clx_mark_vm_proxies_ptr)(LState *clx_L, LState::GCStack &wl) = nullptr;
void (*clx_free_vm_proxy_ptr)(LState *clx_L, LHeader *proxy) = nullptr;

static void clx_trigger_gc(LState *L, LTable *t) {
    LTable *mt = tbl_metatable(t);
    if (!mt)
        return;
    LValue gc_func = mt->gettable(L->str_gc);
    if (gc_func.type == Nil)
        return;

    LValue args[1] = { LValue(Table, t) };
    try {
        static constexpr CallSite finalizer_site = { "GC_Finalizer", 0, false, false };
        call_function_rooted(L, gc_func, args, 1, &finalizer_site);
    } catch (const LRuntimeException &e) {
        std::cerr << "error in __gc metamethod: " << e.what() << "\n";
    } catch (std::exception &e) {
        std::cerr << "error in __gc metamethod: " << e.what() << "\n";
    } catch (...) {
        std::cerr << "error in __gc metamethod\n";
    }
}

#define GC_SUB(TAG, PTR, amt)                                                                                          \
    do {                                                                                                               \
        if (static_cast<size_t>(amt) > allocated_bytes)                                                                \
            allocated_bytes = 0;                                                                                       \
        else                                                                                                           \
            allocated_bytes -= (amt);                                                                                  \
    } while (0)

//------------------ shared sweep helpers — used by both the incremental major sweep (gc_step)

static void gc_dispose_swept(LState *L, LHeader *curr) {
    auto &GC_SUB = L->allocated_bytes;
    (void)GC_SUB;
    curr->flags &= ~(LFLAG_REMEMBERED | LFLAG_GC_PIN);
    //------------------ Old-byte accounting only changes when an OLD object dies (to is always YOUNG
    //------------------ here); skip the type-switch + size computation for young/survivor garbage.
    if (curr->age == AGE_OLD)
        L->gc_update_old_bytes(curr, AGE_OLD, AGE_YOUNG);
    if (curr->flags & LFLAG_VM_PROXY) {
        if (clx_free_vm_proxy_ptr)
            clx_free_vm_proxy_ptr(L, curr);
    } else if (curr->type == static_cast<uint8_t>(Table)) {
        LTable *t = static_cast<LTable *>(curr);
        if (tbl_metatable(t)) {
            t->flags |= LFLAG_FINALIZABLE;
            t->next = L->gc_finalizable;
            L->gc_finalizable = t;
        } else {
            const size_t tbl_bytes = sizeof(LTable) + table_heap_bytes(t);
            if (tbl_bytes > L->allocated_bytes)
                L->allocated_bytes = 0;
            else
                L->allocated_bytes -= tbl_bytes;
            if (t->ext) {
                t->ext->hash_count = 0;
                t->ext->hash_tombs = 0;
                //------------------ A pooled table keeps its hash buffer for reuse, but any table can be handed any creation: a 4096-entry set parked in the pool made every
                //------------------ small table created from it clear 4096 entries, and each parked buffer stayed allocated for good (uncounted, since the bytes are subtracted
                //------------------ above). Only small buffers are kept.
                if (t->ext->entries && t->ext->hash_size > LState::kPooledHashMax) {
                    table_free_hash(t->ext->entries, t->ext->hash_bitmap);
                    t->ext->hash_size = 0;
                    t->ext->hash_version++;
                }
            }
            t->array_size = 0;
            t->array_lim = 0;
            if (t->array && t->array != t->small_array) {
                t->next = L->free_tables_big;
                L->free_tables_big = t;
            } else {
                t->next = L->free_tables;
                L->free_tables = t;
            }
        }
    } else if (curr->type == static_cast<uint8_t>(Function)) {
        LCFunction *f = static_cast<LCFunction *>(curr);
        f->func = nullptr;
        f->direct = nullptr;
        f->env = nullptr;
        f->self_ref = LValue();
        f->gc_cells.clear();
        f->next = L->free_functions;
        L->free_functions = f;
    } else if (curr->type == static_cast<uint8_t>(Thread)) {
        LThread *th = static_cast<LThread *>(curr);
        size_t th_bytes = sizeof(LThread) + th->stack_bytes;
        if (th_bytes > L->allocated_bytes)
            L->allocated_bytes = 0;
        else
            L->allocated_bytes -= th_bytes;
#if defined(_WIN32)
        if (th->fiber && L->free_fiber_threads < kMaxPooledFibers) {
            L->free_fiber_threads++;
        } else {
            if (th->fiber)
                DeleteFiber(th->fiber);
            th->fiber = nullptr;
        }
#endif
        th->next = L->free_threads;
        L->free_threads = th;
    } else if (curr->type == static_cast<uint8_t>(UserData)) {
        LUserdata *ud = static_cast<LUserdata *>(curr);
        if (ud->metatable) {
            ud->next = L->gc_finalizable_ud;
            L->gc_finalizable_ud = ud;
        } else {
            size_t ud_bytes = sizeof(LUserdata) + ud->size;
            if (ud_bytes > L->allocated_bytes)
                L->allocated_bytes = 0;
            else
                L->allocated_bytes -= ud_bytes;
            delete[] reinterpret_cast<char *>(ud);
        }
    }
}

//------------------ gc_prune_meta_list: unlink dead metatabled tables in one O(N) pass; per-table removal would be O(M*N).

static void gc_prune_meta_list(LState *L) {
    LTable **pp = &L->metatabled_tables;
    while (*pp) {
        LTable *n = *pp;
        if (n->flags & LFLAG_FINALIZABLE) {
            *pp = n->ext->meta_next;
            n->ext->meta_next = nullptr;
            n->flags &= ~(LFLAG_META_LIST | LFLAG_FINALIZABLE);
        } else {
            pp = &n->ext->meta_next;
        }
    }
}

static void gc_drain_finalizables(LState *L) {
    gc_prune_meta_list(L);
    for (LTable *t = static_cast<LTable *>(L->gc_finalizable); t;) {
        LTable *nx = static_cast<LTable *>(t->next);
        meta_list_remove(L, t);
        clx_trigger_gc(L, t);
        size_t t_bytes = sizeof(LTable) + table_heap_bytes(t);
        if (t_bytes > L->allocated_bytes)
            L->allocated_bytes = 0;
        else
            L->allocated_bytes -= t_bytes;
        table_free_array(t->array, t->array_types, t->small_array);
        if (t->ext) {
            table_free_hash(t->ext->entries, t->ext->hash_bitmap);
            t->ext->hash_count = 0;
            t->ext->hash_tombs = 0;
            t->ext->hash_size = 0;
            t->ext->metatable = nullptr;
            t->ext->meta_next = nullptr;
        }
        t->array_size = t->array_cap = 0;
        t->array_lim = 0;
        t->flags &= ~(LFLAG_META_LIST | LFLAG_FINALIZABLE);
        if (t->array && t->array != t->small_array) {
            t->next = L->free_tables_big;
            L->free_tables_big = t;
        } else {
            t->next = L->free_tables;
            L->free_tables = t;
        }
        t = nx;
    }
    L->gc_finalizable = nullptr;
    for (LUserdata *ud = static_cast<LUserdata *>(L->gc_finalizable_ud); ud;) {
        LUserdata *nx = static_cast<LUserdata *>(ud->next);
        meta_ud_list_remove(L, ud);
        L->invoke_gc_finalizer(ud, "GC_Finalizer");
        size_t ud_bytes = sizeof(LUserdata) + ud->size;
        if (ud_bytes > L->allocated_bytes)
            L->allocated_bytes = 0;
        else
            L->allocated_bytes -= ud_bytes;
        delete[] reinterpret_cast<char *>(ud);
        ud = nx;
    }
    L->gc_finalizable_ud = nullptr;
}

//------------------ LState::gc_remember — record an old-generation owner for the next minor
void LState::gc_remember(LHeader *owner) {
    if (owner->flags & LFLAG_REMEMBERED)
        return;
    owner->flags |= LFLAG_REMEMBERED;
    gc_remembered.push_back(owner);
}

//------------------ GCStack growth (cold) + reserve/free
void LState::gc_stack_grow(LState::GCStack *s) {
    size_t ncap = s->cap ? s->cap * 2 : 1024;
    LHeader **nd = static_cast<LHeader **>(std::realloc(s->data, ncap * sizeof(LHeader *)));
    if (!nd)
        throw std::bad_alloc();
    s->data = nd;
    s->cap = ncap;
}

void LState::GCStack::reserve(size_t n) {
    if (n <= cap)
        return;
    LHeader **nd = static_cast<LHeader **>(std::realloc(data, n * sizeof(LHeader *)));
    if (!nd)
        throw std::bad_alloc();
    data = nd;
    cap = n;
}

void LState::GCStack::free() {
    std::free(data);
    data = nullptr;
    top = cap = 0;
}

//------------------ LState::gc_remember_cell — record an upvalue cell for the next minor
void LState::gc_remember_cell(const LUpValue &cell) {
    if (!cell)
        return;
    if (!gc_remembered_cell_set.insert(cell.get()).second)
        return;
    gc_remembered_cells.push_back(cell);
}

//------------------ gc_barrier_header — slow path of the write barrier
void gc_barrier_header(LState *L, LHeader *owner, const LValue &newval) {
    if (!newval.is_gc_obj())
        return;
    LHeader *nv = static_cast<LHeader *>(newval.as_pointer());
    if (!nv)
        return;
    if (L->gc_barrier_all && owner->marked != 0) {
        //------------------ marking: an owner that is already marked (traced, or queued to be) must not hide a white object from the marking. An owner that is still white is traced
        //------------------ later and sees the new value then.
        if (L->gc_phase == LState::GCPhase::Marking)
            L->gc_mark_gray(newval);
        //------------------ sweeping: an owner the sweep has not reached yet (still marked) is promoted to the old generation when it does, and by then the object stored now is not
        //------------------ old: the owner is remembered, which keeps it where it is (young or survivor) until a minor collection has seen what it holds
        else if (L->gc_phase == LState::GCPhase::Sweeping && nv->age != AGE_OLD && owner->age != AGE_OLD) {
            L->gc_remember(owner);
            return;
        }
    }
    if (nv->age == AGE_OLD)
        return;
    if (owner->age == AGE_OLD || owner->age == AGE_SURVIVOR)
        L->gc_remember(owner);
}

//------------------ gc_barrier_cell — upvalue cell write barrier
void gc_barrier_cell(LState *L, const LUpValue &cell, const LValue &newval) {
    if (!newval.is_gc_obj())
        return;
    //------------------ a cell has no mark of its own; the closures that share it may all be traced already
    if (L->gc_phase == LState::GCPhase::Marking)
        L->gc_mark_gray(newval);
    LHeader *nv = static_cast<LHeader *>(newval.as_pointer());
    if (!nv || nv->age == AGE_OLD)
        return;
    L->gc_remember_cell(cell);
}

//------------------ mark helpers shared by major and minor marking

//------------------ chain_contains — membership check on one GC chain
static bool chain_contains(LHeader *head, const LHeader *h) {
    for (LHeader *o = head; o; o = o->next)
        if (o == h)
            return true;
    return false;
}

static CLX_INLINE_HOT LHeader *gc_mark_value(LState *L, const LValue &v, uint8_t markval) {
    if (!v.is_gc_obj())
        return nullptr;
    LHeader *h = v.as_pointer();
    if (!h)
        return nullptr;
    if (h->type != static_cast<uint8_t>(v.type))
        return nullptr;
    if (h->marked != 0)
        return nullptr;
    if (v.type == ValueType::UserData && !L->gc_userdata_known(h))
        return nullptr;
    h->marked = markval;
    return h;
}

static CLX_INLINE_HOT void gc_push_for_trace(LState::GCStack &wl, LHeader *h, LValue v) {
    ValueType t = v.type;
    if (t == Table || t == Thread || t == Function || t == UserData)
        wl.push(h);
}

static bool gc_trace_table_young(LState *L, LTable *t, LState::GCStack &wl) {
    bool has_nonold = false;
    for (size_t i = 0; i < t->array_size; ++i) {
        if (t->array_types[i] == Nil)
            continue;
        LValue v(t->array[i], t->array_types[i]);
        if (!v.is_gc_obj())
            continue;
        LHeader *h = v.as_pointer();
        if (!h || h->type != static_cast<uint8_t>(v.type))
            continue;
        if (h->age == AGE_OLD)
            continue;
        has_nonold = true;
        if (h->marked != 0)
            continue;
        if (v.type == ValueType::UserData && !L->is_allocated_userdata(h))
            continue;
        h->marked = 1;
        gc_push_for_trace(wl, h, v);
    }
    LTableExt *ex = t->ext;
    if (!ex)
        return has_nonold;
    auto trace_entry = [&](const HashEntry &e) {
        if (e.ktype == Nil)
            return;
        for (int which = 0; which < 2; ++which) {
            LValue v(which == 0 ? e.key : e.val, which == 0 ? e.ktype : e.vtype);
            if (!v.is_gc_obj())
                continue;
            LHeader *h = v.as_pointer();
            if (!h || h->type != static_cast<uint8_t>(v.type))
                continue;
            if (h->age == AGE_OLD)
                continue;
            has_nonold = true;
            if (h->marked != 0)
                continue;
            if (v.type == ValueType::UserData && !L->is_allocated_userdata(h))
                continue;
            h->marked = 1;
            gc_push_for_trace(wl, h, v);
        }
    };
    if (ex->hash_bitmap) {
        size_t words = (ex->hash_size + 63) / 64;
        for (size_t w = 0; w < words; ++w) {
            uint64_t bits = ex->hash_bitmap[w];
            while (bits) {
                size_t b = static_cast<size_t>(clx_ctzll(bits));
                bits &= bits - 1;
                size_t _i = (w << 6) + b;
                if (_i >= ex->hash_size)
                    break;
                trace_entry(ex->entries[_i]);
            }
        }
    } else {
        for (size_t _i = 0; _i < ex->hash_size; ++_i)
            trace_entry(ex->entries[_i]);
    }
    if (ex->metatable && ex->metatable->age != AGE_OLD && ex->metatable->marked == 0) {
        has_nonold = true;
        ex->metatable->marked = 1;
        wl.push(ex->metatable);
    } else if (ex->metatable && ex->metatable->age != AGE_OLD) {
        has_nonold = true;
    }
    return has_nonold;
}

static CLX_INLINE_HOT bool gc_mark_young(LState *L, const LValue &v, LState::GCStack &wl) {
    if (!v.is_gc_obj())
        return false;
    LHeader *h = v.as_pointer();
    if (!h || h->type != static_cast<uint8_t>(v.type) || h->age == AGE_OLD)
        return false;
    if (h->marked != 0)
        return true;
    if (v.type == ValueType::UserData && !L->is_allocated_userdata(h))
        return true;
    h->marked = 1;
    gc_push_for_trace(wl, h, v);
    return true;
}

static bool gc_trace_thread_young(LState *L, LThread *th, LState::GCStack &wl) {
    bool has_nonold = false;
    auto one = [&](const LValue &v) {
        if (gc_mark_young(L, v, wl))
            has_nonold = true;
    };
    one(th->function);
    if (th->caller)
        one(LValue(Thread, th->caller));
    for (size_t i = 0; i < th->yield_args.count; ++i)
        one(th->yield_args[i]);
    for (size_t i = 0; i < th->resume_args.count; ++i)
        one(th->resume_args[i]);
    gc_walk_roots(th->top_root, one);
    return has_nonold;
}

static bool gc_trace_function_young(LState *L, LCFunction *f, LState::GCStack &wl) {
    bool has_nonold = false;
    if (f->env) {
        LValue envv(Table, f->env);
        if (gc_mark_young(L, envv, wl))
            has_nonold = true;
    }
    for (const LUpValue &c : f->gc_cells)
        if (c && gc_mark_young(L, *c, wl))
            has_nonold = true;
    return has_nonold;
}

//------------------ LState::is_allocated_userdata — membership check on both GC chains
bool LState::is_allocated_userdata(const LHeader *h) const {
    return chain_contains(allocated_objects, h) || chain_contains(old_objects, h);
}

//------------------ gc_trace_remember_nonold_children — write-barrier catch-up for

static void gc_trace_remember_nonold_children(LTable *t) {
    LState *L = clx_current_L;
    if (!L)
        return;
    for (size_t i = 0; i < t->array_size; ++i) {
        if (t->array_types[i] == Nil)
            continue;
        LValue v(t->array[i], t->array_types[i]);
        if (!v.is_gc_obj())
            continue;
        LHeader *h = v.as_pointer();
        if (h && h->type == static_cast<uint8_t>(v.type) && h->age != AGE_OLD)
            L->gc_remember(t);
    }
    if (t->ext) {
        for (size_t i = 0; i < t->ext->hash_size; ++i) {
            HashEntry &e = t->ext->entries[i];
            if (e.ktype == Nil)
                continue;
            for (int which = 0; which < 2; ++which) {
                LValue v(which == 0 ? e.key : e.val, which == 0 ? e.ktype : e.vtype);
                if (!v.is_gc_obj())
                    continue;
                LHeader *h = v.as_pointer();
                if (h && h->type == static_cast<uint8_t>(v.type) && h->age != AGE_OLD) {
                    L->gc_remember(t);
                    return;
                }
            }
        }
        if (t->ext->metatable && t->ext->metatable->age != AGE_OLD)
            L->gc_remember(t);
    }
}

//------------------ LState::gc_minor — young-generation collection (generational mode)

void LState::gc_minor() {
    //------------------ re-entrancy guard: draining finalizables below runs Lua, which may allocate
    //------------------ and re-enter through gc_maybe_collect().
    if (gc_minor_active)
        return;
    struct timespec _gt0, _gt1;
    clock_gettime(CLOCK_MONOTONIC, &_gt0);
    ++gc_stats_minors;
    gc_minor_active = true;
    if (gc_stats_remembered_high < gc_remembered.size())
        gc_stats_remembered_high = gc_remembered.size();
    auto &wl = gc_worklist;
    wl.clear();

    //------------------ arena tables: reset stale marks (never cleared by sweep)
    for (FuncArena *ar = active_arenas; ar; ar = ar->next_active)
        for (LTable *at = ar->tables; at; at = static_cast<LTable *>(at->next))
            at->marked = 0;

    auto push_if_needed = [&](const LValue &v) {
        if (!v.is_gc_obj())
            return;
        LHeader *h = v.as_pointer();
        if (!h || h->type != static_cast<uint8_t>(v.type) || h->age == AGE_OLD || h->marked != 0)
            return;
        if (v.type == ValueType::UserData && !is_allocated_userdata(h))
            return;
        h->marked = 1;
        gc_push_for_trace(wl, h, v);
    };

    //------------------ roots: root-frame chain, state threads, permanent roots
    if (_G)
        push_if_needed(LValue(Table, _G));
    if (main_thread)
        push_if_needed(LValue(Thread, main_thread));
    if (running_thread && running_thread != main_thread)
        push_if_needed(LValue(Thread, running_thread));
    gc_walk_roots(top_root, push_if_needed);
    for (const LValue &r : permanent_roots)
        push_if_needed(r);

    //------------------ roots: gc_recent. Objects here were allocated since the previous collection and
    //------------------ are now ordinary objects: normal reachability decides them. Rooting every one of
    //------------------ them unconditionally promoted each dead object to AGE_SURVIVOR, so it could only be
    //------------------ freed one cycle later — that lag is what kept dead tables off free_tables and forced
    //------------------ fresh array growth. Everything reachable from a live object is still marked through
    //------------------ that object's own trace, so nothing in flight is lost here.

    gc_recent.clear();

    //------------------ roots: remembered old-generation owners and upvalue cells.

    for (LHeader *owner : gc_remembered) {
        if (!(owner->flags & LFLAG_REMEMBERED))
            continue;
        if (owner->flags & LFLAG_VM_PROXY)
            continue;
        if (owner->age != AGE_OLD) {
            if (owner->marked == 0) {
                owner->marked = 1;
                if (owner->type == static_cast<uint8_t>(Table) || owner->type == static_cast<uint8_t>(Thread)
                    || owner->type == static_cast<uint8_t>(Function) || owner->type == static_cast<uint8_t>(UserData))
                    wl.push(owner);
            }
        } else if (owner->type == static_cast<uint8_t>(Table)) {

            if (!gc_trace_table_young(this, static_cast<LTable *>(owner), wl))
                owner->flags &= ~LFLAG_REMEMBERED;
        } else if (owner->type == static_cast<uint8_t>(UserData)) {
            LUserdata *ud = static_cast<LUserdata *>(owner);
            if (ud->metatable && ud->metatable->age != AGE_OLD) {
                if (ud->metatable->marked == 0) {
                    ud->metatable->marked = 1;
                    wl.push(ud->metatable);
                }
            } else {
                owner->flags &= ~LFLAG_REMEMBERED;
            }
        } else if (owner->type == static_cast<uint8_t>(Thread)) {

            if (!gc_trace_thread_young(this, static_cast<LThread *>(owner), wl))
                owner->flags &= ~LFLAG_REMEMBERED;
        } else if (owner->type == static_cast<uint8_t>(Function)) {

            if (!gc_trace_function_young(this, static_cast<LCFunction *>(owner), wl))
                owner->flags &= ~LFLAG_REMEMBERED;
        } else {
            owner->flags &= ~LFLAG_REMEMBERED;
        }
    }
    for (const LUpValue &cell : gc_remembered_cells)
        if (cell)
            push_if_needed(*cell);

    //------------------ trace: descend only into YOUNG children

    bool meta_protected = false;
    while (!wl.empty() || !meta_protected) {
        if (wl.empty()) {
            //------------------ protect metatables of dead finalizable tables. gc_dispose_swept() will push
            //------------------ those tables onto gc_finalizable, but their metatable (and the __gc closure
            //------------------ it holds) is only reachable through the dead object, so the sweep below would
            //------------------ null the closure's func and the drain would throw std::bad_function_call.
            //------------------ Mirrors the protect_wl pass in collect_garbage_full().
            meta_protected = true;
            for (LTable *obj = metatabled_tables; obj; obj = obj->ext->meta_next) {
                if (obj->marked != 0 || (obj->flags & LFLAG_VM_PROXY))
                    continue;
                LTable *mt = obj->ext ? obj->ext->metatable : nullptr;
                if (!mt || mt->marked != 0 || mt->age == AGE_OLD)
                    continue;
                mt->marked = 1;
                wl.push(mt);
            }
            //------------------ same for dead finalizable userdata (files): their metatable (+ __gc
            //------------------ closure) is otherwise swept in-cycle and the drain throws
            //------------------ std::bad_function_call on the nulled closure.
            for (LUserdata *ud = metatabled_ud; ud; ud = ud->meta_next) {
                if (ud->marked != 0)
                    continue;
                LTable *mt = ud->metatable;
                if (!mt || mt->marked != 0 || mt->age == AGE_OLD)
                    continue;
                mt->marked = 1;
                wl.push(mt);
            }
            continue;
        }
        LHeader *curr = wl.back();
        wl.pop_back();
        if (curr->flags & LFLAG_VM_PROXY)
            continue;
        if (curr->type == static_cast<uint8_t>(Table)) {
            gc_trace_table_young(this, static_cast<LTable *>(curr), wl);
        } else if (curr->type == static_cast<uint8_t>(Thread)) {
            gc_trace_thread_young(this, static_cast<LThread *>(curr), wl);
        } else if (curr->type == static_cast<uint8_t>(Function)) {
            gc_trace_function_young(this, static_cast<LCFunction *>(curr), wl);
        }

        else if (curr->type == static_cast<uint8_t>(UserData)) {
            LUserdata *ud = static_cast<LUserdata *>(curr);
            if (ud->metatable && ud->metatable->age != AGE_OLD && ud->metatable->marked == 0) {
                ud->metatable->marked = 1;
                wl.push(ud->metatable);
            }
        }
    }

    //------------------ single walk over the allocation chain: age/promote, then free.

    auto age_and_promote = [&](LHeader *h) {
        uint8_t from = h->age;
        uint8_t to = (from == AGE_YOUNG) ? AGE_SURVIVOR : AGE_OLD;
        //------------------ Young->survivor promotion never moves old-bytes (both non-old); only
        //------------------ pay for the size computation on the survivor->old transition.
        if (to == AGE_OLD)
            gc_update_old_bytes(h, from, to);
        h->age = to;

        if (to == AGE_OLD && h->marked == 1 && !(h->flags & LFLAG_VM_PROXY)) {
            if (h->type == static_cast<uint8_t>(Table)) {
                gc_trace_remember_nonold_children(static_cast<LTable *>(h));
            } else if (h->type == static_cast<uint8_t>(UserData)) {
                LUserdata *ud = static_cast<LUserdata *>(h);
                if (ud->metatable && ud->metatable->age != AGE_OLD)
                    gc_remember(ud);
            } else if (h->type == static_cast<uint8_t>(Thread)) {
                LThread *th = static_cast<LThread *>(h);
                auto needs_root = [&](const LValue &v) {
                    if (!v.is_gc_obj())
                        return false;
                    LHeader *ch = v.as_pointer();
                    return ch && ch->type == static_cast<uint8_t>(v.type) && ch->age != AGE_OLD;
                };
                bool young_ref = needs_root(th->function);
                if (th->caller && needs_root(LValue(Thread, th->caller)))
                    young_ref = true;
                for (size_t i = 0; !young_ref && i < th->yield_args.count; ++i)
                    if (needs_root(th->yield_args[i]))
                        young_ref = true;
                for (size_t i = 0; !young_ref && i < th->resume_args.count; ++i)
                    if (needs_root(th->resume_args[i]))
                        young_ref = true;
                gc_walk_roots(th->top_root, [&](const LValue &v) {
                    if (!young_ref && needs_root(v))
                        young_ref = true;
                });
                if (young_ref)
                    gc_remember(th);
            } else if (h->type == static_cast<uint8_t>(Function)) {
                LCFunction *fn = static_cast<LCFunction *>(h);
                if (fn->env && fn->env->age != AGE_OLD)
                    gc_remember(fn);
                for (const LUpValue &c : fn->gc_cells) {
                    if (c && c->is_gc_obj()) {
                        LHeader *ch = static_cast<LHeader *>(c->as_pointer());
                        if (ch && ch->type == static_cast<uint8_t>(c->type) && ch->age != AGE_OLD) {
                            gc_remember(fn);
                            break;
                        }
                    }
                }
            }
        }
    };

    //------------------ single walk over the allocation chain: age/promote,

    size_t before_sweep = allocated_bytes;
    LHeader **link = &allocated_objects;
    while (*link) {
        LHeader *o = *link;
        if (o->age == AGE_OLD) {
            //------------------ Should not happen (promotions move to old_objects below, majors
            //------------------ re-partition); kept as a safety net, not a fast path.
            *link = o->next;
            o->next = old_objects;
            old_objects = o;
            continue;
        }
        if (o->marked == 1) {
            age_and_promote(o);
            o->marked = 0;
            if (o->age == AGE_OLD) {
                //------------------ Segregate: minors never walk old_objects, killing the per-minor
                //------------------ re-walk of the long-lived set (2.9M wasted visits in binarytrees).
                *link = o->next;
                o->next = old_objects;
                old_objects = o;
            } else {
                link = &o->next;
            }
        } else if (o->age == AGE_SURVIVOR) {
            LHeader *next_obj = o->next;
            gc_dispose_swept(this, o);
            *link = next_obj;
            object_count--;
        } else if (!(o->flags & (LFLAG_GC_PIN | LFLAG_REMEMBERED))) {
            //------------------ dead young: unreachable at this collection. gc_recent was cleared above and no Lua
            //------------------ runs between that clear and this walk, so an unmarked young object here is garbage,
            //------------------ not something in flight. Returning it to free_tables now is what lets the next
            //------------------ table reuse its array buffer instead of re-growing 8->16->32->64. Pinned objects
            //------------------ are spared: one freshly taken off the free list may not be rooted yet.
            LHeader *next_obj = o->next;
            gc_dispose_swept(this, o);
            *link = next_obj;
            object_count--;
        } else {
            link = &o->next;
        }
    }
    gc_stats_minor_freed += (before_sweep > allocated_bytes) ? (before_sweep - allocated_bytes) : 0;

    //------------------ drop stale remembered entries and pins (mark bits were

    {
        size_t w = 0;
        for (size_t i = 0; i < gc_remembered.size(); ++i) {
            LHeader *h = gc_remembered[i];
            if (h->flags & LFLAG_REMEMBERED) {
                gc_remembered[w++] = h;
            } else {
                h->flags &= ~LFLAG_REMEMBERED;
            }
        }
        gc_remembered.resize(w);
        w = 0;
        for (size_t i = 0; i < gc_remembered_cells.size(); ++i) {
            const LUpValue &cell = gc_remembered_cells[i];
            if (cell && cell->is_gc_obj()) {
                LHeader *ch = static_cast<LHeader *>(cell->as_pointer());
                if (ch && ch->type == static_cast<uint8_t>(cell->type) && ch->age != AGE_OLD
                    && (cell->type != ValueType::UserData || is_allocated_userdata(ch))) {
                    gc_remembered_cells[w++] = cell;
                    continue;
                }
            }
        }
        gc_remembered_cells.resize(w);
        gc_remembered_cell_set.clear();
        for (const LUpValue &cell : gc_remembered_cells)
            gc_remembered_cell_set.insert(cell.get());
        for (size_t _pi = 0; _pi < gc_pinned.top; ++_pi)
            gc_pinned.data[_pi]->flags &= ~LFLAG_GC_PIN;
        gc_pinned.clear();
    }

    //------------------ rebuild pacing state
    gc_recent.clear();

    //------------------ drain finalizables. gc_dispose_swept() routes every swept metatabled table onto
    //------------------ gc_finalizable, but only the incremental major sweep used to drain that list, so
    //------------------ minors disposed objects whose __gc could then never run (finalizers stalled at 1).
    //------------------ Runs before gc_minor_active drops so a finalizer that allocates cannot nest a minor
    //------------------ inside this drain.

    if (gc_finalizable || gc_finalizable_ud)
        gc_drain_finalizables(this);


    const size_t prev_after = gc_bytes_at_minor;
    gc_bytes_at_minor = allocated_bytes;
    gc_minor_active = false;

    //------------------ adaptive minor pacing: next trigger ≈ one current heap-full, clamped.
    //------------------ Hysteresis on the way down (at most halves per minor): without it the threshold
    //------------------ collapses to the post-minor live set after every good minor, then oscillates — churn
    //------------------ workloads like binarytrees re-walk their whole live chain in mark+sweep on each of
    //------------------ those too-frequent minors. Growth stays immediate via allocated_bytes.

    if (!gc_minor_threshold_env && gc_policy == 1) {
        //------------------ the window this minor closed: bytes allocated since the previous minor ended (before_sweep - prev_after); what survived it: the heap's growth over the sweep.
        const size_t window = before_sweep > prev_after ? before_sweep - prev_after : 1;
        const size_t survived = allocated_bytes > prev_after ? allocated_bytes - prev_after : 0;
        size_t next = gc_minor_threshold;
        if (survived * 100 > window * size_t(gc_policy_grow_pct))
            next = gc_minor_threshold * 2;
        else if (survived * 100 < window * size_t(gc_policy_shrink_pct))
            next = gc_minor_threshold / 2;
        if (next < gc_policy_min) next = gc_policy_min;
        if (next > gc_policy_max) next = gc_policy_max;
        gc_minor_threshold = next;
    } else if (!gc_minor_threshold_env) {
        size_t next = allocated_bytes;
        size_t floor = gc_minor_threshold / 2;
        if (next < floor)
            next = floor;
        if (next < kMinorThresholdMin)
            next = kMinorThresholdMin;
        if (next > kMinorThresholdMax)
            next = kMinorThresholdMax;
        gc_minor_threshold = next;
    }

    //------------------ major scheduling: if the old set outgrew its budget, the next

    gc_major_pending = (gc_old_bytes >= gc_major_threshold);

    wl.clear();
    clock_gettime(CLOCK_MONOTONIC, &_gt1);
    gc_stats_minor_us += static_cast<uint64_t>(((int64_t)_gt1.tv_sec - (int64_t)_gt0.tv_sec) * 1000000
        + ((int64_t)_gt1.tv_nsec - (int64_t)_gt0.tv_nsec) / 1000);
}

//------------------ now_us — monotonic microseconds, for the pause statistics
static uint64_t gc_now_us() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000u + static_cast<uint64_t>(ts.tv_nsec) / 1000u;
}

//------------------ LState::gc_step — one increment of a major collection: marking while Marking, the sweep while Sweeping
bool LState::gc_step() {
    if (gc_phase == GCPhase::Marking) {
        gc_mark_step(GC_STEP_BUDGET * 8);
        return false;
    }
    if (gc_phase != GCPhase::Sweeping)
        return true;

    size_t budget = GC_STEP_BUDGET;
    LHeader *curr = gc_sweep_cursor;
    LHeader *prev = gc_prev;
    LHeader **head = gc_sweep_old ? &old_objects : &allocated_objects;

    //------------------ take `c` out of the chain being swept; `prev` is its predecessor when it is known (the sweep may start behind objects allocated since)
    auto unlink = [&](LHeader *c, LHeader *nx) {
        if (prev) {
            prev->next = nx;
        } else if (c == *head) {
            *head = nx;
        } else {
            LHeader *h = *head;
            while (h && h->next != c)
                h = h->next;
            if (h) {
                h->next = nx;
                prev = h;
            }
        }
    };

    while (curr && budget--) {
        LHeader *next_obj = curr->next;

        if (curr->marked == 0 && !(curr->flags & (LFLAG_REMEMBERED | LFLAG_GC_PIN))) {
            unlink(curr, next_obj);
            gc_dispose_swept(this, curr);
            curr = next_obj;
        } else {
            if (gc_mode == GCMode::Generational && curr->age != AGE_OLD && !(curr->flags & LFLAG_REMEMBERED)
                && !(curr->flags & LFLAG_GC_PIN)) {
                bool promoting_to_old = curr->age == AGE_SURVIVOR;
                gc_update_old_bytes(curr, curr->age, AGE_OLD);
                curr->age = AGE_OLD;
                if (promoting_to_old && curr->marked == 1) {
                    if (curr->type == static_cast<uint8_t>(Table))
                        gc_trace_remember_nonold_children(static_cast<LTable *>(curr));
                    else if (curr->type == static_cast<uint8_t>(Function)) {
                        LCFunction *fn = static_cast<LCFunction *>(curr);
                        if (fn->env && fn->env->age != AGE_OLD)
                            gc_remember(fn);
                        for (const LUpValue &c : fn->gc_cells) {
                            if (c && c->is_gc_obj()) {
                                LHeader *ch = static_cast<LHeader *>(c->as_pointer());
                                if (ch && ch->type == static_cast<uint8_t>(c->type) && ch->age != AGE_OLD) {
                                    gc_remember(fn);
                                    break;
                                }
                            }
                        }
                    }
                }
            }
            curr->marked = 0;
            object_count++;
            if (!gc_sweep_old && curr->age == AGE_OLD) {
                //------------------ an old object belongs on the old chain, which this sweep has not reached: park it on a side list that is joined to the old chain at the end
                unlink(curr, next_obj);
                curr->next = nullptr;
                if (gc_promoted_tail)
                    gc_promoted_tail->next = curr;
                else
                    gc_promoted_head = curr;
                gc_promoted_tail = curr;
            } else {
                prev = curr;
            }
            curr = next_obj;
        }
    }

    gc_sweep_cursor = curr;
    gc_prev = prev;

    if (!gc_sweep_cursor && !gc_sweep_old) {
        gc_sweep_old = true;
        gc_sweep_cursor = old_objects;
        gc_prev = nullptr;
    }
    if (gc_sweep_cursor)
        return false;

    if (gc_draining)
        return true;
    if (gc_promoted_head) {
        gc_promoted_tail->next = old_objects;
        old_objects = gc_promoted_head;
        gc_promoted_head = gc_promoted_tail = nullptr;
    }
    gc_draining = true;
    gc_barrier_all = false;
    if (gc_bytes_at_sweep > allocated_bytes)
        gc_stats_major_freed += gc_bytes_at_sweep - allocated_bytes;
    gc_last_cycle_us = gc_stats_major_us - gc_cycle_us0;
    gc_ud_set.clear();
    gc_ud_ready = false;
    gc_drain_finalizables(this);
    gc_prev = nullptr;
    gc_sweep_old = false;
    gc_phase = GCPhase::Idle;
    overflow_heap_used = 0;

    if (gc_mode == GCMode::Generational) {

        gc_bytes_at_minor = allocated_bytes;
    }

    size_t live = allocated_bytes;
    //------------------ Pacing: one collection is a synchronous mark+sweep of O(heap), so

    size_t headroom = std::clamp(live / 2, size_t(1 * 1024 * 1024), size_t(256 * 1024 * 1024));
    gc_bytes_threshold = live + headroom;
    //------------------ the old set that survived this collection is live: the next major waits until it has grown by half, otherwise a live old set above the floor would re-trigger a full collection after every minor
    if (gc_mode == GCMode::Generational)
        gc_major_threshold = std::max(gc_major_floor, gc_old_bytes + gc_old_bytes / 2);
    gc_draining = false;
    return true;
}

//------------------ gc_children: calls visit(LValue) for each reference a table, thread, function or userdata holds, and returns the work it took (slots looked at)
template <class F>
static size_t gc_children(LHeader *curr, F &&visit) {
    size_t work = 4;
    if (curr->type == static_cast<uint8_t>(Table)) {
        LTable *t = static_cast<LTable *>(curr);
        {
            const uint8_t *types_raw = reinterpret_cast<const uint8_t *>(t->array_types);
            size_t i = 0;
            work += t->array_size / 8;
#if defined(CLX_HAS_AVX2)
            const __m256i zero256 = _mm256_setzero_si256();
            for (; i + 32 <= t->array_size; i += 32) {
                __m256i types = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(types_raw + i));
                __m256i cmp = _mm256_cmpeq_epi8(types, zero256);
                uint32_t mask = ~_mm256_movemask_epi8(cmp);
                while (mask) {
                    int bit = clx_ctz(mask);
                    visit(LValue(t->array[i + bit], t->array_types[i + bit]));
                    ++work;
                    mask &= mask - 1;
                }
            }
            for (; i + 16 <= t->array_size; i += 16) {
                __m128i types = _mm_loadu_si128(reinterpret_cast<const __m128i *>(types_raw + i));
                __m128i cmp = _mm_cmpeq_epi8(types, _mm_setzero_si128());
                uint32_t mask = static_cast<uint32_t>(~_mm_movemask_epi8(cmp)) & 0xFFFF;
                while (mask) {
                    int bit = clx_ctz(mask);
                    visit(LValue(t->array[i + bit], t->array_types[i + bit]));
                    ++work;
                    mask &= mask - 1;
                }
            }
#elif defined(CLX_HAS_SSE2)
            const __m128i zero = _mm_setzero_si128();
            for (; i + 16 <= t->array_size; i += 16) {
                __m128i types = _mm_loadu_si128(reinterpret_cast<const __m128i *>(types_raw + i));
                __m128i cmp = _mm_cmpeq_epi8(types, zero);
                uint32_t mask = static_cast<uint32_t>(~_mm_movemask_epi8(cmp)) & 0xFFFF;
                while (mask) {
                    int bit = clx_ctz(mask);
                    visit(LValue(t->array[i + bit], t->array_types[i + bit]));
                    ++work;
                    mask &= mask - 1;
                }
            }
#elif defined(CLX_HAS_NEON)
            const uint8x16_t zero = vdupq_n_u8(0);
            for (; i + 16 <= t->array_size; i += 16) {
                uint8x16_t types = vld1q_u8(types_raw + i);
                uint8x16_t cmp = vceqq_u8(types, zero);
                uint8_t lane_vals[16];
                vst1q_u8(lane_vals, vmvnq_u8(cmp));
                for (int k = 0; k < 16; ++k) {
                    if (lane_vals[k]) {
                        visit(LValue(t->array[i + k], t->array_types[i + k]));
                        ++work;
                    }
                }
            }
#endif
            for (; i < t->array_size; ++i) {
                visit(LValue(t->array[i], t->array_types[i]));
                ++work;
            }
        }
        LTableExt *ex = t->ext;
        if (ex) {
            work += ex->hash_size / 4;
            if (ex->hash_bitmap) {
                size_t bm_words = (ex->hash_size + 63) / 64;
                for (size_t word = 0; word < bm_words; ++word) {
                    uint64_t bits = ex->hash_bitmap[word];
                    while (bits) {
                        size_t idx = word * 64 + clx_ctzll(bits);
                        if (idx >= ex->hash_size)
                            break;
                        visit(LValue(ex->entries[idx].key, ex->entries[idx].ktype));
                        visit(LValue(ex->entries[idx].val, ex->entries[idx].vtype));
                        work += 2;
                        bits &= bits - 1;
                    }
                }
            } else {
                for (size_t _i = 0; _i < ex->hash_size; ++_i) {
                    if (ex->entries[_i].ktype == Nil)
                        continue;
                    visit(LValue(ex->entries[_i].key, ex->entries[_i].ktype));
                    visit(LValue(ex->entries[_i].val, ex->entries[_i].vtype));
                    work += 2;
                }
            }
            if (ex->metatable)
                visit(LValue(Table, ex->metatable));
        }
    } else if (curr->type == static_cast<uint8_t>(Thread)) {
        LThread *th = static_cast<LThread *>(curr);
        visit(th->function);
        if (th->caller)
            visit(LValue(Thread, th->caller));
        for (size_t i = 0; i < th->yield_args.count; ++i)
            visit(th->yield_args[i]);
        for (size_t i = 0; i < th->resume_args.count; ++i)
            visit(th->resume_args[i]);
        gc_walk_roots(th->top_root, [&](const LValue &v) {
            visit(v);
            ++work;
        });
    } else if (curr->type == static_cast<uint8_t>(Function)) {
        LCFunction *f = static_cast<LCFunction *>(curr);
        if (f->env)
            visit(LValue(Table, f->env));
        for (const LUpValue &cell : f->gc_cells) {
            if (cell)
                visit(*cell);
            ++work;
        }
    } else if (curr->type == static_cast<uint8_t>(UserData)) {
        LUserdata *ud = static_cast<LUserdata *>(curr);
        if (ud->metatable)
            visit(LValue(Table, ud->metatable));
    }
    return work;
}

//------------------ gc_userdata_known: a UserData value is a heap object only if the heap says so (the Lua C API also makes light userdata out of plain pointers)
bool LState::gc_userdata_known(const LHeader *h) const {
    if (gc_ud_ready)
        return gc_ud_set.count(h) != 0;
    return is_allocated_userdata(h);
}

//------------------ gc_mark_gray: white -> marked and queued, for the roots, for the children of a traced object and for the write barrier
void LState::gc_mark_gray(const LValue &v) {
    if (!v.is_gc_obj())
        return;
    LHeader *h = v.as_pointer();
    if (!h || h->marked != 0 || h->type != static_cast<uint8_t>(v.type))
        return;
    if (v.type == ValueType::UserData && !gc_userdata_known(h))
        return;
    h->marked = 1;
    gc_worklist.push(h);
}

//------------------ gc_drain_recent: everything allocated since the last drain counts as reachable (it is new; nothing can have been traced through it yet)
void LState::gc_drain_recent() {
    for (size_t i = 0; i < gc_recent.top; ++i) {
        LHeader *h = gc_recent.data[i];
        if (h->marked != 0)
            continue;
        h->marked = 1;
        uint8_t ty = h->type;
        if (ty == static_cast<uint8_t>(Table) || ty == static_cast<uint8_t>(Thread) || ty == static_cast<uint8_t>(Function)
            || ty == static_cast<uint8_t>(UserData))
            gc_worklist.push(h);
    }
    gc_recent.clear();
}

void LState::gc_mark_roots() {
    auto mark = [&](const LValue &v) { gc_mark_gray(v); };
    if (_G)
        mark(LValue(Table, _G));
    if (main_thread)
        mark(LValue(Thread, main_thread));
    if (running_thread && running_thread != main_thread)
        mark(LValue(Thread, running_thread));
    gc_walk_roots(top_root, mark);
    for (const LValue &r : permanent_roots)
        mark(r);
    gc_drain_recent();
    if (clx_mark_vm_proxies_ptr)
        clx_mark_vm_proxies_ptr(this, gc_worklist);
}

//------------------ gc_trace_major: marks what one queued object references; returns the work done
static size_t gc_trace_major(LState *L, LHeader *curr) {
    if (curr->flags & LFLAG_VM_PROXY)
        return 1;
    if (curr->type == static_cast<uint8_t>(Thread))
        L->gc_marked_threads.push_back(static_cast<LThread *>(curr));
    return gc_children(curr, [L](const LValue &v) { L->gc_mark_gray(v); });
}

//------------------ gc_start_major: begins a cycle. Nothing is paused; the work happens in gc_mark_step.
void LState::gc_start_major() {
    if (gc_phase != GCPhase::Idle || gc_draining || gc_minor_active)
        return;
    ++gc_stats_cycles;
    gc_major_pending = false;
    gc_worklist.clear();
    gc_marked_threads.clear();
    gc_ud_set.clear();
    gc_ud_ready = false;
    gc_scan_cursor = allocated_objects;
    gc_scan_old = false;
    gc_mark_stage = MarkStage::Scan;
    gc_step_alloc_mark = allocated_bytes;
    //------------------ arena tables: reset stale marks (never cleared by sweep)
    for (FuncArena *ar = active_arenas; ar; ar = ar->next_active)
        for (LTable *at = ar->tables; at; at = static_cast<LTable *>(at->next))
            at->marked = 0;
    gc_cycle_us0 = gc_stats_major_us;
    gc_phase = GCPhase::Marking;
}

//------------------ gc_mark_step: `units` of marking work. Returns true once marking is complete and the sweep has been set up.
bool LState::gc_mark_step(size_t units) {
    if (gc_phase != GCPhase::Marking)
        return true;
    const uint64_t t0 = gc_now_us();
    size_t spent = 0;
    bool done = false;

    if (gc_mark_stage == MarkStage::Scan) {
        if (gc_has_userdata) {
            LHeader *c = gc_scan_cursor;
            for (;;) {
                if (!c) {
                    if (gc_scan_old)
                        break;
                    gc_scan_old = true;
                    c = old_objects;
                    continue;
                }
                if (spent >= units)
                    break;
                if (c->type == static_cast<uint8_t>(UserData))
                    gc_ud_set.insert(c);
                ++spent;
                c = c->next;
            }
            gc_scan_cursor = c;
            if (c)
                goto out;
        }
        gc_ud_ready = true;
        gc_mark_stage = MarkStage::Roots;
    }
    if (gc_mark_stage == MarkStage::Roots) {
        gc_barrier_all = true;
        gc_mark_roots();
        gc_mark_stage = MarkStage::Propagate;
    }
    {
        auto &wl = gc_worklist;
        for (;;) {
            gc_drain_recent();
            while (!wl.empty() && spent < units) {
                LHeader *c = wl.back();
                wl.pop_back();
                spent += gc_trace_major(this, c);
            }
            if (!wl.empty())
                break;
            if (gc_recent.empty()) {
                if (spent >= units)
                    break;
                gc_atomic();
                done = true;
                break;
            }
        }
    }
out:
    const uint64_t dt = gc_now_us() - t0;
    gc_stats_major_us += dt;
    ++gc_stats_steps;
    if (dt > gc_stats_step_max_us)
        gc_stats_step_max_us = dt;
    return done;
}

//------------------ gc_protect_finalizable_metatables: a dead object's finalizer still needs its metatable, and what that references. Marked 2 so the sweep keeps them this cycle.
void LState::gc_protect_finalizable_metatables() {
    std::vector<LHeader *> protect_wl;
    for (LTable *obj = metatabled_tables; obj; obj = obj->ext->meta_next) {
        if (obj->marked == 0 && !(obj->flags & LFLAG_VM_PROXY)) {
            LTable *mt = obj->ext ? obj->ext->metatable : nullptr;
            if (mt && mt->marked == 0) {
                mt->marked = 2;
                protect_wl.push_back(mt);
            }
        }
    }
    for (LUserdata *ud = metatabled_ud; ud; ud = ud->meta_next) {
        if (ud->marked == 0) {
            LTable *mt = ud->metatable;
            if (mt && mt->marked == 0) {
                mt->marked = 2;
                protect_wl.push_back(mt);
            }
        }
    }
    auto visit = [&](const LValue &v) {
        if (LHeader *h = gc_mark_value(this, v, 2))
            protect_wl.push_back(h);
    };
    while (!protect_wl.empty()) {
        LHeader *curr = protect_wl.back();
        protect_wl.pop_back();
        if (curr->type == static_cast<uint8_t>(Table))
            gc_children(curr, visit);
    }
}

//------------------ gc_prune_remembered: a major marks from the roots, so LFLAG_REMEMBERED no longer says anything about reachability.
//------------------ gc_step() never disposes a remembered object and minors skip AGE_OLD entirely, so a dead old table mutated by a write barrier would survive every
//------------------ collection. Drop the entries this mark proved dead *before* sweeping, so the sweep can free them and gc_remembered holds no pointer to a soon-freed object.
void LState::gc_prune_remembered() {
    size_t w = 0;
    for (size_t i = 0; i < gc_remembered.size(); ++i) {
        LHeader *h = gc_remembered[i];
        if (!h)
            continue;
        if (h->marked != 0)
            gc_remembered[w++] = h;
        else
            h->flags &= ~LFLAG_REMEMBERED;
    }
    gc_remembered.resize(w);

    size_t wc = 0;
    for (size_t i = 0; i < gc_remembered_cells.size(); ++i) {
        const LUpValue &cell = gc_remembered_cells[i];
        bool live = false;
        if (cell && cell->is_gc_obj()) {
            LHeader *ch = static_cast<LHeader *>(cell->as_pointer());
            live = ch && ch->type == static_cast<uint8_t>(cell->type) && ch->marked != 0;
        }
        if (live)
            gc_remembered_cells[wc++] = cell;
    }
    gc_remembered_cells.resize(wc);
    gc_remembered_cell_set.clear();
    for (size_t i = 0; i < gc_remembered_cells.size(); ++i)
        gc_remembered_cell_set.insert(gc_remembered_cells[i].get());
}

//------------------ gc_verify_marks (CLX_GC_VERIFY=1): at the end of the marking, everything reachable from the roots must be marked. A violation is a missing write barrier.
void LState::gc_verify_marks() {
    std::unordered_set<const LHeader *> seen;
    std::vector<LHeader *> stack;
    auto visit = [&](const LValue &v) {
        if (!v.is_gc_obj())
            return;
        LHeader *h = v.as_pointer();
        if (!h || h->type != static_cast<uint8_t>(v.type))
            return;
        if (v.type == ValueType::UserData && !gc_userdata_known(h))
            return;
        if (!seen.insert(h).second)
            return;
        if (h->marked == 0) {
            std::fprintf(stderr, "clx: GC verify failed: a %s reachable from the roots is unmarked at the end of marking (missing write barrier)\n",
                v.type == Table ? "table" : v.type == Function ? "function" : v.type == Thread ? "thread" : "userdata");
            std::abort();
        }
        stack.push_back(h);
    };
    if (_G)
        visit(LValue(Table, _G));
    if (main_thread)
        visit(LValue(Thread, main_thread));
    if (running_thread && running_thread != main_thread)
        visit(LValue(Thread, running_thread));
    gc_walk_roots(top_root, visit);
    for (const LValue &r : permanent_roots)
        visit(r);
    while (!stack.empty()) {
        LHeader *h = stack.back();
        stack.pop_back();
        if (h->flags & LFLAG_VM_PROXY)
            continue;
        gc_children(h, visit);
    }
}

//------------------ gc_atomic: the one pause of a cycle. The roots, the threads and the objects allocated during the marking are marked again, with the program stopped.
void LState::gc_atomic() {
    const uint64_t t0 = gc_now_us();
    auto &wl = gc_worklist;
    gc_mark_roots();
    //------------------ a thread's stack is mutated without a barrier: look at every thread that was traced again
    for (size_t i = 0; i < gc_marked_threads.size(); ++i)
        gc_children(gc_marked_threads[i], [this](const LValue &v) { gc_mark_gray(v); });
    for (;;) {
        gc_drain_recent();
        while (!wl.empty()) {
            LHeader *c = wl.back();
            wl.pop_back();
            gc_trace_major(this, c);
        }
        if (gc_recent.empty())
            break;
    }
    if (gc_verify)
        gc_verify_marks();
    gc_protect_finalizable_metatables();
    gc_prune_remembered();
    gc_marked_threads.clear();
    gc_start_sweep();
    const uint64_t dt = gc_now_us() - t0;
    if (dt > gc_stats_atomic_max_us)
        gc_stats_atomic_max_us = dt;
}

//------------------ gc_start_sweep: the sweep walks the young chain, then the old chain; survivors that are old by then are moved onto the old chain as they are passed
void LState::gc_start_sweep() {
    gc_bytes_at_sweep = allocated_bytes;
    gc_phase = GCPhase::Sweeping;
    gc_sweep_old = false;
    gc_sweep_cursor = allocated_objects;
    gc_prev = nullptr;
    gc_promoted_head = gc_promoted_tail = nullptr;
    gc_finalizable = nullptr;
    gc_finalizable_ud = nullptr;
    object_count = 0;
}

//------------------ gc_run_cycle_to_end: finishes the cycle in progress without giving the program a turn
void LState::gc_run_cycle_to_end() {
    while (gc_phase == GCPhase::Marking)
        gc_mark_step(~size_t(0));
    const uint64_t t0 = gc_now_us();
    while (gc_phase == GCPhase::Sweeping)
        gc_step();
    gc_stats_major_us += gc_now_us() - t0;
}

//------------------ gc_abort_cycle: the state is being destroyed; the marks left on objects do not matter any more
void LState::gc_abort_cycle() {
    if (gc_phase != GCPhase::Marking)
        return;
    gc_barrier_all = false;
    gc_phase = GCPhase::Idle;
    gc_worklist.clear();
    gc_ud_set.clear();
    gc_ud_ready = false;
    gc_marked_threads.clear();
}

//------------------ gc_incremental_tick: called from the allocation hook while a cycle runs. The marking is paid for by what the program allocates.
void LState::gc_incremental_tick() {
    if (gc_phase == GCPhase::Sweeping) {
        const uint64_t t0 = gc_now_us();
        gc_step();
        const uint64_t dt = gc_now_us() - t0;
        gc_stats_major_us += dt;
        if (dt > gc_stats_step_max_us)
            gc_stats_step_max_us = dt;
        return;
    }
    if (gc_stress_incr) {
        gc_mark_step(gc_stress_units);
        return;
    }
    if (allocated_bytes < gc_step_alloc_mark)
        gc_step_alloc_mark = allocated_bytes;
    const size_t delta = allocated_bytes - gc_step_alloc_mark;
    if (delta < kMarkStepBytes)
        return;
    gc_step_alloc_mark = allocated_bytes;
    //------------------ one slot of marking per (800 / gc_stepmul) bytes allocated, never less than a slice that keeps the cycle moving
    const size_t units = std::max(kMarkStepMinUnits, delta * static_cast<size_t>(gc_stepmul) / 800);
    gc_mark_step(units);
}

//------------------ LState::gc_trigger_major — starts a major collection: incremental when enabled, else one pause
void LState::gc_trigger_major() {
    if (!gc_incremental_major) {
        collect_garbage();
        return;
    }
    if (gc_minor_active || gc_draining || gc_phase != GCPhase::Idle)
        return;
    ++gc_stats_collects;
    if (gc_mode == GCMode::Generational)
        gc_minor();
    gc_start_major();
}

//------------------ LState::gc_frame — the collection a program does at the end of each frame, instead of letting allocation trigger it. The first call stops the automatic
//------------------ collector. Each call: when no major cycle is running, one young-generation collection (bounded by what one frame allocated) and, when the old generation has grown
//------------------ by half over what the last cycle left (and passed the floor), the start of a cycle; when one is running, up to `budget_ms` of its marking or sweeping. Returns true
//------------------ while a cycle is running. No minor collection runs during a cycle, so a long cycle lets the young generation grow (a bigger budget shortens the cycle).
bool LState::gc_frame(double budget_ms) {
    gc_frame_managed = true;
    gc_running = false;
    if (budget_ms <= 0)
        budget_ms = gc_frame_ms;
    if (gc_phase == GCPhase::Idle && !gc_draining && !gc_minor_active) {
        if (gc_mode == GCMode::Generational) {
            gc_minor();
            if (gc_old_bytes >= gc_major_threshold)
                gc_start_major();
        } else if (allocated_bytes >= gc_bytes_threshold) {
            gc_start_major();
        }
    }
    if (gc_phase != GCPhase::Idle) {
        //------------------ `budget_ms` (the argument, else gc_frame_ms) is the most a frame gives to the cycle. A cycle that needs little (a small heap) is spread over about kFrameCycleFrames frames instead, so
        //------------------ that it does not stretch a frame by the whole budget: the estimate is what the last cycle took, or 0.5 ms per MB of heap before there has been one.
        const double estimate_ms = gc_last_cycle_us ? static_cast<double>(gc_last_cycle_us) / 1000.0
                                                     : static_cast<double>(allocated_bytes) / (1024.0 * 1024.0) * 0.5;
        const double effective_ms = std::clamp(estimate_ms / kFrameCycleFrames, 0.25, budget_ms);
        const uint64_t deadline = gc_now_us() + static_cast<uint64_t>(effective_ms * 1000.0);
        do {
            const uint64_t t0 = gc_now_us();
            if (gc_phase == GCPhase::Marking) {
                gc_mark_step(GC_STEP_BUDGET * 8);
            } else {
                gc_step();
                const uint64_t dt = gc_now_us() - t0;
                gc_stats_major_us += dt;
                if (dt > gc_stats_step_max_us)
                    gc_stats_step_max_us = dt;
            }
        } while (gc_phase != GCPhase::Idle && gc_now_us() < deadline);
    }
    return gc_phase != GCPhase::Idle;
}

//------------------ LState::collect_garbage — full mark-sweep collection (entry point)

void LState::collect_garbage() {
    //------------------ re-entrancy guard: gc_minor() drains finalizables before dropping gc_minor_active,
    //------------------ so a __gc that calls collectgarbage() must not start a full collection mid-drain.
    if (gc_minor_active)
        return;
    ++gc_stats_collects;
    //------------------ a cycle already in progress is finished first: what the program allocated meanwhile is not collected by it
    if (gc_phase != GCPhase::Idle)
        gc_run_cycle_to_end();
    if (gc_mode == GCMode::Generational && !gc_draining && gc_phase == GCPhase::Idle)
        gc_minor();
    collect_garbage_full();
    if (gc_mode == GCMode::Generational)
        gc_major_pending = false;
}

//------------------ LState::collect_garbage_full — mark-sweep collection in one go: the incremental cycle, run without interruption
void LState::collect_garbage_full() {
    if (gc_draining || gc_phase != GCPhase::Idle)
        return;
    gc_start_major();
    gc_run_cycle_to_end();
}

}
