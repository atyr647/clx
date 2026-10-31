// ┌─────────────────────────────────────────────┐
// │  clx — Lua to C++ Native Compiler           │
// ├─────────────────────────────────────────────┤
// │  costack.cpp · Stackless coroutine driver   │
// └─────────────────────────────────────────────┘
// Runs and releases threads whose body is a C++20 coroutine (include/clx_co.h). Mirrors what threads.cpp does for fibers: the same
// root-chain hand-over (root_stash/root_load), the same status transitions, the same yield_args/resume_args protocol -- only the transfer
// of control differs: resuming calls the saved frame's resume(), and a yield is a coroutine suspension that returns to that call.

#include "clx.h"
#include "clx_co.h"
#include <vector>

// A target with no way to switch stacks (WebAssembly without green threads) runs every body that has no coroutine form to completion;
// other targets fall back to a fiber for such bodies. CLX_STACKLESS_ONLY forces the first behaviour (tests use it on native).
#if defined(__wasi__) && !defined(__wasi_cooperative_threads__) && !defined(CLX_WASM_STACK_SWITCHING) && !defined(CLX_STACKLESS_ONLY)
#define CLX_STACKLESS_ONLY 1
#endif

namespace clx {

static inline void root_stash(LState *L, LThread *t) {
    t->top_root = L->top_root;
    L->top_root = nullptr;
}
static inline void root_load(LState *L, LThread *t) {
    L->top_root = t->top_root;
    t->top_root = nullptr;
}

//---------- the body of a thread whose function has no coroutine form: call it to completion (it cannot yield)
static CoResult sync_body(LState *L, LValue fn, std::vector<LValue> args) {
    co_return call_function_rooted(L, fn, args.data(), args.size());
}

void co_choose_kind(LThread *t, const LValue &func) {
    t->co_root = nullptr;
    t->co_leaf = nullptr;
    t->co_depth = 0;
    LCFunction *f = (func.type == Function) ? static_cast<LCFunction *>(func.as_pointer()) : nullptr;
    if (f && f->co_invoke) {
        t->co_kind = 1;
        return;
    }
#if defined(CLX_STACKLESS_ONLY)
    t->co_kind = 2;   // no stacks to switch on this target: the body runs to completion
#else
    t->co_kind = 0;   // a fiber / green thread, as before
#endif
}

bool co_stackless_only() {
#if defined(CLX_STACKLESS_ONLY)
    return true;
#else
    return false;
#endif
}

void co_release(LState *L, LThread *t) {
    if (!t->co_root)
        return;
    // destroying a suspended chain runs the destructors of its Frames, which unlink from the root chain: that chain is the thread's own
    RootRange *saved = L->top_root;
    L->top_root = t->top_root;
    t->top_root = nullptr;
    std::coroutine_handle<>::from_address(t->co_root).destroy();
    t->co_root = nullptr;
    t->co_leaf = nullptr;
    L->top_root = saved;
}

void co_run(LState *L, LThread *t) {
    LThread *caller = t->caller;
    std::coroutine_handle<> h;
    if (!t->co_root) {
        // first resume: create the root coroutine from the body and run it from its start
        std::vector<LValue> args(t->resume_args.count);
        for (size_t i = 0; i < args.size(); ++i)
            args[i] = t->resume_args[i];
        t->resume_args = MultiValue();
        if (t->co_kind == 1) {
            LCFunction *f = static_cast<LCFunction *>(t->function.as_pointer());
            t->co_root = f->co_invoke(f->co_box.get(), L, args.data(), args.size());
        } else {
            t->co_root = sync_body(L, t->function, std::move(args)).release();
        }
        t->co_leaf = t->co_root;
        h = std::coroutine_handle<>::from_address(t->co_leaf);
        t->co_leaf = nullptr;
        h.resume();   // the first run copies its arguments before it can suspend, so `args` (gone after this block) is not needed later
    } else {
        h = std::coroutine_handle<>::from_address(t->co_leaf);
        t->co_leaf = nullptr;
        h.resume();
    }

    // control is back: the coroutine yielded (co_leaf is set) or its root finished
    root_stash(L, t);
    root_load(L, caller);
    L->running_thread = caller;
    caller->status = THREAD_RUNNING;

    auto root = std::coroutine_handle<CoPromise<MultiValue>>::from_address(t->co_root);
    if (root.done()) {
        CoPromise<MultiValue> &p = root.promise();
        if (p.ex) {
            try {
                std::rethrow_exception(p.ex);
            } catch (const LRuntimeException &e) {
                t->yield_args = MultiValue(e.error_obj);
            } catch (...) {
                t->yield_args = MultiValue(LValue(L->intern_string("unknown error")));
            }
            t->has_error = true;
        } else {
            t->yield_args = p.value;
        }
        t->status = THREAD_DEAD;
        root.destroy();
        t->co_root = nullptr;
        t->co_leaf = nullptr;
    }
}

CoYield co_yield_(LState *L, const LValue *args, size_t count) {
    LThread *t = L->running_thread;
    if (t->is_main)
        error(L, "attempt to yield from outside a coroutine");
    if (t->co_kind != 1)
        error(L, "attempt to yield across a C-call boundary");
    return CoYield { L, args, count };
}

void CoYield::await_suspend(std::coroutine_handle<> h) {
    LThread *t = L->running_thread;
    t->co_leaf = h.address();
    t->yield_args.assign(args, count, L);
    if (L->gc_mode == LState::GCMode::Generational && t->age == AGE_OLD) {
        for (size_t i = 0; i < t->yield_args.count; ++i)
            gc_barrier_header(L, t, t->yield_args[i]);
    }
    t->status = THREAD_SUSPENDED;
    // the frames may hold young objects stored since the last minor collection scanned this thread
    if (L->gc_mode == LState::GCMode::Generational && t->age == AGE_OLD)
        L->gc_remember(t);
}

MultiValue CoYield::await_resume() {
    LThread *t = L->running_thread;
    if (t->close_requested) {
        t->close_requested = false;
        t->has_error = true;
        throw LRuntimeException(string(L, "thread is being closed"));
    }
    ++t->yield_count;
    return t->resume_args;
}

}  // namespace clx
