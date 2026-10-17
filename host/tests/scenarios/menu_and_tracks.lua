-- Boot to the menu, then fly every built-in track for a few seconds.
dofile(SCEN_DIR .. "/lib.lua")
frames(90)
no_error("boot")
expect_state(MENU, "boot")
shot("menu")
local top = test("info")
check(top and top >= 8, "highest track " .. tostring(top))
for t = 1, 8 do
  test("track", t)
  test("start", 2)                      -- practice: COUNT, then FLY after 3 s
  sticks(0, 0, -1024, 0)
  frames(330)
  check(state() == FLY or state() == READY or state() == CRASHED, "track " .. t .. ": state " .. state())
  sticks(80, 150, 250, 40)              -- climb and turn a little
  frames(150)
  no_error("track " .. t)
  shot("track" .. t)
  sticks()
  to_menu()
end
local s = stats()
log(string.format("run avg %.2f ms, p95 %.2f ms, lua %d KB, game %s", s.run_avg_ms, s.run_p95_ms, s.lua_kb, s.game_version))
return true
