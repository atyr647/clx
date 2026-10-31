// ┌─────────────────────────────────────────────┐
// │  clx — Lua to C++ Native Compiler           │
// ├─────────────────────────────────────────────┤
// │  stackless.cpp · coroutine-form analysis    │
// └─────────────────────────────────────────────┘
// Decides which functions get a coroutine form (include/clx_co.h) and which calls inside them are awaited. Runs only with `--stackless`.
//
//  * An awaitable position is where the emitter can await a call: a statement, the right-hand side of an assignment or local declaration, a
//    returned value, or an argument of another awaited call. A call elsewhere (a condition, a loop bound, an operand, a table constructor, an
//    index) is called to completion.
//  * A call in an awaitable position is awaited unless it is a call of a standard-library function, which cannot yield. A call of anything else
//    (a named function, a method, a field, a parameter) may yield, because Lua dispatches dynamically, so it is awaited: a callee that has a
//    coroutine form runs as a child coroutine, any other callee runs to completion on the spot (clx_co.h, CoCall).
//  * A function gets a coroutine form when its own body has an awaited call.
//  * A call that is certain to yield (`coroutine.yield`, or a call by name of a function that can yield, a fixpoint over the call graph) in a
//    position that cannot be awaited is reported (CLX_STACKLESS_DIAG=1 prints the reports): it will raise Lua's "attempt to yield across a
//    C-call boundary" if it runs under a coroutine.
#include "codegen.h"
#include "../optimizer/analysis_state.h"
#include <cstdlib>
#include <functional>
#include <unordered_map>

namespace clx {

namespace {

struct Analyzer {
    const ASTContext &ctx;
    AnalysisState &state;
    std::unordered_map<std::string_view, uint32_t> named;   // single-definition function names -> FunctionDef node
    std::set<uint32_t> yielders;                            // FunctionDef nodes that can yield
    std::set<uint32_t> candidates;                          // CallExpression nodes that can yield

    Analyzer(const ASTContext &c, AnalysisState &s)
        : ctx(c)
        , state(s) { }

    static std::string_view ident_name(const ASTNode &n) { return std::string_view(n.as.ident.name, n.as.ident.length); }

    // the children of a node that belong to the same function body (a nested FunctionDef is a leaf)
    template<class Cb> void children(uint32_t idx, Cb &&cb) const {
        if (idx == 0xFFFFFFFF || idx >= ctx.nodes.size())
            return;
        const ASTNode &n = ctx.nodes[idx];
        auto list = [&](uint32_t first, uint32_t count) {
            for (uint32_t i = 0; i < count; ++i)
                cb(ctx.block_statements[first + i]);
        };
        switch (n.type) {
        case NodeType::Block: list(n.as.block.first_statement, n.as.block.count); break;
        case NodeType::LocalDecl: list(n.as.local_decl.first_value, n.as.local_decl.value_count); break;
        case NodeType::GlobalDeclStatement: list(n.as.global_decl.first_value, n.as.global_decl.value_count); break;
        case NodeType::Assignment:
            list(n.as.assign.first_target, n.as.assign.target_count);
            list(n.as.assign.first_value, n.as.assign.value_count);
            break;
        case NodeType::BinaryOp: cb(n.as.bin_op.left); cb(n.as.bin_op.right); break;
        case NodeType::UnaryOp: cb(n.as.unary_op.expr); break;
        case NodeType::CallExpression:
            cb(n.as.call_expr.target);
            list(n.as.call_expr.first_arg, n.as.call_expr.arg_count);
            break;
        case NodeType::IntrinsicCall: list(n.as.intrinsic_call.first_arg, n.as.intrinsic_call.arg_count); break;
        case NodeType::IfStatement: cb(n.as.if_stmt.condition); cb(n.as.if_stmt.then_block); cb(n.as.if_stmt.else_block); break;
        case NodeType::WhileStatement: cb(n.as.while_stmt.condition); cb(n.as.while_stmt.body_block); break;
        case NodeType::RepeatStatement: cb(n.as.repeat_stmt.body_block); cb(n.as.repeat_stmt.condition); break;
        case NodeType::ForStatement:
            cb(n.as.for_stmt.start_expr); cb(n.as.for_stmt.limit_expr); cb(n.as.for_stmt.step_expr); cb(n.as.for_stmt.body_block);
            break;
        case NodeType::GenericForStatement:
            list(n.as.generic_for.first_iter, n.as.generic_for.iter_count);
            cb(n.as.generic_for.body_block);
            break;
        case NodeType::DoStatement: cb(n.as.do_stmt.body_block); break;
        case NodeType::TableConstructor:
            for (uint32_t i = 0; i < n.as.table_cons.count; ++i) {
                cb(ctx.block_statements[n.as.table_cons.first_item + i * 2]);
                cb(ctx.block_statements[n.as.table_cons.first_item + i * 2 + 1]);
            }
            break;
        case NodeType::TableAccess: cb(n.as.table_access.table); cb(n.as.table_access.key); break;
        case NodeType::ReturnStatement: list(n.as.return_stmt.first_value, n.as.return_stmt.value_count); break;
        case NodeType::ParenExpression: cb(n.as.paren_expr.expr); break;
        default: break;
        }
    }

    bool is_yield_call(const ASTNode &call) const {
        if (call.type != NodeType::CallExpression)
            return false;
        uint32_t t = call.as.call_expr.target;
        if (t >= ctx.nodes.size() || ctx.nodes[t].type != NodeType::TableAccess)
            return false;
        uint32_t tb = ctx.nodes[t].as.table_access.table, k = ctx.nodes[t].as.table_access.key;
        if (tb >= ctx.nodes.size() || k >= ctx.nodes.size())
            return false;
        const ASTNode &tbn = ctx.nodes[tb], &kn = ctx.nodes[k];
        return tbn.type == NodeType::Identifier && tbn.as.ident.is_global && ident_name(tbn) == "coroutine" && kn.type == NodeType::String
            && std::string_view(kn.as.string.text, kn.as.string.length) == "yield" && state.reassigned_vars.count(std::string("coroutine")) == 0
            && state.builtin_module_writes.count("coroutine") == 0;
    }

    // the FunctionDef a call names, or 0xFFFFFFFF
    uint32_t named_callee(const ASTNode &call) const {
        if (call.type != NodeType::CallExpression)
            return 0xFFFFFFFF;
        uint32_t t = call.as.call_expr.target;
        if (t >= ctx.nodes.size() || ctx.nodes[t].type != NodeType::Identifier)
            return 0xFFFFFFFF;
        auto it = named.find(ident_name(ctx.nodes[t]));
        return it == named.end() ? 0xFFFFFFFF : it->second;
    }

    void collect_names() {
        std::unordered_map<std::string_view, int> defs;      // assignments to the name anywhere in the chunk
        std::unordered_map<std::string_view, uint32_t> fdef;  // the FunctionDef assigned to it
        for (uint32_t i = 0; i < ctx.nodes.size(); ++i) {
            const ASTNode &n = ctx.nodes[i];
            uint32_t fi = 0, ic = 0, fv = 0, vc = 0;
            if (n.type == NodeType::LocalDecl) {
                fi = n.as.local_decl.first_ident; ic = n.as.local_decl.ident_count; fv = n.as.local_decl.first_value; vc = n.as.local_decl.value_count;
            } else if (n.type == NodeType::GlobalDeclStatement) {
                fi = n.as.global_decl.first_ident; ic = n.as.global_decl.ident_count; fv = n.as.global_decl.first_value; vc = n.as.global_decl.value_count;
            } else if (n.type == NodeType::Assignment) {
                fi = n.as.assign.first_target; ic = n.as.assign.target_count; fv = n.as.assign.first_value; vc = n.as.assign.value_count;
            } else
                continue;
            for (uint32_t k = 0; k < ic; ++k) {
                uint32_t t = ctx.block_statements[fi + k];
                if (t >= ctx.nodes.size() || ctx.nodes[t].type != NodeType::Identifier)
                    continue;
                std::string_view nm = ident_name(ctx.nodes[t]);
                ++defs[nm];
                if (k < vc) {
                    uint32_t v = ctx.block_statements[fv + k];
                    if (v < ctx.nodes.size() && ctx.nodes[v].type == NodeType::FunctionDef)
                        fdef[nm] = v;
                }
            }
        }
        for (auto &kv : fdef)
            if (defs[kv.first] == 1)
                named[kv.first] = kv.second;
    }

    // every FunctionDef with the calls of its own body
    void for_each_function(const std::function<void(uint32_t f)> &cb) const {
        for (uint32_t i = 0; i < ctx.nodes.size(); ++i)
            if (ctx.nodes[i].type == NodeType::FunctionDef)
                cb(i);
    }

    void walk_calls(uint32_t idx, const std::function<void(uint32_t call)> &cb) const {
        if (idx == 0xFFFFFFFF || idx >= ctx.nodes.size())
            return;
        if (ctx.nodes[idx].type == NodeType::CallExpression)
            cb(idx);
        children(idx, [&](uint32_t c) { walk_calls(c, cb); });
    }

    void fixpoint() {
        std::vector<uint32_t> funcs;
        for_each_function([&](uint32_t f) { funcs.push_back(f); });
        bool changed = true;
        while (changed) {
            changed = false;
            for (uint32_t f : funcs) {
                if (yielders.count(f))
                    continue;
                bool y = false;
                walk_calls(ctx.nodes[f].as.func_def.body_block, [&](uint32_t c) {
                    const ASTNode &call = ctx.nodes[c];
                    if (is_yield_call(call)) {
                        y = true;
                        return;
                    }
                    uint32_t g = named_callee(call);
                    if (g != 0xFFFFFFFF && yielders.count(g))
                        y = true;
                });
                if (y) {
                    yielders.insert(f);
                    changed = true;
                }
            }
        }
        // the call nodes that can yield
        for (uint32_t i = 0; i < ctx.nodes.size(); ++i) {
            const ASTNode &n = ctx.nodes[i];
            if (n.type != NodeType::CallExpression)
                continue;
            if (is_yield_call(n)) {
                candidates.insert(i);
                state.co_yield_calls.insert(i);
                continue;
            }
            uint32_t g = named_callee(n);
            if (g != 0xFFFFFFFF && yielders.count(g))
                candidates.insert(i);
        }
    }

    //------------------ may_yield: a function that can yield, or whose body calls something that may: a yield, a call of anything that is not a standard
    //------------------ library function and not a known function (a method, a field, a parameter), or a call of a known function that may yield.
    //------------------ A known function that cannot yield is called to completion and gets no coroutine form for calling it.
    std::set<uint32_t> may_yield;
    void compute_may_yield() {
        std::vector<uint32_t> funcs;
        for_each_function([&](uint32_t f) { funcs.push_back(f); });
        bool changed = true;
        while (changed) {
            changed = false;
            for (uint32_t f : funcs) {
                if (may_yield.count(f))
                    continue;
                bool y = false;
                walk_calls(ctx.nodes[f].as.func_def.body_block, [&](uint32_t c) {
                    if (y)
                        return;
                    const ASTNode &call = ctx.nodes[c];
                    if (is_yield_call(call)) {
                        y = true;
                        return;
                    }
                    if (is_std_call(call) && !is_pcall(call))
                        return;
                    if (is_pcall(call)) {
                        y = true;   // pcall(f): f may yield
                        return;
                    }
                    uint32_t g = named_callee(call);
                    if (g == 0xFFFFFFFF)
                        y = true;   // dynamic dispatch
                    else if (may_yield.count(g))
                        y = true;
                });
                if (y) {
                    may_yield.insert(f);
                    changed = true;
                }
            }
        }
    }

    // does the subtree (same function) contain a call that is certain to yield
    bool has_cand(uint32_t idx) const {
        if (idx == 0xFFFFFFFF || idx >= ctx.nodes.size())
            return false;
        if (candidates.count(idx))
            return true;
        bool found = false;
        children(idx, [&](uint32_t c) {
            if (!found && has_cand(c))
                found = true;
        });
        return found;
    }

    //------------------ calls of the standard library cannot yield: plain calls
    bool is_std_call(const ASTNode &call) const {
        static const std::set<std::string_view> funcs = { "print", "type", "tostring", "tonumber", "pairs", "ipairs", "next", "select", "rawget",
            "rawset", "rawequal", "rawlen", "setmetatable", "getmetatable", "assert", "error", "collectgarbage", "unpack", "require", "load",
            "loadstring", "dofile", "loadfile", "xpcall" };
        static const std::set<std::string_view> mods = { "string", "table", "math", "io", "os", "utf8", "coroutine", "debug", "package" };
        uint32_t t = call.as.call_expr.target;
        if (t >= ctx.nodes.size())
            return false;
        const ASTNode &tn = ctx.nodes[t];
        if (tn.type == NodeType::Identifier)
            return tn.as.ident.is_global && funcs.count(ident_name(tn)) && state.reassigned_vars.count(std::string(ident_name(tn))) == 0;
        if (tn.type == NodeType::TableAccess) {
            uint32_t tb = tn.as.table_access.table;
            if (tb < ctx.nodes.size() && ctx.nodes[tb].type == NodeType::Identifier && ctx.nodes[tb].as.ident.is_global
                && mods.count(ident_name(ctx.nodes[tb])) && state.reassigned_vars.count(std::string(ident_name(ctx.nodes[tb]))) == 0
                && !(ident_name(ctx.nodes[tb]) == "coroutine" && is_yield_call(call)))
                return true;
        }
        return false;
    }

    bool is_pcall(const ASTNode &call) const {
        uint32_t t = call.as.call_expr.target;
        return t < ctx.nodes.size() && ctx.nodes[t].type == NodeType::Identifier && ctx.nodes[t].as.ident.is_global && ident_name(ctx.nodes[t]) == "pcall"
            && state.reassigned_vars.count(std::string("pcall")) == 0;
    }

    struct FuncResult {
        std::set<uint32_t> awaited;
        std::set<uint32_t> pcalls;
        std::vector<std::pair<int, std::string>> reports;   // certain-yield calls in positions that cannot be awaited
    };

    // a subtree evaluated by plain code: nothing in it is awaited; calls that are certain to yield are reported
    void plain_walk(uint32_t e, FuncResult &r, const char *where) const {
        if (e == 0xFFFFFFFF || e >= ctx.nodes.size())
            return;
        if (candidates.count(e))
            r.reports.push_back({ ctx.nodes[e].line, std::string("a call that yields ") + where });
        children(e, [&](uint32_t c) { plain_walk(c, r, where); });
    }

    // an expression in an awaitable position; true when it ends up awaited
    bool top(uint32_t e, FuncResult &r) const {
        if (e >= ctx.nodes.size())
            return false;
        const ASTNode &n = ctx.nodes[e];
        if (n.type != NodeType::CallExpression) {
            plain_walk(e, r, "inside an expression (an operand, a table constructor or an index): put it in its own statement");
            return false;
        }
        // the callee expression is evaluated by plain code
        plain_walk(n.as.call_expr.target, r, "while computing the callee");
        bool arg_awaited = false;
        for (uint32_t i = 0; i < n.as.call_expr.arg_count; ++i) {
            uint32_t a = ctx.block_statements[n.as.call_expr.first_arg + i];
            // the receiver of a method call is the first argument and is evaluated with the callee
            bool is_receiver = ctx.nodes[n.as.call_expr.target].type == NodeType::TableAccess && i == 0
                && ctx.block_statements[n.as.call_expr.first_arg] == ctx.nodes[n.as.call_expr.target].as.table_access.table;
            if (is_receiver)
                continue;
            if (top(a, r))
                arg_awaited = true;
        }
        bool awaited = arg_awaited || candidates.count(e) > 0;
        if (!awaited && !is_std_call(n)) {
            uint32_t g = named_callee(n);
            awaited = g == 0xFFFFFFFF || may_yield.count(g) > 0;   // a known function that cannot yield is simply called
        }
        if (!awaited && is_pcall(n))
            awaited = true;
        if (awaited) {
            r.awaited.insert(e);
            if (is_pcall(n))
                r.pcalls.insert(e);
        }
        return awaited;
    }

    void stmt(uint32_t s, FuncResult &r) const {
        if (s == 0xFFFFFFFF || s >= ctx.nodes.size())
            return;
        const ASTNode &n = ctx.nodes[s];
        switch (n.type) {
        case NodeType::Block:
            for (uint32_t i = 0; i < n.as.block.count; ++i)
                stmt(ctx.block_statements[n.as.block.first_statement + i], r);
            break;
        case NodeType::CallExpression: top(s, r); break;
        case NodeType::LocalDecl:
            for (uint32_t i = 0; i < n.as.local_decl.value_count; ++i)
                top(ctx.block_statements[n.as.local_decl.first_value + i], r);
            break;
        case NodeType::GlobalDeclStatement:
            for (uint32_t i = 0; i < n.as.global_decl.value_count; ++i)
                top(ctx.block_statements[n.as.global_decl.first_value + i], r);
            break;
        case NodeType::Assignment:
            for (uint32_t i = 0; i < n.as.assign.target_count; ++i)
                plain_walk(ctx.block_statements[n.as.assign.first_target + i], r, "inside an assignment target");
            for (uint32_t i = 0; i < n.as.assign.value_count; ++i)
                top(ctx.block_statements[n.as.assign.first_value + i], r);
            break;
        case NodeType::ReturnStatement:
            for (uint32_t i = 0; i < n.as.return_stmt.value_count; ++i)
                top(ctx.block_statements[n.as.return_stmt.first_value + i], r);
            break;
        case NodeType::IfStatement:
            plain_walk(n.as.if_stmt.condition, r, "in an if condition: assign it to a local first");
            stmt(n.as.if_stmt.then_block, r);
            stmt(n.as.if_stmt.else_block, r);
            break;
        case NodeType::WhileStatement:
            plain_walk(n.as.while_stmt.condition, r, "in a while condition: loop with `while true` and test inside");
            stmt(n.as.while_stmt.body_block, r);
            break;
        case NodeType::RepeatStatement:
            plain_walk(n.as.repeat_stmt.condition, r, "in an until condition");
            stmt(n.as.repeat_stmt.body_block, r);
            break;
        case NodeType::ForStatement:
            plain_walk(n.as.for_stmt.start_expr, r, "in a for bound");
            plain_walk(n.as.for_stmt.limit_expr, r, "in a for bound");
            plain_walk(n.as.for_stmt.step_expr, r, "in a for bound");
            stmt(n.as.for_stmt.body_block, r);
            break;
        case NodeType::GenericForStatement:
            for (uint32_t i = 0; i < n.as.generic_for.iter_count; ++i)
                plain_walk(ctx.block_statements[n.as.generic_for.first_iter + i], r, "in the iterator expression of a for loop");
            stmt(n.as.generic_for.body_block, r);
            break;
        case NodeType::DoStatement: stmt(n.as.do_stmt.body_block, r); break;
        default: plain_walk(s, r, "in an unsupported position"); break;
        }
    }

    void run() {
        collect_names();
        fixpoint();
        compute_may_yield();
        // a function that can yield is only ever called through its closure value (where the coroutine form is found): it gets none of
        // the direct-call variants (native, fast, tuple), which would bypass it
        for (auto &kv : named) {
            if (!may_yield.count(kv.second))
                continue;
            state.native_direct_funcs.erase(kv.first);
            state.fast_body_funcs.erase(kv.first);
            state.native_return_funcs.erase(kv.first);
            state.int_returning_funcs.erase(kv.first);
            state.slow_call_funcs.erase(kv.first);
            auto mit = state.multi_native_return_counts.find(std::string(kv.first));
            if (mit != state.multi_native_return_counts.end())
                state.multi_native_return_counts.erase(mit);
        }
        const bool diag = std::getenv("CLX_STACKLESS_DIAG") != nullptr;
        // lowering a function makes calls of it awaitable chains too, but every non-std call is awaited already, so one pass settles it. The same
        // direct-call variants are withheld from every function that gets a form, not only those known to yield
        std::vector<uint32_t> lowered;
        for_each_function([&](uint32_t f) {
            FuncResult r;
            stmt(ctx.nodes[f].as.func_def.body_block, r);
            if (r.awaited.empty())
                return;
            state.co_lowered.insert(f);
            lowered.push_back(f);
            state.co_await_calls.insert(r.awaited.begin(), r.awaited.end());
            state.co_pcall_calls.insert(r.pcalls.begin(), r.pcalls.end());
            for (auto &rep : r.reports) {
                std::string msg = ctx.filename + ":" + std::to_string(rep.first) + ": stackless: " + rep.second;
                state.co_diagnostics.push_back(msg);
                if (diag)
                    std::fprintf(stderr, "%s\n", msg.c_str());
            }
        });
    }
};

}   // namespace

//------------------ analyze_stackless: fills state.co_lowered and state.co_await_calls for the chunk
void analyze_stackless(const ASTContext &ctx, AnalysisState &state) {
    state.co_lowered.clear();
    state.co_pcall_calls.clear();
    state.co_await_calls.clear();
    state.co_yield_calls.clear();
    state.co_diagnostics.clear();
    if (!state.co_enabled)
        return;
    Analyzer a(ctx, state);
    a.run();
}

}   // namespace clx
