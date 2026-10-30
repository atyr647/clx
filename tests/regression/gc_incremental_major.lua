-- Regression test: a major collection that runs in increments between the program's own steps.
--
-- Marking is spread over many steps while the program keeps changing the heap. A pointer stored into an object the marking has
-- already passed must be marked by the write barrier (tables, upvalue cells, metatables), and a coroutine's stack, which has no
-- barrier, must be looked at again at the end. Each case below moves the only reference to an unmarked object into an object that is
-- already marked, drops every other reference, and lets the cycle finish; if the object is then gone, the barrier is missing.

local pass, fail = 0, 0
local function check(cond, name, detail)
    if cond then pass = pass + 1; print("[OK]   " .. name)
    else fail = fail + 1; print("[FAIL] " .. name .. (detail and (": " .. detail) or "")) end
end

local CHAIN = 60000

-- a long chain so the marking frontier is still far from the tail when the program acts. Only the head is returned: a local that named the tail would make the marking reach the
-- tail (and what hangs off it) at once, as a root. The link is array slot 1: a table's array part is visited before its hash part and the marking takes the most recently queued
-- object first, so the closures, coroutine and metatable that hang off the head in hash fields are traced before the chain is followed.
local function build()
    local head = { tag = "head" }
    local node = head
    for i = 1, CHAIN do
        local n = { i = i }
        node[1] = n
        node = n
    end
    return head
end

local function tail_of(head)
    local node = head
    while node[1] do node = node[1] end
    return node
end

local function finish_cycle()
    local guard = 0
    while not collectgarbage("step") do
        guard = guard + 1
        if guard > 1000000 then return false end
    end
    return true
end

-- take back every table the collector has freed (they are handed out again, newest first) and keep the new ones alive
local function churn()
    local kept = {}
    for i = 1, 60000 do kept[i] = { tag = "churn" .. i, __index = { found = "no" } } end
    return kept
end

local function stats() return collectgarbage("stats") end

local function begin_cycle(head)
    collectgarbage("collect")
    collectgarbage("collect")
    local before = stats().cycles
    check(collectgarbage("startmajor"), "a major cycle can be started")
    -- the cycle first lists the heap's userdata (phase 1); once it is marking (phase 2) a few steps trace the head and leave the tail far ahead of the frontier
    local guard = 0
    while stats().phase == 1 and guard < 100000 do collectgarbage("step"); guard = guard + 1 end
    check(stats().phase == 2, "the cycle is marking")
    for _ = 1, 3 do collectgarbage("step") end
    check(stats().phase == 2, "the cycle is still marking after a few steps")
    return before
end

-- 1. a table field
do
    local head = build()
    tail_of(head).child = { tag = "child-of-tail" }
    local before = begin_cycle(head)
    local function move() local t = tail_of(head); local w = t.child; t.child = nil; head.moved = w end
    move()
    check(finish_cycle(), "table store: the cycle finishes")
    check(stats().cycles == before + 1, "table store: exactly one cycle ran")
    local reused = churn()
    check(head.moved and head.moved.tag == "child-of-tail", "table store: an object moved into a marked table survives the cycle")
    check(stats().major_steps > 3, "table store: the cycle took several steps")
end

-- 2. an upvalue cell
do
    local head = build()
    tail_of(head).child = { tag = "child-of-cell" }
    -- the captured variable lives only in the two closures: no frame names it as a root
    local function install(h)
        local cell
        h.set = function(v) cell = v end
        h.get = function() return cell end
    end
    install(head)
    begin_cycle(head)
    local function move() local t = tail_of(head); local w = t.child; t.child = nil; head.set(w) end
    move()
    check(finish_cycle(), "upvalue store: the cycle finishes")
    local reused = churn()
    check(head.get() and head.get().tag == "child-of-cell", "upvalue store: an object stored into a captured variable survives the cycle")
end

-- 3. a metatable
do
    local head = build()
    tail_of(head).child = { tag = "child-mt", __index = { found = "yes" } }
    begin_cycle(head)
    local function move() local t = tail_of(head); local w = t.child; t.child = nil; setmetatable(head, w) end
    move()
    check(finish_cycle(), "setmetatable: the cycle finishes")
    local reused = churn()
    local mt = getmetatable(head)
    check(mt and mt.tag == "child-mt" and head.found == "yes", "setmetatable: a metatable set on a marked table survives the cycle")
end

-- 4. a coroutine's stack. The coroutine itself takes the object off the end of the chain into a local of its own: nothing is stored into a marked object, so no barrier sees it
-- (arguments handed to resume() are stored in the coroutine and would be seen).
do
    local head = build()
    tail_of(head).child = { tag = "child-co" }
    -- the coroutine is reachable only through the head: no frame names it as a root
    local function spawn(h)
        local co = coroutine.create(function(box)
            local kept
            while true do
                local cmd = coroutine.yield(kept and kept.tag)
                if cmd == "grab" then
                    local node = box
                    while node[1] do node = node[1] end
                    kept = node.child
                    node.child = nil
                end
            end
        end)
        coroutine.resume(co, h)
        h.co = co
    end
    spawn(head)
    begin_cycle(head)
    coroutine.resume(head.co, "grab")
    check(finish_cycle(), "coroutine stack: the cycle finishes")
    local reused = churn()
    local ok, tag = coroutine.resume(head.co, nil)
    check(ok and tag == "child-co", "coroutine stack: an object only a suspended coroutine holds survives the cycle", tostring(tag))
end

-- 5. objects created while the cycle runs
do
    local head = build()
    local before = begin_cycle(head)
    local made = {}
    for i = 1, 3000 do
        local t = { id = i, inner = { i } }
        made[#made + 1] = t
        head["k" .. (i % 7)] = t
    end
    check(finish_cycle(), "allocation during marking: the cycle finishes")
    local reused = churn()
    local good = 0
    for i = 1, #made do if made[i].id == i and made[i].inner[1] == i then good = good + 1 end end
    check(good == 3000, "allocation during marking: objects created while the cycle runs are intact", good .. " of 3000")
end

-- 7. a store during the sweep: the sweep promotes the objects it has not reached to the old generation, so a young object stored into one of them now must keep it from being
-- promoted unseen (the minor collection after the cycle would otherwise free what only an old object holds)
do
    collectgarbage("collect")
    collectgarbage("collect")
    local saved_kb = collectgarbage("param", "youngkb", 1 << 20)
    -- take every pooled (freed) table first: a table recycled during the sweep is pinned, which would hide the case
    local sink = {}
    for i = 1, 300000 do sink[i] = {} end
    local holder = { tag = "holder" }
    local bulk = {}
    for i = 1, 100000 do bulk[i] = { i } end
    check(collectgarbage("startmajor"), "a major cycle can be started")
    local guard = 0
    while stats().phase ~= 3 and guard < 1000000 do collectgarbage("step"); guard = guard + 1 end
    check(stats().phase == 3, "the cycle is sweeping")
    local function put(h) h.x = { tag = "stored-during-sweep" } end
    put(holder)
    check(finish_cycle(), "sweep store: the cycle finishes")
    collectgarbage("minor")
    local reused = churn()
    check(holder.x and holder.x.tag == "stored-during-sweep", "sweep store: a young object stored into an object the sweep had not reached survives the next minor collection")
    collectgarbage("param", "youngkb", saved_kb)
end

-- 6. garbage is still collected by an incremental cycle, and the one-pause mode gives the same live data
do
    collectgarbage("collect")
    local base = collectgarbage("count")
    local freed0 = stats().major_freed_kb
    do
        local head = build()
        collectgarbage("startmajor")
        finish_cycle()
    end
    collectgarbage("collect")
    check(collectgarbage("count") < base + 256, "a dropped structure is freed", string.format("%.0f KB over the start", collectgarbage("count") - base))
    check(stats().major_freed_kb >= freed0, "the statistics count what a cycle frees", string.format("%.0f -> %.0f KB", freed0, stats().major_freed_kb))
    check(collectgarbage("param", "incmajor") == 1, "incremental major collection is on by default")
    collectgarbage("param", "incmajor", 0)
    local c0 = stats().cycles
    collectgarbage("collect")
    check(stats().cycles >= c0 + 1, "with incmajor off a full collection still runs as a cycle")
    collectgarbage("param", "incmajor", 1)
end

print(string.format("%d passed, %d failed", pass, fail))
