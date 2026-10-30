-- Regression test: collectgarbage("frame"), the end-of-frame collection.
--
-- A program that runs frames calls it once at the end of each. The first call stops the automatic collector; each call does one young collection or advances a running major cycle
-- for a frame's share of time (at most 4 ms, less for a cycle that needs little), and starts a cycle when the old generation has grown by half. This program has to be run on its own
-- (the state a larger program leaves behind, such as a big old generation, moves the point where a cycle starts).

local pass, fail = 0, 0
local function check(cond, name, detail)
    if cond then pass = pass + 1; print("[OK]   " .. name)
    else fail = fail + 1; print("[FAIL] " .. name .. (detail and (": " .. detail) or "")) end
end
local function stats() return collectgarbage("stats") end

do
    collectgarbage("collect")
    collectgarbage("param", "majorkb", 256)
    local before = stats().cycles
    local live = {}
    local running_seen = false
    check(collectgarbage("frame") == false, "frame: with nothing to do no cycle is running")
    check(collectgarbage("isrunning") == false, "frame: the automatic collector is stopped by the first call")
    for f = 1, 600 do
        for i = 1, 40 do live[#live + 1] = { id = #live + 1, pay = { f } } end          -- survives: the old generation grows, a cycle starts
        for i = 1, 100 do local junk = { i, { f } } end                              -- dies young
        if collectgarbage("frame") then running_seen = true end
    end
    local guard = 0
    while collectgarbage("frame", 50) and guard < 100000 do guard = guard + 1 end       -- a larger budget finishes a cycle in few calls
    check(stats().cycles > before, "frame: cycles were started by the old generation growing", (stats().cycles - before) .. " cycles")
    check(running_seen, "frame: a cycle ran across frames (the call returned true)")
    local bad = 0
    for i = 1, #live do local o = live[i]; if o.id ~= i or o.pay[1] < 1 then bad = bad + 1 end end
    check(bad == 0, "frame: everything the program holds is intact after the cycles", bad .. " bad of " .. #live)
    check(collectgarbage("count") < 40 * 1024, "frame: the heap stays bounded", string.format("%.1f MB", collectgarbage("count") / 1024))
    collectgarbage("restart")
    collectgarbage("param", "majorkb", 32768)
end

print(string.format("%d passed, %d failed", pass, fail))
if fail > 0 then os.exit(1) end
