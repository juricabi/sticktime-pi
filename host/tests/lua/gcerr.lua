-- finalizers and metamethods that fail or hang must not take the host down. Lua runs __gc with
-- hooks off (the watchdog can't stop it), so setmetatable refuses __gc; other metamethods run
-- with the watchdog on.
local T = STICKTIME_TEST
local mode = 0
T.junk = function(k)
  if k == 1 then setmetatable({}, { __gc = function() error("gc boom") end })
  else setmetatable({}, { __gc = function() while true do end end }) end
end
T.falsegc = function() setmetatable({}, { __gc = false }) end
-- __gc added after setmetatable never makes the object finalizable (Lua 5.3)
T.late = function()
  local mt = {}
  setmetatable({}, mt)
  mt.__gc = function() while true do end end
  collectgarbage()
  collectgarbage()
  return 1
end
T.plain = function()
  local t = setmetatable({}, { __index = function(_, k) return k .. "!" end })
  return getmetatable(t) ~= nil and t.x == "x!" and setmetatable(t, nil) == t and getmetatable(t) == nil
end
T.badarg = function() setmetatable({}, 5) end
T.filemt = function()
  local f = io.open("/SCRIPTS/TESTS/gcerr.lua")
  local m = getmetatable(f)
  local ok = pcall(setmetatable, f, {})
  io.close(f)
  return m, ok
end
T.spinidx = function() local t = setmetatable({}, { __index = function() while true do end end }) return t.x end
T.mode = function(m) mode = m end
local function run(e)
  if mode == 1 then error(setmetatable({}, { __tostring = function() while true do end end })) end
  if mode == 2 then error(setmetatable({}, { __tostring = function() error("again") end })) end
  return 0
end
return { run = run }
