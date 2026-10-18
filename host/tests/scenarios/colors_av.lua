-- Analog video and the frame interval: the game's Colors setting (Auto = Summer on analog video,
-- STICKTIME_AV) and the host's STICKTIME_DT (the simulation steps evenly at NTSC's 59.94 Hz).
-- StickTime 1.8.0 and older have neither: skipped.
dofile(SCEN_DIR .. "/lib.lua")
frames(60)
no_error("boot")
if test("clock") == nil then
  log("colors_av: the game has no Colors setting (StickTime 1.8.0 or older): skipped")
  return true
end
local W, H = size()

-- the flat sky above the haze, looking up from the Meadow's start
local function sky()
  test("track", 1)
  test("start", 2)
  frames(5)
  test("state", 4)
  test("pose", 0, 3, 0, 0, 35, 0)
  frames(1)
  no_error("sky")
  return pixel(math.floor(W * 0.35), math.floor(H * 0.3))
end

-- Settings > Colors as the game's save has it (0 Auto, 1 Classic, 2 Summer, 3 Evening, 4 Overcast)
local function colors(c, av)
  sd_write("/SCRIPTS/TOOLS/StickTime.dat", "FPVSIM2 lay=3 colors=" .. c .. "\n")
  check(reload(av), "reload")
  frames(30)
  no_error("colors " .. c)
end

local classic = sky()                    -- not analog video: Auto is Classic
colors(0, true)
local auto = sky()
check(auto ~= classic, "Auto on analog video should be Summer, not the Classic sky (" .. auto .. ")")
colors(1, true)
local classicAV = sky()
check(classicAV == classic, "Classic on analog video should be the Classic sky (" .. classicAV .. " vs " .. classic .. ")")
colors(2, false)
check(sky() == auto, "Summer off analog video should be the Summer sky")
for c = 3, 4 do
  colors(c, true)
  local p = sky()
  check(p ~= classic and p ~= auto, "Colors " .. c .. " should have a sky of its own (" .. p .. ")")
end
colors(0, true)

-- STICKTIME_DT: at 59.94 Hz the simulation steps 16.683 ms every frame (10 ms ticks alone give
-- steps 0.6 ms off on average, at times 3 ms)
test("track", 1)
test("start", 2)
frames(200, 16.683)
sticks(0, 80, 200, 0)
local prev, worst, n = nil, 0, 0
for _ = 1, 600 do
  frames(1, 16.683)
  local pt = test("clock")
  if state() == FLY and prev then
    local d = math.abs((pt - prev) * 10 - 16.683)
    if d > worst then worst = d end
    n = n + 1
  end
  prev = state() == FLY and pt or nil
end
sticks()
no_error("flight at 59.94 Hz")
check(n > 300, "flew only " .. n .. " frames")
check(worst < 0.25, string.format("uneven simulation steps: %.3f ms off", worst))
log(string.format("colors_av: Auto = Summer on analog video; %d frames at 16.683 ms, steps within %.3f ms", n, worst))
return true
