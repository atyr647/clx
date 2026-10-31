-- yielding through named helpers, recursion, closures, upvalues, varargs and method-style tables
local counter = 0
local function bump(n) counter = counter + n; return coroutine.yield(counter) end
local function rec(d)
  if d == 0 then return bump(100) end
  local v = rec(d - 1)
  return v, d
end
local function vararg(...)
  local n = select("#", ...)
  local got = coroutine.yield(n, ...)
  return got, ...
end
local function make()
  local state = 0
  local function step() state = state + 1; coroutine.yield(state) end
  return function() step(); step(); return state end
end
local co = coroutine.wrap(function()
  print("rec", rec(3))
  print("vararg", vararg("a", nil, "c"))
  local f = make()
  print("closure", f())
  return "ok"
end)
local v = co()
while v ~= "ok" do print("yielded", v); v = co("back" .. tostring(v)) end
print(v, counter)
