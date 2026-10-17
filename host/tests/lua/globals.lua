-- globals the host sets
local T = STICKTIME_TEST
T.lat = function() return STICKTIME_LAT, math.type(STICKTIME_LAT) == "integer", STICKTIME_LAT == nil end
T.tool = function() return TOOL_RUN_US, TOOL_SHOW_US, math.type(TOOL_RUN_US) == "integer" end
T.touch = function() return EVT_TOUCH_TAP == nil and 0 or EVT_TOUCH_TAP end
T.size = function() return LCD_W, LCD_H end
T.draw = function() lcd.clear(RED) lcd.drawText(1, 1, "x") return lcd.sizeText("abc") end
T.ev = function() return lastev end
local function run(e) lastev = e return 0 end
return { run = run }
