-- io with EdgeTX signatures on the SD folder. T.selftest() checks what can be checked from
-- inside; test_etx.c drives the rest (files on disk, the writer thread) through T.put/T.get.
local T = STICKTIME_TEST
local fails, total = 0, 0
local function check(ok, msg)
  total = total + 1
  if not ok then fails = fails + 1 print("FAIL " .. msg) end
end
local function eq(a, b, msg)
  check(a == b, msg .. ": got " .. string.format("%q", tostring(a)) .. ", want " .. string.format("%q", tostring(b)))
end
local function err(f, pat, msg)
  local ok, e = pcall(f)
  check(not ok and string.find(tostring(e), pat, 1, true) ~= nil, msg .. ": " .. (ok and "no error" or tostring(e)))
end
local function slurp(p)
  local f = io.open(p, "r")
  if not f then return nil end
  local t = {}
  while true do
    local s = io.read(f, 7)
    if s == "" then break end
    t[#t + 1] = s
  end
  io.close(f)
  return table.concat(t)
end
local function put(p, s, m)
  local f = io.open(p, m or "w")
  if not f then return false end
  io.write(f, s)
  io.close(f)
  return true
end

T.selftest = function()
  -- write, read back; io.write converts with tostring
  local f = io.open("/IO/t1.txt", "w")
  check(f ~= nil, "open w")
  io.write(f, "hello", 1, 2.5, true)
  eq(io.close(f), true, "close returns true")
  eq(slurp("/IO/t1.txt"), "hello12.5true", "write values")

  -- reads: default 1 byte, n floored, "" at EOF
  f = io.open("/IO/t1.txt")
  check(f ~= nil, "default mode is r")
  eq(io.read(f), "h", "read default 1")
  eq(io.read(f, 0), "", "read 0")
  eq(io.read(f, -3), "", "read negative")
  eq(io.read(f, 2.9), "el", "read floors n")
  eq(io.read(f, 100), "lo12.5true", "short read at the end")
  eq(io.read(f, 5), "", "EOF gives an empty string")
  eq(io.read(f), "", "EOF again")
  -- seek
  eq(io.seek(f, 1), 0, "seek returns 0")
  eq(io.read(f, 4), "ello", "read after seek")
  io.seek(f, 1000)
  eq(io.read(f, 1), "", "seek past the end")
  io.seek(f, -5)
  eq(io.read(f, 1), "h", "negative seek goes to 0")
  io.seek(f, 3)
  io.seek(f)
  eq(io.read(f, 1), "h", "seek default 0")
  io.close(f)
  eq(io.read(f, 1), nil, "read on a closed handle")
  eq(io.close(f), nil, "second close does nothing")
  io.write(f, "x")
  eq(io.seek(f, 1), nil, "seek on a closed handle")

  -- an r handle ignores writes
  f = io.open("/IO/t1.txt", "r")
  io.write(f, "ZZZ")
  io.close(f)
  eq(slurp("/IO/t1.txt"), "hello12.5true", "r handle ignores writes")

  -- append: writes go to the end whatever the position; reads start at the end
  f = io.open("/IO/t1.txt", "a")
  eq(io.read(f, 1), "", "a starts at the end")
  io.write(f, "+A")
  io.seek(f, 0)
  eq(io.read(f, 5), "hello", "a reads after a seek")
  io.write(f, "+B")
  io.close(f)
  eq(slurp("/IO/t1.txt"), "hello12.5true+A+B", "append")
  check(put("/IO/new_a.txt", "x", "a"), "a creates a missing file")
  eq(slurp("/IO/new_a.txt"), "x", "a on a missing file")
  f = io.open("/IO/new_a.txt", "a")
  io.close(f)
  eq(slurp("/IO/new_a.txt"), "x", "a without writes leaves the file")

  -- w truncates; seek + write overwrites; a gap is zero-filled
  f = io.open("/IO/t2.txt", "w")
  io.write(f, "abcdef")
  io.seek(f, 2)
  io.write(f, "XY")
  io.seek(f, 8)
  io.write(f, "Z")
  io.close(f)
  eq(slurp("/IO/t2.txt"), "abXYef\0\0Z", "w: seek overwrites, the gap is zeros")
  f = io.open("/IO/t2.txt", "w")
  io.close(f)
  eq(slurp("/IO/t2.txt"), "", "w without writes truncates")
  -- r+: read and write an existing file
  put("/IO/t2.txt", "12345")
  f = io.open("/IO/t2.txt", "r+")
  eq(io.read(f, 1), "1", "r+ reads")
  io.write(f, "ab")
  io.close(f)
  eq(slurp("/IO/t2.txt"), "1ab45", "r+ writes at the position")
  check(io.open("/IO/missing.txt", "r+") == nil, "r+ needs the file")
  -- modes
  for _, m in ipairs({ "rb", "r+b", "wb", "w+", "w+b", "ab", "a+", "a+b", "rbb" }) do
    local h = io.open("/IO/t2.txt", m)
    check(h ~= nil, "mode " .. m)
    if h then io.close(h) end
  end
  for _, m in ipairs({ "x", "rw", "rr", "+r", "", "rbx", "r+ ", "R", "r++" }) do
    err(function() io.open("/IO/t2.txt", m) end, "invalid mode", "mode '" .. m .. "'")
  end
  err(function() io.open() end, "bad argument #1", "open needs a path")
  -- missing files
  local h, msg = io.open("/IO/none.txt")
  check(h == nil and type(msg) == "string" and string.find(msg, "none.txt", 1, true), "missing file -> nil, message")
  check(io.open("/NODIR/none.txt") == nil, "missing folder")
  -- unsafe paths
  for _, p in ipairs({ "/../evil.txt", "/IO/../../evil.txt", "..", "../evil.txt", "/IO/..\\..\\evil.txt",
    "\\..\\evil.txt", "/IO/.../x", "/IO/.. /x", "/IO/ . /x", "C:/evil.txt", "/IO/a:b", "/IO/x\1y", "/IO/x\127y" }) do
    check(io.open(p, "w") == nil, "unsafe path " .. p)
    check(io.open(p, "r") == nil, "unsafe path (r) " .. p)
  end
  check(io.open("/IO/nul\0x", "w") == nil, "NUL in the path")
  check(io.open(string.rep("a", 2000), "w") == nil, "too long a path")
  -- directories
  check(io.open("/IO", "r") == nil, "a directory can't be read")
  check(io.open("/IO", "a") == nil, "a directory can't be appended")
  check(io.open("/IO", "w") == nil, "a directory can't be written")
  check(io.open("/", "w") == nil and io.open("", "r") == nil, "the SD root is a directory")
  -- the same file by other spellings
  check(put("IO//./t3.txt", "rel"), "relative path")
  eq(slurp("/IO/t3.txt"), "rel", "relative path names the SD root")
  eq(slurp("\\IO\\t3.txt"), "rel", "backslashes")
  eq(slurp("/IO/sub/../t3.txt"), nil, "'..' even inside the SD")
  -- not a handle
  err(function() io.read("x") end, "FILE* expected", "read needs a handle")
  err(function() io.write(nil, "x") end, "FILE* expected", "write needs a handle")
  err(function() io.close({}) end, "FILE* expected", "close needs a handle")
  err(function() io.seek(5) end, "FILE* expected", "seek needs a handle")
  f = io.open("/IO/t3.txt")
  check(string.find(tostring(f), "file (", 1, true) == 1, "tostring of a handle")
  io.close(f)
  eq(tostring(f), "file (closed)", "tostring of a closed handle")
  -- a __tostring that closes the file in the middle of a write
  f = io.open("/IO/t4.txt", "w")
  local evil = setmetatable({}, { __tostring = function() io.close(f) return "E" end })
  io.write(f, "a", evil, "b")
  eq(slurp("/IO/t4.txt"), "a", "closed by a __tostring during io.write")
  -- too big a write is dropped (the folder is not a place for runaway scripts)
  f = io.open("/IO/big.txt", "w")
  io.write(f, string.rep("x", 5 * 1024 * 1024))
  io.write(f, "ok")
  io.close(f)
  eq(slurp("/IO/big.txt"), "ok", "a write over 4 MB is dropped")
  -- binary data survives
  local bin = {}
  for i = 0, 255 do bin[#bin + 1] = string.char(i) end
  bin = table.concat(bin)
  put("/IO/bin.dat", bin)
  eq(slurp("/IO/bin.dat"), bin, "all 256 byte values")
  return fails, total
end

-- the most handles at once
T.many = function()
  local hs, n, msg = {}, 0, nil
  for i = 1, 20 do
    local f, m = io.open("/IO/t3.txt", "r")
    if f then n = n + 1 hs[#hs + 1] = f elseif not msg then msg = m end
  end
  local toomany = msg and string.find(msg, "too many open files", 1, true) and 1 or 0
  io.close(hs[1])
  local again = io.open("/IO/t3.txt", "r") and 1 or 0
  return n, toomany, again
end
T.closeall = function() collectgarbage() collectgarbage() end

local PATHS = { "/IO/a.txt", "/IO/b.txt", "/IO/new/deep/dir/c.txt", "/IO/d.txt", "/SCRIPTS/TOOLS/StickTime.dat",
  "/IO/blocker/x.txt" }
T.put = function(k, v) return put(PATHS[k], "v=" .. v) and 1 or 0 end
T.app = function(k, v) return put(PATHS[k], "+" .. v, "a") and 1 or 0 end
T.get = function(k) return slurp(PATHS[k]) end
T.has = function(k) local f = io.open(PATHS[k]) if f then io.close(f) return 1 end return 0 end
-- a handle dropped without io.close: nothing is written
T.drop = function(k, v)
  local f = io.open(PATHS[k], "w")
  io.write(f, "dropped " .. v)
  f = nil
  collectgarbage()
  collectgarbage()
end
-- an error between open and close: nothing half-written
T.half = function(k)
  local f = io.open(PATHS[k], "w")
  io.write(f, "partial")
  error("boom in the middle of a save")
end
T.result = function() return fails, total end

local function init() end
local function run(e) return 0 end
return { init = init, run = run }
