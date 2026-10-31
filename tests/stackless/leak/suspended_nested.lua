local function nested(depth) if depth == 0 then return coroutine.yield(1) end local v = nested(depth - 1) return v end
local co = coroutine.create(function() nested(3) end)
coroutine.resume(co)
print("end, coroutine suspended")
