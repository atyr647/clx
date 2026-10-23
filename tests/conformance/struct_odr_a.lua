-- Cross-TU struct shape-name collision, module A.
-- Promotes {a = double, b = string} (shape key "a:d|b:s"), which the name
-- derivation flattens to S_a_d_b_s -- the same C++ name module B produces
-- for a different layout. Must link and run cleanly (see struct_odr_main.lua).
local p = { a = 1.5, b = "x" }
local r = p.a + 1
local s = p.b .. "!"
return tostring(r) .. s
