-- Regression test: the major-collection trigger measures the real old set.
--
-- gc_old_bytes used to add only sizeof(LTable) when a table was promoted, while allocated_bytes counted the table plus its array
-- and hash buffers. A promoted table with a few fields therefore looked about five times smaller than it was, and the old set could
-- grow to five times the major threshold before a major collection ran. With the young generation pinned small every
-- replaced `last` table is promoted before it dies, so this program's heap reached 175 MB against a 32 MB threshold.

local pass, fail = 0, 0
local function check(cond, name, detail)
    if cond then pass = pass + 1; print("[OK] " .. name)
    else fail = fail + 1; print("[FAIL] " .. name .. (detail and (": " .. detail) or "")) end
end

-- 1. promoted garbage is bounded by the major threshold (4 MB here)
collectgarbage("param", "youngkb", 128)
collectgarbage("param", "majorkb", 4096)
local old = {}
for i = 1, 4000 do old[i] = { id = i, last = { t = 0, v = 0 } } end
local seed, peak = 12345, 0
for frame = 1, 3000 do
    for _ = 1, 400 do
        seed = (seed * 1103515245 + 12345) % 2147483648
        local r = old[1 + seed % 4000]
        r.last = { t = frame, v = r.last.v + 1 }
    end
    if frame % 10 == 0 then
        local kb = collectgarbage("count")
        if kb > peak then peak = kb end
    end
end
check(peak < 12 * 1024, "heap with replaced old-to-young references stays within about twice the major threshold",
      string.format("peak %.1f MB", peak / 1024))

-- 2. the collector parameters read back and reject bad values
check(collectgarbage("param", "majorkb") == 4096, "majorkb reads back the value set above")
check(collectgarbage("param", "policy") == 0, "the nursery policy defaults to 0")
check(collectgarbage("param", "policy", 1) == 0 and collectgarbage("param", "policy") == 1, "policy 1 can be selected")
check(not pcall(collectgarbage, "param", "policy", 5), "an unknown policy is an error")
collectgarbage("param", "policy", 0)

print(string.format("gc_old_set_accounting: %d passed, %d failed", pass, fail))
