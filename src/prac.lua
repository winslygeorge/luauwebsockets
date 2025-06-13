-- main.lua

-- Load the uws binding library
local uws = require("uwebsockets") -- Assuming the compiled C++ library is named uwsgibinding

-- Create the uWS application instance
local app_created = uws.create_app()
if app_created then
    print("uWS application created successfully!")
else
    error("Failed to create uWS application.")
end

---

-- Define a GET route for '/'
uws.get("/", function(req, res)
    print("Received GET request for /")
    res:writeHeader("Content-Type", "text/plain")
    res:send("Hello from Lua uWS!")
end)

-- Define a GET route with a URL parameter
uws.get("/users/:id", function(req, res)
    local user_id = req:getUrl():match("/users/(%d+)") -- Simple regex to extract ID
    print("Received GET request for user ID:", user_id)
    res:writeHeader("Content-Type", "text/html")
    res:send("<h1>User Profile</h1><p>You requested user ID: " .. tostring(user_id) .. "</p>")
end)

-- Another GET request example accessing headers and remote address
uws.get("/info", function(req, res)
    local user_agent = req:getHeader("user-agent") or "N/A"
    local accept_header = req:getHeader("accept") or "N/A"
    local remote_addr = res:getRemoteAddress() or "N/A"

    print("Request for /info from:", remote_addr)
    print("User-Agent:", user_agent)
    print("Accept Header:", accept_header)

    res:writeStatus(200)
    res:writeHeader("Content-Type", "text/plain")
    res:send("Your User-Agent: " .. user_agent .. "\n")
    res:send("Your Accept Header: " .. accept_header .. "\n")
    res:send("Your Remote Address: " .. remote_addr .. "\n")
end)

uws.listen(3000)

uws.run()