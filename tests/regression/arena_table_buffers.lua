-- Regression test: buffers an arena table grows or caches are freed when its function returns.
--
-- A table the compiler keeps in a per-function arena lives in memory that is released in one piece when the function returns. The memory it points to
-- is not part of that block: the inline cache a field read allocates on first use, an array that grew past the arena slots, a hash that was resized.
-- None of them was freed, so a loop calling a function that builds small tables and reads their fields grew the process by about 1 MB per call
-- (1.6 GB after 2000 calls of 2000 iterations) while collectgarbage("count") stayed flat. The check that fails without the fix is the last one (254 MB
-- of growth). The first two exercise the growth paths the fix also changed: a hash resize used to clear the flag that tells the array growth path not to
-- free the arena's own array; no program the compiler currently puts in an arena reaches them, so they pass with and without the fix.

local pass, fail = 0, 0
local function check(cond, name, detail)
    if cond then pass = pass + 1; print("[OK] " .. name)
    else fail = fail + 1; print("[FAIL] " .. name .. (detail and (": " .. detail) or "")) end
end

local function hwm_kb()
    local f = io.open("/proc/self/status")
    if not f then return nil end
    local s = f:read("a"); f:close()
    return tonumber(s:match("VmHWM:%s*(%d+)"))
end

local es = {}
for i = 1, 2000 do es[i] = { x = i * 0.5, y = 0.0, vx = 1.0, vy = 1.0 } end

-- 1. field reads on arena tables: every table gets an inline cache
local function reads(n)
    for i = 1, n do
        local e = es[i]
        local p = { x = e.x, y = e.y }
        local v = { x = e.vx, y = e.vy }
        local q = { x = p.x + v.x, y = p.y + v.y }
        e.x = q.x; e.y = q.y
    end
end

-- 2. one table whose hash and array both outgrow the arena's slots
local function grow(n)
    local acc = 0
    for i = 1, n do
        local t = { a = 1, b = 2 }
        for k = 1, 20 do t["k" .. k] = k end
        for k = 1, 20 do t[k] = k end
        acc = acc + t.k20 + t[20] + t.a + #t
    end
    return acc
end
local function grow_array_first(n)
    local acc = 0
    for i = 1, n do
        local t = { a = 1, b = 2 }
        for k = 1, 20 do t[k] = k end
        for k = 1, 20 do t["k" .. k] = k end
        acc = acc + t.k20 + t[20] + t.a + #t
    end
    return acc
end

check(grow(200) == 200 * (20 + 20 + 1 + 20), "hash resized, then array grown, in one function-local table")
check(grow_array_first(200) == 200 * (20 + 20 + 1 + 20), "array grown, then hash resized")

local before = hwm_kb()
if before then
    for _ = 1, 300 do reads(2000) end
    for _ = 1, 30 do grow(2000); grow_array_first(2000) end
    local after = hwm_kb()
    check(after - before < 100 * 1024, "300 calls building 2000 function-local tables each do not grow the process",
          string.format("%.1f MB", (after - before) / 1024))
else
    print("[SKIP] no /proc/self/status")
end
print(string.format("arena_table_buffers: %d passed, %d failed", pass, fail))
