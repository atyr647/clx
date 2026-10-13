local passed = 0
local failed = 0

local function assert_eq(actual, expected, name)
    if actual == expected then
        passed = passed + 1
        print("[OK]   ", name)
    else
        failed = failed + 1
        print("[FAIL] ", name, "| Expected:", expected, "Got:", tostring(actual))
    end
end

local function count_values(...)
    return select("#", ...)
end

local function print_summary(domain)
    print("-----")
    print("SUITE :", domain)
    print("PASS  :", passed)
    print("FAIL  :", failed)
    print("-----")
end

print("----------------- math.type fail semantics (nil for non-numbers, error on missing arg)")

assert_eq(math.type("str"), nil, "string -> nil")
assert_eq(math.type(""), nil, "empty string -> nil")
assert_eq(math.type(nil), nil, "nil -> nil")
assert_eq(math.type(true), nil, "boolean -> nil")
assert_eq(math.type({}), nil, "table -> nil")
assert_eq(math.type(print), nil, "function -> nil")

assert_eq(math.type(42), "integer", "integer -> 'integer'")
assert_eq(math.type(-7), "integer", "negative integer -> 'integer'")
assert_eq(math.type(4.5), "float", "4.5 -> 'float'")
assert_eq(math.type(3.0), "float", "3.0 -> 'float'")

assert_eq(count_values(math.type("str")), 1, "non-number returns exactly one value")
assert_eq(count_values(math.type(1)), 1, "integer returns exactly one value")
assert_eq(select(1, math.type(nil)), nil, "nil argument yields nil result")

local ok, err = pcall(math.type)
assert_eq(ok, false, "math.type() with no argument raises")
assert_eq(type(err) == "string" and err:find("bad argument #1") ~= nil, true, "error names argument #1")

print_summary("math_type_fail")

if failed > 0 then os.exit(1) end
