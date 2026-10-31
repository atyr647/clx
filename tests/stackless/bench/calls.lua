-- ordinary calls with no coroutine involved: the plain forms must not be slower for being built with --stackless
local function fib(n) if n < 2 then return n end return fib(n - 1) + fib(n - 2) end
local obj = { v = 0 }
function obj:add(x) self.v = self.v + x; return self.v end
local t0 = os.clock()
local r = fib(32)
local s = 0
for i = 1, 5000000 do s = s + obj:add(1) end
local dt = os.clock() - t0
print(string.format("calls: fib(32)=%d, 5M method calls=%d in %.1f ms", r, s, dt * 1000))
