-- ─────────────────────────────────────────────
-- │ clx — Lua to C++ Native Compiler           │
-- │  string_dump.lua · conformance test        │
-- ─────────────────────────────────────────────
--
-- AOT-compiled functions have no bytecode, so string.dump cannot serialize
-- them. Instead it returns a callable snapshot (a function proxy): calling
-- it runs the original. Under --dynamic, functions created by load() live
-- on the embedded VM, so dumping them returns real stock-format bytecode
-- that load() accepts back. Non-function arguments raise stock's error.
--
-- The VM round-trip section only runs when load() exists (--dynamic).

local total = 0
local passed = 0
local function assert_eq(actual, expected, name)
    total = total + 1
    if actual == expected then
        passed = passed + 1
        print("[OK] " .. name)
    else
        print(string.format("[FAIL] %s — expected %s, got %s",
            name, tostring(expected), tostring(actual)))
    end
end

local function assert_true(cond, name)
    total = total + 1
    if cond then
        passed = passed + 1
        print("[OK] " .. name)
    else
        print("[FAIL] " .. name)
    end
end

-- 1. Dumping an AOT closure returns a callable snapshot, not a string.
local function add(x, y) return x + y end
local dadd = string.dump(add)
assert_eq(type(dadd), "function", "dump of AOT closure returns a function")
assert_eq(dadd(3, 4), 7, "snapshot calls the original")

-- 2. Snapshots keep upvalues alive and shared with the original.
local count = 0
local function bump() count = count + 1 return count end
local dbump = string.dump(bump)
assert_eq(dbump(), 1, "snapshot sees upvalues (1st call)")
assert_eq(bump(), 2, "original shares state with snapshot")
assert_eq(dbump(), 3, "snapshot sees upvalues (2nd call)")

-- 3. Multiple returns survive the snapshot call.
local function multi() return 1, 2, 3 end
local a, b, c = string.dump(multi)()
assert_eq(a, 1, "snapshot multi-return a")
assert_eq(b, 2, "snapshot multi-return b")
assert_eq(c, 3, "snapshot multi-return c")

-- 4. The strip argument is accepted; the snapshot still works.
local dstrip = string.dump(add, true)
assert_eq(type(dstrip), "function", "dump with strip returns a function")
assert_eq(dstrip(10, 20), 30, "stripped snapshot still calls through")

-- 5. Dumping a C function returns a callable snapshot too.
local dprint = string.dump(print)
assert_eq(type(dprint), "function", "dump of C function returns a function")
local ok_print = pcall(dprint, "snapshot-print-probe")
assert_true(ok_print, "C-function snapshot is callable")

-- 6. Non-function arguments raise stock's error, verbatim.
local stock_msg = "bad argument #1 to 'string.dump' (Lua function expected)"
local cases = { { t = {} }, { s = "x" }, { n = 42 }, { b = true } }
for _, v in ipairs(cases) do
    local _, val = next(v)
    local ok, err = pcall(string.dump, val)
    assert_true(not ok, "dump rejects " .. type(val))
    assert_eq(err, stock_msg, "dump error text for " .. type(val))
end
do
    local ok, err = pcall(string.dump)
    assert_true(not ok, "dump rejects missing argument")
    assert_eq(err, stock_msg, "dump error text for missing argument")
    local okn, errn = pcall(string.dump, nil)
    assert_true(not okn, "dump rejects nil")
    assert_eq(errn, stock_msg, "dump error text for nil")
end

-- 7. VM round trip (only when load() exists, i.e. --dynamic builds).
if type(load) == "function" then
    local f = load("return function(x) return x * 2 + 1 end")()
    local dumped = string.dump(f)
    assert_eq(type(dumped), "string", "dump of VM function returns a string")
    local g = load(dumped)
    assert_eq(type(g), "function", "dumped bytecode reloads")
    assert_eq(g(20), 41, "reloaded bytecode runs correctly")

    local stripped = string.dump(f, true)
    local gs = load(stripped)
    assert_eq(gs(20), 41, "stripped dump reloads and runs")

    local gn, gerr = load(dumped, nil, "t")
    assert_eq(gn, nil, "binary chunk rejected in text mode")
    assert_true(type(gerr) == "string", "text-mode rejection reports an error")

    local _, cerr = load(" @@ not lua @@ ")
    assert_true(type(cerr) == "string" and cerr:find("@@ not lua @@", 1, true) ~= nil,
        "load chunkname defaults to the source string")
else
    total = total + 1; passed = passed + 1
    print("[OK] VM round trip skipped (no load(); rebuild with --dynamic)")
end

print(string.format("\nstring.dump conformance: %d/%d passed", passed, total))
if passed ~= total then os.exit(1) end
