-- Helpers shared by the scenarios (dofile(SCEN_DIR .. "/lib.lua")).
MENU, SETUP, COUNT, FLY, CRASHED, READY, PAUSED, DONE = 1, 2, 3, 4, 5, 6, 7, 8

function check(cond, msg)
  if not cond then error(msg or "check failed", 2) end
end

function no_error(where)
  local e = err()
  check(e == nil, (where or "") .. ": game error: " .. tostring(e))
end

function expect_state(s, where)
  check(state() == s, (where or "") .. ": state " .. tostring(state()) .. ", expected " .. s)
end

-- a USB radio in slot 2 holding these channels for n frames (sent every frame so it stays live)
function hold(n, ch)
  for _ = 1, n do
    source(2, "usb", ch)
    frames(1)
  end
end

function center() return { 0, 0, -1024, 0, -1024 } end

-- from flight: pause, then "Main menu" (pause item 5)
function to_menu()
  if state() ~= PAUSED then key("exit") frames(2) end
  expect_state(PAUSED, "to_menu")
  for _ = 1, 4 do key("next") frames(1) end
  key("enter") frames(3)
  expect_state(MENU, "to_menu")
end
