-- Regression test: a runtime error inside a statement that spans several lines names the line of the failing operation, like Lua does.
--
-- clx recorded one line per statement (where it starts), so `return` on one line with its expression on a later line, a table constructor whose third field fails, a call whose
-- argument is an indexing of nil, all reported the first line. Generated Lua (YueScript with its line-preserving option puts a keyword and its expression on different lines)
-- read this way: messages pointed 1 to 4 lines away from the YueScript source. Also: nodes the parser never gave a line (postfix `.name` and `[key]` accesses) held
-- uninitialised memory in that field.
--
-- The expected lines are the ones the reference Lua 5.4 reports for this exact file (run it with lua to re-derive them if the file is edited).

local pass, fail = 0, 0
local function check(cond, name, detail)
    if cond then pass = pass + 1; print("[OK] " .. name)
    else fail = fail + 1; print("[FAIL] " .. name .. (detail and (": " .. detail) or "")) end
end
local function line_of(f)
    local ok, err = pcall(f)
    if ok then return nil, "no error" end
    return tonumber(string.match(tostring(err), ":(%d+):")), tostring(err)
end

local t = { v = 1, items = {} }
local nothing = nil

local function ret_split()
    return
        t.v.nope.x            -- line 26
end
local function local_split()
    local s =
        t.v.nope.x            -- line 30
    return s
end
local function table_field()
    local cfg = {
        a = 1,
        b = 2,
        c = nothing.nope,     -- line 37
        d = 4,
    }
    return cfg
end
local function call_arg()
    local function f(...) return ... end
    return f(
        1,
        t.items.q.z,          -- line 46
        3)
end
local function arith()
    local x =
        1 +
        nothing +             -- line 51
        2
    return x
end
local function concat_index()
    local s = "a" ..
        "b" ..
        nothing.name          -- line 59
    return s
end
local function bracket()
    return t
        .items
        [1]
        .field                -- line 66
end
local function call_nil()
    return
        nothing(1, 2)         -- line 70
end
local function same_line()
    return t.v.nope           -- line 73
end

local cases = {
    { ret_split, 26 }, { local_split, 30 }, { table_field, 37 }, { call_arg, 46 }, { arith, 51 },
    { concat_index, 59 }, { bracket, 66 }, { call_nil, 70 }, { same_line, 73 },
}
local names = { "return split from its expression", "local split from its value", "field of a multi-line constructor", "argument of a multi-line call", "operator on a later line",
    "concatenation and index", "chained index over several lines", "call of nil on a later line", "everything on one line" }
for i, c in ipairs(cases) do
    local got, msg = line_of(c[1])
    check(got == c[2], names[i], string.format("expected line %d, got %s (%s)", c[2], tostring(got), msg))
end

print(string.format("error_lines_multiline: %d passed, %d failed", pass, fail))
if fail > 0 then os.exit(1) end
