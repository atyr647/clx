-- errors after a yield, close of a suspended coroutine, dead coroutine, resume of non-suspended
local function boom() coroutine.yield(1); error("after yield") end
local co = coroutine.create(function() boom(); return "not reached" end)
print(coroutine.resume(co))
local ok, err = coroutine.resume(co)
print(ok, (tostring(err):gsub("^.-:%d+: ", "")))
print(coroutine.status(co), coroutine.resume(co))
local co2 = coroutine.create(function() local t = { 1, 2, 3 }; coroutine.yield(t[1]); coroutine.yield(t[2]); return t[3] end)
print(coroutine.resume(co2))
print(coroutine.close(co2), coroutine.status(co2))
local co3
co3 = coroutine.create(function() print(coroutine.resume(co3)); coroutine.yield("x") end)
print(coroutine.resume(co3))
print(pcall(coroutine.yield, 1))
local co4 = coroutine.wrap(function() coroutine.yield(1); error({ code = 42 }) end)
print(co4())
local ok2, e2 = pcall(co4)
print(ok2, type(e2), e2.code)
