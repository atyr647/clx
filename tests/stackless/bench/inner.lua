-- calls inside a coroutine that never yield: through a method (a call that may yield, so it is awaited), through a plain local function
-- that cannot yield (called directly), and a call that finally yields once per 1000 calls
local N = tonumber(arg and arg[1]) or 2000000
local obj = { v = 0 }
function obj:bump(x) self.v = self.v + x; return self.v end
local function plain(x) return x + 1 end
local co = coroutine.create(function()
  local s = 0
  for i = 1, N do
    s = s + obj:bump(1)
    s = s + plain(i)
    if i % 1000 == 0 then coroutine.yield(s) end
  end
  return s
end)
local t0 = os.clock()
while coroutine.status(co) ~= "dead" do assert(coroutine.resume(co)) end
local dt = os.clock() - t0
print(string.format("inner: %d iterations of (method call + plain call), a yield per 1000: %.1f ms = %.1f ns per iteration", N, dt * 1000, dt * 1e9 / N))
