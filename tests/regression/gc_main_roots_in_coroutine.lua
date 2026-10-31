-- A collection that runs while a coroutine is running must still trace the main thread's saved root frames. The main thread is not on the
-- allocation chain, so its mark used to stay set after the first collection that marked it and every later collection inside a coroutine skipped
-- it: a table that only the main thread's frame referenced (and that was still young) was freed and its memory reused.
local failed = 0
local function check(cond, what)
  if not cond then failed = failed + 1; print("[FAIL] " .. what) end
end

local churn = coroutine.wrap(function()
  while true do
    local junk = {}
    for i = 1, 4000 do junk[i] = { i, tostring(i) } end   -- enough allocation for several young collections per resume
    coroutine.yield(#junk)
  end
end)

for round = 1, 300 do
  local fresh = { round = round, name = "fresh" .. round }   -- young, referenced only from this frame
  for i = 1, 8 do fresh[i] = { round * 10 + i } end
  churn()                                                      -- collections happen inside the coroutine
  local ok = fresh.round == round and fresh.name == "fresh" .. round
  for i = 1, 8 do ok = ok and type(fresh[i]) == "table" and fresh[i][1] == round * 10 + i end
  if not ok then check(false, "round " .. round .. ": a table held only by the main thread was collected"); break end
end

if failed == 0 then print("[PASS] gc_main_roots_in_coroutine") end
os.exit(failed == 0 and 0 or 1)
