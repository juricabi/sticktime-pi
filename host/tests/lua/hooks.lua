-- STICKTIME_TEST access from C: numbers in, numbers / booleans / strings out
local T = STICKTIME_TEST
local state = 1
local last
T.sum = function(a, b, c) return a + b + (c or 0) end
T.types = function(a, b) return math.type(a) == "integer", math.type(b) == "float", select("#", a, b) end
T.multi = function() return 1, true, false, "s", {}, nil, 2.5, print end
T.many = function() local t = {} for i = 1, 50 do t[i] = i end return table.unpack(t) end
T.none = function() end
T.err = function() error("hook failed") end
T.name = function() return 7, "Hoop Forest", "second" end
T.long = function() return string.rep("n", 300) end
T.state = function(s) state = s end
T.get = function() return 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, state, 17, 18, 19, 20 end
T.spin = function() while true do end end
T.touch = function() return last and last.x, last and last.y, last and last.startX, last and last.startY,
  last and last.slideX, last and last.slideY, last and last.tapCount end
T.notfn = 5
local function run(e, touch)
  if touch then last = touch end
  return 0
end
return { run = run }
