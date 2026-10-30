// ┌─────────────────────────────────────────────┐
// │  clx — Lua to C++ Native Compiler           │
// │  Copyright (c) 2026 Tine Samir. MIT License.│
// ├─────────────────────────────────────────────┤
// │  builtins.h · Std module builtin registry   │
// └─────────────────────────────────────────────┘

#ifndef CLX_BUILTINS_H
#define CLX_BUILTINS_H

#include <cstdint>
#include <deque>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace clx {

//------------------ BuiltinEntry: statically known std module function (module.name), the single
//------------------ source of truth for parser intrinsic rewrite, alias registration, direct-call
//------------------ lowering and hoisted cfunc selection. intrinsic = raw C++ expression name for
//------------------ unboxed lowering (nullptr when none); cfunc = clx:: runtime symbol (nullptr
//------------------ when absent from the runtime table); min_args = arity floor for intrinsic
//------------------ lowering (below it the call keeps generic dispatch so runtime validates);
//------------------ alias_ok = the module table actually contains this function as a value;
//------------------ numeric = the call result is always a number (native-double contexts).
struct BuiltinEntry {
    std::string_view module;
    std::string_view name;
    const char *intrinsic;
    const char *cfunc;
    uint8_t min_args;
    bool alias_ok;
    bool numeric;
};

//------------------ BuiltinConst: foldable std module constant field (math.huge etc.); boxed =
//------------------ full LValue C++ expression, native = raw numeric expression for double contexts.
struct BuiltinConst {
    std::string_view module;
    std::string_view name;
    const char *boxed;
    const char *native;
    bool is_int;
};

//------------------ NativeEntry: an engine function or value declared by a natives manifest (clx --natives FILE). A call
//------------------ module.name(args) is lowered to the C function cname with unboxed arguments, through the same intrinsic
//------------------ path as math.sqrt; a getter module.name reads cname() where a number is expected. Signatures use the
//------------------ types void (return only), double, int64 (int64_t) and int32 (C int). An integer argument that is not an integral
//------------------ number raises Lua's "number has no integer representation"; an int32 argument outside the int range raises
//------------------ "integer out of range" instead of being truncated. Manifest lines (# starts a comment):
//------------------   fn  engine.set_xy       engine_set_xy       void   int32 double double
//------------------   get engine.dt           engine_dt           double
//------------------ The host must keep these fields fixed at run time (the engine module table should refuse writes), since a
//------------------ lowered call no longer looks the field up; a write to the field anywhere in the program disables lowering.
struct NativeEntry {
    std::string module, name, cname, ret;
    std::vector<std::string> params;
    bool getter = false;
    std::string intrinsic, boxed, native_expr;
    BuiltinEntry be {};
    BuiltinConst bc {};
};
inline std::deque<NativeEntry> &native_registry() {
    static std::deque<NativeEntry> r;   // deque: entries never move, so the string_views in be/bc stay valid
    return r;
}
inline const NativeEntry *native_by_intrinsic(std::string_view cname) {
    for (const auto &e : native_registry())
        if (!e.getter && e.intrinsic == cname)
            return &e;
    return nullptr;
}
inline bool is_native_module_name(std::string_view m) {
    for (const auto &e : native_registry())
        if (e.module == m)
            return true;
    return false;
}
//------------------ load_natives_manifest: parses FILE into the registry; returns an error message, empty on success
inline std::string load_natives_manifest(const std::string &path) {
    std::ifstream f(path);
    if (!f)
        return "cannot open natives manifest " + path;
    std::string line;
    int ln = 0;
    auto type_ok = [](const std::string &t, bool ret) { return t == "double" || t == "int64" || t == "int32" || (ret && t == "void"); };
    while (std::getline(f, line)) {
        ++ln;
        if (auto h = line.find('#'); h != std::string::npos)
            line.erase(h);
        std::istringstream ss(line);
        std::string kind, qual, cname, ret;
        if (!(ss >> kind))
            continue;
        if (!(ss >> qual >> cname >> ret))
            return path + ":" + std::to_string(ln) + ": expected '<fn|get> module.name cname type...'";
        auto dot = qual.find('.');
        if (dot == std::string::npos || dot == 0 || dot + 1 == qual.size())
            return path + ":" + std::to_string(ln) + ": '" + qual + "' is not module.name";
        NativeEntry e;
        e.module = qual.substr(0, dot);
        e.name = qual.substr(dot + 1);
        e.cname = cname;
        e.ret = ret;
        if (kind == "fn") {
            if (!type_ok(ret, true))
                return path + ":" + std::to_string(ln) + ": return type must be void, double, int64 or int32";
            std::string p;
            while (ss >> p) {
                if (!type_ok(p, false))
                    return path + ":" + std::to_string(ln) + ": parameter type must be double, int64 or int32";
                e.params.push_back(p);
            }
        } else if (kind == "get") {
            if (ret != "double")
                return path + ":" + std::to_string(ln) + ": a getter's type must be double";
            e.getter = true;
        } else {
            return path + ":" + std::to_string(ln) + ": unknown kind '" + kind + "' (fn or get)";
        }
        native_registry().push_back(std::move(e));
        NativeEntry &r = native_registry().back();
        if (r.getter) {
            r.native_expr = "static_cast<double>(" + r.cname + "())";
            r.boxed = "clx::LValue(" + r.native_expr + ")";
            r.bc = BuiltinConst { r.module, r.name, r.boxed.c_str(), r.native_expr.c_str(), false };
        } else {
            r.intrinsic = "__native_" + r.cname;
            //------------------ int results stay boxed (numeric=false): a double context would lose the integer subtype
            r.be = BuiltinEntry { r.module, r.name, r.intrinsic.c_str(), nullptr, static_cast<uint8_t>(r.params.size()), true,
                r.ret == "double" };
        }
    }
    return { };
}

//------------------ builtin_lookup: registry entry for module.name, else nullptr
inline const BuiltinEntry *builtin_lookup(std::string_view module, std::string_view name) {
    static constexpr BuiltinEntry entries[] = {
        //------------------ math (intrinsics, 2-arg list, extension-only entries)
        { "math", "sin", "std::sin", "math_sin", 1, true, true },
        { "math", "cos", "std::cos", "math_cos", 1, true, true },
        { "math", "sqrt", "std::sqrt", "math_sqrt", 1, true, true },
        { "math", "exp", "std::exp", "math_exp", 1, true, true },
        { "math", "tan", "std::tan", "math_tan", 1, true, true },
        { "math", "atan", "std::atan", "math_atan", 1, true, true },
        { "math", "asin", "std::asin", "math_asin", 1, true, true },
        { "math", "acos", "std::acos", "math_acos", 1, true, true },
        { "math", "sinh", "std::sinh", nullptr, 1, false, true },
        { "math", "cosh", "std::cosh", nullptr, 1, false, true },
        { "math", "tanh", "std::tanh", nullptr, 1, false, true },
        { "math", "atan2", "std::atan2", nullptr, 2, false, true },
        { "math", "pow", "std::pow", nullptr, 2, false, true },
        { "math", "fmod", nullptr, "math_fmod", 2, true, true },
        { "math", "deg", "__clx_deg", "math_deg", 1, true, true },
        { "math", "rad", "__clx_rad", "math_rad", 1, true, true },
        //------------------ math (cfunc only)
        { "math", "floor", nullptr, "math_floor", 1, true, true },
        { "math", "ceil", nullptr, "math_ceil", 1, true, true },
        { "math", "abs", nullptr, "math_abs", 1, true, true },
        { "math", "log", nullptr, "math_log", 1, true, true },
        { "math", "max", nullptr, "math_max", 1, true, true },
        { "math", "min", nullptr, "math_min", 1, true, true },
        { "math", "ldexp", nullptr, "math_ldexp", 1, true, false },
        { "math", "tointeger", nullptr, "math_tointeger", 1, true, false },
        { "math", "frexp", nullptr, "math_frexp", 1, true, false },
        { "math", "modf", nullptr, "math_modf", 1, true, false },
        { "math", "random", nullptr, "math_random", 1, true, false },
        { "math", "randomseed", nullptr, "math_randomseed", 1, true, false },
        { "math", "type", nullptr, "math_type", 1, true, false },
        { "math", "ult", nullptr, "math_ult", 2, true, false },
        //------------------ string (cfunc only)
        { "string", "byte", nullptr, "str_byte", 1, true, false },
        { "string", "sub", nullptr, "str_sub", 1, true, false },
        { "string", "match", nullptr, "str_match", 1, true, false },
        { "string", "find", nullptr, "str_find", 1, true, false },
        { "string", "gsub", nullptr, "str_gsub", 1, true, false },
        { "string", "gmatch", nullptr, "str_gmatch", 1, true, false },
        { "string", "len", nullptr, "str_len", 1, true, false },
        { "string", "format", nullptr, "str_format", 1, true, false },
        { "string", "char", nullptr, "str_char", 1, true, false },
        { "string", "rep", nullptr, "str_rep", 2, true, false },
        { "string", "reverse", nullptr, "str_reverse", 1, true, false },
        { "string", "lower", nullptr, "str_lower", 1, true, false },
        { "string", "upper", nullptr, "str_upper", 1, true, false },
        { "string", "dump", nullptr, "str_dump", 1, true, false },
        { "string", "pack", nullptr, "str_pack", 1, true, false },
        { "string", "packsize", nullptr, "str_packsize", 1, true, false },
        { "string", "unpack", nullptr, "str_unpack", 1, true, false },
        //------------------ os (cfunc only)
        { "os", "clock", nullptr, "os_clock", 0, true, true },
        { "os", "date", nullptr, "os_date", 0, true, false },
        { "os", "difftime", nullptr, "os_difftime", 2, true, true },
        { "os", "execute", nullptr, "os_execute", 0, true, false },
        { "os", "exit", nullptr, "os_exit", 0, true, false },
        { "os", "getenv", nullptr, "os_getenv", 1, true, false },
        { "os", "remove", nullptr, "os_remove", 1, true, false },
        { "os", "rename", nullptr, "os_rename", 2, true, false },
        { "os", "setlocale", nullptr, "os_setlocale", 0, true, false },
        { "os", "time", nullptr, "os_time", 0, true, true },
        { "os", "tmpname", nullptr, "os_tmpname", 0, true, false },
        //------------------ table (cfunc only)
        { "table", "concat", nullptr, "table_concat", 1, true, false },
        { "table", "insert", nullptr, "table_insert", 2, true, false },
        { "table", "remove", nullptr, "table_remove", 1, true, false },
        { "table", "sort", nullptr, "table_sort", 1, true, false },
        { "table", "pack", nullptr, "table_pack", 1, true, false },
        { "table", "unpack", nullptr, "table_unpack", 1, true, false },
        { "table", "move", nullptr, "table_move", 5, true, false },
        { "table", "create", nullptr, "table_create", 0, true, false },
        //------------------ _G (intrinsic only — no clx:: CFunc symbols exist)
        { "_G", "type", "__clx_type", nullptr, 1, true, false },
        { "_G", "tostring", "__clx_tostring", nullptr, 1, true, false },
    };
    for (const auto &e : entries) {
        if (e.module == module && e.name == name)
            return &e;
    }
    for (const auto &e : native_registry())
        if (!e.getter && e.module == module && e.name == name)
            return &e.be;
    return nullptr;
}

//------------------ builtin_const_lookup: registry constant for module.name, else nullptr
inline const BuiltinConst *builtin_const_lookup(std::string_view module, std::string_view name) {
    static constexpr BuiltinConst consts[] = {
        { "math", "huge", "clx::LValue(static_cast<double>(INFINITY))", "INFINITY", false },
        { "math", "pi", "clx::LValue(static_cast<double>(3.141592653589793))", "3.141592653589793", false },
        { "math", "maxinteger", "clx::integer(static_cast<int64_t>(9223372036854775807))",
            "static_cast<int64_t>(9223372036854775807)", true },
        { "math", "mininteger", "clx::integer(static_cast<int64_t>(-9223372036854775807LL - 1))",
            "-9223372036854775807LL - 1", true },
    };
    for (const auto &c : consts) {
        if (c.module == module && c.name == name)
            return &c;
    }
    for (const auto &e : native_registry())
        if (e.getter && e.module == module && e.name == name)
            return &e.bc;
    return nullptr;
}

//------------------ kStdModules: std module globals known to the registry (write-scan coverage)
inline constexpr std::string_view kStdModules[] = { "math", "string", "table", "os", "_G" };

//------------------ is_std_module_name: name is a std module known to the registry
inline bool is_std_module_name(std::string_view name) {
    for (const auto &m : kStdModules) {
        if (m == name)
            return true;
    }
    if (is_native_module_name(name))   // natives modules get the same write scan: a write to module.field disables lowering
        return true;
    return false;
}

}

#endif
