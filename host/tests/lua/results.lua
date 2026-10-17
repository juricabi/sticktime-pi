-- run() return values: E closes the script on any number other than 0
local T = STICKTIME_TEST
local r = 0
T.ret = function(k) r = k end
local VALS = { [0] = 0, 1, "1", true, false, 0.0, 0 / 0, -1, "x", 2.5, "0" }
local function run(e) if r == 99 then return nil end return VALS[r] end
return { run = run }
