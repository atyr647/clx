// ┌─────────────────────────────────────────────┐
// │  clx — Lua to C++ Native Compiler           │
// │  Copyright (c) 2026 Tine Samir. MIT License.│
// ├─────────────────────────────────────────────┤
// │  numbers.cpp · Numeric analysis passes      │
// └─────────────────────────────────────────────┘

#include "optimizer.h"
#include "passes.h"
#include "../../include/clx_runtime.h"
#include "../codegen/codegen.h"
#include <algorithm>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace clx {

//------------------ pass_constants_and_bounds: constant upvalues, global constants, loop/array bounds
void pass_constants_and_bounds(const ASTContext &ctx, AnalysisState &state, PassScratch &sk, uint32_t root_node) {
    auto &known_numbers = sk.known_numbers;
    auto &disqualified = sk.disqualified;
    auto &for_vars = sk.for_vars;
    auto &array_bounds = sk.array_bounds;
    auto &loop_limits = sk.loop_limits;
    auto &loop_limit_conflicts = sk.loop_limit_conflicts;
    auto &param_names = sk.param_names;
    state.constant_upvalues.clear();
    for (const auto &node : ctx.nodes) {
        if (node.type == NodeType::Identifier && node.as.ident.is_captured) {
            std::string_view name(node.as.ident.name, node.as.ident.length);
            if (state.reassigned_vars.count(name) == 0 && for_vars.count(name) == 0)
                state.constant_upvalues.insert(name);
        }
    }

    for (const auto &node : ctx.nodes) {
        if (node.type == NodeType::Identifier && (node.as.ident.is_captured || node.as.ident.is_global)) {
            disqualified.insert(std::string_view(node.as.ident.name, node.as.ident.length));
        }
    }

    if (root_node < ctx.nodes.size() && ctx.nodes[root_node].type == NodeType::Block) {
        const ASTNode &root_block = ctx.nodes[root_node];
        for (uint32_t i = 0; i < root_block.as.block.count; ++i) {
            uint32_t stmt_idx = ctx.block_statements[root_block.as.block.first_statement + i];
            const ASTNode &stmt = ctx.nodes[stmt_idx];

            if (stmt.type == NodeType::GlobalDeclStatement && !stmt.as.global_decl.is_wildcard) {
                if (stmt.as.global_decl.ident_count == 1 && stmt.as.global_decl.value_count == 1) {
                    uint32_t id_idx = ctx.block_statements[stmt.as.global_decl.first_ident];
                    uint32_t val_idx = ctx.block_statements[stmt.as.global_decl.first_value];
                    double out_d;
                    int64_t out_i;
                    bool is_int;
                    if (is_literal_number(ctx, val_idx, out_d, out_i, is_int)) {
                        std::string_view name(ctx.nodes[id_idx].as.ident.name, ctx.nodes[id_idx].as.ident.length);
                        state.global_constants[name] = out_d;
                    }
                }
            } else if (stmt.type == NodeType::Assignment) {
                if (stmt.as.assign.target_count == 1 && stmt.as.assign.value_count == 1) {
                    uint32_t t_idx = ctx.block_statements[stmt.as.assign.first_target];
                    uint32_t val_idx = ctx.block_statements[stmt.as.assign.first_value];
                    if (ctx.nodes[t_idx].type == NodeType::Identifier && ctx.nodes[t_idx].as.ident.is_global) {
                        double out_d;
                        int64_t out_i;
                        bool is_int;
                        if (is_literal_number(ctx, val_idx, out_d, out_i, is_int)) {
                            std::string_view name(ctx.nodes[t_idx].as.ident.name, ctx.nodes[t_idx].as.ident.length);
                            state.global_constants[name] = out_d;
                        }
                    }
                }
            }
        }
    }

    state.numeric_builtin_aliases.clear();
    for (const auto &node : ctx.nodes) {
        uint32_t t_idx = 0xFFFFFFFF, v_idx = 0xFFFFFFFF;
        if (node.type == NodeType::LocalDecl && node.as.local_decl.ident_count == 1
            && node.as.local_decl.value_count == 1) {
            t_idx = ctx.block_statements[node.as.local_decl.first_ident];
            v_idx = ctx.block_statements[node.as.local_decl.first_value];
        } else if (node.type == NodeType::Assignment && node.as.assign.target_count == 1
            && node.as.assign.value_count == 1) {
            t_idx = ctx.block_statements[node.as.assign.first_target];
            v_idx = ctx.block_statements[node.as.assign.first_value];
        } else {
            continue;
        }
        if (t_idx >= ctx.nodes.size() || v_idx >= ctx.nodes.size())
            continue;
        if (ctx.nodes[t_idx].type != NodeType::Identifier || ctx.nodes[v_idx].type != NodeType::TableAccess)
            continue;
        const auto &ta = ctx.nodes[v_idx].as.table_access;
        if (ta.table >= ctx.nodes.size() || ta.key >= ctx.nodes.size())
            continue;
        if (ctx.nodes[ta.table].type != NodeType::Identifier || ctx.nodes[ta.key].type != NodeType::String)
            continue;
        std::string_view mod(ctx.nodes[ta.table].as.ident.name, ctx.nodes[ta.table].as.ident.length);
        std::string_view key(ctx.nodes[ta.key].as.string.text, ctx.nodes[ta.key].as.string.length);
        if (mod != "math")
            continue;
        static const char *numeric_fns[] = { "abs", "acos", "asin", "atan", "atan2", "ceil", "cos",
            "cosh", "deg", "exp", "floor", "fmod", "log", "max", "min", "pow", "rad", "sin",
            "sinh", "sqrt", "tan", "tanh", nullptr };
        bool is_num = false;
        for (const char **f = numeric_fns; *f; ++f) {
            if (key == *f) {
                is_num = true;
                break;
            }
        }
        if (!is_num)
            continue;
        std::string_view alias(ctx.nodes[t_idx].as.ident.name, ctx.nodes[t_idx].as.ident.length);
        if (state.reassigned_vars.count(alias))
            continue;
        state.numeric_builtin_aliases.emplace(alias);
    }

    auto block_writes_table = [&](auto &self, uint32_t block_idx, std::string_view table_name) -> bool {
        if (block_idx == 0xFFFFFFFF || block_idx >= ctx.nodes.size())
            return false;
        const auto &block = ctx.nodes[block_idx];
        if (block.type != NodeType::Block)
            return false;
        for (uint32_t i = 0; i < block.as.block.count; ++i) {
            uint32_t s_idx = ctx.block_statements[block.as.block.first_statement + i];
            const auto &stmt = ctx.nodes[s_idx];
            if (stmt.type == NodeType::Assignment) {
                for (uint32_t t = 0; t < stmt.as.assign.target_count; ++t) {
                    uint32_t tgt = ctx.block_statements[stmt.as.assign.first_target + t];
                    auto table_of_target = [&](auto &self2, uint32_t node_idx) -> uint32_t {
                        const auto &n = ctx.nodes[node_idx];
                        if (n.type == NodeType::TableAccess)
                            return self2(self2, n.as.table_access.table);
                        return node_idx;
                    };
                    uint32_t base = table_of_target(table_of_target, tgt);
                    if (base < ctx.nodes.size() && ctx.nodes[base].type == NodeType::Identifier) {
                        std::string_view base_name(ctx.nodes[base].as.ident.name, ctx.nodes[base].as.ident.length);
                        if (base_name == table_name)
                            return true;
                    }
                }
            } else if (stmt.type == NodeType::Block) {
                if (self(self, s_idx, table_name))
                    return true;
            } else if (stmt.type == NodeType::ForStatement) {
                if (self(self, stmt.as.for_stmt.body_block, table_name))
                    return true;
            } else if (stmt.type == NodeType::WhileStatement) {
                if (self(self, stmt.as.while_stmt.body_block, table_name))
                    return true;
            } else if (stmt.type == NodeType::RepeatStatement) {
                if (self(self, stmt.as.repeat_stmt.body_block, table_name))
                    return true;
            } else if (stmt.type == NodeType::IfStatement) {
                if (stmt.as.if_stmt.then_block < ctx.nodes.size() && self(self, stmt.as.if_stmt.then_block, table_name))
                    return true;
                if (stmt.as.if_stmt.else_block != 0xFFFFFFFF && self(self, stmt.as.if_stmt.else_block, table_name))
                    return true;
            }
        }
        return false;
    };

    for (const auto &node : ctx.nodes) {
        if (node.type == NodeType::Block) {
            std::map<uint32_t, uint32_t> pending_tables;

            for (uint32_t i = 0; i < node.as.block.count; ++i) {
                uint32_t stmt_idx = ctx.block_statements[node.as.block.first_statement + i];
                const ASTNode &stmt = ctx.nodes[stmt_idx];

                if (stmt.type == NodeType::LocalDecl && stmt.as.local_decl.value_count == 1) {
                    uint32_t val_idx = ctx.block_statements[stmt.as.local_decl.first_value];
                    if (ctx.nodes[val_idx].type == NodeType::TableConstructor
                        && ctx.nodes[val_idx].as.table_cons.count == 0) {
                        pending_tables[ctx.block_statements[stmt.as.local_decl.first_ident]] = val_idx;
                    }
                } else if (stmt.type == NodeType::Assignment && stmt.as.assign.value_count == 1) {
                    uint32_t val_idx = ctx.block_statements[stmt.as.assign.first_value];
                    if (ctx.nodes[val_idx].type == NodeType::TableConstructor
                        && ctx.nodes[val_idx].as.table_cons.count == 0) {
                        pending_tables[ctx.block_statements[stmt.as.assign.first_target]] = val_idx;
                    }
                } else if (stmt.type == NodeType::ForStatement) {
                    auto has_forward_ref
                        = [&](uint32_t expr_node, const std::vector<std::string> &pending_names) -> bool {
                        std::vector<uint32_t> stack = { expr_node };
                        while (!stack.empty()) {
                            uint32_t nid = stack.back();
                            stack.pop_back();
                            const auto &n = ctx.nodes[nid];
                            if (n.type == NodeType::Identifier && !n.as.ident.is_global) {
                                std::string_view nm(n.as.ident.name, n.as.ident.length);
                                for (auto &pn : pending_names) {
                                    if (nm == pn)
                                        return true;
                                }
                                continue;
                            }
                            auto push = [&](uint32_t child) {
                                if (child != 0xFFFFFFFF && child < ctx.nodes.size())
                                    stack.push_back(child);
                            };
                            switch (n.type) {
                            case NodeType::BinaryOp:
                                push(n.as.bin_op.left);
                                push(n.as.bin_op.right);
                                break;
                            case NodeType::UnaryOp:
                                push(n.as.unary_op.expr);
                                break;
                            case NodeType::ParenExpression:
                                push(n.as.paren_expr.expr);
                                break;
                            case NodeType::CallExpression: {
                                for (uint32_t i = 0; i < n.as.call_expr.arg_count; ++i)
                                    push(ctx.block_statements[n.as.call_expr.first_arg + i]);
                                push(n.as.call_expr.target);
                                break;
                            }
                            case NodeType::TableAccess:
                                push(n.as.table_access.table);
                                push(n.as.table_access.key);
                                break;
                            case NodeType::IntrinsicCall: {
                                for (uint32_t i = 0; i < n.as.intrinsic_call.arg_count; ++i)
                                    push(ctx.block_statements[n.as.intrinsic_call.first_arg + i]);
                                break;
                            }
                            default:
                                break;
                            }
                        }
                        return false;
                    };
                    std::vector<std::string> _pending_names;
                    for (auto &_pp : pending_tables) {
                        _pending_names.push_back(get_ast_string(ctx, _pp.first));
                    }
                    for (auto &pair : pending_tables) {
                        uint32_t table_node = pair.second;
                        if (state.table_presize.find(table_node) == state.table_presize.end()) {
                            std::string table_name = get_ast_string(ctx, pair.first);
                            bool hw = !table_name.empty()
                                && block_writes_table(block_writes_table, stmt.as.for_stmt.body_block, table_name);
                            if (hw && !has_forward_ref(stmt.as.for_stmt.limit_expr, _pending_names)) {
                                state.table_presize[table_node] = stmt.as.for_stmt.limit_expr;
                                if (!table_name.empty())
                                    array_bounds[table_name] = stmt.as.for_stmt.limit_expr;
                            }
                        }
                    }
                    pending_tables.clear();
                }
            }
        }

        if (node.type == NodeType::ForStatement) {
            std::string_view name(ctx.nodes[node.as.for_stmt.var_ident].as.ident.name,
                ctx.nodes[node.as.for_stmt.var_ident].as.ident.length);
            known_numbers.insert(name);

            uint32_t limit_expr = node.as.for_stmt.limit_expr;
            if (limit_expr < ctx.nodes.size() && ctx.nodes[limit_expr].type == NodeType::Identifier) {
                std::string_view lim_name(ctx.nodes[limit_expr].as.ident.name, ctx.nodes[limit_expr].as.ident.length);
                known_numbers.insert(lim_name);
            }

            std::string vname = get_ast_string(ctx, node.as.for_stmt.var_ident);
            if (!vname.empty()) {
                if (loop_limits.count(vname) && loop_limits[vname] != node.as.for_stmt.limit_expr) {
                    std::string l1 = get_ast_string(ctx, loop_limits[vname]);
                    std::string l2 = get_ast_string(ctx, node.as.for_stmt.limit_expr);
                    if (l1 != l2)
                        loop_limit_conflicts[vname] = true;
                }
                loop_limits[vname] = node.as.for_stmt.limit_expr;
            }
        }
    }

    for (const auto &nd : ctx.nodes) {
        if (nd.type == NodeType::FunctionDef) {
            for (size_t p = 0; p < nd.as.func_def.param_count; ++p) {
                uint32_t pi = ctx.block_statements[nd.as.func_def.first_param + p];
                if (pi < ctx.nodes.size() && ctx.nodes[pi].type == NodeType::Identifier) {
                    auto pn = std::string_view(ctx.nodes[pi].as.ident.name, ctx.nodes[pi].as.ident.length);
                    param_names.insert(pn);
                }
            }
        }
    }

    for (uint32_t ni = 0; ni < ctx.nodes.size(); ++ni) {
        const auto &node = ctx.nodes[ni];
        if (node.type != NodeType::ForStatement)
            continue;
        uint32_t var = node.as.for_stmt.var_ident;
        if (var >= ctx.nodes.size() || ctx.nodes[var].type != NodeType::Identifier)
            continue;
        auto nm = std::string_view(ctx.nodes[var].as.ident.name, ctx.nodes[var].as.ident.length);
        if (param_names.count(nm))
            continue;
        double sd = 0;
        int64_t si = 0;
        bool sint = false;
        bool int_start = is_literal_number(ctx, node.as.for_stmt.start_expr, sd, si, sint) && sint;
        bool int_step = node.as.for_stmt.step_expr == 0xFFFFFFFF;
        if (!int_step) {
            double td = 0;
            int64_t ti = 0;
            bool tint = false;
            int_step = is_literal_number(ctx, node.as.for_stmt.step_expr, td, ti, tint) && tint;
        }
        if (int_start && int_step) {
            std::string s(nm);
            if (!state.native_integers.count(s))
                state.native_integers.insert(s);
            state.for_counter_int.insert(s);
        }
    }
}

//------------------ pass_numeric_params: numeric parameter detection and native_numbers finalization
void pass_numeric_params(const ASTContext &ctx, AnalysisState &state, PassScratch &sk, uint32_t root_node) {
    auto &known_numbers = sk.known_numbers;
    auto &disqualified = sk.disqualified;
    auto &param_names = sk.param_names;
    state.param_numbers.clear();

    std::set<std::string> numeric_params;

    for (uint32_t node_idx = 0; node_idx < ctx.nodes.size(); ++node_idx) {
        const auto &node = ctx.nodes[node_idx];
        if (node.type == NodeType::LocalDecl) {
            for (uint32_t ii = 0; ii < node.as.local_decl.ident_count; ++ii) {
                uint32_t idi = ctx.block_statements[node.as.local_decl.first_ident + ii];
                if (idi >= ctx.nodes.size() || ctx.nodes[idi].type != NodeType::Identifier)
                    continue;
                std::string nm(ctx.nodes[idi].as.ident.name, ctx.nodes[idi].as.ident.length);
                if (known_numbers.count(nm) && !state.native_integers.count(nm)) {
                    uint32_t vi = (ii < node.as.local_decl.value_count)
                        ? ctx.block_statements[node.as.local_decl.first_value + ii]
                        : 0xFFFFFFFF;
                    if (is_purely_integer_expr(ctx, state, vi) && !reassigned_with_non_int(ctx, state, nm, node_idx))
                        state.native_integers.insert(std::string(nm));
                }
            }
        } else if (node.type == NodeType::Assignment) {
            for (uint32_t ii = 0; ii < node.as.assign.target_count; ++ii) {
                uint32_t ti = ctx.block_statements[node.as.assign.first_target + ii];
                if (ti >= ctx.nodes.size() || ctx.nodes[ti].type != NodeType::Identifier)
                    continue;
                std::string_view nm(ctx.nodes[ti].as.ident.name, ctx.nodes[ti].as.ident.length);
                if (known_numbers.count(nm) && !state.native_integers.count(std::string(nm))) {
                    uint32_t vi = (ii < node.as.assign.value_count)
                        ? ctx.block_statements[node.as.assign.first_value + ii]
                        : 0xFFFFFFFF;
                    if (is_purely_integer_expr(ctx, state, vi) && !reassigned_with_non_int(ctx, state, nm, node_idx))
                        state.native_integers.insert(std::string(nm));
                }
            }
        }
    }

    int _np_debug_node_count = 0;
    state.param_numbers.clear();
    std::map<std::string_view, std::map<uint32_t, uint32_t>> func_to_param_indices;
    for (uint32_t ni = 0; ni < ctx.nodes.size(); ++ni) {
        const auto &nd = ctx.nodes[ni];
        if (nd.type != NodeType::FunctionDef)
            continue;
        _np_debug_node_count++;
        for (size_t p = 0; p < nd.as.func_def.param_count; ++p) {
            uint32_t pi = ctx.block_statements[nd.as.func_def.first_param + p];
            if (pi < ctx.nodes.size() && ctx.nodes[pi].type == NodeType::Identifier)
                func_to_param_indices[std::string_view(ctx.nodes[pi].as.ident.name, ctx.nodes[pi].as.ident.length)][ni]
                    = (uint32_t)p;
        }
    }
    std::map<uint32_t, std::vector<std::string_view>> func_def_params;
    for (uint32_t ni = 0; ni < ctx.nodes.size(); ++ni) {
        const auto &nd = ctx.nodes[ni];
        if (nd.type != NodeType::FunctionDef)
            continue;
        _np_debug_node_count++;
        for (size_t p = 0; p < nd.as.func_def.param_count; ++p) {
            uint32_t pi = ctx.block_statements[nd.as.func_def.first_param + p];
            if (pi < ctx.nodes.size() && ctx.nodes[pi].type == NodeType::Identifier)
                func_def_params[ni].push_back(
                    std::string_view(ctx.nodes[pi].as.ident.name, ctx.nodes[pi].as.ident.length));
        }
    }

    bool np_changed = true;
    for (int iter = 0; iter < 10 && np_changed; ++iter) {
        np_changed = false;
        for (uint32_t ni = 0; ni < ctx.nodes.size(); ++ni) {
            const auto &nd = ctx.nodes[ni];
            if (nd.type != NodeType::FunctionDef)
                continue;
            auto &fparams = func_def_params[ni];
            if (fparams.empty())
                continue;

            std::vector<uint32_t> stack = { nd.as.func_def.body_block };
            std::set<uint32_t> visited;
            while (!stack.empty()) {
                uint32_t nid = stack.back();
                stack.pop_back();
                if (nid == 0xFFFFFFFF || nid >= ctx.nodes.size())
                    continue;
                if (!visited.insert(nid).second)
                    continue;
                const auto &nn = ctx.nodes[nid];

                if (nn.type == NodeType::Block) {
                    for (uint32_t bi = 0; bi < nn.as.block.count; ++bi)
                        stack.push_back(ctx.block_statements[nn.as.block.first_statement + bi]);
                }
                if (nn.type == NodeType::LocalDecl) {
                    for (uint32_t vi = 0; vi < nn.as.local_decl.value_count; ++vi)
                        stack.push_back(ctx.block_statements[nn.as.local_decl.first_value + vi]);
                }
                if (nn.type == NodeType::Assignment) {
                    for (uint32_t vi = 0; vi < nn.as.assign.value_count; ++vi)
                        stack.push_back(ctx.block_statements[nn.as.assign.first_value + vi]);
                }
                if (nn.type == NodeType::ForStatement) {
                    stack.push_back(nn.as.for_stmt.start_expr);
                    stack.push_back(nn.as.for_stmt.limit_expr);
                    if (nn.as.for_stmt.step_expr != 0xFFFFFFFF)
                        stack.push_back(nn.as.for_stmt.step_expr);
                    stack.push_back(nn.as.for_stmt.body_block);
                }
                if (nn.type == NodeType::IfStatement) {
                    stack.push_back(nn.as.if_stmt.condition);
                    stack.push_back(nn.as.if_stmt.then_block);
                    if (nn.as.if_stmt.else_block != 0xFFFFFFFF)
                        stack.push_back(nn.as.if_stmt.else_block);
                }
                if (nn.type == NodeType::WhileStatement || nn.type == NodeType::RepeatStatement) {
                    stack.push_back(nn.as.while_stmt.condition);
                    stack.push_back(nn.as.while_stmt.body_block);
                }
                if (nn.type == NodeType::BinaryOp) {
                    stack.push_back(nn.as.bin_op.left);
                    stack.push_back(nn.as.bin_op.right);
                }
                if (nn.type == NodeType::UnaryOp) {
                    stack.push_back(nn.as.unary_op.expr);
                }
                if (nn.type == NodeType::ParenExpression) {
                    stack.push_back(nn.as.paren_expr.expr);
                }
                if (nn.type == NodeType::GenericForStatement) {
                    stack.push_back(nn.as.generic_for.body_block);
                }
                if (nn.type == NodeType::ReturnStatement) {
                    for (uint32_t ri = 0; ri < nn.as.return_stmt.value_count; ++ri)
                        stack.push_back(ctx.block_statements[nn.as.return_stmt.first_value + ri]);
                }

                if (nn.type == NodeType::BinaryOp) {
                    int op = nn.as.bin_op.op;
                    if ((op >= static_cast<int>(BinaryOp::Add) && op <= static_cast<int>(BinaryOp::Div))
                        || (op >= static_cast<int>(BinaryOp::Mod) && op <= static_cast<int>(BinaryOp::Shr))) {
                        auto check_arith = [&](uint32_t idx) {
                            if (idx < ctx.nodes.size() && ctx.nodes[idx].type == NodeType::Identifier) {
                                std::string_view nm(ctx.nodes[idx].as.ident.name, ctx.nodes[idx].as.ident.length);
                                for (auto &fp : fparams) {
                                    if (fp == nm) {
                                        if (!state.param_numbers[ni].count(std::string(nm))) {
                                            state.param_numbers[ni].insert(std::string(nm));
                                            np_changed = true;
                                        }
                                    }
                                }
                            }
                        };
                        check_arith(nn.as.bin_op.left);
                        check_arith(nn.as.bin_op.right);
                    }
                }
                if (nn.type == NodeType::CallExpression) {
                    uint32_t tgt = nn.as.call_expr.target;
                    if (tgt < ctx.nodes.size() && ctx.nodes[tgt].type == NodeType::Identifier) {
                        std::string_view callee(ctx.nodes[tgt].as.ident.name, ctx.nodes[tgt].as.ident.length);
                        auto cit = func_to_param_indices.find(callee);
                        if (cit != func_to_param_indices.end()) {
                            for (auto &[ci, start_idx] : cit->second) {
                                auto &cparams = func_def_params[ci];
                                for (size_t ai = 0; ai < nn.as.call_expr.arg_count && ai < cparams.size(); ++ai) {
                                    uint32_t arg_nid = ctx.block_statements[nn.as.call_expr.first_arg + ai];
                                    if (arg_nid < ctx.nodes.size() && ctx.nodes[arg_nid].type == NodeType::Identifier) {
                                        std::string_view arg_nm(
                                            ctx.nodes[arg_nid].as.ident.name, ctx.nodes[arg_nid].as.ident.length);
                                        std::string callee_pn(cparams[ai]);
                                        if (state.param_numbers[ci].count(callee_pn)
                                            && !state.param_numbers[ni].count(std::string(arg_nm))) {
                                            for (auto &fp : fparams) {
                                                if (fp == arg_nm) {
                                                    state.param_numbers[ni].insert(std::string(arg_nm));
                                                    np_changed = true;
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    state.native_numbers.clear();
    for (auto name : known_numbers) {
        if (disqualified.find(name) == disqualified.end() && param_names.find(name) == param_names.end()) {
            state.native_numbers.push_back(name);
        }
    }

    {
        std::set<std::string_view> loop_vars;
        for (const auto &nd : ctx.nodes) {
            if (nd.type == NodeType::ForStatement) {
                uint32_t vi = nd.as.for_stmt.var_ident;
                if (vi < ctx.nodes.size() && ctx.nodes[vi].type == NodeType::Identifier) {
                    loop_vars.insert(std::string_view(ctx.nodes[vi].as.ident.name, ctx.nodes[vi].as.ident.length));
                }
            }
        }
        std::vector<const ASTNode *> func_defs;
        for (const auto &nd : ctx.nodes) {
            if (nd.type == NodeType::FunctionDef) {
                func_defs.push_back(&nd);
            } else if (nd.type == NodeType::LocalDecl) {
                for (uint32_t ii = 0; ii < nd.as.local_decl.value_count; ++ii) {
                    uint32_t vi = ctx.block_statements[nd.as.local_decl.first_value + ii];
                    if (vi < ctx.nodes.size() && ctx.nodes[vi].type == NodeType::FunctionDef) {
                        func_defs.push_back(&ctx.nodes[vi]);
                    }
                }
            }
        }
        std::unordered_set<std::string_view> assigned_vars;
        assigned_vars.reserve(1024);
        for (const auto &nd2 : ctx.nodes) {
            if (nd2.type == NodeType::Assignment) {
                for (uint32_t ii = 0; ii < nd2.as.assign.target_count; ++ii) {
                    uint32_t ti = ctx.block_statements[nd2.as.assign.first_target + ii];
                    if (ti < ctx.nodes.size() && ctx.nodes[ti].type == NodeType::Identifier) {
                        assigned_vars.insert(
                            std::string_view(ctx.nodes[ti].as.ident.name, ctx.nodes[ti].as.ident.length));
                    }
                }
            }
        }
        std::unordered_set<std::string_view> all_loop_vars;
        all_loop_vars.reserve(1024);
        for (const auto &nd2 : ctx.nodes) {
            if (nd2.type == NodeType::ForStatement) {
                uint32_t vi = nd2.as.for_stmt.var_ident;
                if (vi < ctx.nodes.size() && ctx.nodes[vi].type == NodeType::Identifier) {
                    all_loop_vars.insert(std::string_view(ctx.nodes[vi].as.ident.name, ctx.nodes[vi].as.ident.length));
                }
            }
        }
        std::unordered_map<std::string_view, std::vector<uint32_t>> pure_numeric_bin_by_sname2;
        pure_numeric_bin_by_sname2.reserve(1024);
        for (uint32_t idx = 0; idx < ctx.nodes.size(); ++idx) {
            const auto &bn2 = ctx.nodes[idx];
            if (bn2.type != NodeType::BinaryOp)
                continue;
            int bop2 = bn2.as.bin_op.op;
            if (bop2 < static_cast<int>(BinaryOp::Add) || bop2 > static_cast<int>(BinaryOp::Div))
                continue;
            std::function<void(uint32_t, std::vector<std::string_view> &)> collect_pure2
                = [&](uint32_t side, std::vector<std::string_view> &out) {
                      if (side >= ctx.nodes.size())
                          return;
                      const auto &sn2 = ctx.nodes[side];
                      if (sn2.type == NodeType::TableAccess) {
                          uint32_t stbl2 = sn2.as.table_access.table;
                          if (stbl2 < ctx.nodes.size() && ctx.nodes[stbl2].type == NodeType::Identifier) {
                              uint32_t key_idx2 = sn2.as.table_access.key;
                              if (key_idx2 < ctx.nodes.size()) {
                                  const auto &kn2 = ctx.nodes[key_idx2];
                                  bool key_is_num2 = (kn2.type == NodeType::Number || kn2.type == NodeType::Integer);
                                  if (!key_is_num2 && kn2.type == NodeType::Identifier) {
                                      std::string_view knm2(kn2.as.ident.name, kn2.as.ident.length);
                                      key_is_num2 = state.native_integers.count(knm2) > 0;
                                      if (!key_is_num2) {
                                          key_is_num2 = std::find(state.native_numbers.begin(),
                                                            state.native_numbers.end(), knm2)
                                              != state.native_numbers.end();
                                      }
                                      if (!key_is_num2) {
                                          key_is_num2 = all_loop_vars.count(knm2) > 0;
                                      }
                                  }
                                  if (key_is_num2) {
                                      std::string_view sname2(
                                          ctx.nodes[stbl2].as.ident.name, ctx.nodes[stbl2].as.ident.length);
                                      out.push_back(sname2);
                                  }
                              }
                          }
                      } else if (sn2.type == NodeType::ParenExpression) {
                          collect_pure2(sn2.as.paren_expr.expr, out);
                      } else if (sn2.type == NodeType::BinaryOp) {
                          collect_pure2(sn2.as.bin_op.left, out);
                          collect_pure2(sn2.as.bin_op.right, out);
                      }
                  };
            std::vector<std::string_view> tmp_snames2;
            collect_pure2(bn2.as.bin_op.left, tmp_snames2);
            collect_pure2(bn2.as.bin_op.right, tmp_snames2);
            for (auto &sname2 : tmp_snames2) {
                pure_numeric_bin_by_sname2[sname2].push_back(idx);
            }
        }
        for (const auto *fnd : func_defs) {
            const auto &nd = *fnd;
            std::set<std::string_view> func_params;
            for (size_t p = 0; p < nd.as.func_def.param_count; ++p) {
                uint32_t pi = ctx.block_statements[nd.as.func_def.first_param + p];
                if (pi < ctx.nodes.size() && ctx.nodes[pi].type == NodeType::Identifier)
                    func_params.insert(std::string_view(ctx.nodes[pi].as.ident.name, ctx.nodes[pi].as.ident.length));
            }
            if (func_params.empty())
                continue;

            for (auto it = func_params.begin(); it != func_params.end();) {
                if (assigned_vars.count(*it))
                    it = func_params.erase(it);
                else
                    ++it;
            }
            if (func_params.empty())
                continue;

            for (auto &sname : func_params) {
                auto it2 = pure_numeric_bin_by_sname2.find(sname);
                if (it2 != pure_numeric_bin_by_sname2.end()) {
                    uint32_t fnd_idx = static_cast<uint32_t>(fnd - &ctx.nodes[0]);
                    state.pure_numeric_func_params[sname].insert(fnd_idx);
                }
            }
        }
    }
}

//------------------ pass_int_returns_masks: integer-returning functions and int-preserving masks
void pass_int_returns_masks(const ASTContext &ctx, AnalysisState &state, PassScratch &sk, uint32_t root_node) {
    auto &func_defs = sk.func_defs;

    {
        std::function<bool(uint32_t, std::set<std::string_view> &)> walk_for_int_returns
            = [&](uint32_t bi, std::set<std::string_view> &loop_vars) -> bool {
            if (bi == 0xFFFFFFFF || bi >= ctx.nodes.size())
                return true;
            const auto &b = ctx.nodes[bi];
            if (b.type != NodeType::Block)
                return true;
            for (uint32_t j = 0; j < b.as.block.count; ++j) {
                uint32_t si = ctx.block_statements[b.as.block.first_statement + j];
                if (si >= ctx.nodes.size())
                    continue;
                const auto &st = ctx.nodes[si];
                if (st.type == NodeType::ReturnStatement) {
                    if (st.as.return_stmt.value_count != 1)
                        return false;
                    uint32_t vi = ctx.block_statements[st.as.return_stmt.first_value];
                    if (vi == 0xFFFFFFFF || vi >= ctx.nodes.size())
                        return false;
                    const auto &v = ctx.nodes[vi];
                    if (v.type == NodeType::Integer)
                        continue;
                    if (v.type == NodeType::Identifier) {
                        std::string_view n(v.as.ident.name, v.as.ident.length);
                        if (state.native_integers.count(std::string(n)))
                            continue;
                        if (loop_vars.count(n))
                            continue;
                    }
                    return false;
                }
                if (st.type == NodeType::Block) {
                    if (!walk_for_int_returns(si, loop_vars))
                        return false;
                } else if (st.type == NodeType::IfStatement) {
                    if (!walk_for_int_returns(st.as.if_stmt.then_block, loop_vars))
                        return false;
                    if (st.as.if_stmt.else_block != 0xFFFFFFFF
                        && !walk_for_int_returns(st.as.if_stmt.else_block, loop_vars))
                        return false;
                } else if (st.type == NodeType::WhileStatement) {
                    if (!walk_for_int_returns(st.as.while_stmt.body_block, loop_vars))
                        return false;
                } else if (st.type == NodeType::RepeatStatement) {
                    if (!walk_for_int_returns(st.as.repeat_stmt.body_block, loop_vars))
                        return false;
                } else if (st.type == NodeType::DoStatement) {
                    if (!walk_for_int_returns(st.as.do_stmt.body_block, loop_vars))
                        return false;
                } else if (st.type == NodeType::ForStatement) {
                    std::string_view tracked_name;
                    if (st.as.for_stmt.var_ident < ctx.nodes.size() && st.as.for_stmt.start_expr < ctx.nodes.size()
                        && ctx.nodes[st.as.for_stmt.start_expr].type == NodeType::Integer
                        && ctx.nodes[st.as.for_stmt.var_ident].type == NodeType::Identifier) {
                        tracked_name = std::string_view(ctx.nodes[st.as.for_stmt.var_ident].as.ident.name,
                            ctx.nodes[st.as.for_stmt.var_ident].as.ident.length);
                        if (!tracked_name.empty())
                            loop_vars.insert(tracked_name);
                    }
                    bool ok = walk_for_int_returns(st.as.for_stmt.body_block, loop_vars);
                    if (!tracked_name.empty())
                        loop_vars.erase(tracked_name);
                    if (!ok)
                        return false;
                } else if (st.type == NodeType::GenericForStatement) {
                    if (!walk_for_int_returns(st.as.generic_for.body_block, loop_vars))
                        return false;
                }
            }
            return true;
        };
        std::unordered_map<uint32_t, std::string_view> func_name_map;
        func_name_map.reserve(ctx.nodes.size());
        for (uint32_t j = 0; j < ctx.nodes.size(); ++j) {
            const auto &ln = ctx.nodes[j];
            uint32_t fv = 0xFFFFFFFF, fi = 0xFFFFFFFF;
            if (ln.type == NodeType::LocalDecl && ln.as.local_decl.value_count == 1) {
                fv = ctx.block_statements[ln.as.local_decl.first_value];
                fi = ctx.block_statements[ln.as.local_decl.first_ident];
            } else if (ln.type == NodeType::Assignment && ln.as.assign.value_count == 1) {
                fv = ctx.block_statements[ln.as.assign.first_value];
                fi = ctx.block_statements[ln.as.assign.first_target];
            } else
                continue;
            if (fv >= ctx.nodes.size() || fi >= ctx.nodes.size())
                continue;
            if (ctx.nodes[fv].type != NodeType::FunctionDef)
                continue;
            if (ctx.nodes[fi].type != NodeType::Identifier)
                continue;
            if (ctx.nodes[fi].as.ident.is_global)
                continue;
            std::string_view nm(ctx.nodes[fi].as.ident.name, ctx.nodes[fi].as.ident.length);
            func_name_map.emplace(fv, nm);
        }

        for (uint32_t i = 0; i < ctx.nodes.size(); ++i) {
            const auto &nd = ctx.nodes[i];
            if (nd.type != NodeType::FunctionDef)
                continue;
            if (nd.as.func_def.body_block == 0xFFFFFFFF)
                continue;
            std::set<std::string_view> loop_vars;
            if (!walk_for_int_returns(nd.as.func_def.body_block, loop_vars))
                continue;
            auto it = func_name_map.find(i);
            if (it != func_name_map.end())
                state.int_returning_funcs.insert(it->second);
        }

        //------------------ int_preserving_masks: for each fast-eligible function, compute (fixed

        {
            auto param_ident = [&](uint32_t fdef_idx, size_t p) -> uint32_t {
                const auto &fn = ctx.nodes[fdef_idx].as.func_def;
                if (p >= fn.param_count)
                    return 0xFFFFFFFF;
                return ctx.block_statements[fn.first_param + p];
            };

            struct Req {
                uint32_t mask;
                bool possible;
            };

            auto arg_req_for = [&](auto &&self, uint32_t fdef_idx, const std::string_view fname,
                                   std::map<std::string_view, uint32_t> &masks, uint32_t arg_idx,
                                   std::set<uint32_t> &visited) -> Req {
                if (arg_idx == 0xFFFFFFFF || arg_idx >= ctx.nodes.size())
                    return { 0u, false };
                const auto &an = ctx.nodes[arg_idx];
                if (an.type == NodeType::ParenExpression)
                    return self(self, fdef_idx, fname, masks, an.as.paren_expr.expr, visited);
                if (an.type == NodeType::Integer)
                    return { 0u, true };
                if (an.type == NodeType::Number)
                    return { 0u, false };
                if (an.type == NodeType::Identifier && !an.as.ident.is_global) {
                    std::string_view nm(an.as.ident.name, an.as.ident.length);
                    const auto &fn = ctx.nodes[fdef_idx].as.func_def;
                    for (size_t p = 0; p < fn.param_count; ++p) {
                        uint32_t pi = param_ident(fdef_idx, p);
                        if (pi != 0xFFFFFFFF
                            && std::string_view(ctx.nodes[pi].as.ident.name, ctx.nodes[pi].as.ident.length) == nm)
                            return { (1u << p), true };
                    }
                    if (clx::is_purely_integer_expr(ctx, state, arg_idx))
                        return { 0u, true };
                    return { 0u, false };
                }
                if (an.type == NodeType::BinaryOp) {
                    int bop = an.as.bin_op.op;
                    bool int_op = (bop >= static_cast<int>(BinaryOp::Add) && bop <= static_cast<int>(BinaryOp::Mul))
                        || bop == static_cast<int>(BinaryOp::FloorDiv) || bop == static_cast<int>(BinaryOp::Mod);
                    if (!int_op)
                        return { 0u, false };
                    Req l = self(self, fdef_idx, fname, masks, an.as.bin_op.left, visited);
                    Req r = self(self, fdef_idx, fname, masks, an.as.bin_op.right, visited);
                    if (!l.possible || !r.possible)
                        return { 0u, false };
                    return { l.mask | r.mask, true };
                }
                if (an.type == NodeType::UnaryOp && an.as.unary_op.op == static_cast<int>(UnaryOp::Minus))
                    return self(self, fdef_idx, fname, masks, an.as.unary_op.expr, visited);
                if (an.type == NodeType::CallExpression) {
                    uint32_t tgt = an.as.call_expr.target;
                    if (tgt >= ctx.nodes.size() || ctx.nodes[tgt].type != NodeType::Identifier
                        || ctx.nodes[tgt].as.ident.is_global)
                        return { 0u, false };
                    std::string_view callee(ctx.nodes[tgt].as.ident.name, ctx.nodes[tgt].as.ident.length);
                    std::string_view mask_owner;
                    uint32_t cm;
                    if (callee == fname) {
                        mask_owner = fname;
                        cm = masks.count(fname) ? masks.at(fname) : 0u;
                    } else if (masks.count(callee)) {
                        mask_owner = callee;
                        cm = masks.at(callee);
                    } else {
                        return { 0u, false };
                    }
                    if (!visited.insert(arg_idx).second)
                        return { 0u, false };
                    Req result = { 0u, true };
                    for (size_t p = 0; p < state.func_param_counts[mask_owner]; ++p) {
                        if (!(cm & (1u << p)))
                            continue;
                        uint32_t ai = (p < an.as.call_expr.arg_count)
                            ? ctx.block_statements[an.as.call_expr.first_arg + p]
                            : 0xFFFFFFFF;
                        Req ar = self(self, fdef_idx, fname, masks, ai, visited);
                        if (!ar.possible) {
                            result = { 0u, false };
                            break;
                        }
                        result.mask |= ar.mask;
                    }
                    visited.erase(arg_idx);
                    return result;
                }
                return { 0u, false };
            };
            for (const auto &fdef : func_defs) {
                const auto &fn = ctx.nodes[fdef.second].as.func_def;
                if (fn.param_count >= 32)
                    continue;
                if (!state.func_param_counts.count(fdef.first))
                    continue;
                const auto &pn = state.func_param_native;
                if (!pn.count(fdef.first))
                    continue;
                bool all_native = true;
                for (bool p : pn.at(fdef.first))
                    if (!p)
                        all_native = false;
                if (!all_native || pn.at(fdef.first).empty())
                    continue;
                uint32_t mask = (fn.param_count == 32) ? 0xFFFFFFFFu : ((1u << fn.param_count) - 1u);
                std::map<std::string_view, uint32_t> masks;
                masks[fdef.first] = mask;
                uint32_t cur = masks[fdef.first];
                bool impossible = false;
                int guard = 0;
                for (; guard < 64; ++guard) {
                    uint32_t acc = 0;
                    impossible = false;
                    std::function<bool(uint32_t, std::set<uint32_t> &)> walk
                        = [&](uint32_t bi, std::set<uint32_t> &visited) -> bool {
                        if (bi == 0xFFFFFFFF || bi >= ctx.nodes.size())
                            return true;
                        const auto &b = ctx.nodes[bi];
                        if (b.type != NodeType::Block)
                            return true;
                        for (uint32_t j = 0; j < b.as.block.count; ++j) {
                            uint32_t si = ctx.block_statements[b.as.block.first_statement + j];
                            if (si >= ctx.nodes.size())
                                continue;
                            const auto &st = ctx.nodes[si];
                            if (st.type == NodeType::ReturnStatement) {
                                if (st.as.return_stmt.value_count != 1)
                                    return false;
                                uint32_t vi = ctx.block_statements[st.as.return_stmt.first_value];
                                Req r = arg_req_for(arg_req_for, fdef.second, fdef.first, masks, vi, visited);
                                if (!r.possible) {
                                    impossible = true;
                                    return false;
                                }
                                acc |= r.mask;
                                continue;
                            }
                            if (st.type == NodeType::Block) {
                                if (!walk(si, visited))
                                    return false;
                            } else if (st.type == NodeType::IfStatement) {
                                if (!walk(st.as.if_stmt.then_block, visited))
                                    return false;
                                if (st.as.if_stmt.else_block != 0xFFFFFFFF && !walk(st.as.if_stmt.else_block, visited))
                                    return false;
                            } else if (st.type == NodeType::WhileStatement) {
                                if (!walk(st.as.while_stmt.body_block, visited))
                                    return false;
                            } else if (st.type == NodeType::RepeatStatement) {
                                if (!walk(st.as.repeat_stmt.body_block, visited))
                                    return false;
                            } else if (st.type == NodeType::DoStatement) {
                                if (!walk(st.as.do_stmt.body_block, visited))
                                    return false;
                            } else if (st.type == NodeType::ForStatement) {
                                if (!walk(st.as.for_stmt.body_block, visited))
                                    return false;
                            } else if (st.type == NodeType::GenericForStatement) {
                                if (!walk(st.as.generic_for.body_block, visited))
                                    return false;
                            }
                        }
                        return true;
                    };
                    std::set<uint32_t> visited;
                    walk(fn.body_block, visited);
                    if (impossible)
                        break;
                    if (acc == cur)
                        break;
                    cur = acc;
                    masks[fdef.first] = cur;
                }
                if (guard < 64 && !impossible && cur != 0)
                    state.int_preserving_masks[fdef.first] = cur;
            }
        }
        for (uint32_t i = 0; i < ctx.nodes.size(); ++i) {
            const auto &nd = ctx.nodes[i];
            if (nd.type != NodeType::LocalDecl)
                continue;
            if (nd.as.local_decl.ident_count != 1 || nd.as.local_decl.value_count != 1)
                continue;
            uint32_t fi = ctx.block_statements[nd.as.local_decl.first_ident];
            uint32_t fv = ctx.block_statements[nd.as.local_decl.first_value];
            if (fi >= ctx.nodes.size() || fv >= ctx.nodes.size())
                continue;
            if (ctx.nodes[fi].type != NodeType::Identifier)
                continue;
            if (ctx.nodes[fv].type != NodeType::CallExpression)
                continue;
            uint32_t tgt = ctx.nodes[fv].as.call_expr.target;
            if (tgt >= ctx.nodes.size() || ctx.nodes[tgt].type != NodeType::Identifier)
                continue;
            std::string_view fn(ctx.nodes[tgt].as.ident.name, ctx.nodes[tgt].as.ident.length);
            if (state.int_returning_funcs.count(fn)) {
                std::string ln(ctx.nodes[fi].as.ident.name, ctx.nodes[fi].as.ident.length);
                state.int_typed_locals.insert(ln);
            }
        }
    }
}

//------------------ nogc_is_expanding: true if a value expression can expand to multiple values
//------------------ (uncaptured call or vararg, ignoring parenthesis truncation which disables it).
static bool nogc_is_expanding(const ASTContext &ctx, uint32_t node_idx) {
    uint32_t nidx = node_idx;
    while (nidx < ctx.nodes.size() && ctx.nodes[nidx].type == NodeType::ParenExpression)
        nidx = ctx.nodes[nidx].as.paren_expr.expr;
    if (nidx >= ctx.nodes.size())
        return false;
    return ctx.nodes[nidx].type == NodeType::CallExpression || ctx.nodes[nidx].type == NodeType::Vararg;
}

//------------------ nogc_rhs_ok: true if a value expression can only produce numbers, booleans,
//------------------ nil or strings. The collector traces only Table/Thread/Function/UserData
//------------------ (gc_push_for_trace) and never touches strings, so such values need no frame
//------------------ slot. Soundness-critical: unknown node types default to false. In clx (unlike
//------------------ stock Lua) binary/unary operators on non-table/userdata operands never consult
//------------------ metamethods (find_bin_metamethod checks Table/UserData only), so arithmetic,
//------------------ bitwise, concat and comparisons of non-GC operands yield plain values or throw.
static bool nogc_rhs_ok(const ASTContext &ctx, const AnalysisState &state,
    const std::set<std::string_view> &cands, uint32_t node_idx, int depth) {
    if (depth > 64 || node_idx == 0xFFFFFFFF || node_idx >= ctx.nodes.size())
        return false;
    const auto &n = ctx.nodes[node_idx];
    switch (n.type) {
    case NodeType::Number:
    case NodeType::Integer:
    case NodeType::String:
    case NodeType::TrueLiteral:
    case NodeType::FalseLiteral:
    case NodeType::NilLiteral:
        return true;
    case NodeType::ParenExpression:
        return nogc_rhs_ok(ctx, state, cands, n.as.paren_expr.expr, depth + 1);
    case NodeType::UnaryOp: {
        int op = n.as.unary_op.op;
        if (op == static_cast<int>(UnaryOp::Not) || op == static_cast<int>(UnaryOp::Len))
            return true;
        return nogc_rhs_ok(ctx, state, cands, n.as.unary_op.expr, depth + 1);
    }
    case NodeType::BinaryOp: {
        int op = n.as.bin_op.op;
        if (op == static_cast<int>(BinaryOp::Eq) || op == static_cast<int>(BinaryOp::Ne)
            || op == static_cast<int>(BinaryOp::Lt) || op == static_cast<int>(BinaryOp::Gt)
            || op == static_cast<int>(BinaryOp::Le) || op == static_cast<int>(BinaryOp::Ge))
            return true;
        return nogc_rhs_ok(ctx, state, cands, n.as.bin_op.left, depth + 1)
            && nogc_rhs_ok(ctx, state, cands, n.as.bin_op.right, depth + 1);
    }
    case NodeType::TableAccess:
        if (yields_number(ctx, state, node_idx, nullptr, "", nullptr))
            return true;
        return struct_field_kind(ctx, state, node_idx) == FieldKind::Str;
    case NodeType::CallExpression: {
        uint32_t tgt = n.as.call_expr.target;
        if (tgt < ctx.nodes.size() && ctx.nodes[tgt].type == NodeType::Identifier
            && !ctx.nodes[tgt].as.ident.is_global) {
            std::string_view fname(ctx.nodes[tgt].as.ident.name, ctx.nodes[tgt].as.ident.length);
            if (state.native_return_funcs.count(fname) || state.int_returning_funcs.count(fname))
                return true;
            if (state.numeric_builtin_aliases.count(std::string(fname)))
                return true;
        }
        return false;
    }
    case NodeType::Identifier: {
        if (n.as.ident.is_global)
            return false;
        std::string_view nm(n.as.ident.name, n.as.ident.length);
        if (cands.count(nm))
            return true;
        if (std::find(state.native_numbers.begin(), state.native_numbers.end(), nm)
            != state.native_numbers.end())
            return true;
        if (state.int_typed_locals.count(std::string(nm)))
            return true;
        if (state.native_integers.count(std::string(nm)))
            return true;
        return state.for_counter_names.count(nm) > 0;
    }
    default:
        return false;
    }
}

//------------------ pass_nogc_locals: block locals proven to never hold a GC-managed value.
//------------------ A candidate survives only if every reaching assignment (declaration values
//------------------ and later stores, with Lua multi-value adjustment) is nogc-clean. Captured
//------------------ locals, parameters and unknown RHS kinds stay rooted. Runs last so that
//------------------ yields_number, native/int-returning funcs and builtin aliases are final.
void pass_nogc_locals(const ASTContext &ctx, AnalysisState &state, PassScratch &sk, uint32_t root_node) {
    (void)root_node;
    (void)sk;
    std::map<uint32_t, std::set<std::string_view>> cands;
    for (uint32_t i = 0; i < ctx.nodes.size(); ++i) {
        const auto &nd = ctx.nodes[i];
        if (nd.type != NodeType::LocalDecl)
            continue;
        uint32_t owner = owner_of_node(state, i);
        for (uint32_t k = 0; k < nd.as.local_decl.ident_count; ++k) {
            uint32_t idi = ctx.block_statements[nd.as.local_decl.first_ident + k];
            if (idi >= ctx.nodes.size() || ctx.nodes[idi].type != NodeType::Identifier)
                continue;
            if (ctx.nodes[idi].as.ident.is_captured)
                continue;
            cands[owner].emplace(ctx.nodes[idi].as.ident.name, ctx.nodes[idi].as.ident.length);
        }
    }
    if (cands.empty())
        return;
    auto paired_rhs = [&](const auto &stmt, uint32_t first_v, uint32_t v_count, uint32_t ti) -> uint32_t {
        (void)stmt;
        if (v_count == 0)
            return 0xFFFFFFFF;
        if (ti < v_count - 1)
            return ctx.block_statements[first_v + ti];
        if (ti == v_count - 1)
            return ctx.block_statements[first_v + ti];
        uint32_t last = ctx.block_statements[first_v + v_count - 1];
        if (nogc_is_expanding(ctx, last))
            return last;
        return 0xFFFFFFFF;
    };
    bool changed = true;
    while (changed) {
        changed = false;
        for (uint32_t i = 0; i < ctx.nodes.size(); ++i) {
            const auto &nd = ctx.nodes[i];
            uint32_t t_count = 0, first_t = 0, v_count = 0, first_v = 0;
            bool is_decl = nd.type == NodeType::LocalDecl;
            if (is_decl) {
                t_count = nd.as.local_decl.ident_count;
                first_t = nd.as.local_decl.first_ident;
                v_count = nd.as.local_decl.value_count;
                first_v = nd.as.local_decl.first_value;
            } else if (nd.type == NodeType::Assignment) {
                t_count = nd.as.assign.target_count;
                first_t = nd.as.assign.first_target;
                v_count = nd.as.assign.value_count;
                first_v = nd.as.assign.first_value;
            } else {
                continue;
            }
            uint32_t owner = owner_of_node(state, i);
            auto cit = cands.find(owner);
            if (cit == cands.end() || cit->second.empty())
                continue;
            std::set<std::string_view> exclude;
            if (is_decl) {
                for (uint32_t k = 0; k < t_count; ++k) {
                    uint32_t idi = ctx.block_statements[first_t + k];
                    if (idi < ctx.nodes.size() && ctx.nodes[idi].type == NodeType::Identifier
                        && !ctx.nodes[idi].as.ident.is_global)
                        exclude.emplace(ctx.nodes[idi].as.ident.name, ctx.nodes[idi].as.ident.length);
                }
            }
            for (uint32_t ti = 0; ti < t_count; ++ti) {
                uint32_t tgt = ctx.block_statements[first_t + ti];
                if (tgt >= ctx.nodes.size() || ctx.nodes[tgt].type != NodeType::Identifier)
                    continue;
                if (ctx.nodes[tgt].as.ident.is_global)
                    continue;
                std::string_view tn(ctx.nodes[tgt].as.ident.name, ctx.nodes[tgt].as.ident.length);
                if (cit->second.count(tn) == 0)
                    continue;
                uint32_t rhs = paired_rhs(nd, first_v, v_count, ti);
                bool ok;
                if (rhs == 0xFFFFFFFF) {
                    ok = true;
                } else if (is_decl) {
                    std::set<std::string_view> scoped = cit->second;
                    for (const auto &e : exclude)
                        scoped.erase(e);
                    ok = nogc_rhs_ok(ctx, state, scoped, rhs, 0);
                } else {
                    ok = nogc_rhs_ok(ctx, state, cit->second, rhs, 0);
                }
                if (!ok) {
                    cit->second.erase(tn);
                    changed = true;
                }
            }
        }
    }
    for (const auto &kv : cands) {
        for (const auto &nm : kv.second)
            state.nogc_locals.emplace(kv.first, nm);
    }
}

}
