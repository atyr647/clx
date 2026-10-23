-- Cross-TU struct shape-name collision entry point.
-- struct_odr_a emits `struct S_a_d_b_s { double a; const LValue* b; }` while
-- struct_odr_b emits `struct S_a_d_b_s { const LValue* a_d_b; }`. Both live in
-- anonymous namespaces (internal linkage), so each TU keeps its own type and
-- the link is clean. Fails at link time if the structs ever gain external
-- linkage, and fails at runtime if a TU reads the other's layout.
local passed = 0
local failed = 0

local function assert_eq(actual, expected, name)
    if actual == expected then
        passed = passed + 1
        print("[OK]   ", name)
    else
        failed = failed + 1
        print("[FAIL] ", name, "| Expected:", expected, "Got:", actual)
    end
end

local a = require("struct_odr_a")
local b = require("struct_odr_b")
assert_eq(a, "2.5x!", "module A struct value across TUs")
assert_eq(b, "y!", "module B struct value across TUs")

print("-----")
print("SUITE :", "STRUCT_ODR")
print("PASS  :", passed)
print("FAIL  :", failed)
print("-----")
