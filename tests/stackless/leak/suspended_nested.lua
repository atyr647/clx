-- A thread still suspended inside a call that is itself a coroutine (a chain of two frames) when the state closes must give back both frames: the awaiter of the call owns the frame of the child it started
-- until the call returns. The program ends with such a thread; the test builds it with the frame pool off and AddressSanitizer's leak detection on, and the run must report nothing.
local function nested(depth) if depth == 0 then return coroutine.yield(1) end local v = nested(depth - 1) return v end
local function body() nested(3) end
local co = coroutine.create(function() body() end)
coroutine.resume(co)
print("end, coroutine suspended")
