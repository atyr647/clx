-- Regression test: a closure that is only called and dropped is not promoted to the old generation.
--
-- `local f = function ... end` makes a cell for f (so the closure can call itself) and stored the closure into it through the generational write barrier. The barrier
-- cannot tell that this cell is brand new and that no older object holds it, so it put the cell in the remembered set, and the next minor collection treated the cell
-- as a root: every closure created since the last minor was kept alive, promoted, and left to a full collection. A loop creating a few dozen callbacks per frame
-- filled the old generation with dead closures (21 MB after 90,000 closures with a 128 KB young generation, nothing freed by minors) and made each minor trace thousands
-- of them (a pinned 128 KB young generation was 13 times slower on an event queue that creates a closure per event).

local pass, fail = 0, 0
local function check(cond, name, detail)
    if cond then pass = pass + 1; print("[OK] " .. name)
    else fail = fail + 1; print("[FAIL] " .. name .. (detail and (": " .. detail) or "")) end
end

collectgarbage("param", "youngkb", 128)
local sink, tbl = 0, { n = 0 }
local before = collectgarbage("stats")
for _ = 1, 1500 do
    for i = 1, 60 do
        local calls = function() sink = sink + 1 end
        calls()
        local reads = function() return tbl.n + i end
        sink = sink + reads()
    end
end
local after = collectgarbage("stats")
check(sink > 0 and after.minors - before.minors >= 5, "the loop runs many minor collections", tostring(after.minors - before.minors))
check(after.old_kb < 2048, "dead closures were not promoted to the old generation", string.format("%.0f KB old", after.old_kb))
check(after.remembered_high < 1000, "the remembered set stays small", tostring(after.remembered_high))

-- a closure that does survive keeps working, including after being promoted
local keep = {}
for i = 1, 200 do
    local counter = 0
    local f = function() counter = counter + i; return counter end
    keep[i] = f
end
for _ = 1, 50 do collectgarbage("minor") end
local total = 0
for i = 1, 200 do total = total + keep[i]() + keep[i]() end
check(total == 3 * 200 * 201 // 2, "closures kept across several minor collections still see their own variables", tostring(total))

-- recursion through the closure's own variable
local function fib(n) if n < 2 then return n end return fib(n - 1) + fib(n - 2) end
local fibs = {}
for i = 1, 100 do local g = function(n) if n < 2 then return n end return fibs[1](n - 1) + fibs[1](n - 2) end; fibs[1] = g; assert(g(15) == 610) end
check(fib(20) == 6765, "a local function calling itself works")
for _ = 1, 20 do collectgarbage("minor") end
check(fibs[1](15) == 610, "a self-referencing closure survives collections")
print(string.format("gc_closure_cells: %d passed, %d failed", pass, fail))
