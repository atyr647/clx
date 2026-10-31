-- Unbounded recursion through calls that may yield is cut by the frame-depth limit with Lua's "stack overflow" error, and the thread dies cleanly.
local obj = {}
function obj:down(n) local t = { n }; local r = self:down(n + 1); return r end
local co = coroutine.create(function() return obj:down(1) end)
local ok, msg = coroutine.resume(co)
print("overflow raised", ok == false, string.find(tostring(msg), "stack overflow", 1, true) ~= nil, coroutine.status(co))
local co2 = coroutine.wrap(function() coroutine.yield(7) end)
print("later coroutines work", co2())
