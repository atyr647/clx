-- creating, running to completion, and dropping many short coroutines, each calling a helper that yields once
local N = tonumber(arg and arg[1]) or 200000
local function step(i) coroutine.yield(i); return i + 1 end
local t0 = os.clock()
local sum = 0
for i = 1, N do
  local co = coroutine.wrap(function() local a = step(i); return a * 2 end)
  sum = sum + co() + co()
end
local dt = os.clock() - t0
print(string.format("spawn: %d coroutines (2 resumes each) in %.1f ms = %.0f ns per coroutine (checksum %d)", N, dt * 1000, dt * 1e9 / N, sum))
