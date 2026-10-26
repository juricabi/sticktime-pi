-- what the game sees of its track slots: the menu scan at load (as StickTime.lua 1.10.0 does it:
-- formats 1-4), and later opens (T.open) like buildTrack (up to format 4's 10000 bytes)
local T = STICKTIME_TEST
local P = "/SCRIPTS/TOOLS/StickTimeTracks/c"
local listed = {}
for i = 1, 8 do
  local f = io.open(P .. i .. ".txt", "r")
  if f then
    local s = io.read(f, 60)
    io.close(f)
    local name, ver = string.match(s or "", "^%s*([^\r\n]+)[\r\n]+%s*(%d+)%s+(%d+)")
    if name and (ver == "1" or ver == "2" or ver == "3" or ver == "4") then listed[i] = string.sub(name, 1, 16) end
  end
end
local last = ""
T.listed = function(i) return listed[i] end
T.open = function(i, how)
  local p = P .. i .. ".txt"
  if how == 1 then p = "/scripts/tools/sticktimetracks/C" .. i .. ".TXT" end
  if how == 2 then p = "SCRIPTS\\TOOLS\\StickTimeTracks\\c" .. i .. ".txt" end
  local f = io.open(p, how == 3 and "r+" or "r")
  if not f then last = "" return 0 end
  last = io.read(f, 10000)
  io.close(f)
  return 1, #last
end
T.last = function() return last end
-- writes through io (pending on the writer thread when async): 1 good, 2 crashes the game
local CONTENT = { "Pending\n1 31 3 9\n", "Bad\n1 5\n" }
T.write = function(i, k)
  local f = io.open(P .. i .. ".txt", "w")
  io.write(f, CONTENT[k])
  io.close(f)
end
T.nonslot = function()
  local f = io.open("/SCRIPTS/TOOLS/StickTimeTracks/c9.txt", "r")
  if f then io.close(f) return 1 end
  return 0
end
return { run = function() return 0 end }
