-- Regression test: os.exit flushes what print and io.write have buffered, and accepts a boolean status.
--
-- print and io.write batch their output per FILE*; os.exit called std::exit directly, which does not know about those buffers, so a program that printed its result and then
-- called os.exit(n) produced no output at all (to a pipe or a terminal), and os.exit(true) raised "number expected, got boolean". The program runs itself as a child to see its output.

local pass, fail = 0, 0
local function check(cond, name, detail)
    if cond then pass = pass + 1; print("[OK] " .. name)
    else fail = fail + 1; print("[FAIL] " .. name .. (detail and (": " .. detail) or "")) end
end

if arg and arg[1] == "child" then
    print("printed before exit")
    io.write("written without newline")
    os.exit(tonumber(arg[2]) or arg[2] == "true" or false)
end

local function run(status)
    local p = io.popen(arg[0] .. " child " .. status .. "; echo \"[status $?]\"", "r")
    local out = p:read("a")
    p:close()
    return out
end

local out = run("3")
check(out:find("printed before exit\nwritten without newline", 1, true) ~= nil, "output written before os.exit(3) is flushed", out)
check(out:find("[status 3]", 1, true) ~= nil, "os.exit(3) exits with status 3", out)
out = run("true")
check(out:find("printed before exit", 1, true) ~= nil and out:find("[status 0]", 1, true) ~= nil, "os.exit(true) is a success exit", out)
out = run("false")
check(out:find("printed before exit", 1, true) ~= nil and out:find("[status 1]", 1, true) ~= nil, "os.exit(false) is a failure exit", out)

print(string.format("os_exit_flushes_output: %d passed, %d failed", pass, fail))
