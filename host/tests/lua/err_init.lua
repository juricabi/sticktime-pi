local function init()
  error("init boom")
end
return { init = init, run = function() return 0 end }
