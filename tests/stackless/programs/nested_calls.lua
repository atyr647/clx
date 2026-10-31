-- a yielding call as an argument of another call, as the last argument (multi-value) and in returns
local function y(v) return coroutine.yield(v) end
local function add(a, b) return (a or 0) + (b or 0) end
local function pass(...) return ... end
local co = coroutine.wrap(function()
  local s = add(y(1), y(2))
  print("sum", s)
  print("pass", pass(y(3)))
  print("max", math.max(y(4), 10))
  return pass(y(5), 6)
end)
local out = { co() }
local i = 0
while true do
  i = i + 1
  print("got", table.unpack(out))
  if i > 6 then break end
  out = { co(i * 10) }
  if #out == 2 and out[2] == 6 then print("final", table.unpack(out)); break end
end
