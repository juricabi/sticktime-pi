-- 50 Hz frames (PAL) and 30 Hz frames: the game keeps working.
dofile(SCEN_DIR .. "/lib.lua")
frames(50, 20)
expect_state(MENU, "boot at 50 Hz")
test("track", 2)
test("start", 2)
frames(400, 20)
sticks(0, 100, 300, 0)
frames(300, 20)
no_error("50 Hz")
frames(200, 33.3)
no_error("30 Hz")
sticks()
return true
