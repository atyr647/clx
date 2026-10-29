-- Regression test: a dead table parked for reuse does not keep a large hash buffer.
--
-- Dead tables go to a pool and keep their hash buffer, and any creation can be handed any pooled table. A frame that builds a set of a few thousand keys parked a
-- 4096-entry buffer (96 KB) in the pool each time its set died; the next frames drew those tables for small records, which then carried (and cleared, on every
-- creation) a buffer they never use. The buffers were also invisible to collectgarbage("count"): the process grew to 400 MB over 4,000 frames of a breadth-first search
-- while the counted heap stayed near 40 MB, and each frame took longer than the one before (1.5 ms at 300 frames, 9.4 ms at 4,000).

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

-- a breadth-first search over a 48 x 48 grid: a set of visited cells (a hash that reaches 4096 entries) and a record per cell, all dead when the frame ends
local GW = 48
local frame_no = 0
local function frame()
    local seen, open, head = {}, {}, 1
    local sx, sy = 1 + frame_no % GW, 1 + (frame_no * 7) % GW
    open[1] = { x = sx, y = sy, d = 0 }
    seen[sy * 64 + sx] = true
    local count = 0
    while head <= #open and count < 1500 do
        local c = open[head]; head = head + 1; count = count + 1
        for k = 1, 4 do
            local nx, ny = c.x + (k == 1 and 1 or k == 2 and -1 or 0), c.y + (k == 3 and 1 or k == 4 and -1 or 0)
            if nx >= 1 and nx <= GW and ny >= 1 and ny <= GW and ((nx * 31 + ny * 17) % 11 ~= 0) then
                local key = ny * 64 + nx
                if not seen[key] then seen[key] = true; open[#open + 1] = { x = nx, y = ny, d = c.d + 1, from = c } end
            end
        end
    end
    frame_no = frame_no + 1
    return count + #open
end

local total = 0
for _ = 1, 50 do total = total + frame() end
check(total > 0, "frames run")

local before = hwm_kb()
if before then
    for _ = 1, 1500 do total = total + frame() end
    local grown = hwm_kb() - before
    check(grown < 100 * 1024, "1500 frames of a large set plus small records do not keep growing the process", string.format("%.1f MB", grown / 1024))
else
    print("[SKIP] no /proc/self/status")
end
print(string.format("gc_pooled_hash: %d passed, %d failed", pass, fail))
