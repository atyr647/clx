-- yields in statement, assignment, local, return, loops, and a generator through a named helper
local function wait(n)
  for i = 1, n do coroutine.yield("w" .. i) end
  return "waited", n
end
local function gen(limit)
  local out = {}
  for i = 1, limit do
    local got = coroutine.yield(i)
    out[#out + 1] = tostring(got)
  end
  return table.concat(out, ",")
end
local function multi()
  local a, b = coroutine.yield("m1")
  local c = wait(2)
  return a, b, c
end
local co = coroutine.create(function(a, b)
  local r1, r2 = wait(3)
  print("wait ->", r1, r2)
  local s = gen(2)
  print("gen ->", s)
  local x, y, z = multi()
  print("multi ->", x, y, z)
  return "done", a + b
end)
local args = { 10, 20 }
local step = 0
while true do
  local res = { coroutine.resume(co, table.unpack(args)) }
  step = step + 1
  print(step, coroutine.status(co), table.unpack(res))
  args = { "r" .. step, step }
  if coroutine.status(co) == "dead" then break end
end
print(coroutine.resume(co))
