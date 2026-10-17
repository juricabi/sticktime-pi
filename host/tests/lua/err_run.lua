local T = STICKTIME_TEST
local n = 0
T.count = function() return n end
local function run(e)
  n = n + 1
  if n == 3 then local t = nil return t.x end
  return 0
end
return { run = run }
