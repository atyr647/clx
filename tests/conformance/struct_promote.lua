-- Struct local promotion: record locals compiled to native C++ structs.
--
-- A `local p = {name = "bob", age = 30}` whose fields are only ever touched as
-- literal-key `p.f` reads and `p.f = v` writes is emitted as a native struct with
-- typed members (int64_t/double/const LValue*) instead of an LTable. These cases
-- pin down that promoted structs still behave exactly like tables: integer
-- subtypes, string fields, late fields (has_ flag), multi-assign swaps, and
-- shape sharing between same-layout locals.

local passed = 0
local failed = 0

local function assert_eq(actual, expected, name)
    if actual == expected then
        passed = passed + 1
        print("[OK]   ", name)
    else
        failed = failed + 1
        print("[FAIL] ", name, "| Expected:", expected, "Got:", actual)
    end
end

local function print_summary(domain)
    print("-----")
    print("SUITE :", domain)
    print("PASS  :", passed)
    print("FAIL  :", failed)
    print("-----")
end

print("----------------- 1. Integer fields")

local vec = { x = 1, y = 2, z = 3 }
vec.x = vec.x + 10
assert_eq(vec.x, 11, "int field read after write")
assert_eq(vec.y, 2, "untouched int field")
assert_eq(vec.z, 3, "untouched int field (2)")
assert_eq(math.type(vec.x), "integer", "int field keeps integer subtype")
assert_eq(vec.x + vec.y * vec.z, 17, "int field arithmetic")

local acc = 0
for i = 1, 10 do
    acc = acc + vec.x
    vec.x = vec.x - 1
end
assert_eq(acc, 65, "int field accumulates in loop (11+10+...+2)")
assert_eq(vec.x, 1, "int field restored")

print("----------------- 2. String fields")

local person = { name = "bob", age = 30 }
person.name = "alice"
assert_eq(person.name, "alice", "string field write/read")
assert_eq(person.age, 30, "int field beside string field")
assert_eq(#person.name, 5, "string field usable as string")
assert_eq(person.name .. "!", "alice!", "string field concatenates")

print("----------------- 3. Float fields")

local pt = { x = 1.5, y = 2.25 }
pt.x = pt.x * 2
assert_eq(pt.x, 3.0, "float field write/read")
assert_eq(pt.y, 2.25, "untouched float field")
assert_eq(math.type(pt.x), "float", "float field keeps float subtype")
assert_eq(pt.x + pt.y, 5.25, "float field arithmetic")

print("----------------- 4. Late fields")

local late = { x = 1 }
late.count = 5
assert_eq(late.count, 5, "late field read after write")
assert_eq(late.x, 1, "ctor field beside late field")
late.count = late.count * 2
assert_eq(late.count, 10, "late field accumulates")

local unread = { x = 1 }
unread.extra = 7
assert_eq(unread.extra, 7, "late field written and read once")

print("----------------- 5. Multi-assign swap")

local a = { x = 1, y = 2 }
a.x, a.y = a.y, a.x
assert_eq(a.x, 2, "swap left side")
assert_eq(a.y, 1, "swap right side")

local s = { p = 1, q = 2, r = 3 }
s.p, s.q, s.r = s.r, s.p, s.q
assert_eq(s.p, 3, "3-way rotate p")
assert_eq(s.q, 1, "3-way rotate q")
assert_eq(s.r, 2, "3-way rotate r")

print("----------------- 6. Scope and shape sharing")

local function make_point()
    local pt2 = { x = 4, y = 5 }
    pt2.x = pt2.x + pt2.y
    return pt2.x
end
assert_eq(make_point(), 9, "struct inside function body")

local first = { x = 10, y = 20 }
local second = { x = 30, y = 40 }
first.x = first.x + 1
second.y = second.y + 1
assert_eq(first.x + first.y + second.x + second.y, 102, "two same-shape structs (11+20+30+41)")

local mixed = { flag = true, count = 7 }
assert_eq(mixed.flag, true, "bool field")
assert_eq(mixed.count + 1, 8, "int field beside bool field")

print("----------------- 7. Struct fields in expressions")

local box = { w = 3, h = 4 }
assert_eq(box.w * box.h, 12, "field multiply")
assert_eq(box.w < box.h, true, "field comparison")
assert_eq(tostring(box.w), "3", "field tostring keeps integer")

local sum = 0
for i = 1, 3 do
    sum = sum + box.w
    box.w = box.w + i
end
assert_eq(sum, 13, "field accumulated across iterations (3+4+6)")
assert_eq(box.w, 9, "field final value (3+1+2+3)")

print_summary("struct_promote")
if failed > 0 then os.exit(1) end
