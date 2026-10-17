-- EdgeTX API semantics, checked from inside a script (test_etx.c loads it at 640x480 with
-- stub fonts of 10 px advance and stick mode 3, then reads T.result() and calls the T.* drawing
-- helpers with arguments of its own, checking the frame buffer).
local T = STICKTIME_TEST
local fails, total = 0, 0
local function check(ok, msg)
  total = total + 1
  if not ok then fails = fails + 1 print("FAIL " .. msg) end
end
local function eq(a, b, msg)
  check(a == b and math.type(a) == math.type(b), msg .. ": got " .. tostring(a) .. " (" .. tostring(math.type(a)) .. "), want " .. tostring(b))
end
local function err(f, pat, msg)
  local ok, e = pcall(f)
  check(not ok and string.find(tostring(e), pat, 1, true) ~= nil, msg .. ": " .. (ok and "no error" or tostring(e)))
end

-- constants, exactly engine.js (integers)
eq(LCD_W, 640, "LCD_W") eq(LCD_H, 480, "LCD_H")
local FLAGS = { INVERS = 0x01, VCENTER = 0x02, CENTER = 0x04, RIGHT = 0x08, LEFT = 0, SHADOWED = 0x80, BLINK = 0x1000,
  TIMEHOUR = 0x2000, LEADING0 = 0x10, PREC1 = 0x20, PREC2 = 0x30, SOLID = 0xff, DOTTED = 0x55, BOLD = 0x100,
  TINSIZE = 0x200, SMLSIZE = 0x300, MIDSIZE = 0x400, DBLSIZE = 0x500, XXLSIZE = 0x600 }
for k, v in pairs(FLAGS) do eq(_G[k], v, k) end
local NAMED = { "COLOR_THEME_PRIMARY1", "COLOR_THEME_PRIMARY2", "COLOR_THEME_PRIMARY3", "COLOR_THEME_SECONDARY1",
  "COLOR_THEME_SECONDARY2", "COLOR_THEME_SECONDARY3", "COLOR_THEME_FOCUS", "COLOR_THEME_EDIT", "COLOR_THEME_ACTIVE",
  "COLOR_THEME_WARNING", "COLOR_THEME_DISABLED", "BLACK", "WHITE", "LIGHTWHITE", "LIGHTGREY", "GREY", "DARKGREY", "RED",
  "DARKRED", "LIGHTRED", "GREEN", "DARKGREEN", "BRIGHTGREEN", "BLUE", "DARKBLUE", "CYAN", "YELLOW", "LIGHTBROWN",
  "DARKBROWN", "ORANGE", "MAGENTA" }
for i, k in ipairs(NAMED) do eq(_G[k], (i - 1) << 16, k) end
local EVENTS = { EVT_VIRTUAL_PREV = 0x1D00, EVT_VIRTUAL_NEXT = 0x1E00, EVT_VIRTUAL_DEC = 0x1D00, EVT_VIRTUAL_INC = 0x1E00,
  EVT_VIRTUAL_PREV_PAGE = 0x203, EVT_VIRTUAL_NEXT_PAGE = 0x204, EVT_VIRTUAL_MENU = 0x206, EVT_VIRTUAL_MENU_LONG = 0x806,
  EVT_VIRTUAL_ENTER = 0x202, EVT_VIRTUAL_ENTER_LONG = 0x802, EVT_VIRTUAL_EXIT = 0x201, EVT_EXIT_BREAK = 0x201,
  EVT_ENTER_FIRST = 0x602, EVT_ENTER_BREAK = 0x202, EVT_ENTER_LONG = 0x802, EVT_ENTER_REPT = 0x402,
  EVT_PAGEUP_FIRST = 0x603, EVT_PAGEUP_BREAK = 0x203, EVT_PAGEUP_LONG = 0x803, EVT_PAGEDN_FIRST = 0x604,
  EVT_PAGEDN_BREAK = 0x204, EVT_PAGEDN_LONG = 0x804, EVT_MODEL_FIRST = 0x606, EVT_MODEL_BREAK = 0x206,
  EVT_MODEL_LONG = 0x806, EVT_SYS_FIRST = 0x607, EVT_SYS_BREAK = 0x207, EVT_SYS_LONG = 0x807, EVT_TELEM_FIRST = 0x608,
  EVT_TELEM_BREAK = 0x208, EVT_TELEM_LONG = 0x808, EVT_ROT_LEFT = 0x1D00, EVT_ROT_RIGHT = 0x1E00,
  EVT_TOUCH_FIRST = 0x1001, EVT_TOUCH_BREAK = 0x1002, EVT_TOUCH_SLIDE = 0x1003, EVT_TOUCH_TAP = 0x1004,
  PLAY_NOW = 0x10, PLAY_BACKGROUND = 0x20 }
for k, v in pairs(EVENTS) do eq(_G[k], v, k) end
-- engine.js has no _REPT virtual events (the game treats them as optional)
for _, k in ipairs({ "EVT_VIRTUAL_NEXT_REPT", "EVT_VIRTUAL_PREV_REPT", "EVT_VIRTUAL_INC_REPT", "EVT_VIRTUAL_DEC_REPT",
  "EVT_EXIT_FIRST", "EVT_EXIT_LONG" }) do check(_G[k] == nil, k .. " should be undefined") end

-- functions and libraries
for _, k in ipairs({ "getTime", "getValue", "getFieldInfo", "getStickMode", "playTone", "playHaptic", "playFile",
  "playNumber", "killEvents", "getUsage", "getAvailableMemory", "getRSSI", "getVersion", "print", "loadScript",
  "collectgarbage", "load" }) do check(type(_G[k]) == "function", k .. " is a function") end
for _, k in ipairs({ "clear", "refresh", "resetBacklightTimeout", "exitFullScreen", "RGB", "setColor", "getColor",
  "drawPoint", "drawLine", "drawLineWithClipping", "drawFilledRectangle", "drawRectangle", "invertRect",
  "drawFilledTriangle", "drawTriangle", "drawFilledCircle", "drawCircle", "drawText", "sizeText", "drawNumber",
  "drawTimer", "drawGauge", "drawHudRectangle", "drawBitmap", "drawSource", "drawSwitch", "drawChannel" }) do
  check(type(lcd[k]) == "function", "lcd." .. k .. " is a function")
end
for _, k in ipairs({ "open", "read", "write", "seek", "close" }) do check(type(io[k]) == "function", "io." .. k) end
check(io.lines == nil and io.popen == nil and io.stdout == nil, "only EdgeTX's io")
for _, k in ipairs({ "string", "math", "table", "coroutine", "utf8", "bit32" }) do check(type(_G[k]) == "table", k .. " library") end
for _, k in ipairs({ "os", "debug", "package", "require", "dofile", "loadfile" }) do check(_G[k] == nil, k .. " should be absent") end
eq(("x"):rep(3), "xxx", "string methods")
eq(bit32 and bit32.band(0xF0, 0x3C), 0x30, "bit32")

-- general functions
local v = { getVersion() }
check(#v == 6 and v[1] == "2.12.0-emu" and v[2] == "emulator" and v[3] == 2 and v[4] == 12 and v[5] == 0 and v[6] == "EdgeTX", "getVersion")
eq(model.getInfo().name, "SIM", "model.getInfo")
eq(getUsage(), 0, "getUsage") eq(getAvailableMemory(), 4194304, "getAvailableMemory") eq(getRSSI(), 0, "getRSSI")
eq(select("#", playFile("/x.wav")), 0, "playFile returns nothing")
eq(select("#", playNumber(1, 2, 3)), 0, "playNumber") eq(select("#", killEvents(1)), 0, "killEvents")
local fi = getFieldInfo("ail")
check(fi and fi.id == 1 and fi.name == "ail" and fi.desc == "ail", "getFieldInfo ail")
fi = getFieldInfo("THR")
check(fi and fi.id == 3 and fi.name == "thr", "getFieldInfo is case-insensitive")
eq(getFieldInfo("rud").id, 4, "rud") eq(getFieldInfo("ele").id, 2, "ele")
check(getFieldInfo("xyz") == nil and getFieldInfo("ch1") == nil and getFieldInfo(1) == nil and getFieldInfo("ailx") == nil, "unknown fields")
err(function() getFieldInfo() end, "bad argument #1", "getFieldInfo needs a name")
eq(getStickMode(), 3, "getStickMode")
eq(math.type(getTime()), "integer", "getTime is an integer")
err(function() getValue(nil) end, "string expected", "getValue(nil)")
err(function() getValue({}) end, "bad argument #1", "getValue({})")
err(function() playTone() end, "bad argument #1", "playTone needs a frequency")
err(function() playTone(100) end, "bad argument #2", "playTone needs a length")
err(function() playHaptic() end, "bad argument #1", "playHaptic needs a length")

-- collectgarbage is the real one
check(math.type(collectgarbage("count")) == "float" and collectgarbage("count") > 0, "collectgarbage count")
eq(collectgarbage(), 0, "collectgarbage()") eq(collectgarbage("collect"), 0, "collect")
check(type(collectgarbage("step")) == "boolean", "collectgarbage step")

-- lcd.RGB: (rgb565 << 16 | 0x8000) as an int32
eq(lcd.RGB(255, 255, 255), -32768, "RGB white")
eq(lcd.RGB(255, 0, 0), -134184960, "RGB red")
eq(lcd.RGB(0, 0, 255), 0x1F8000, "RGB blue")
eq(lcd.RGB(0x123456), 0x11AA8000, "RGB(0xRRGGBB)")
eq(lcd.RGB(255.9, 0.5, 0), lcd.RGB(255, 0, 0), "RGB floors")
eq(lcd.RGB(256, 0, 0), 0x8000, "RGB masks like the firmware")
eq(lcd.RGB(0, 0, 263), 0x8000 | (0x00 << 16), "RGB masks blue to 8 bits")
err(function() lcd.RGB(1, 2) end, "bad argument #3", "RGB with two arguments")
eq(lcd.getColor(5.5), 5, "getColor floors")
eq(select("#", lcd.setColor(1, 2)), 0, "setColor")

-- sizeText: two integers, the class's line heights (lrg at 640x480)
local w, h = lcd.sizeText("abc")
eq(w, 30, "sizeText width") eq(h, 27, "sizeText STD height")
eq(select(2, lcd.sizeText("abc", SMLSIZE)), 23, "SMLSIZE") eq(select(2, lcd.sizeText("abc", MIDSIZE)), 42, "MIDSIZE")
eq(select(2, lcd.sizeText("abc", DBLSIZE)), 55, "DBLSIZE") eq(select(2, lcd.sizeText("abc", BOLD)), 28, "BOLD")
eq(select(2, lcd.sizeText("abc", SMLSIZE + 0.75)), 23, "float flags floor")
eq(select(2, lcd.sizeText("abc", SMLSIZE + lcd.RGB(255, 255, 255) + SHADOWED)), 23, "flags with bit 31")
eq(lcd.sizeText(""), 0, "empty text")
eq(lcd.sizeText(12345), 50, "numbers are text")
eq(lcd.sizeText("\xC4\x8C\xFF\xFEab"), 50, "bad UTF-8 is measured, never fatal")

-- argument checks the way EdgeTX does them (luaL_checkinteger)
err(function() lcd.drawLine(0, 0, 10, 10) end, "bad argument #5", "drawLine needs a pattern")
err(function() lcd.drawFilledRectangle(3e9, 0, 1, 1) end, "number has no integer representation", "coordinate beyond int32")
err(function() lcd.drawFilledRectangle(0 / 0, 0, 1, 1) end, "number has no integer representation", "NaN coordinate")
err(function() lcd.drawFilledTriangle(0, 0, 1, 1, 2) end, "bad argument #6", "triangle needs six coordinates")
err(function() lcd.drawText(0, 0, nil) end, "string expected", "drawText needs text")
err(function() lcd.drawText(0, 0, "x", "abc") end, "number expected", "flags must be a number")
err(function() lcd.clear({}) end, "number expected", "clear(table)")
err(function() lcd.drawNumber(0, 0) end, "bad argument #3", "drawNumber needs a value")
lcd.drawText(1.5, 2.5, 12345, 0)                        -- numbers as text
lcd.drawLine("10", "10", "20", "20", SOLID, 0)           -- numeric strings
lcd.drawText(0, 0, "\xC4\x8C\xFF", SMLSIZE + lcd.RGB(255, 255, 255) + SHADOWED)
lcd.drawText(-100000, 2000000, "far away", RIGHT + CENTER + INVERS + VCENTER)
lcd.drawNumber(10, 10, 12.7, PREC1) lcd.drawNumber(10, 10, -5, PREC2 + LEADING0)
lcd.drawTimer(10, 10, 125.9, TIMEHOUR) lcd.drawTimer(10, 10, -61)
lcd.drawGauge(10, 10, 100, 10, 30, 0, RED) lcd.drawGauge(10, 10, 100, 10, 5, 10)
lcd.drawFilledTriangle(-1e6, -1e6, 1e6, 1e6, -2000000000, 2000000000, BLUE)   -- huge but int32
lcd.drawCircle(320, 240, 50, WHITE) lcd.drawFilledCircle(320, 240, -5, WHITE) lcd.drawFilledCircle(0, 0, 3)
lcd.drawRectangle(5, 5, 10, 10) lcd.drawRectangle(5, 5, 10, 10, RED, 3, 7)
lcd.invertRect(-5, -5, 2000, 2000) lcd.invertRect(-5, -5, 2000, 2000)
lcd.drawLineWithClipping(0, 0, 100, 100, 0, 10, 0, 10, SOLID, RED)
lcd.drawTriangle(0, 0, 50, 0, 0, 50, GREEN)
lcd.drawPoint(-1, -1) lcd.drawPoint(639, 479, WHITE)
lcd.drawHudRectangle() lcd.drawBitmap() lcd.drawSource() lcd.drawSwitch() lcd.drawChannel()
lcd.refresh() lcd.resetBacklightTimeout() lcd.exitFullScreen()

-- hooks for test_etx.c
T.result = function() return fails, total end
T.vals = function()
  return getValue("ail"), getValue("ELE"), getValue("Thr"), getValue("RUD"), getValue(1), getValue(2.9),
    getValue(3), getValue(4), getValue(0), getValue(5), getValue("xyz"), getValue("ch1"), getValue(-1), getValue("1")
end
T.vtypes = function() return math.type(getValue("ail")) == "integer", math.type(getValue(3)) == "integer" end
T.time = function() return getTime() end
T.tones = function()
  playTone(440.7, 100.2, 20, PLAY_NOW, 5, 3)
  playTone(880, 50)
  playHaptic(30, 10)
  playHaptic(15.9)
  playTone(1000, 20, 0, lcd.RGB(255, 255, 255))
end
T.flood = function(n) for i = 1, n do playTone(i, 1) end end
T.say = function() print("a", 1, nil, true, 2.5) end
T.clear = function(f) lcd.clear(f) end
T.clear0 = function() lcd.clear() end
T.rect = function(x, y, w, h, f, o) lcd.drawFilledRectangle(x, y, w, h, f, o) end
T.white = function(x, y) lcd.drawFilledRectangle(x, y, 1, 1, lcd.RGB(255, 255, 255)) end
T.shadowed = function(x, y) lcd.drawFilledRectangle(x, y, 1, 1, SMLSIZE + lcd.RGB(0, 0, 255) + SHADOWED) end
T.line = function(x1, y1, x2, y2, p, f) lcd.drawLine(x1, y1, x2, y2, p, f) end
T.point = function(x, y, f) lcd.drawPoint(x, y, f) end
T.tri = function(x1, y1, x2, y2, x3, y3, f, o) lcd.drawFilledTriangle(x1, y1, x2, y2, x3, y3, f, o) end
T.text = function(x, y, f) lcd.drawText(x, y, "Hi", f) end
T.size = function(f) return lcd.sizeText("Hello", f) end
T.rgb = function(r, g, b) return lcd.RGB(r, g, b) end
T.rgb1 = function(v) return lcd.RGB(v) end

return { init = function() end, run = function() return 0 end }
