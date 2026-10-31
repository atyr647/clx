-- resume/yield pairs: a generator whose body yields in a loop; the driver resumes it N times
local N = tonumber(arg and arg[1]) or 1000000
local co = coroutine.wrap(function()
  local i = 0
  while true do
    i = i + 1
    coroutine.yield(i)
  end
end)
local t0 = os.clock()
local sum = 0
for i = 1, N do sum = sum + co() end
local dt = os.clock() - t0
print(string.format("pairs: %d resume/yield in %.1f ms = %.1f ns per pair (checksum %d)", N, dt * 1000, dt * 1e9 / N, sum))
