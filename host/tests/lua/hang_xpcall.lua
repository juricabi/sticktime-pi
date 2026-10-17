-- xpcall handlers run with hooks off when the watchdog's error reaches them: they must not hang
local T = STICKTIME_TEST
local function spin() while true do end end
T.both = function() return xpcall(spin, spin) end                 -- the watchdog fires in f
T.handler = function() return xpcall(error, spin) end              -- the handler loops
T.normal = function() return xpcall(function() error("e1") end, function(m) return "handled: " .. m end) end
T.args = function() return xpcall(function(a, b) return a + b, "ok" end, print, 2, 3) end
T.yield = function()
  local co = coroutine.wrap(function() return xpcall(function() coroutine.yield(5) return 7 end, print) end)
  return co(), co()
end
T.bad = function() return xpcall(spin) end
local function run(e)
  if e == 1 then xpcall(spin, spin) end
  return 0
end
return { run = run }
