local function mkvec(x, y)
    return setmetatable({ x = x, y = y }, Vec)
end

Vec = {}
Vec.__index = Vec
function Vec:__tostring() return "vec(" .. self.x .. "," .. self.y .. ")" end
function Vec.__add(a, b) return mkvec(a.x + b.x, a.y + b.y) end
function Vec.__sub(a, b) return mkvec(a.x - b.x, a.y - b.y) end
function Vec.__mul(a, b)
    if type(a) == "number" then return mkvec(a * b.x, a * b.y) end
    if type(b) == "number" then return mkvec(a.x * b, a.y * b) end
    return mkvec(a.x * b.x, a.y * b.y)
end
function Vec.__div(a, b)
    if type(b) == "number" then return mkvec(a.x / b, a.y / b) end
    return mkvec(a.x / b.x, a.y / b.y)
end
function Vec.__mod(a, b)
    if type(b) == "number" then return mkvec(a.x % b, a.y % b) end
    return mkvec(a.x % b.x, a.y % b.y)
end
function Vec.__idiv(a, b)
    if type(b) == "number" then return mkvec(a.x // b, a.y // b) end
    return mkvec(a.x // b.x, a.y // b.y)
end
function Vec.__pow(a, b)
    if type(b) == "number" then return mkvec(a.x ^ b, a.y ^ b) end
    return mkvec(a.x ^ b.x, a.y ^ b.y)
end
function Vec.__unm(a) return mkvec(-a.x, -a.y) end
function Vec.__concat(a, b) return tostring(a) .. tostring(b) end
function Vec.__eq(a, b) return a.x == b.x and a.y == b.y end
function Vec.__lt(a, b) return (a.x * a.x + a.y * a.y) < (b.x * b.x + b.y * b.y) end
function Vec.__le(a, b) return (a.x * a.x + a.y * a.y) <= (b.x * b.x + b.y * b.y) end
function Vec.__len(a) return a.x + a.y end
function Vec.__call(self, k) return self[k] end

local a = mkvec(3, 4)
local b = mkvec(1, 2)
print(tostring(a))
print(tostring(a + b))
print(tostring(a - b))
print(tostring(a * b))
print(tostring(2 * a))
print(tostring(a * 2))
print(tostring(a / b))
print(tostring(a % b))
print(tostring(a // b))
print(tostring(a ^ 2))
print(tostring(-a))
print(a .. b)
print(a == b, a == mkvec(3, 4), a ~= b)
print(a < b, b < a, a <= mkvec(3, 4), a <= b, b <= a)
print(#a, #b)
print(a("x"), a("y"))

local proto = { answer = 42 }
local obj = setmetatable({}, {
    __index = proto,
    __newindex = function(t, k, v) rawset(t, "logged_" .. k, v) end,
})
print(obj.answer)
obj.foo = "bar"
print(obj.logged_foo, obj.foo)

local fallback = setmetatable({}, {
    __index = function(t, k) return "missing:" .. k end,
})
print(fallback.whatever)

local adder = setmetatable({ val = 10 }, {
    __call = function(self, inc) return self.val + inc end,
})
print(adder(5))

local seq = setmetatable({ 10, 20, 30 }, {
    __len = function() return 99 end,
})
print(#seq)

local mt_holder = setmetatable({}, { __metatable = "locked" })
print(getmetatable(mt_holder))
