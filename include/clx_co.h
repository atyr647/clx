// ┌─────────────────────────────────────────────┐
// │  clx — stackless coroutines (C++20)         │
// ├─────────────────────────────────────────────┤
// │  clx_co.h · task type and awaiters          │
// └─────────────────────────────────────────────┘
//
// A Lua function that can yield is compiled twice: the ordinary form, and a coroutine form (a C++20 coroutine returning CoTask<MultiValue>)
// that lives in LCFunction::co_invoke. Under a Lua coroutine the coroutine form runs: a call that can yield is `co_await co_call(...)`, a
// yield is `co_await co_yield_(...)`, and pcall is `co_await co_pcall(...)`. Each suspension returns to the resumer (co_run, costack.cpp)
// with the innermost frame saved in LThread::co_leaf; the next resume continues from there. No stack is switched, so this needs nothing from
// the WebAssembly target. A call to a function without a coroutine form runs it to completion; if it tries to yield, Lua's own
// "attempt to yield across a C-call boundary" is raised.
#ifndef CLX_CO_H
#define CLX_CO_H

#include "clx_runtime.h"
#include <coroutine>
#include <exception>
#include <memory>
#include <utility>

// the coroutine frame of a boxed lambda holds the lambda's shared_ptr: a type with internal linkage when the lambda is local to a translation unit
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic ignored "-Wsubobject-linkage"
#endif

namespace clx {

template<class T> class CoTask;

//---------- coroutine frames come from per-size free lists: a call that can yield allocates and frees a frame each time, and the general
//---------- allocator is the dominant cost of a short call. Frames of up to kCoPoolMax bytes are kept in bins of 64; larger ones use operator new.
//---------- The runtime runs one LState on one thread, and a frame is freed on the thread that ran it.
namespace co_detail {
static constexpr size_t kCoPoolMax = 1024;
struct CoFreeNode { CoFreeNode *next; };
inline CoFreeNode *&co_bin(size_t n) {
    static thread_local CoFreeNode *bins[kCoPoolMax / 64 + 1] = {};
    return bins[(n + 63) / 64];
}
inline void *co_alloc(size_t n) {
    if (n <= kCoPoolMax) {
        CoFreeNode *&head = co_bin(n);
        if (head) {
            void *p = head;
            head = head->next;
            return p;
        }
        return ::operator new(((n + 63) / 64) * 64);
    }
    return ::operator new(n);
}
inline void co_free(void *p, size_t n) noexcept {
    if (n <= kCoPoolMax) {
        CoFreeNode *&head = co_bin(n);
        CoFreeNode *node = static_cast<CoFreeNode *>(p);
        node->next = head;
        head = node;
        return;
    }
    ::operator delete(p);
}
}   // namespace co_detail

struct CoPromiseBase {
    static void *operator new(size_t n) { return co_detail::co_alloc(n); }
    static void operator delete(void *p, size_t n) noexcept { co_detail::co_free(p, n); }
    std::coroutine_handle<> cont {};   // the awaiting parent; empty for a root coroutine
    std::exception_ptr ex;
    std::suspend_always initial_suspend() noexcept { return {}; }
    struct Final {
        bool await_ready() noexcept { return false; }
        template<class P> std::coroutine_handle<> await_suspend(std::coroutine_handle<P> h) noexcept {
            std::coroutine_handle<> c = h.promise().cont;
            return c ? c : std::noop_coroutine();   // a root coroutine hands control back to whoever resumed it
        }
        void await_resume() noexcept { }
    };
    Final final_suspend() noexcept { return {}; }
    void unhandled_exception() { ex = std::current_exception(); }
};

template<class T> struct CoPromise : CoPromiseBase {
    T value {};
    CoTask<T> get_return_object();
    void return_value(T v) { value = std::move(v); }
};

template<class T> class CoTask {
public:
    using promise_type = CoPromise<T>;
    using handle_type = std::coroutine_handle<promise_type>;

    CoTask() = default;
    explicit CoTask(handle_type h) : h_(h) { }
    CoTask(CoTask &&o) noexcept : h_(std::exchange(o.h_, {})) { }
    CoTask &operator=(CoTask &&o) noexcept {
        if (this != &o) { reset(); h_ = std::exchange(o.h_, {}); }
        return *this;
    }
    CoTask(const CoTask &) = delete;
    CoTask &operator=(const CoTask &) = delete;
    ~CoTask() { reset(); }

    //---------- the address of the frame, giving up ownership (LThread::co_root owns it from then on)
    void *release() noexcept { return std::exchange(h_, {}).address(); }
    static CoTask adopt(void *address) noexcept { return CoTask(handle_type::from_address(address)); }

    // awaiting a CoTask starts it by symmetric transfer; the parent continues when it ends
    bool await_ready() const noexcept { return false; }
    std::coroutine_handle<> await_suspend(std::coroutine_handle<> parent) noexcept {
        h_.promise().cont = parent;
        return h_;
    }
    T await_resume() {
        if (h_.promise().ex) std::rethrow_exception(std::exchange(h_.promise().ex, nullptr));
        return std::move(h_.promise().value);
    }

private:
    void reset() noexcept { if (h_) { h_.destroy(); h_ = {}; } }
    handle_type h_ {};
};

template<class T> CoTask<T> CoPromise<T>::get_return_object() {
    return CoTask<T>(std::coroutine_handle<CoPromise<T>>::from_promise(*this));
}

using CoResult = CoTask<MultiValue>;

//---------- set_co_form: gives a function value its coroutine form. `make` is any callable (LState*, const LValue*, size_t) -> CoResult; it lives in a box
//---------- owned by the function value. A coroutine started from it refers to the callable, so the function value must stay reachable while it runs:
//---------- the awaiter of a call roots the callee (CoCallN), and a thread roots its body function.
template<class F> void set_co_form(const LValue &fn, F make) {
    LCFunction *f = static_cast<LCFunction *>(fn.as_pointer());
    f->co_box = std::make_shared<F>(std::move(make));
    f->co_invoke = [](void *box, LState *L, const LValue *args, size_t count) -> void * {
        return (*static_cast<F *>(box))(L, args, count).release();
    };
}

//---------- with_co_form: set_co_form on a freshly created closure, returning it (the form an anonymous function expression is emitted as)
template<class F> LValue with_co_form(LValue fn, F make) {
    set_co_form(fn, std::move(make));
    return fn;
}

//---------- co_call: call a function value from a coroutine form. A function with a coroutine form runs as a child coroutine (and may
//---------- yield); any other callable runs to completion here.
//---------- nesting limit of coroutine frames in one coroutine: Lua's own limit on nested Lua calls is of this order (LUAI_MAXSTACK is 1,000,000 slots)
static constexpr uint32_t kCoMaxDepth = 200000;

struct CoCall {
    LState *L;
    LValue fn;
    const LValue *args;
    size_t count;
    const CallSite *cs;
    MultiValue ready;
    void *child = nullptr;

    bool await_ready() {
        if (fn.type == Function) {
            LCFunction *f = static_cast<LCFunction *>(fn.as_pointer());
            if (f->co_invoke) {
                LThread *t = L->running_thread;
                if (t->co_depth >= kCoMaxDepth)
                    error(L, "stack overflow");
                ++t->co_depth;
                child = f->co_invoke(f->co_box.get(), L, args, count);
                return false;
            }
        }
        ready = call_function_rooted(L, fn, args, count, cs);
        return true;
    }
    std::coroutine_handle<> await_suspend(std::coroutine_handle<> parent) {
        auto c = std::coroutine_handle<CoPromise<MultiValue>>::from_address(child);
        c.promise().cont = parent;
        return c;
    }
    MultiValue await_resume() {
        if (!child) return std::move(ready);
        --L->running_thread->co_depth;
        CoResult task = CoResult::adopt(child);   // destroys the child frame on scope exit
        child = nullptr;
        return task.await_resume();
    }
};
inline CoCall co_call(LState *L, const LValue &fn, const LValue *args, size_t count, const CallSite *cs = default_call_site()) {
    return CoCall { L, fn, args, count, cs, {}, nullptr };
}

//---------- CoSite: a call-site descriptor with static storage that an expression can name (a statement cannot be declared in the middle of one)
template<int Line, bool Method, bool Short, const char *File> struct CoSite {
    static constexpr CallSite value { File, Line, Method, Short };
};

inline LValue co_first(MultiValue &&m) { return m.count > 0 ? m[0] : LValue(); }

//---------- CoCallN<N>: an awaited call of a callee with N arguments, written as one expression: `co_await CoCallN<2>(L, fn, cs).a(x).a(y)`.
//---------- The callee and the arguments live in `slot` as soon as they are evaluated, and `root` keeps them reachable for the collector from
//---------- the first one on (an argument can allocate before the next is evaluated) until the call has returned. A callee with a coroutine form
//---------- runs as a child coroutine, anything else runs to completion in await_ready.
template<size_t N> struct CoCallN {
    LState *L;
    const CallSite *cs;
    LValue slot[N + 1];   // slot[0]: the callee, slot[1..N]: the arguments
    RootRange root;
    size_t n = 0;
    MultiValue ready;
    void *child = nullptr;

    CoCallN(LState *L_, const LValue &fn, const CallSite *cs_)
        : L(L_)
        , cs(cs_)
        , root(L_, slot, N + 1) {
        slot[0] = fn;
    }
    CoCallN(const CoCallN &) = delete;
    CoCallN &a(const LValue &v) {
        slot[1 + n++] = v;
        return *this;
    }
    // a method call: the receiver is evaluated once, it is the first argument, and the callee is looked up in it
    CoCallN &method(const LValue &recv, const LValue &key) {
        slot[1] = recv;
        n = 1;
        slot[0] = table_get(L, recv, key);
        return *this;
    }

    bool await_ready() {
        if (slot[0].type == Function) {
            LCFunction *f = static_cast<LCFunction *>(slot[0].as_pointer());
            if (f->co_invoke) {
                LThread *t = L->running_thread;
                if (t->co_depth >= kCoMaxDepth)
                    error(L, "stack overflow");
                ++t->co_depth;
                child = f->co_invoke(f->co_box.get(), L, &slot[1], n);
                return false;
            }
        }
        ready = call_function_rooted(L, slot[0], &slot[1], n, cs);
        return true;
    }
    std::coroutine_handle<> await_suspend(std::coroutine_handle<> parent) {
        auto c = std::coroutine_handle<CoPromise<MultiValue>>::from_address(child);
        c.promise().cont = parent;
        return c;
    }
    MultiValue await_resume() {
        if (!child)
            return std::move(ready);
        --L->running_thread->co_depth;
        CoResult task = CoResult::adopt(child);   // destroys the child frame on scope exit
        child = nullptr;
        return task.await_resume();
    }
};

//---------- co_yield_: coroutine.yield from a coroutine form
struct CoYield {
    LState *L;
    const LValue *args;
    size_t count;
    bool await_ready() const noexcept { return false; }
    void await_suspend(std::coroutine_handle<> h);   // costack.cpp
    MultiValue await_resume();                        // costack.cpp
};
CoYield co_yield_(LState *L, const LValue *args, size_t count);

//---------- CoYieldN<N>: `co_await CoYieldN<2>(L).a(x).a(y)` is coroutine.yield(x, y); the awaited value is what the next resume passes
template<size_t N> struct CoYieldN {
    LState *L;
    LValue slot[N ? N : 1];
    RootRange root;
    size_t n = 0;
    CoYield inner { nullptr, nullptr, 0 };

    explicit CoYieldN(LState *L_)
        : L(L_)
        , root(L_, slot, N ? N : 1) { }
    CoYieldN(const CoYieldN &) = delete;
    CoYieldN &a(const LValue &v) {
        slot[n++] = v;
        return *this;
    }
    bool await_ready() {
        inner = co_yield_(L, slot, n);   // raises when this thread cannot yield here
        return false;
    }
    void await_suspend(std::coroutine_handle<> h) { inner.await_suspend(h); }
    MultiValue await_resume() { return inner.await_resume(); }
};

//---------- co_pcall: pcall from a coroutine form; a yield inside the called function suspends the whole coroutine and continues after resume
inline CoResult co_pcall(LState *L, LValue fn, std::vector<LValue> args) {
    MultiValue out;
    try {
        MultiValue r = co_await co_call(L, fn, args.data(), args.size());
        std::vector<LValue> v;
        v.reserve(r.count + 1);
        v.push_back(LValue(true));
        for (size_t i = 0; i < r.count; ++i) v.push_back(r[i]);
        out = MultiValue(v.data(), v.size(), L);
    } catch (const LRuntimeException &e) {
        LValue v[2] = { LValue(false), e.error_obj };
        out = MultiValue(v, 2, L);
    }
    co_return out;
}

}  // namespace clx

#endif
