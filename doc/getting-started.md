# Getting started with clx

This guide walks you through building clx, compiling your first program, and
using the Lua language just like you normally would. clx turns your Lua source
into a fast native executable, so you can keep writing plain Lua.

## Quickstart

### Build clx

```bash
# Clone the repository
git clone https://github.com/samyeyo/clx.git
cd clx

# Build (release mode)
./build.sh

# Debug build
./build.sh debug

# Build and install to system
./build.sh install

# Uninstall clx
./build.sh uninstall

# Remove installed files from /usr/local, then delete the build directory
./build.sh clean
```

You can also build manually with CMake:

```bash
mkdir build
cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build .
```

### Compile your first Lua program

Create a file called `hello.lua`:

```lua
print("Hello, World!")
```

Compile and run:

```bash
./build/clx hello.lua
./hello
```

You should see `Hello, World!` printed.

## Your second program

Let's try something more interesting:

```lua
-- fib.lua
function fib(n)
    if n <= 1 then return n end
    return fib(n - 1) + fib(n - 2)
end

print("Fibonacci(20) = " .. fib(20))
```

Compile it with `--fast` for the best performance:

```bash
./build/clx --fast fib.lua
./fib
```

It should run faster than the reference Lua 5.5 interpreter.

## Language Features

clx supports most Lua 5.5 features:

### Variables and Types

```lua
-- Numbers
local x = 42
local pi = 3.14159

-- Strings
local greeting = "Hello"
local name = 'World'

-- Booleans
local flag = true

-- Tables
local t = { a = 1, b = 2 }
local arr = { 1, 2, 3 }

-- Functions
local function add(a, b)
    return a + b
end

-- Closures
local function counter()
    local n = 0
    return function()
        n = n + 1
        return n
    end
end
```

### Control Flow

```lua
-- If/else
if x > 10 then
    print("big")
elseif x > 5 then
    print("medium")
else
    print("small")
end

-- While loop
while x > 0 do
    print(x)
    x = x - 1
end

-- For loop (numeric)
for i = 1, 10 do
    print(i)
end

-- For loop (generic)
for k, v in pairs(t) do
    print(k, v)
end

-- Repeat/until
repeat
    x = x - 1
until x == 0
```

### Functions

```lua
-- Basic function
function greet(name)
    return "Hello, " .. name
end

-- Multiple return values
function divmod(a, b)
    return math.floor(a / b), a % b
end

-- Variadic
function sum(...)
    local total = 0
    for i = 1, select("#", ...) do
        total = total + select(i, ...)
    end
    return total
end

-- Method syntax
local obj = { value = 10 }
function obj:double()
    self.value = self.value * 2
end
```

### Tables and Metatables

```lua
-- Operator overloading via a shared metatable
local VectorMeta = {}
VectorMeta.__index = VectorMeta

function VectorMeta:add(other)
    return setmetatable({ x = self.x + other.x, y = self.y + other.y }, VectorMeta)
end

VectorMeta.__add = function(a, b)
    return a:add(b)
end

VectorMeta.__tostring = function(self)
    return "(" .. self.x .. "," .. self.y .. ")"
end

local function vec(x, y)
    return setmetatable({ x = x, y = y }, VectorMeta)
end

local v1 = vec(1, 2)
local v2 = vec(3, 4)
local v3 = v1 + v2   -- Uses VectorMeta.__add
print(v3.x, v3.y)    -- 4  6
```

### Coroutines

```lua
-- Producer/consumer with coroutines
local function producer(max)
    for i = 1, max do
        coroutine.yield(i)
    end
end

local function consumer()
    local co = coroutine.create(producer)
    while true do
        local status, value = coroutine.resume(co, 5)  -- pass producer's max
        if not status or value == nil then break end
        print("Received: " .. value)
    end
end

consumer()
```

### String module

```lua
-- Basic operations
local s = "Hello, World!"
print(string.len(s))          -- 13
print(string.sub(s, 1, 5))    -- Hello
print(string.upper(s))        -- HELLO, WORLD!
print(string.lower(s))        -- hello, world!
print(string.reverse(s))      -- !dlroW ,olleH

-- Character conversion
print(string.byte("A"))       -- 65
print(string.char(65, 66, 67)) -- ABC

-- Repetition
print(string.rep("ab", 3))    -- ababab
print(string.rep("x", 5, "-")) -- x-x-x-x-x

-- Format
print(string.format("Pi: %.2f", 3.14159))  -- Pi: 3.14
print(string.format("%d + %d = %d", 1, 2, 3))  -- 1 + 2 = 3

-- Pattern matching
local start, finish = string.find("hello world", "world")
print(start, finish)  -- 7 11

local match = string.match("hello world", "(%a+)")
print(match)  -- hello

for word in string.gmatch("hello world from lua", "%a+") do
    print(word)
end

local result, count = string.gsub("hello world", "world", "lua")
print(result, count)  -- hello lua, 1
```

### Bitwise operations

```lua
-- Bitwise AND, OR, XOR
print(0xFF & 0x0F)    -- 15
print(0xF0 | 0x0F)    -- 255
print(0xFF ~ 0xF0)    -- 15

-- Bitwise shifts
print(1 << 8)         -- 256
print(256 >> 4)       -- 16

-- Bitwise NOT
print(~0)             -- -1
```

## Writing fast Lua

A few habits help clx generate the fastest code:

### Use local variables

```lua
-- Good: locals are the fastest to access
local function compute()
    local result = 0
    for i = 1, 1000 do
        local temp = i * 2
        result = result + temp
    end
    return result
end
```

### Prefer numeric loops

```lua
-- Good: numeric for loops get optimized heavily
for i = 1, 1000000 do
    -- body
end
```

### Keep types consistent

```lua
-- Slower: mixing a number and a string needs extra handling
local x = 1 + "2"

-- Faster: plain numbers can go straight to native arithmetic
local x = 1 + 2
```

## Troubleshooting

### Seeing the generated C++

If compilation produces an error and you want to see exactly what clx
generated, use `--cpp` to emit the C++ source without compiling it:

```bash
clx script.lua --cpp
# This creates script.cpp in the current directory
```

### Reading runtime errors

Runtime errors show the Lua file and line where the problem happened:

```
script.lua:10: attempt to perform arithmetic on a table value
```

The format is `filename:line: message`. The message names the *offending* value's
type (a table, nil, string, or boolean) — arithmetic on a plain number does not error.

## Next Steps

- See the [CLI options](./cli.md) for build modes and flags
- Learn about [Lua modules](./modules.md) and native C++ modules
- Explore [Dynamic Lua](./dynamic-lua.md) for loading code at runtime
- Compare [benchmarks](./benchmarks.md) with other Lua runtimes
- Check [Lua 5.5 compatibility](./compatibility.md)
- Dive into the [internals](./internals/index.md) when you're ready