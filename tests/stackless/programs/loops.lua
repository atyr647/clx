-- yields inside every kind of loop and branch
local function tick(tag) return coroutine.yield(tag) end
local function body()
  for i = 1, 3 do tick("for" .. i) end
  local n = 0
  while n < 3 do n = n + 1; tick("while" .. n) end
  repeat n = n - 1; tick("repeat" .. n) until n == 0
  for k, v in pairs({ a = 1 }) do tick("pairs " .. k .. v) end
  for i, v in ipairs({ 5, 6 }) do tick("ipairs " .. i .. v) end
  for i = 3, 1, -1 do
    if i == 2 then tick("two") elseif i == 3 then tick("three") else tick("one") end
  end
  do tick("block") end
  return "end"
end
local co = coroutine.wrap(body)
for i = 1, 20 do
  local v = co()
  print(i, v)
  if v == "end" then break end
end
