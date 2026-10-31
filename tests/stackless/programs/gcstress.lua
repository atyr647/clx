-- many suspended coroutines with live locals while the collector runs
local function work(id)
  local t = {}
  for i = 1, 50 do t[i] = { id = id, i = i, s = "s" .. id .. "_" .. i } end
  for round = 1, 5 do
    coroutine.yield(#t)
    for i = 1, 50 do t[i].s = t[i].s .. "x" end
    collectgarbage("step")
  end
  local sum = 0
  for i = 1, 50 do sum = sum + t[i].i + #t[i].s end
  return sum
end
local cos = {}
for id = 1, 60 do cos[id] = coroutine.create(function() return work(id) end) end
local total = 0
for round = 1, 6 do
  for id = 1, 60 do
    local ok, v = coroutine.resume(cos[id])
    if not ok then print("error", v) end
    if round == 6 then total = total + v end
    local junk = {}
    for k = 1, 20 do junk[k] = { k } end
  end
  collectgarbage()
end
print("total", total, collectgarbage("count") > 0)
-- drop suspended coroutines without finishing them
for id = 1, 60, 2 do cos[id] = nil end
collectgarbage()
print("done")
