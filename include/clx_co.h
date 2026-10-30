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

namespace clx {

template<class T> class CoTask;

struct CoPromiseBase {
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

//---------- set_co_form: gives a function value its coroutine form. `make` is any callable (LState*, const LValue*, size_t) -> CoResult;
//---------- the callable lives in a shared box, and every running coroutine of it holds the box (the function value may be collected mid-run)
namespace co_detail {
template<class F> CoResult run_boxed(std::shared_ptr<F> keep, LState *L, const LValue *args, size_t count) {
    co_return co_await (*keep)(L, args, count);
}
}
template<class F> void set_co_form(const LValue &fn, F make) {
    LCFunction *f = static_cast<LCFunction *>(fn.as_pointer());
    // the box holds a shared_ptr<F>; each invocation copies it into the coroutine it starts (run_boxed), so the callable and its captures
    // outlive the run even if the function value itself is collected
    f->co_box = std::make_shared<std::shared_ptr<F>>(std::make_shared<F>(std::move(make)));
    f->co_invoke = [](void *box, LState *L, const LValue *args, size_t count) -> void * {
        return co_detail::run_boxed<F>(*static_cast<std::shared_ptr<F> *>(box), L, args, count).release();
    };
}

//---------- co_call: call a function value from a coroutine form. A function with a coroutine form runs as a child coroutine (and may
//---------- yield); any other callable runs to completion here.
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
        CoResult task = CoResult::adopt(child);   // destroys the child frame on scope exit
        child = nullptr;
        return task.await_resume();
    }
};
inline CoCall co_call(LState *L, const LValue &fn, const LValue *args, size_t count, const CallSite *cs = default_call_site()) {
    return CoCall { L, fn, args, count, cs, {}, nullptr };
}

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
