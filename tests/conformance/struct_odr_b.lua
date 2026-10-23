-- Cross-TU struct shape-name collision, module B.
-- Promotes {a_d_b = string} (shape key "a_d_b:s"), which flattens to the same
-- C++ name S_a_d_b_s as module A with a different member list. Must link and
-- run cleanly (see struct_odr_main.lua).
local q = { a_d_b = "y" }
local s = q.a_d_b .. "!"
return s
