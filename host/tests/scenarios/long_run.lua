-- Ten minutes of flying the Bando at 60 Hz: no errors, Lua memory stays bounded.
dofile(SCEN_DIR .. "/lib.lua")
frames(30)
test("track", 7)
test("start", 3)
local peak = 0
for minute = 1, 10 do
  for i = 1, 60 do
    sticks(math.floor(300 * math.sin(i / 7)), math.floor(200 * math.cos(i / 11)), 100, 50)
    frames(60)
  end
  no_error("minute " .. minute)
  local s = stats()
  if s.lua_kb > peak then peak = s.lua_kb end
  log(string.format("minute %d: state %d, run p95 %.2f ms, lua %d KB", minute, s.state, s.run_p95_ms, s.lua_kb))
end
check(peak < 8192, "Lua memory peak " .. peak .. " KB")
return true
