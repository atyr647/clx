-- A call that yields in a position the compiler cannot await (an if condition) is reported at compile time and fails at run time with Lua's own
-- message when there is no stack to switch. The same call as its own statement works. Prints one line per case.
local function ask(v) return coroutine.yield(v) end
local co1 = coroutine.create(function()
  local r = ask(1)               -- awaitable: a local declaration
  return "statement ok", r
end)
print("statement", coroutine.resume(co1))
print("statement", coroutine.resume(co1, "x"))
local co2 = coroutine.create(function()
  if ask(2) then return "condition ok" end   -- not awaitable: an if condition
  return "condition false"
end)
local ok, a = coroutine.resume(co2)
print("condition", ok, a)
if ok and a == 2 then print("condition", coroutine.resume(co2, true)) end
