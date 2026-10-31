-- a yield from the bottom of a chain of calls: each resume continues the innermost frame (stackless) or switches stacks (fibers)
local N = tonumber(arg and arg[1]) or 200000
local DEPTH = 8
local function leaf(i) return coroutine.yield(i) end
local function l1(i) return leaf(i) end
local function l2(i) return l1(i) end
local function l3(i) return l2(i) end
local function l4(i) return l3(i) end
local function l5(i) return l4(i) end
local function l6(i) return l5(i) end
local function l7(i) return l6(i) end
local co = coroutine.wrap(function()
  local i = 0
  while true do i = i + 1; l7(i) end
end)
local t0 = os.clock()
local sum = 0
for i = 1, N do sum = sum + co() end
local dt = os.clock() - t0
print(string.format("depth %d: %d resumes in %.1f ms = %.1f ns per resume (checksum %d)", DEPTH, N, dt * 1000, dt * 1e9 / N, sum))
