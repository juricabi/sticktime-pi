-- Menus with the sticks, the pause switch and signal loss (nav.h rules).
dofile(SCEN_DIR .. "/lib.lua")
frames(60)
expect_state(MENU, "boot")
hold(5, center())
-- roll right = ENTER on "Race" -> countdown
hold(3, { 1024, 0, -1024, 0, -1024 })
expect_state(COUNT, "roll right selects Race")
hold(400, center())
check(state() == FLY or state() == READY, "flying after the countdown: " .. state())
-- pause switch (CH5) on -> paused; off -> resumed
hold(3, { 0, 0, -1024, 0, 1024 })
expect_state(PAUSED, "pause switch on")
hold(3, { 0, 0, -1024, 0, -1024 })
check(state() ~= PAUSED, "pause switch off resumes")
hold(3, { 0, 0, -1024, 0, 1024 })
expect_state(PAUSED, "paused again")
-- pitch down x4 -> "Main menu", roll right -> menu (sticks centered between pushes)
for _ = 1, 4 do
  hold(2, { 0, -1024, -1024, 0, 1024 })
  hold(2, { 0, 0, -1024, 0, 1024 })
end
hold(2, { 1024, 0, -1024, 0, 1024 })
expect_state(MENU, "pitch x4 + roll right = Main menu")
-- roll left in the main menu must NOT close the game
hold(3, center())
hold(3, { -1024, 0, -1024, 0, -1024 })
expect_state(MENU, "roll left in the menu is ignored")
no_error("nav")
-- signal loss in flight pauses: start Freestyle, fly, then the radio goes silent
hold(3, center())
test("start", 3)
hold(60, { 0, 0, 200, 0, -1024 })
check(state() == FLY or state() == READY, "freestyle flying: " .. state())
frames(60)                              -- no more packets: stale after 0.5 s
expect_state(PAUSED, "signal loss pauses")
local s = stats()
check(s.thr == -1024, "throttle low without input: " .. tostring(s.thr))
return true
