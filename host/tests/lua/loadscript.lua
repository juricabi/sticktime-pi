-- loadScript and load(): text chunks only, and EdgeTX's nil-_ENV quirk.
local T = STICKTIME_TEST
local fails, total = 0, 0
local function check(ok, msg)
  total = total + 1
  if not ok then fails = fails + 1 print("FAIL " .. msg) end
end
local D = "/SCRIPTS/TESTS/"

local f, e = loadScript(D .. "child.lua")
check(type(f) == "function", "loadScript loads: " .. tostring(e))
check(f and f() == 42, "the chunk sees the globals")
f = loadScript(D .. "child.lua", "bt")
check(f and f() == 42, "mode bt")
f = loadScript(D .. "child.lua", "t")
check(f and f() == 42, "mode t")
-- a third argument, even nil, leaves the chunk with no _ENV (EdgeTX clears the stack first)
for _, env in ipairs({ {}, false, 0 }) do
  f = loadScript(D .. "child.lua", "bt", env)
  local ok, msg = pcall(f)
  check(not ok and string.find(tostring(msg), "_ENV", 1, true), "env argument -> no _ENV: " .. tostring(msg))
end
f = loadScript(D .. "child.lua", "bt", nil)
local ok, msg = pcall(f)
check(not ok and string.find(tostring(msg), "_ENV", 1, true), "explicit nil env -> no _ENV: " .. tostring(msg))
f = loadScript(D .. "pure.lua", "t", {})
check(f and f() == 7, "a chunk without globals still runs with no _ENV")
-- binary chunks are refused, and so are modes without t
f, e = loadScript(D .. "child.lua", "b")
check(f == nil and string.find(e, "binary chunks are not supported", 1, true), "mode b: " .. tostring(e))
local h = io.open(D .. "dumped.lua", "w")
io.write(h, string.dump(function() return 1 end))
io.close(h)
f, e = loadScript(D .. "dumped.lua")
check(f == nil and string.find(e, "binary", 1, true), "bytecode file refused: " .. tostring(e))
-- missing files and syntax errors: nil and a message, chunk names are the base name
f, e = loadScript(D .. "nothere.lua")
check(f == nil and e == D .. "nothere.lua: file not found", "missing: " .. tostring(e))
f, e = loadScript("/../x.lua")
check(f == nil and string.find(e, "file not found", 1, true), "unsafe path: " .. tostring(e))
f, e = loadScript(D .. "bad.lua")
check(f == nil and string.find(e, "bad.lua:1:", 1, true) == 1, "syntax error message: " .. tostring(e))
f = loadScript(D .. "throws.lua")
ok, msg = pcall(f)
check(not ok and string.find(msg, "throws.lua:2: thrown", 1, true) == 1, "runtime error position: " .. tostring(msg))
check(not pcall(loadScript), "loadScript needs a path")

-- load(): text only, env still works
check(load("return 5")() == 5, "load text")
check(load("return x", "c", "t", { x = 3 })() == 3, "load with env")
check(load("return x", "c", "b", { x = 3 })() == 3, "mode b is read as t")
f, e = load(string.dump(function() return 1 end))
check(f == nil and string.find(e, "binary", 1, true), "load refuses bytecode: " .. tostring(e))
f, e = load(string.dump(function() return 1 end), "d", "b")
check(f == nil, "load refuses bytecode even with mode b")
local parts, i = { "return ", "6" }, 0
check(load(function() i = i + 1 return parts[i] end)() == 6, "load from a function")
check(not pcall(load), "load needs a chunk")
check(load("return ...", "c", "t")(1, 2) == 1, "varargs")

T.result = function() return fails, total end
return { run = function() return 0 end }
