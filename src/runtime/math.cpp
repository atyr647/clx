// ┌─────────────────────────────────────────────┐
// │  clx — Lua to C++ Native Compiler           │
// │  Copyright (c) 2026 Tine Samir. MIT License.│
// ├─────────────────────────────────────────────┤
// │  math.cpp · Math Library                    │
// └─────────────────────────────────────────────┘

#include "clx.h"
#include <cmath>
#include <random>

namespace clx {

//------------------ rng: returns thread-local 64-bit Mersenne Twister RNG
static std::mt19937_64 &rng() {
    static std::mt19937_64 gen(std::random_device { }());
    return gen;
}

//------------------ math_abs: returns absolute value of a number
MultiValue math_abs(LState *L, const LValue *args, size_t count) {
    if (count == 0)
        clx::error(L, bad_arg_msg(L, 1, "math.abs", "number expected, got no value").c_str());
    if (clx::is_integer(args[0])) {
        int64_t n;
        clx::to_integer(args[0], n);
        if (n < 0)
            n = static_cast<int64_t>(0u - static_cast<uint64_t>(n));
        return MultiValue(clx::integer(n));
    }
    return MultiValue(clx::number(std::fabs(clx::check_number(L, args[0]))));
}

//------------------ math_acos: returns arc cosine of a number
MultiValue math_acos(LState *L, const LValue *args, size_t count) {
    if (count == 0)
        clx::error(L, bad_arg_msg(L, 1, "math.acos", "number expected, got no value").c_str());
    return MultiValue(clx::number(CLX_MATH_NS::acos(clx::check_number(L, args[0]))));
}

//------------------ math_asin: returns arc sine of a number
MultiValue math_asin(LState *L, const LValue *args, size_t count) {
    if (count == 0)
        clx::error(L, bad_arg_msg(L, 1, "math.asin", "number expected, got no value").c_str());
    return MultiValue(clx::number(CLX_MATH_NS::asin(clx::check_number(L, args[0]))));
}

//------------------ math_atan: returns arc tangent (or atan2 if 2 args)
MultiValue math_atan(LState *L, const LValue *args, size_t count) {
    if (count == 0)
        clx::error(L, bad_arg_msg(L, 1, "math.atan", "number expected, got no value").c_str());
    double y = clx::check_number(L, args[0]);
    if (count > 1) {
        double x = clx::check_number(L, args[1]);
        return MultiValue(clx::number(CLX_MATH_NS::atan2(y, x)));
    }
    return MultiValue(clx::number(CLX_MATH_NS::atan(y)));
}

//------------------ math_ceil: returns smallest integer >= x
MultiValue math_ceil(LState *L, const LValue *args, size_t count) {
    if (count == 0)
        clx::error(L, bad_arg_msg(L, 1, "math.ceil", "number expected, got no value").c_str());
    if (clx::is_integer(args[0]))
        return MultiValue(args[0]);
    double d = std::ceil(clx::check_number(L, args[0]));
    int64_t n;
    if (clx::flt_to_integer(d, n))
        return MultiValue(clx::integer(n));
    return MultiValue(clx::number(d));
}

//------------------ math_cos: returns cosine of a number
MultiValue math_cos(LState *L, const LValue *args, size_t count) {
    if (count == 0)
        clx::error(L, bad_arg_msg(L, 1, "math.cos", "number expected, got no value").c_str());
    return MultiValue(clx::number(CLX_MATH_NS::cos(clx::check_number(L, args[0]))));
}

//------------------ math_deg: converts radians to degrees
MultiValue math_deg(LState *L, const LValue *args, size_t count) {
    if (count == 0)
        clx::error(L, bad_arg_msg(L, 1, "math.deg", "number expected, got no value").c_str());
    return MultiValue(clx::number(clx::check_number(L, args[0]) * (180.0 / 3.14159265358979323846)));
}

//------------------ math_exp: returns e^x
MultiValue math_exp(LState *L, const LValue *args, size_t count) {
    if (count == 0)
        clx::error(L, bad_arg_msg(L, 1, "math.exp", "number expected, got no value").c_str());
    return MultiValue(clx::number(CLX_MATH_NS::exp(clx::check_number(L, args[0]))));
}

//------------------ math_floor: returns largest integer <= x
MultiValue math_floor(LState *L, const LValue *args, size_t count) {
    if (count == 0)
        clx::error(L, bad_arg_msg(L, 1, "math.floor", "number expected, got no value").c_str());
    if (clx::is_integer(args[0]))
        return MultiValue(args[0]);
    double d = std::floor(clx::check_number(L, args[0]));
    int64_t n;
    if (clx::flt_to_integer(d, n))
        return MultiValue(clx::integer(n));
    return MultiValue(clx::number(d));
}

//------------------ math_fmod: returns remainder of x/y
MultiValue math_fmod(LState *L, const LValue *args, size_t count) {
    if (count < 2)
        clx::error(L, bad_arg_msg(L, 2, "math.fmod", "number expected, got no value").c_str());
    if (clx::is_integer(args[0]) && clx::is_integer(args[1])) {
        int64_t d;
        clx::to_integer(args[1], d);
        if (static_cast<uint64_t>(d) + 1u <= 1u) {
            if (d == 0)
                clx::error(L, bad_arg_msg(L, 2, "math.fmod", "zero").c_str());
            return MultiValue(clx::integer(0));
        }
        int64_t a;
        clx::to_integer(args[0], a);
        return MultiValue(clx::integer(a % d));
    }
    return MultiValue(clx::number(std::fmod(clx::check_number(L, args[0]), clx::check_number(L, args[1]))));
}

//------------------ math_max: returns the maximum of its arguments
MultiValue math_max(LState *L, const LValue *args, size_t count) {
    if (count == 0)
        clx::error(L, bad_arg_msg(L, 1, "math.max", "number expected, got no value").c_str());
    LValue m = args[0];
    for (size_t i = 1; i < count; ++i)
        if ((m < args[i]).as_bool())
            m = args[i];
    return MultiValue(m);
}

//------------------ math_min: returns the minimum of its arguments
MultiValue math_min(LState *L, const LValue *args, size_t count) {
    if (count == 0)
        clx::error(L, bad_arg_msg(L, 1, "math.min", "number expected, got no value").c_str());
    LValue m = args[0];
    for (size_t i = 1; i < count; ++i)
        if ((args[i] < m).as_bool())
            m = args[i];
    return MultiValue(m);
}

//------------------ math_rad: converts degrees to radians
MultiValue math_rad(LState *L, const LValue *args, size_t count) {
    if (count == 0)
        clx::error(L, bad_arg_msg(L, 1, "math.rad", "number expected, got no value").c_str());
    return MultiValue(clx::number(clx::check_number(L, args[0]) * (3.14159265358979323846 / 180.0)));
}

//------------------ math_sin: returns sine of a number
MultiValue math_sin(LState *L, const LValue *args, size_t count) {
    if (count == 0)
        clx::error(L, bad_arg_msg(L, 1, "math.sin", "number expected, got no value").c_str());
    return MultiValue(clx::number(CLX_MATH_NS::sin(clx::check_number(L, args[0]))));
}

//------------------ math_sqrt: returns square root of a number
MultiValue math_sqrt(LState *L, const LValue *args, size_t count) {
    if (count == 0)
        clx::error(L, bad_arg_msg(L, 1, "math.sqrt", "number expected, got no value").c_str());
    return MultiValue(clx::number(std::sqrt(clx::check_number(L, args[0]))));
}

//------------------ math_tan: returns tangent of a number
MultiValue math_tan(LState *L, const LValue *args, size_t count) {
    if (count == 0)
        clx::error(L, bad_arg_msg(L, 1, "math.tan", "number expected, got no value").c_str());
    return MultiValue(clx::number(CLX_MATH_NS::tan(clx::check_number(L, args[0]))));
}

//------------------ math_frexp: breaks float into mantissa and exponent
MultiValue math_frexp(LState *L, const LValue *args, size_t count) {
    if (count == 0)
        clx::error(L, bad_arg_msg(L, 1, "math.frexp", "number expected, got no value").c_str());
    double x = clx::check_number(L, args[0]);
    int e;
    double m = std::frexp(x, &e);
    return MultiValue({ clx::number(m), clx::integer(e) });
}

//------------------ math_ldexp: computes m * 2^e
MultiValue math_ldexp(LState *L, const LValue *args, size_t count) {
    if (count < 2)
        clx::error(L, bad_arg_msg(L, 2, "math.ldexp", "number expected, got no value").c_str());
    double m = clx::check_number(L, args[0]);
    int64_t e = clx::check_integer(L, args[1]);
    return MultiValue(clx::number(std::ldexp(m, static_cast<int>(e))));
}

//------------------ math_log: returns log(x) or log(x, base)
MultiValue math_log(LState *L, const LValue *args, size_t count) {
    if (count == 0)
        clx::error(L, bad_arg_msg(L, 1, "math.log", "number expected, got no value").c_str());
    double x = clx::check_number(L, args[0]);
    if (count > 1 && args[1].type != ValueType::Nil) {
        double base = clx::check_number(L, args[1]);
        return MultiValue(clx::number(CLX_MATH_NS::log(x) / CLX_MATH_NS::log(base)));
    }
    return MultiValue(clx::number(CLX_MATH_NS::log(x)));
}

//------------------ math_modf: splits number into integer and fractional parts
MultiValue math_modf(LState *L, const LValue *args, size_t count) {
    if (count == 0)
        clx::error(L, bad_arg_msg(L, 1, "math.modf", "number expected, got no value").c_str());
    if (clx::is_integer(args[0]))
        return MultiValue({ args[0], clx::number(0.0) });
    double x = clx::check_number(L, args[0]);
    double intpart;
    double fracpart = std::modf(x, &intpart);
    return MultiValue({ clx::number(intpart), clx::number(fracpart) });
}

//------------------ math_tointeger: converts number to integer if possible, else single nil
MultiValue math_tointeger(LState *L, const LValue *args, size_t count) {
    if (count == 0)
        clx::error(L, bad_arg_msg(L, 1, "math.tointeger", "value expected").c_str());
    int64_t i;
    if (clx::to_integer(args[0], i))
        return MultiValue(clx::integer(i));
    return MultiValue(LValue());
}

//------------------ math_random: returns pseudo-random number
MultiValue math_random(LState *L, const LValue *args, size_t count) {
    auto &gen = rng();
    if (count == 0) {
        std::uniform_real_distribution<double> dist(0.0, 1.0);
        return MultiValue(clx::number(dist(gen)));
    }
    int64_t m = clx::check_integer(L, args[0]);
    if (count == 1) {
        if (m == 0) {
            std::uniform_int_distribution<int64_t> dist(
                std::numeric_limits<int64_t>::min(), std::numeric_limits<int64_t>::max());
            return MultiValue(clx::integer(dist(gen)));
        }
        if (m > 0) {
            std::uniform_int_distribution<int64_t> dist(1, m);
            return MultiValue(clx::integer(dist(gen)));
        }
        clx::error(L, bad_arg_msg(L, 1, "math.random", "interval is empty").c_str());
    }
    int64_t n = clx::check_integer(L, args[1]);
    if (m > n)
        clx::error(L, bad_arg_msg(L, 2, "math.random", "interval is empty").c_str());
    std::uniform_int_distribution<int64_t> dist(m, n);
    return MultiValue(clx::integer(dist(gen)));
}

//------------------ math_randomseed: seeds the random number generator
MultiValue math_randomseed(LState *L, const LValue *args, size_t count) {
    auto &gen = rng();
    if (count == 0) {
        std::random_device rd;
        uint64_t seed = (static_cast<uint64_t>(rd()) << 32) ^ rd();
        gen.seed(seed);
        return MultiValue({ clx::integer(static_cast<int64_t>(seed >> 32)),
            clx::integer(static_cast<int64_t>(seed & 0xFFFFFFFFULL)) });
    }
    int64_t x = clx::check_integer(L, args[0]);
    int64_t y = (count > 1) ? clx::check_integer(L, args[1]) : 0;
    uint64_t seed = (static_cast<uint64_t>(static_cast<uint32_t>(x)) << 32) | static_cast<uint32_t>(y);
    gen.seed(seed);
    return MultiValue({ clx::integer(x), clx::integer(y) });
}

//------------------ math_type: returns "integer"/"float" for numbers, nil for any other value (1 value, never 0)
MultiValue math_type(LState *L, const LValue *args, size_t count) {
    if (count == 0)
        clx::error(L, bad_arg_msg(L, 1, "math.type", "value expected").c_str());
    ValueType t = args[0].type;
    if (t == Int64)
        return MultiValue(LValue(L->intern_string("integer")));
    if (t == Double)
        return MultiValue(LValue(L->intern_string("float")));
    return MultiValue(LValue());
}

//------------------ math_ult: unsigned less-than comparison of integers
MultiValue math_ult(LState *L, const LValue *args, size_t count) {
    if (count < 2)
        clx::error(L, bad_arg_msg(L, 2, "math.ult", "number expected, got no value").c_str());
    uint64_t a = static_cast<uint64_t>(clx::check_integer(L, args[0]));
    uint64_t b = static_cast<uint64_t>(clx::check_integer(L, args[1]));
    return MultiValue(LValue(a < b));
}

//------------------ math_funcs: lazy registration table for math library functions
static constexpr LazyReg math_funcs[] = {
    { "abs", math_abs },
    { "acos", math_acos },
    { "asin", math_asin },
    { "atan", math_atan },
    { "ceil", math_ceil },
    { "cos", math_cos },
    { "deg", math_deg },
    { "exp", math_exp },
    { "floor", math_floor },
    { "fmod", math_fmod },
    { "max", math_max },
    { "min", math_min },
    { "rad", math_rad },
    { "sin", math_sin },
    { "sqrt", math_sqrt },
    { "tan", math_tan },
    { "tointeger", math_tointeger },
    { "frexp", math_frexp },
    { "ldexp", math_ldexp },
    { "log", math_log },
    { "modf", math_modf },
    { "random", math_random },
    { "randomseed", math_randomseed },
    { "type", math_type },
    { "ult", math_ult },
};

//------------------ luastd_math: registers the math library into the global state
void luastd_math(LState *L) {
    LValue math_table = clx::table(L);
    clx::set_value(L, math_table, "pi", clx::number(3.14159265358979323846));
    clx::set_value(L, math_table, "huge", clx::number(INFINITY));
    clx::set_value(L, math_table, "maxinteger", clx::integer(9223372036854775807LL));
    clx::set_value(L, math_table, "mininteger", clx::integer(-9223372036854775807LL - 1));
    clx::set_lazy_funcs(L, math_table, math_funcs, 25);
    clx::set_global(L, "math", math_table);
}

}
