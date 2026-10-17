return { run = function() error(setmetatable({}, { __tostring = function() return "custom error object" end })) end }
