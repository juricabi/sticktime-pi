-- a script that catches the watchdog's error must still be stopped
local function spin() while true do end end
local function run(e)
  while true do
    pcall(spin)
    pcall(pcall, spin)
    local co = coroutine.wrap(function() pcall(spin) end)
    pcall(co)
  end
end
return { run = run }
