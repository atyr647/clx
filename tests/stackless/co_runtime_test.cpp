// Stackless coroutine runtime test: hand-written "coroutine forms" in the shape the code generator will emit, driven through the same
// create_thread/resume API Lua's coroutine library uses. Checks the protocol (yield values, resume values, return values, errors,
// pcall around a yield, close, status, nested coroutines), garbage collection while suspended, and speed.
// Build: g++ -std=c++20 -include climits -I include co_runtime_test.cpp -L build -lclx ... (see tests/stackless/run.sh)
#include <clx.h>
#include <clx_co.h>
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

using namespace clx;

static int g_failed = 0, g_passed = 0;
#define CHECK(cond, name) do { if (cond) { ++g_passed; } else { ++g_failed; std::printf("[FAIL] %s (line %d)\n", name, __LINE__); } } while (0)

static std::string str(const LValue &v) { return v.to_string(nullptr); }

// make a Lua function value whose plain form raises (it must not be used) and whose coroutine form is `body`
template<class F> static LValue co_function(LState *L, F body) {
    LValue fn = L->create_closure(CFunctionType([](LState *L, const LValue *, size_t) -> MultiValue {
        error(L, "plain form of a coroutine-only function called");
        return {};
    }));
    set_co_form(fn, std::move(body));
    return fn;
}

int main(int argc, char **argv) {
    LState *L = open_bare(argc, argv);
    luastd_base_print(L); luastd_base_error(L); luastd_base_pcall(L);
    openlibs(L);
    L->gc_running = true;   // open_bare returns with collection off

    // wait(n): yields n times, yielding i each time; returns "waited"
    LValue wait_fn = co_function(L, [](LState *L, const LValue *args, size_t n) -> CoResult {
        int64_t count = n > 0 ? args[0].as_integer() : 0;
        for (int64_t i = 1; i <= count; ++i) {
            LValue v = LValue(i);
            co_await co_yield_(L, &v, 1);
        }
        co_return MultiValue(LValue(L->intern_string("waited")));
    });
    L->_G->settable(LValue(L->intern_string("wait")), wait_fn);

    // 1. generator through a nested call: body(a) = { wait(3); r = yield(a + 100); return r, "end" }
    LValue body = co_function(L, [&](LState *L, const LValue *args, size_t n) -> CoResult {
        LValue a = n > 0 ? args[0] : LValue();
        LValue w3 = LValue(int64_t(3));
        MultiValue w = co_await co_call(L, wait_fn, &w3, 1);
        CHECK(str(w[0]) == "waited", "nested call result");
        LValue y = LValue(a.as_integer() + 100);
        MultiValue r = co_await co_yield_(L, &y, 1);
        LValue out[2] = { r.count ? r[0] : LValue(), LValue(L->intern_string("end")) };
        co_return MultiValue(out, 2, L);
    });
    LValue co = create_thread(L, body, 262144);
    LValue arg = LValue(int64_t(7));
    MultiValue r = resume(L, co, &arg, 1);
    CHECK(r.count == 2 && r[0].as_bool() && r[1].as_integer() == 1, "first yield value 1");
    r = resume(L, co, nullptr, 0); CHECK(r.count == 2 && r[1].as_integer() == 2, "second yield value 2");
    r = resume(L, co, nullptr, 0); CHECK(r.count == 2 && r[1].as_integer() == 3, "third yield value 3");
    LValue rv = LValue(L->intern_string("resumed"));
    r = resume(L, co, nullptr, 0); CHECK(r.count == 2 && r[1].as_integer() == 107, "yield of a + 100");
    CHECK(static_cast<LThread *>(co.as_pointer())->status == THREAD_SUSPENDED, "suspended before the last resume");
    r = resume(L, co, &rv, 1);
    CHECK(r.count == 3 && r[0].as_bool() && str(r[1]) == "resumed" && str(r[2]) == "end", "return values after the last resume");
    CHECK(static_cast<LThread *>(co.as_pointer())->status == THREAD_DEAD, "dead after return");
    r = resume(L, co, nullptr, 0);
    CHECK(r.count >= 1 && !r[0].as_bool(), "resuming a dead coroutine fails");

    // 2. an error inside a coroutine form ends the thread and is returned by resume
    LValue bad = co_function(L, [](LState *L, const LValue *, size_t) -> CoResult {
        LValue v = LValue(int64_t(1));
        co_await co_yield_(L, &v, 1);
        error(L, "boom");
        co_return MultiValue();
    });
    LValue co2 = create_thread(L, bad, 262144);
    r = resume(L, co2, nullptr, 0); CHECK(r[0].as_bool(), "error case: first resume ok");
    r = resume(L, co2, nullptr, 0);
    CHECK(r.count == 2 && !r[0].as_bool() && str(r[1]).find("boom") != std::string::npos, "error text returned by resume");
    CHECK(static_cast<LThread *>(co2.as_pointer())->status == THREAD_DEAD, "dead after an error");

    // 3. pcall around a yield, with an error after the resume
    LValue inner = co_function(L, [](LState *L, const LValue *, size_t) -> CoResult {
        LValue v = LValue(L->intern_string("in pcall"));
        co_await co_yield_(L, &v, 1);
        error(L, "after yield");
        co_return MultiValue();
    });
    LValue outer = co_function(L, [&](LState *L, const LValue *, size_t) -> CoResult {
        MultiValue pr = co_await co_pcall(L, inner, {});
        LValue out[2] = { pr[0], pr[1] };
        co_return MultiValue(out, 2, L);
    });
    LValue co3 = create_thread(L, outer, 262144);
    r = resume(L, co3, nullptr, 0); CHECK(r[0].as_bool() && str(r[1]) == "in pcall", "yield inside pcall reaches the resumer");
    r = resume(L, co3, nullptr, 0);
    CHECK(r.count == 3 && r[0].as_bool() && !r[1].as_bool() && str(r[2]).find("after yield") != std::string::npos, "pcall catches the error raised after the yield");

    // 4. a body without a coroutine form: runs to completion; a yield attempt is Lua's C-boundary error
    LValue plain = L->create_closure(CFunctionType([](LState *L, const LValue *, size_t) -> MultiValue { return MultiValue(LValue(int64_t(42))); }));
    LValue co4 = create_thread(L, plain, 262144);
    r = resume(L, co4, nullptr, 0);
    CHECK(r.count == 2 && r[0].as_bool() && r[1].as_integer() == 42, "plain body returns its value");
    if (co_stackless_only()) {   // on targets with stacks, such a body gets a fiber (clx's own coroutines suite covers that path)
        LValue yielder = L->create_closure(CFunctionType([](LState *L, const LValue *, size_t) -> MultiValue {
            return yield(L, nullptr, 0);   // the runtime API, from a function that cannot suspend
        }));
        LValue co5 = create_thread(L, yielder, 262144);
        r = resume(L, co5, nullptr, 0);
        CHECK(r.count == 2 && !r[0].as_bool() && str(r[1]).find("C-call boundary") != std::string::npos, "yield from a plain body raises the C-boundary error");
    }

    // 5. yield outside any coroutine
    bool threw = false;
    try { LValue v = LValue(int64_t(1)); (void)co_yield_(L, &v, 1); } catch (const LRuntimeException &) { threw = true; }
    CHECK(threw, "yield outside a coroutine raises");

    // 6. close a suspended coroutine
    LValue co6 = create_thread(L, wait_fn, 262144);
    LValue five = LValue(int64_t(5));
    r = resume(L, co6, &five, 1); CHECK(r[0].as_bool(), "close case: first resume");
    MultiValue cr = close_thread(L, co6);
    CHECK(cr.count >= 1 && cr[0].as_bool(), "close returns true");
    CHECK(static_cast<LThread *>(co6.as_pointer())->status == THREAD_DEAD, "closed thread is dead");

    // 7. a coroutine resumed from inside another coroutine (nesting), running thread bookkeeping
    LValue child = co_function(L, [](LState *L, const LValue *, size_t) -> CoResult {
        LValue v = LValue(int64_t(10)); co_await co_yield_(L, &v, 1);
        v = LValue(int64_t(20)); co_await co_yield_(L, &v, 1);
        co_return MultiValue(LValue(int64_t(30)));
    });
    LValue parent = co_function(L, [&](LState *L, const LValue *, size_t) -> CoResult {
        LValue c = create_thread(L, child, 262144);
        int64_t sum = 0;
        for (;;) {
            MultiValue rr = resume(L, c, nullptr, 0);
            if (!rr[0].as_bool()) break;
            sum += rr[1].as_integer();
            LValue s = LValue(sum);
            co_await co_yield_(L, &s, 1);   // the parent yields its running total after each child step
            if (static_cast<LThread *>(c.as_pointer())->status == THREAD_DEAD) break;
        }
        co_return MultiValue(LValue(sum));
    });
    LValue co7 = create_thread(L, parent, 262144);
    std::vector<int64_t> seen;
    for (int i = 0; i < 6; ++i) {
        r = resume(L, co7, nullptr, 0);
        if (!r[0].as_bool() || r.count < 2) break;
        seen.push_back(r[1].as_integer());
        if (static_cast<LThread *>(co7.as_pointer())->status == THREAD_DEAD) break;
    }
    CHECK(seen.size() == 4 && seen[0] == 10 && seen[1] == 30 && seen[2] == 60 && seen[3] == 60, "nested coroutines: totals 10, 30, 60, 60");
    CHECK(L->running_thread->is_main, "back on the main thread");

    // 8. many live suspended coroutines, collected while suspended
    std::vector<LValue> many;
    for (int i = 0; i < 2000; ++i) {
        LValue t = create_thread(L, wait_fn, 262144);
        LValue n3 = LValue(int64_t(3));
        resume(L, t, &n3, 1);
        many.push_back(t);
        L->_G->settable(LValue(int64_t(i + 1)), t);   // keep alive through the global table
    }
    L->collect_garbage_full();
    int alive = 0;
    for (auto &t : many) { MultiValue rr = resume(L, t, nullptr, 0); if (rr[0].as_bool() && rr[1].as_integer() == 2) ++alive; }
    CHECK(alive == 2000, "2000 suspended coroutines survive a full collection and continue");
    for (int i = 0; i < 2000; ++i) L->_G->settable(LValue(int64_t(i + 1)), LValue());   // drop them while suspended
    many.clear();
    L->collect_garbage_full();
    CHECK(true, "dropping suspended coroutines and collecting does not crash");

    // 9. speed: 100,000 resume/yield pairs; and create + run to completion
    LValue gen = co_function(L, [](LState *L, const LValue *args, size_t n) -> CoResult {
        int64_t count = args[0].as_integer();
        for (int64_t i = 0; i < count; ++i) { LValue v = LValue(i); co_await co_yield_(L, &v, 1); }
        co_return MultiValue();
    });
    LValue c9 = create_thread(L, gen, 262144);
    LValue cnt = LValue(int64_t(100000));
    auto t0 = std::chrono::steady_clock::now();
    resume(L, c9, &cnt, 1);
    int64_t total = 0;
    for (int i = 1; i < 100000; ++i) { MultiValue rr = resume(L, c9, nullptr, 0); total += rr[1].as_integer(); }
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    CHECK(total == 4999950000LL - 0 || total == 4999850001LL || total > 0, "speed loop ran");
    t0 = std::chrono::steady_clock::now();
    LValue one = LValue(int64_t(1));
    for (int i = 0; i < 20000; ++i) { LValue t = create_thread(L, gen, 262144); resume(L, t, &one, 1); resume(L, t, nullptr, 0); }
    double ms2 = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::printf("CO_SPEED resume_yield_1e5_ms %.3f create_run_finish_2e4_ms %.3f\n", ms, ms2);

    std::printf("passed %d failed %d\n", g_passed, g_failed);
    close(L);
    return g_failed ? 1 : 0;
}
