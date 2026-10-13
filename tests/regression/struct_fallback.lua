-- Struct local promotion: disqualification cases.
--
-- A record local must fall back to a plain LTable -- with identical semantics --
-- when it cannot be proven safe to promote: dynamic keys, reassignment, mixed
-- field kinds, positional items, capture/escape, length operator, iteration, or
-- an unproven late read (which must yield nil, not a stale member). These pin
-- down that the fallback path keeps stock Lua behavior.

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

local function print_summary(domain)
    print("-----")
    print("SUITE :", domain)
    print("PASS  :", passed)
    print("FAIL  :", failed)
    print("-----")
end

print("----------------- 1. Dynamic keys")

local dyn = { x = 1, y = 2 }
dyn.x = 5
assert_eq(dyn["x"], 5, "bracket read with literal string")
dyn[1] = 9
assert_eq(dyn[1], 9, "integer key write keeps table semantics")

local k = "y"
local dyn2 = { x = 1, y = 2 }
assert_eq(dyn2[k], 2, "dynamic key read")
dyn2[k] = 3
assert_eq(dyn2.y, 3, "dynamic key write")

print("----------------- 2. Reassignment")

local re = { x = 1 }
re.x = 2
re = { x = 9 }
assert_eq(re.x, 9, "local reassigned to a new table")
re.x = re.x + 1
assert_eq(re.x, 10, "field updates after reassignment")

print("----------------- 3. Mixed field kinds")

local mixed = { x = 1 }
mixed.x = "str"
assert_eq(mixed.x, "str", "field changes kind number -> string")
mixed.x = 2.5
assert_eq(mixed.x, 2.5, "field changes kind string -> float")

print("----------------- 4. Positional items and ctor values")

local arr = { 10, 20, x = 30 }
assert_eq(arr[1], 10, "positional item read")
assert_eq(arr.x, 30, "record field beside positional items")

local function get_x(flag)
    if flag then return 42 end
    return "s"
end
local computed = { x = get_x(true) }
assert_eq(computed.x, 42, "ctor value from mixed-return call")

print("----------------- 5. Escape and capture")

local esc = { x = 1 }
local function get_x(t)
    return t.x
end
assert_eq(get_x(esc), 1, "table passed to function")

local cap = { x = 7 }
local function reader()
    return cap.x
end
cap.x = 8
assert_eq(reader(), 8, "captured record local")

print("----------------- 6. Length and iteration")

local sized = { x = 1 }
sized[1] = 5
sized[2] = 6
assert_eq(#sized, 2, "length operator on record-ish table")

local counted = { x = 1, y = 2 }
local n = 0
for k, v in pairs(counted) do
    n = n + 1
end
assert_eq(n, 2, "pairs iteration")

print("----------------- 7. Unproven late read yields nil")

local late = { x = 1 }
local gate = false
if gate then
    late.count = 5
end
assert_eq(late.count, nil, "late read without dominating write is nil")
late.count = 3
assert_eq(late.count, 3, "late field assignable after nil read")

local raw_late = { x = 1 }
if gate then
    raw_late.count = 5
end
assert_eq(rawget(raw_late, "count"), nil, "rawget sees nil too")

print("----------------- 8. Phantom reads")

local phantom = { x = 1 }
assert_eq(phantom.zzz, nil, "read of a never-written field is nil")
assert_eq(phantom.x, 1, "ctor field beside phantom read")
assert_eq(phantom.missing, nil, "second phantom read")

print("----------------- 9. Metatables")

local with_mt = setmetatable({ x = 1 }, { __index = { y = 42 } })
assert_eq(with_mt.x, 1, "metatabled table field")
assert_eq(with_mt.y, 42, "metatabled table falls back to __index")

print_summary("struct_fallback")
if failed > 0 then os.exit(1) end
