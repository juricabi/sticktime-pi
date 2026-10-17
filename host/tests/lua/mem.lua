-- the Lua heap has a ceiling while script code runs
local T = STICKTIME_TEST
T.big = function() local s = string.rep("x", 300 * 1024 * 1024) return #s end
T.ok = function() return #string.rep("y", 1024 * 1024) end
return { run = function() return 0 end }
