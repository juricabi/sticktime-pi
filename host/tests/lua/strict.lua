-- a script with a strict _G: the host's global writes and lookups must not raise errors
local T = STICKTIME_TEST
T.lat = function() return rawget(_G, "STICKTIME_LAT") end
T.tool = function() return rawget(_G, "TOOL_RUN_US"), math.type(rawget(_G, "TOOL_RUN_US")) == "integer" end
local function init()
  setmetatable(_G, {
    __newindex = function(_, k) error("strict: assignment to " .. tostring(k)) end,
    __index = function(_, k) error("strict: undefined " .. tostring(k)) end,
  })
end
return { init = init, run = function() return 0 end }
