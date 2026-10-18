-- realworld.lua : "everyday Lua" workload mix for comparing Lua implementations.
--
-- Usage:  lua realworld.lua [scale]        (scale defaults to 1)
--   stdout : deterministic checksums (diff them between implementations)
--   stderr : per-section CPU times
--
-- Portable Lua 5.1 - 5.5 / LuaJIT: no integer division, no bit operators, no goto.
-- All randomness comes from a tiny Park-Miller generator, so every
-- implementation computes exactly the same data.

local SCALE = tonumber(arg and arg[1]) or 1
local clock, fmt, concat = os.clock, string.format, table.concat
local floor = math.floor

-- deterministic RNG (products stay below 2^53, so doubles are exact too)
local seed = 20240607
local function rand(n)
  seed = (seed * 16807) % 2147483647
  return seed % n + 1
end

local MOD = 1000000007
local function mix(c, v) return (c * 31 + v) % MOD end

local total_chk, total_time = 0, 0
local function section(name, fn)
  local t0 = clock()
  local chk = fn()
  local dt = clock() - t0
  io.stdout:write(fmt("%-10s checksum=%d\n", name, chk))
  io.stderr:write(fmt("%-10s %7.3fs\n", name, dt))
  total_chk = mix(total_chk, chk)
  total_time = total_time + dt
end

---------------------------------------------------------------------------
-- 1. expression tokenizer + recursive-descent parser + AST evaluator
---------------------------------------------------------------------------
local function gen_expr(depth)
  if depth == 0 or rand(5) == 1 then return tostring(rand(9)) end
  local o = rand(3)
  return "(" .. gen_expr(depth - 1) .. " " .. ("+-*"):sub(o, o) .. " " .. gen_expr(depth - 1) .. ")"
end

local function tokenize(s)
  local toks, n, i, len = {}, 0, 1, #s
  while i <= len do
    local c = s:byte(i)
    if c == 32 then
      i = i + 1
    elseif c >= 48 and c <= 57 then
      local j = i
      while j < len do
        local d = s:byte(j + 1)
        if d >= 48 and d <= 57 then j = j + 1 else break end
      end
      n = n + 1; toks[n] = { t = "num", v = tonumber(s:sub(i, j)) }
      i = j + 1
    else
      n = n + 1; toks[n] = { t = s:sub(i, i) }
      i = i + 1
    end
  end
  return toks
end

local function parse(toks)
  local pos = 1
  local expr
  local function factor()
    local tk = toks[pos]
    pos = pos + 1
    if tk.t == "num" then return { k = "num", v = tk.v } end
    local e = expr()           -- tk was "("
    pos = pos + 1              -- skip ")"
    return e
  end
  local function term()
    local l = factor()
    while toks[pos] and toks[pos].t == "*" do
      pos = pos + 1
      l = { k = "bin", op = "*", l = l, r = factor() }
    end
    return l
  end
  expr = function()
    local l = term()
    while toks[pos] and (toks[pos].t == "+" or toks[pos].t == "-") do
      local op = toks[pos].t
      pos = pos + 1
      l = { k = "bin", op = op, l = l, r = term() }
    end
    return l
  end
  return expr()
end

local function eval(n)
  if n.k == "num" then return n.v end
  local a, b = eval(n.l), eval(n.r)
  local op = n.op
  if op == "+" then return a + b elseif op == "-" then return a - b else return a * b end
end

local function sec_expr()
  local chk = 0
  for _ = 1, 6000 * SCALE do
    local src = gen_expr(4)
    local v = eval(parse(tokenize(src)))
    chk = mix(chk, v % MOD + #src)
  end
  return chk
end

---------------------------------------------------------------------------
-- 2. class hierarchy with inheritance (method calls through __index chains)
---------------------------------------------------------------------------
local function class(parent)
  local c = {}
  c.__index = c
  if parent then setmetatable(c, { __index = parent }) end
  c.new = function(...)
    local o = setmetatable({}, c)
    o:init(...)
    return o
  end
  return c
end

local Shape = class()
function Shape:init(name) self.name = name end
function Shape:area() return 0 end
function Shape:perimeter() return 0 end
function Shape:describe() return self.name .. ":" .. self:area() end

local Rect = class(Shape)
function Rect:init(w, h) Shape.init(self, "rect"); self.w = w; self.h = h end
function Rect:area() return self.w * self.h end
function Rect:perimeter() return 2 * (self.w + self.h) end

local Square = class(Rect)
function Square:init(s) Rect.init(self, s, s); self.name = "square" end
function Square:perimeter() return 4 * self.w end

local Circle = class(Shape)
function Circle:init(r) Shape.init(self, "circle"); self.r = r end
function Circle:area() return 3 * self.r * self.r end
function Circle:perimeter() return 6 * self.r end

local function sec_oop()
  local n = 40000 * SCALE
  local shapes = {}
  for i = 1, n do
    local k = rand(3)
    if k == 1 then shapes[i] = Rect.new(rand(20), rand(20))
    elseif k == 2 then shapes[i] = Square.new(rand(20))
    else shapes[i] = Circle.new(rand(20)) end
  end
  local chk = 0
  for _ = 1, 10 do
    for i = 1, n do
      local s = shapes[i]
      chk = mix(chk, s:area() + s:perimeter())
    end
  end
  for i = 1, n, 50 do chk = mix(chk, #shapes[i]:describe()) end
  return chk
end

---------------------------------------------------------------------------
-- 3. operator overloading (metamethods allocate many short-lived objects)
---------------------------------------------------------------------------
local Vec2 = {}
Vec2.__index = Vec2
local function vec(x, y) return setmetatable({ x = x, y = y }, Vec2) end
Vec2.__add = function(a, b) return vec(a.x + b.x, a.y + b.y) end
Vec2.__sub = function(a, b) return vec(a.x - b.x, a.y - b.y) end
Vec2.__mul = function(a, k) return vec(a.x * k, a.y * k) end
Vec2.__eq  = function(a, b) return a.x == b.x and a.y == b.y end
function Vec2:len2() return self.x * self.x + self.y * self.y end

local function sec_vec()
  local n = 2000 * SCALE
  local ps, vs = {}, {}
  for i = 1, n do
    ps[i] = vec(rand(1000) * 0.5, rand(1000) * 0.5)
    vs[i] = vec(rand(21) - 11, rand(21) - 11)
  end
  local dt, bounced = 0.25, 0
  for _ = 1, 60 do
    for i = 1, n do
      local p = ps[i] + vs[i] * dt
      if p.x < 0 or p.x > 500 then
        vs[i] = vec(-vs[i].x, vs[i].y); p = ps[i] + vs[i] * dt; bounced = bounced + 1
      end
      if p.y < 0 or p.y > 500 then
        vs[i] = vec(vs[i].x, -vs[i].y); p = ps[i] + vs[i] * dt; bounced = bounced + 1
      end
      ps[i] = p
    end
  end
  local sum = vec(0, 0)
  for i = 1, n do sum = sum + ps[i] end
  local same = 0
  for i = 2, n do if ps[i] == ps[1] then same = same + 1 end end
  local chk = mix(floor(sum.x * 1000) % MOD, floor(sum.y * 1000) % MOD)
  return mix(chk, bounced + same + floor(ps[n]:len2()))
end

---------------------------------------------------------------------------
-- 4. JSON-like encode / decode round trip (string building and scanning)
---------------------------------------------------------------------------
local escapes = { ['"'] = '\\"', ['\\'] = '\\\\', ['\n'] = '\\n', ['\t'] = '\\t' }

local function encode(v, out)
  local t = type(v)
  if t == "table" then
    if #v > 0 or next(v) == nil then
      out[#out + 1] = "["
      for i = 1, #v do
        if i > 1 then out[#out + 1] = "," end
        encode(v[i], out)
      end
      out[#out + 1] = "]"
    else
      local keys = {}
      for k in pairs(v) do keys[#keys + 1] = k end
      table.sort(keys)
      out[#out + 1] = "{"
      for i, k in ipairs(keys) do
        if i > 1 then out[#out + 1] = "," end
        out[#out + 1] = '"' .. k .. '":'
        encode(v[k], out)
      end
      out[#out + 1] = "}"
    end
  elseif t == "string" then
    out[#out + 1] = '"' .. v:gsub('[%c"\\]', escapes) .. '"'
  else
    out[#out + 1] = tostring(v)
  end
end

local function decode(s)
  local pos = 1
  local function skip() pos = s:find("%S", pos) or #s + 1 end
  local function str()
    local out, i = {}, pos + 1
    while true do
      local j = s:find('["\\]', i)
      out[#out + 1] = s:sub(i, j - 1)
      if s:sub(j, j) == '"' then pos = j + 1; break end
      local e = s:sub(j + 1, j + 1)
      out[#out + 1] = (e == "n" and "\n") or (e == "t" and "\t") or e
      i = j + 2
    end
    return concat(out)
  end
  local value
  value = function()
    skip()
    local c = s:sub(pos, pos)
    if c == "{" then
      local t = {}
      pos = pos + 1; skip()
      if s:sub(pos, pos) == "}" then pos = pos + 1; return t end
      while true do
        skip(); local k = str(); skip(); pos = pos + 1
        t[k] = value(); skip()
        local d = s:sub(pos, pos); pos = pos + 1
        if d == "}" then return t end
      end
    elseif c == "[" then
      local t, n = {}, 0
      pos = pos + 1; skip()
      if s:sub(pos, pos) == "]" then pos = pos + 1; return t end
      while true do
        n = n + 1; t[n] = value(); skip()
        local d = s:sub(pos, pos); pos = pos + 1
        if d == "]" then return t end
      end
    elseif c == '"' then
      return str()
    elseif s:sub(pos, pos + 3) == "true" then
      pos = pos + 4; return true
    elseif s:sub(pos, pos + 4) == "false" then
      pos = pos + 5; return false
    else
      local a, b = s:find("^-?%d+", pos)
      pos = b + 1
      return tonumber(s:sub(a, b))
    end
  end
  return value()
end

local tagwords = { "alpha", "beta", "gamma", "delta", "epsilon", "zeta", "eta", "theta" }
local function gen_record(i)
  local tags = {}
  for j = 1, rand(5) do tags[j] = tagwords[rand(#tagwords)] end
  local rec = { id = i, name = "user" .. i, active = rand(2) == 1, score = rand(1000),
                tags = tags, note = "line1\nline2 \"quoted\"\ttab" }
  if rand(3) == 1 then rec.child = { id = i * 10, name = "child" .. i, tags = {} } end
  return rec
end

local function sec_json()
  local n = 1500 * SCALE
  local data = {}
  for i = 1, n do data[i] = gen_record(i) end
  local chk = 0
  for _ = 1, 3 do
    local out = {}
    encode(data, out)
    local s = concat(out)
    local back = decode(s)
    local out2 = {}
    encode(back, out2)
    if s ~= concat(out2) then error("json round trip mismatch") end
    chk = mix(chk, #s)
    chk = mix(chk, back[n].id + #back[1].tags)
  end
  return chk
end

---------------------------------------------------------------------------
-- 5. text processing: gmatch, gsub, word frequencies, sort with comparator
---------------------------------------------------------------------------
local function sec_text()
  local syll = { "ka", "lo", "mi", "ne", "ru", "sa", "to", "vi", "xe", "zu" }
  local vocab = {}
  for i = 1, 400 do
    vocab[i] = syll[rand(10)] .. syll[rand(10)] .. (rand(3) > 1 and syll[rand(10)] or "")
  end
  local nwords = 120000 * SCALE
  local parts = {}
  for i = 1, nwords do parts[i] = vocab[rand(400)] end
  local text = concat(parts, " ")
  text = text:gsub("(%a+) (%a+) (%a+) ", "%1, %2. %3\n")
  local freq, total = {}, 0
  for w in text:gmatch("%a+") do
    freq[w] = (freq[w] or 0) + 1
    total = total + 1
  end
  local list = {}
  for w, c in pairs(freq) do list[#list + 1] = { w = w, c = c } end
  table.sort(list, function(a, b)
    if a.c ~= b.c then return a.c > b.c end
    return a.w < b.w
  end)
  local chk = mix(total, #list)
  for i = 1, 10 do chk = mix(chk, list[i].c + #list[i].w) end
  local _, nka = text:upper():gsub("KA", "")
  return mix(chk, nka + #text)
end

---------------------------------------------------------------------------
-- 6. arrays: sort (default and comparator), insert/remove, binary search
---------------------------------------------------------------------------
local function sec_sort()
  local n = 60000 * SCALE
  local a, b = {}, {}
  for i = 1, n do a[i] = rand(1000000); b[i] = a[i] end
  table.sort(a)
  table.sort(b, function(x, y) return x > y end)
  local chk = 0
  for i = 1, n, 97 do chk = mix(chk, a[i] + b[i]) end
  local m = floor(n / 2)
  local recs = {}
  for i = 1, m do recs[i] = { key = rand(500), id = i } end
  table.sort(recs, function(x, y)
    if x.key ~= y.key then return x.key < y.key end
    return x.id < y.id
  end)
  for i = 1, m, 53 do chk = mix(chk, recs[i].key * 7 + recs[i].id) end
  local q = {}
  for i = 1, m do table.insert(q, i) end
  local s = 0
  while #q > 0 do s = s + table.remove(q) end
  chk = mix(chk, s % MOD)
  local found = 0
  for _ = 1, m do
    local target = rand(1000000)
    local lo, hi = 1, n
    while lo <= hi do
      local mid = floor((lo + hi) / 2)
      local v = a[mid]
      if v == target then found = found + 1; break
      elseif v < target then lo = mid + 1 else hi = mid - 1 end
    end
  end
  return mix(chk, found)
end

---------------------------------------------------------------------------
-- 7. event simulation: binary heap + closures scheduling more closures
---------------------------------------------------------------------------
local function sec_events()
  local heap, size = {}, 0
  local function less(a, b)
    if a.t ~= b.t then return a.t < b.t end
    return a.id < b.id
  end
  local function push(e)
    size = size + 1
    local i = size
    heap[i] = e
    while i > 1 do
      local p = floor(i / 2)
      if less(heap[i], heap[p]) then heap[i], heap[p] = heap[p], heap[i]; i = p else break end
    end
  end
  local function pop()
    local top = heap[1]
    heap[1] = heap[size]; heap[size] = nil; size = size - 1
    local i = 1
    while true do
      local l, r, m = i * 2, i * 2 + 1, i
      if l <= size and less(heap[l], heap[m]) then m = l end
      if r <= size and less(heap[r], heap[m]) then m = r end
      if m == i then break end
      heap[i], heap[m] = heap[m], heap[i]
      i = m
    end
    return top
  end
  local nextid, processed, chk = 0, 0, 0
  local limit = 60000 * SCALE
  local function schedule(t, depth)
    nextid = nextid + 1
    local id = nextid
    push({ t = t, id = id, fn = function(now)
      chk = mix(chk, id + now % 1000)
      if depth < 8 and nextid < limit then
        schedule(now + rand(50), depth + 1)
        if rand(3) == 1 then schedule(now + rand(100), depth + 1) end
      end
    end })
  end
  for _ = 1, 3000 do schedule(rand(100), 0) end
  while size > 0 and processed < limit do
    local e = pop()
    e.fn(e.t)
    processed = processed + 1
  end
  return mix(chk, processed)
end

---------------------------------------------------------------------------
-- 8. misc: memoized recursion, varargs, pcall/error, string.format
---------------------------------------------------------------------------
local function memoize(f)
  local cache = {}
  return function(...)
    local key = concat({ ... }, ",")
    local v = cache[key]
    if v == nil then v = f(...); cache[key] = v end
    return v
  end
end

local function sumargs(...)
  local s = 0
  for i = 1, select("#", ...) do s = s + (select(i, ...)) end
  return s
end

local function sec_misc()
  local chk = 0
  for _ = 1, 3 * SCALE do
    local paths
    paths = memoize(function(r, c)
      if r == 0 or c == 0 then return 1 end
      return (paths(r - 1, c) + paths(r, c - 1)) % MOD
    end)
    chk = mix(chk, paths(120, 120))
  end
  for i = 1, 30000 * SCALE do
    chk = mix(chk, sumargs(i, i + 1, i + 2, i + 3, i + 4) % MOD)
  end
  local errs = 0
  for i = 1, 40000 * SCALE do
    local ok, e = pcall(function()
      if i % 3 == 0 then error({ code = i }) end
      if i % 7 == 0 then error("bad " .. i, 0) end
      return i
    end)
    if ok then chk = mix(chk, e)
    elseif type(e) == "table" then errs = errs + e.code % 10
    else errs = errs + #e end
  end
  chk = mix(chk, errs)
  local parts = {}
  for i = 1, 20000 * SCALE do
    parts[#parts + 1] = fmt("%05d:%s:%x", i, "k" .. (i % 17), i * 7)
  end
  return mix(chk, #concat(parts, ";"))
end

---------------------------------------------------------------------------
section("expr",   sec_expr)
section("oop",    sec_oop)
section("vec2",   sec_vec)
section("json",   sec_json)
section("text",   sec_text)
section("sort",   sec_sort)
section("events", sec_events)
section("misc",   sec_misc)
io.stdout:write(fmt("%-10s checksum=%d\n", "TOTAL", total_chk))
io.stderr:write(fmt("%-10s %7.3fs\n", "TOTAL", total_time))