local app = require("uwebsockets")
local json = require("dkjson")

app.create_app()

 -- Using 1000ms for a quicker test

  -- Set a interval_id to print a message after 3000ms
    -- This will not block the app loop, allowing it to continue processing other requests
    print("Setting a timeout for 3000 milliseconds...")

    app.setTimeout(function()
        print("Timeout reached after 3000 milliseconds!")
    end, 3000)

    app.setTimeout(function(msg)
    print("Timeout:", msg)
end, 1000, "Hello!")

app.setInterval(function(ctx)
    print("Tick:", ctx.args[1], ctx.args[2])
end, 2000, "Tick message", 42)




-- define setInterval to print a message every 1000ms
local count = 0

app.setInterval(function(ctx)
    print("Interval ID:", ctx.id)
    print("Type:", ctx.type)
    print("Interval (ms):", ctx.interval_ms)

    local args = ctx.args or {}
    local var3 = json.encode(args[3]) or "nil"
    print("Extra args:", args[1], args[2], var3)

    count = (count or 0) + 1
    -- if count >= 10 then
    --     app.clearTimer(ctx.id)
    --     print("Interval cleared.")
    -- end
end, 1000, "hello", 123, { key = "value" })



-- app.clearInterval() -- Clear the interval after 5 seconds

app.get("/hello", function(req, res)
    res:writeHeader("Content-Type", "text/plain")
    print("Received GET request for /hello")
    res:send('Hello, World! This is a response from the uWebSockets app.\n')

end)



app.listen(3000, function(token)
    if token then
        print("Listening on port 3000")
    else
        print("Failed to listen on port 3000")
    end
end)




print("Running uWS application loop. Press Ctrl+C to exit.")
app.run() -- This blocks until the app loop is stopped (e.g., by Ctrl+C)

-- After app:run() returns, explicitly clean up the uWS app
print("App loop has ended. Initiating graceful cleanup...")
-- app.cleanup_app() -- Call your new cleanup function
print("Cleanup initiated. Program should now exit cleanly.")