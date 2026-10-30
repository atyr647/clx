// ┌─────────────────────────────────────────────┐
// │  clx — Lua to C++ Native Compiler           │
// │  Copyright (c) 2026 Tine Samir. MIT License.│
// ├─────────────────────────────────────────────┤
// │  threads.cpp · Coroutine thread management  │
// └─────────────────────────────────────────────┘

#include "clx.h"
#include "clx_runtime.h"
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

//------------------ LThread::LThread — thread constructor
LThread::LThread()
    : state(nullptr)
    , status(THREAD_SUSPENDED)
    , caller(nullptr)
    , is_main(false)
    , has_error(false)
    , close_requested(false) {
    type = static_cast<uint8_t>(Thread);
    marked = 0;
    next = nullptr;
#if defined(_WIN32)
    fiber = nullptr;
#else
    stack_memory = nullptr;
#endif
}

//------------------ LThread::~LThread — thread destructor
LThread::~LThread() {
    if (co_root && state)
        co_release(state, this);   // a stackless thread still suspended: destroy its coroutine frames
#if defined(_WIN32)
    if (fiber && !is_main)
        DeleteFiber(fiber);
#else
    if (stack_memory)
        delete[] stack_memory;
#endif
}

//---------- root_stash: move LState's live root-frame chain into a thread that is about to stop running
static inline void root_stash(LState *L, LThread *t) {
    t->top_root = L->top_root;
    L->top_root = nullptr;
}

//---------- root_load: move a thread's root-frame chain back into LState, leaving the thread's copy empty
static inline void root_load(LState *L, LThread *t) {
    L->top_root = t->top_root;
    t->top_root = nullptr;
}

#if defined(__wasi__) && defined(CLX_WASM_STACK_SWITCHING)
//------------------ WebAssembly stack switching. Wasmtime's stack-switching instructions (cont.new / resume / suspend) cannot be emitted by clang, so the
//------------------ four clx_costack_* functions below are stubs whose bodies tools/wasm-costack-splice.py replaces after linking (scripts/ and
//------------------ decisions/scripting-direction.md section 3b). The stubs are noinline/optnone with a memory clobber so the optimiser infers nothing about them.
//
//------------------ A coroutine runs on a CoWorker: a continuation that never finishes, plus a stack in linear memory (the wasm "shadow" stack, which
//------------------ the host cannot protect). Wasmtime 49 frees no continuation until the store is dropped, so workers are created once and reused:
//------------------ a worker runs one thread's body, is released when it ends, and is picked up by the next thread that starts.
#define CLX_CO_STUB __attribute__((noinline, optnone))
extern "C" {
CLX_CO_STUB __attribute__((export_name("clx_costack_new"))) void clx_costack_new(uint32_t slot, void (*entry)(void)) { __asm__ volatile("" ::"r"(slot), "r"(entry) : "memory"); }
// unspliced, resume returns kCoroUnspliced; the real body returns the worker's stack pointer (a linear-memory address, never this value)
static constexpr uint32_t kCoroUnspliced = 0xDEADBEEFu;
// read through a volatile so that no optimisation level can fold the comparison below into a constant (interprocedural constant propagation did at -O3)
static volatile uint32_t g_coro_unspliced_value = kCoroUnspliced;
CLX_CO_STUB __attribute__((export_name("clx_costack_resume"))) uint32_t clx_costack_resume(uint32_t slot, uint32_t sp) { __asm__ volatile("" ::"r"(slot), "r"(sp) : "memory"); return g_coro_unspliced_value; }
CLX_CO_STUB __attribute__((export_name("clx_costack_suspend"))) void clx_costack_suspend(void) { __asm__ volatile("" ::: "memory"); }
}

struct CoKill { };   // unwinds the body of a thread that is being destroyed unfinished; not a Lua error, so no pcall sees it

}   // namespace clx
struct CoWorker {   // the global one clx_runtime.h declares for CoroutineContext
    uint32_t slot;
    char *mem;           // the stack allocation; mem[0..15] holds the canary
    size_t size;
    uintptr_t limit;     // FrameBase raises "stack overflow" below this address
    uint32_t sp;         // the worker's stack pointer at its last suspension
    clx::LThread *job;
    CoWorker *next_free;
};
namespace clx {

static constexpr uint32_t kCoCanary = 0xC0DE5AFEu;
static constexpr uint32_t kCoMaxWorkers = 65535;   // the splice allocates a 65536-entry continuation table
static CoWorker *g_co_free = nullptr;
static CoWorker *g_co_cur = nullptr;
static uint32_t g_co_workers = 0;

static void fiber_entry_impl(LThread *t);

static void co_worker_entry() {
    CoWorker *self = g_co_cur;
    for (;;)
        fiber_entry_impl(self->job);
}

[[noreturn]] void stack_overflow(LState *L) {
    L->stack_limit = 0;   // the error needs room: unchecked until the thread's next resume sets its limit again
    error(L, "stack overflow");
}

static CoWorker *co_acquire(LThread *t) {
    size_t need = t->stack_bytes ? t->stack_bytes : 262144;
    CoWorker **link = &g_co_free;
    for (CoWorker *w = g_co_free; w; link = &w->next_free, w = w->next_free) {
        if (w->size >= need) { *link = w->next_free; w->next_free = nullptr; w->job = t; t->ctx.worker = w; return w; }
    }
    if (g_co_workers >= kCoMaxWorkers)
        throw_runtime_error("too many live coroutines (65535)");
    CoWorker *w = new CoWorker();
    w->size = (need + 15) & ~size_t(15);
    w->mem = static_cast<char *>(std::malloc(w->size + 16));
    if (!w->mem) { delete w; throw_runtime_error("not enough memory for a coroutine stack"); }
    std::memset(w->mem, 0, 16);
    for (int i = 0; i < 4; ++i) reinterpret_cast<uint32_t *>(w->mem)[i] = kCoCanary;
    size_t margin = w->size >= 65536 ? 16384 : w->size / 4;   // room for the runtime's own frames below the last check
    w->limit = reinterpret_cast<uintptr_t>(w->mem) + 16 + margin;
    w->sp = static_cast<uint32_t>((reinterpret_cast<uintptr_t>(w->mem) + 16 + w->size) & ~uintptr_t(15));
    w->slot = g_co_workers++;
    w->job = t;
    t->ctx.worker = w;
    clx_costack_new(w->slot, co_worker_entry);
    return w;
}

//---------- called by the body's last step, on the worker: hand the worker back before the final switch to the caller
static void wasm_co_finish(LThread *t) {
    CoWorker *w = t->ctx.worker;
    t->ctx.worker = nullptr;
    w->job = nullptr;
    w->next_free = g_co_free;
    g_co_free = w;
}

static void wasm_co_switch(LThread *from, LThread *to) {
    if (!from->is_main && from->caller == to) {   // a yield, or the end of a body: back to the resumer's clx_costack_resume
        clx_costack_suspend();
        return;
    }
    LState *L = to->state;
    CoWorker *w = to->ctx.worker ? to->ctx.worker : co_acquire(to);
    uintptr_t prev_limit = L->stack_limit;
    CoWorker *prev_cur = g_co_cur;
    L->stack_limit = w->limit;
    g_co_cur = w;
    uint32_t sp = clx_costack_resume(w->slot, w->sp);
    g_co_cur = prev_cur;
    L->stack_limit = prev_limit;
    if (sp == kCoroUnspliced) {
        // the stub body is still there: nothing was switched and the coroutine did not run. Undo the acquire and say what is missing.
        to->ctx.worker = nullptr; w->job = nullptr; w->next_free = g_co_free; g_co_free = w;
        throw_runtime_error("coroutines need tools/wasm_costack_splice.py applied to the linked module (stack-switching splice missing)");
    }
    if (!sp)
        std::abort();   // a worker never finishes; reaching here means the continuation table is corrupt
    w->sp = sp;
    if (reinterpret_cast<uint32_t *>(w->mem)[0] != kCoCanary || reinterpret_cast<uint32_t *>(w->mem)[3] != kCoCanary) {
        std::fprintf(stderr, "clx: coroutine stack overflow clobbered the memory below its stack; aborting\n");
        std::abort();
    }
}

//---------- destroy the unfinished body of a thread (garbage collected while suspended): resume it once with kill_requested so that its yield
//---------- throws CoKill, which unwinds the body's frames and returns the worker to the pool
static void wasm_co_kill(LThread *t) {
    LState *L = t->state;
    if (!t->ctx.worker || !L || !L->running_thread)
        return;
    LThread *prev = L->running_thread;
    t->kill_requested = true;
    t->caller = prev;
    prev->status = THREAD_NORMAL;
    t->status = THREAD_RUNNING;
    root_stash(L, prev);
    root_load(L, t);
    L->running_thread = t;
    wasm_co_switch(prev, t);   // returns once the body has unwound and the worker has handed itself back (fiber_entry_impl restores the roots)
    t->kill_requested = false;
}

#endif

//---------- coro_switch_to: the single site for the platform context switch (fiber end, resume
//---------- and yield each had their own copy). Conditions preserved exactly: the two middle
//---------- branches had identical bodies, so they are OR-ed, nothing else changes.
static inline void coro_switch_to(LThread *from, LThread *to) {
#if defined(_WIN32)
    SwitchToFiber(to->fiber);
#elif ((defined(__APPLE__) || defined(__linux__)) && defined(__aarch64__)) || (defined(__linux__) && defined(__x86_64__))
    clx_coro_switch(&from->ctx, &to->ctx);
#elif defined(__wasi__) && defined(CLX_WASM_STACK_SWITCHING)
    wasm_co_switch(from, to);
#elif defined(__wasi__) && defined(__wasi_cooperative_threads__)
    pthread_mutex_lock(&to->ctx.mu); to->ctx.go = true; pthread_cond_signal(&to->ctx.cv); pthread_mutex_unlock(&to->ctx.mu);
    pthread_mutex_lock(&from->ctx.mu);
    while (!from->ctx.go) pthread_cond_wait(&from->ctx.cv, &from->ctx.mu);
    from->ctx.go = false;
    pthread_mutex_unlock(&from->ctx.mu);
#elif defined(__wasi__)
    (void)from; (void)to;
    throw_runtime_error("coroutines are not available on this WebAssembly target (no stack switching)");
#else
    swapcontext(&from->ctx, &to->ctx);
#endif
}

static void fiber_entry_impl(LThread *t) {
    LState *L = t->state;
    for (;;) {
        try {
            clx::LValue args[8];
            size_t argc = t->resume_args.count < 8 ? t->resume_args.count : 8;
            for (size_t i = 0; i < argc; ++i)
                args[i] = t->resume_args[i];
            t->resume_args = MultiValue();

            static constexpr CallSite coroutine_site = { "coroutine", 0, false, false };
            t->yield_args = call_function_rooted(L, t->function, args, argc, &coroutine_site);
            t->status = THREAD_DEAD;
#if defined(CLX_WASM_STACK_SWITCHING)
        } catch (const CoKill &) {
            t->yield_args = MultiValue();
            t->status = THREAD_DEAD;
#endif
        } catch (const LRuntimeException &e) {

            t->yield_args = MultiValue(e.error_obj);
            t->status = THREAD_DEAD;
            t->has_error = true;
        } catch (...) {
            t->yield_args = MultiValue(clx::LValue(L->intern_string("unknown error")));
            t->status = THREAD_DEAD;
            t->has_error = true;
        }

        if (t->pre_unwind) {
            t->pre_unwind = false;
            t->has_error = false;
            continue;
        }
        break;
    }

    LThread *caller = t->caller;
    root_stash(L, t);
    root_load(L, caller);
    L->running_thread = caller;
    caller->status = THREAD_RUNNING;
#if defined(CLX_WASM_STACK_SWITCHING)
    wasm_co_finish(t);
#endif

    coro_switch_to(t, caller);
}

#if defined(_WIN32)

static void WINAPI fiber_trampoline(LPVOID param) {
    LThread *t = static_cast<LThread *>(param);
    t->fiber_started = true;
    for (;;) {
        fiber_entry_impl(t);
    }
}
#elif defined(__wasi__) && defined(__wasi_cooperative_threads__)
//------------------ one green thread per LThread for its whole life: it waits for its first switch, runs the body, and after the body's final
//------------------ switch back to the caller waits again, so a recycled LThread (free_threads) reuses its green thread
static void *fiber_trampoline(void *param) {
    LThread *t = static_cast<LThread *>(param);
    pthread_mutex_lock(&t->ctx.mu);
    while (!t->ctx.go) pthread_cond_wait(&t->ctx.cv, &t->ctx.mu);
    t->ctx.go = false;
    pthread_mutex_unlock(&t->ctx.mu);
    for (;;) fiber_entry_impl(t);
    return nullptr;
}
#else
static thread_local LThread *g_starting_thread = nullptr;

static void fiber_trampoline() {
    fiber_entry_impl(g_starting_thread);
}
#endif

//------------------ create_thread: creates a new coroutine thread (public API)
LValue create_thread(LState *L, const LValue &func, double stack_size) {
    L->gc_maybe_collect();

    LThread *t = L->free_threads;
#if defined(CLX_WASM_STACK_SWITCHING)
    if (t && t->ctx.worker)
        wasm_co_kill(t);   // an unreachable thread collected while suspended: unwind its body so its worker can be reused
    t = L->free_threads;   // the unwinding can run a collection through a destructor; re-read the head
#endif
    bool recycled = (t != nullptr);
    uint8_t prev_age_th = recycled ? t->age : AGE_YOUNG;
#if defined(_WIN32)
    bool pooled_suspended = recycled && t->status == THREAD_SUSPENDED && t->fiber_started;
#endif
    if (t) {
        L->free_threads = static_cast<LThread *>(t->next);
    } else {
        t = new LThread();
    }
    t->state = L;
    t->function = func;
    t->stack_bytes = static_cast<size_t>(stack_size);
    t->resume_args = MultiValue();
    t->yield_args = MultiValue();
    t->has_error = false;
    t->close_requested = false;
    t->caller = nullptr;
    t->status = THREAD_SUSPENDED;
    t->top_root = nullptr;
    t->started = false;
    t->marked = 0;
    t->age = AGE_YOUNG;
    if (t->co_root)
        co_release(L, t);
    co_choose_kind(t, func);   // stackless (the body is a coroutine, or runs to completion) or a stack of its own
    if (t->luaapi && L->luaapi_thread_reset)
        L->luaapi_thread_reset(L, t->luaapi);
    if (recycled && prev_age_th != AGE_YOUNG && (L->gc_phase == LState::GCPhase::Sweeping || L->gc_minor_active)) {

        t->flags |= LFLAG_GC_PIN;
        L->gc_pinned.push(t);
    }

    if (!t->co_kind) {
#if defined(_WIN32)
    if (recycled) {
        if (t->fiber) {
            L->free_fiber_threads--;
            t->pre_unwind = pooled_suspended;
        } else {
            t->fiber = CreateFiber(static_cast<SIZE_T>(stack_size), fiber_trampoline, t);
        }
    } else {
        t->fiber = CreateFiber(static_cast<SIZE_T>(stack_size), fiber_trampoline, t);
    }
#elif (defined(__APPLE__) || defined(__linux__)) && defined(__aarch64__)
    if (!t->stack_memory)
        t->stack_memory = new char[t->stack_bytes];
    clx_coro_init(&t->ctx, t->stack_memory + t->stack_bytes, (void *)fiber_trampoline);
#elif defined(__linux__) && defined(__x86_64__)
    if (!t->stack_memory)
        t->stack_memory = new char[t->stack_bytes];
    clx_coro_init(&t->ctx, t->stack_memory + t->stack_bytes, (void *)fiber_trampoline);
#elif defined(__wasi__) && defined(__wasi_cooperative_threads__)
    if (!t->ctx.has_thread) {
        pthread_attr_t attr; pthread_attr_init(&attr); pthread_attr_setstacksize(&attr, t->stack_bytes);
        if (pthread_create(&t->ctx.th, &attr, fiber_trampoline, t) != 0) throw_runtime_error("cannot create coroutine (green thread)");
        pthread_attr_destroy(&attr);
        pthread_detach(t->ctx.th);
        t->ctx.has_thread = true;
    }
#elif defined(__wasi__) && defined(CLX_WASM_STACK_SWITCHING)
    (void)0;   // the worker is acquired at the first resume
#elif defined(__wasi__)
    throw_runtime_error("coroutines are not available on this WebAssembly target (no stack switching)");
#else
    getcontext(&t->ctx);
    if (!t->stack_memory)
        t->stack_memory = new char[static_cast<size_t>(stack_size)];
    t->ctx.uc_stack.ss_sp = t->stack_memory;
    t->ctx.uc_stack.ss_size = static_cast<size_t>(stack_size);
    t->ctx.uc_link = nullptr;
    makecontext(&t->ctx, fiber_trampoline, 0);
#endif
    }   // !co_kind

    t->next = L->allocated_objects;
    L->allocated_objects = t;
    L->gc_recent.push(t);
    if (!recycled)
        L->object_count++;
    L->allocated_bytes += sizeof(LThread) + (t->co_kind ? 0 : t->stack_bytes);
    return LValue(Thread, t);
}

//------------------ resume: resumes a suspended coroutine (public API)
MultiValue resume(LState *L, const LValue &thread, const LValue *args, size_t count) {
    LThread *t = static_cast<LThread *>(thread.as_pointer());
    if (t->co_kind && t->status == THREAD_DEAD) {   // a finished stackless thread has no frame to continue (coroutine.resume checks this too)
        LValue r[2] = { LValue(false), LValue(L->intern_string("cannot resume dead coroutine")) };
        return MultiValue(r, 2, L);
    }
    //------------------ assign() reuses the thread's already-constructed inline slots: no temp
    //------------------ MultiValue + move-assign copy chain on every resume.
    t->resume_args.assign(args, count, L);
    t->has_error = false;
#if !defined(_WIN32) && !(defined(__wasi__) && defined(__wasi_cooperative_threads__))
    // fiber_trampoline() reads g_starting_thread on the thread's first entry. create_thread() also sets it,
    // but with several coroutines created before the first resume it would point at the last one created.
    if (!t->started) g_starting_thread = t;
#endif
    t->started = true;
    if (L->gc_mode == LState::GCMode::Generational && t->age == AGE_OLD) {
        for (size_t i = 0; i < t->resume_args.count; ++i)
            gc_barrier_header(L, t, t->resume_args[i]);
    }
    t->caller = L->running_thread;
    t->caller->status = THREAD_NORMAL;
    t->status = THREAD_RUNNING;
    root_stash(L, L->running_thread);
    root_load(L, t);
    L->running_thread = t;

    if (t->co_kind)
        co_run(L, t);   // stackless: continues the saved coroutine frame, and hands the root chains and the running thread back itself
    else
        coro_switch_to(t->caller, t);

    //------------------ Build (ok, yield_args...) directly in the return slot: one placement-new per
    //------------------ value, no temp buffer + re-copy (was: fill buf[total], then MultiValue(buf)
    //------------------ copied every slot a second time, plus dead default-ctors for the inline buf).
    size_t total = 1 + t->yield_args.count;
    MultiValue ret;
    ret.count = total;
    ret.alloc_L = L;
    LValue *dst = ret.inline_ptr();
    size_t n_inline = (total < MultiValue::INLINE_CAP) ? total : MultiValue::INLINE_CAP;
    new (dst) LValue(!t->has_error);
    for (size_t i = 1; i < n_inline; ++i)
        new (dst + i) LValue(t->yield_args[i - 1]);
    if (total <= MultiValue::INLINE_CAP) {
        ret.overflow = nullptr;
    } else {
        ret.overflow = L->alloc_overflow(total - MultiValue::INLINE_CAP);
        for (size_t i = MultiValue::INLINE_CAP; i < total; ++i)
            ret.overflow[i - MultiValue::INLINE_CAP] = t->yield_args[i - 1];
    }
    return ret;
}

//------------------ yield: yields from a coroutine (public API)
MultiValue yield(LState *L, const LValue *args, size_t count) {
    LThread *t = L->running_thread;
    if (t->is_main)
        clx::error(L, "attempt to yield from outside a coroutine");
    if (t->co_kind)   // a stackless thread yields only from its coroutine forms (co_yield_), never through a call that cannot suspend
        clx::error(L, "attempt to yield across a C-call boundary");
    t->yield_args.assign(args, count, L);
    if (L->gc_mode == LState::GCMode::Generational && t->age == AGE_OLD) {
        for (size_t i = 0; i < t->yield_args.count; ++i)
            gc_barrier_header(L, t, t->yield_args[i]);
    }
    t->status = THREAD_SUSPENDED;
    // The coroutine may have stored young objects in its frame slots since the last minor GC scanned it.
    // An old suspended thread is only rescanned if remembered, so remember it whenever it suspends.
    if (L->gc_mode == LState::GCMode::Generational && t->age == AGE_OLD)
        L->gc_remember(t);

    LThread *caller = t->caller;
    root_stash(L, t);
    root_load(L, caller);
    L->running_thread = caller;
    caller->status = THREAD_RUNNING;

    coro_switch_to(t, caller);

#if defined(CLX_WASM_STACK_SWITCHING)
    if (t->kill_requested)
        throw CoKill{};
#endif
    if (t->close_requested) {
        t->close_requested = false;
        t->has_error = true;
        throw LRuntimeException(clx::string(L, "thread is being closed"));
    }

    ++t->yield_count;

    return t->resume_args;
}

//------------------ close_thread: closes a suspended coroutine (public API)
MultiValue close_thread(LState *L, const LValue &thread) {
    LThread *t = static_cast<LThread *>(thread.as_pointer());

    if (t->status == THREAD_DEAD)
        return MultiValue(true);

    if (t->status == THREAD_RUNNING)
        clx::error(L, "cannot close running coroutine");
    if (t->status == THREAD_NORMAL)
        clx::error(L, "cannot close normal coroutine");

    t->close_requested = true;

    MultiValue result = resume(L, thread, nullptr, 0);

    if (t->status == THREAD_DEAD)
        return MultiValue(true);

    return MultiValue(false);
}

}
