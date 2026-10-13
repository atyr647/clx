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

local function print_summary(domain)
    print("-----")
    print("SUITE :", domain)
    print("PASS  :", passed)
    print("FAIL  :", failed)
    print("-----")
end

print("----------------- integer subtype: int arrays, string coercion, tonumber")

local t = { 1, 2 }
assert_eq(t[1] + t[2], 3, "int array sum value")
assert_eq(math.type(t[1] + t[2]), "integer", "int array sum stays integer")
assert_eq(math.type(t[1]), "integer", "int array element stays integer")
local u = { 1.5, 2 }
assert_eq(u[1], 1.5, "mixed array keeps float element")
assert_eq(math.type(u[1]), "float", "mixed array element stays float")
assert_eq(u[1] + u[2], 3.5, "mixed array sum value")
assert_eq(math.type(u[1] + u[2]), "float", "float array sum stays float")

assert_eq(("10") + 1, 11, "string+int value")
assert_eq(math.type(("10") + 1), "integer", "string+int stays integer")
assert_eq(("10") + ("20"), 30, "string+string value")
assert_eq(math.type(("10") + ("20")), "integer", "string+string stays integer")
assert_eq(("10") - ("4"), 6, "string-string stays integer")
assert_eq(math.type(("10") * ("2")), "integer", "string*string stays integer")
assert_eq(("2") + 3.5, 5.5, "string+float value")
assert_eq(math.type(("2") + 3.5), "float", "string+float stays float")
assert_eq(-("5"), -5, "unm string value")
assert_eq(math.type(-("5")), "integer", "unm string stays integer")
assert_eq(("10") // ("3"), 3, "string floor-div value")
assert_eq(math.type(("10") // ("3")), "integer", "string floor-div stays integer")
assert_eq(("10") % ("3"), 1, "string modulo value")
assert_eq(math.type(("10") % ("3")), "integer", "string modulo stays integer")
assert_eq(math.type(("1.5") + 1), "float", "decimal string stays float")

local vs = tonumber("10")
assert_eq(vs, 10, "tonumber('10') value")
assert_eq(math.type(vs), "integer", "tonumber('10') integer")
assert_eq(math.type(tonumber("0x10")), "integer", "tonumber('0x10') integer")
assert_eq(tonumber("0x10"), 16, "tonumber('0x10') value")
assert_eq(math.type(tonumber("010")), "integer", "tonumber('010') integer (not octal)")
assert_eq(tonumber("010"), 10, "tonumber('010') value")
assert_eq(tonumber(" 10 "), 10, "tonumber strips spaces")
assert_eq(tonumber("10 "), 10, "tonumber strips trailing space")
assert_eq(tonumber("+10"), 10, "tonumber leading plus")
assert_eq(tonumber("-10"), -10, "tonumber leading minus")
assert_eq(math.type(tonumber("10.")), "float", "tonumber('10.') float")
assert_eq(math.type(tonumber(".5")), "float", "tonumber('.5') float")
assert_eq(math.type(tonumber("1e2")), "float", "tonumber('1e2') float")
assert_eq(tonumber("0x"), nil, "tonumber('0x') nil")
assert_eq(tonumber("0b101"), nil, "tonumber rejects binary literal")
assert_eq(tonumber("1_000"), nil, "tonumber rejects separators")
assert_eq(tonumber("0xffffffffffffffff"), -1, "tonumber hex wraps like stock")
assert_eq(math.type(tonumber("0xffffffffffffffff")), "integer", "wrapped hex is integer")
assert_eq(tonumber("9223372036854775807"), math.maxinteger, "tonumber maxinteger exact")
assert_eq(math.type(tonumber("9223372036854775808")), "float", "overflow decimal is float")
assert_eq(tonumber("-9223372036854775808"), math.mininteger, "tonumber mininteger exact")
assert_eq(math.type(tonumber("-9223372036854775808")), "integer", "mininteger stays integer")

assert_eq(("10") == 10, false, "no coercion in equality")
assert_eq(pcall(function() return 10 < "11 " end), false, "no coercion in ordered comparison")
assert_eq(pcall(function() return "10" < 11 end), false, "no coercion in ordered comparison (reversed)")
assert_eq(-("-9223372036854775808") == math.mininteger, true, "unm wrap on string mininteger")

print_summary("int_subtype_coercion")

if failed > 0 then os.exit(1) end
