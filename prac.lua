local uws = require("uwebsockets")

uws.create_app()

-- Simple counter for SSE events
local sse_event_counter = 0

-- Store active SSE IDs if you need to broadcast or manage them
local active_sse_clients = {}

-- Define an SSE route
uws.sse("/events", function(req, sse_id)
    print("New SSE client connected! ID: " .. sse_id)
    print("Request URL:", req:getUrl())

    -- Add the client ID to our list of active clients
    active_sse_clients[sse_id] = true

    -- Send an initial event
    uws.sse_send(sse_id, "Welcome!", "welcome", "1")

    -- Example: Periodically send updates
    -- In a real application, you'd trigger this based on actual events
    -- For demonstration, let's just send some updates
    local function send_updates()
        if active_sse_clients[sse_id] then -- Check if connection is still active
            sse_event_counter = sse_event_counter + 1
            local message = "Current server time: " .. os.date() .. " (Event #" .. sse_event_counter .. ")"
            local event_id = tostring(sse_event_counter)

            local success, err = uws.sse_send(sse_id, message, "update", event_id)
            if not success then
                print("Failed to send SSE message to " .. sse_id .. ": " .. err)
                active_sse_clients[sse_id] = nil -- Remove if send failed
                -- You might want to close the connection here if it's dead
                -- uws.sse_close(sse_id) -- Or let client disconnection handle it
            else
                -- Schedule next update
                -- This part is illustrative. Lua itself doesn't have a built-in event loop
                -- for timers. You'd typically integrate with uWS's event loop or
                -- another library for timers if you need server-side pushes.
                -- For now, this is just to show how you'd call sse_send.
                -- In a true event-driven system, `send_updates` would be called externally
                -- when new data is available.
            end
        else
            print("SSE client " .. sse_id .. " is no longer active.")
        end
    end

    -- In a real system, you'd likely register a timer or an event listener
    -- here that calls `send_updates` when data changes.
    -- For this simple example, let's simulate a few immediate sends.
    send_updates()
    send_updates()
    send_updates()

    -- You can also close the connection from Lua when done
    -- uws.sse_close(sse_id)
end)

-- Serve a basic HTML file to demonstrate SSE client
uws.get("/", function(req, res)
    res:writeHeader("Content-Type", "text/html")
    res:send([[
        <!DOCTYPE html>
        <html>
        <head>
            <title>SSE Test</title>
        </head>
        <body>
            <h1>Server-Sent Events Demo</h1>
            <div id="events"></div>
            <script>
                const eventSource = new EventSource('/events');

                eventSource.onopen = function() {
                    document.getElementById('events').innerHTML += '<p style="color: green;">Connection opened!</p>';
                };

                eventSource.onmessage = function(event) {
                    const p = document.createElement('p');
                    p.textContent = `[Default Message] Data: ${event.data}, Last Event ID: ${event.lastEventId}`;
                    document.getElementById('events').appendChild(p);
                };

                eventSource.addEventListener('welcome', function(event) {
                    const p = document.createElement('p');
                    p.style.fontWeight = 'bold';
                    p.textContent = `[Welcome Event] Data: ${event.data}, Last Event ID: ${event.lastEventId}`;
                    document.getElementById('events').appendChild(p);
                });

                eventSource.addEventListener('update', function(event) {
                    const p = document.createElement('p');
                    p.style.color = 'blue';
                    p.textContent = `[Update Event] Data: ${event.data}, Last Event ID: ${event.lastEventId}`;
                    document.getElementById('events').appendChild(p);
                });

                eventSource.onerror = function(event) {
                    console.error('EventSource error:', event);
                    document.getElementById('events').innerHTML += '<p style="color: red;">Connection error!</p>';
                    eventSource.close();
                };

                // Example of sending an event from the client side (not directly related to SSE but common)
                // fetch('/api/some-action', { method: 'POST', body: '...' });
            </script>
        </body>
        </html>
    ]])
end)

-- Example of an API endpoint that might trigger an SSE broadcast (conceptual)
uws.post("/broadcast", function(req, res, data, last)
    if last then
        print("Received broadcast request: ", data)
        for sse_id, _ in pairs(active_sse_clients) do
            local success, err = uws.sse_send(sse_id, "Broadcast from API: " .. data, "api_broadcast")
            if not success then
                print("Failed to broadcast to " .. sse_id .. ": " .. err)
                -- Consider removing the client from active_sse_clients here if it consistently fails
            end
        end
        res:send("Broadcast initiated!")
    end
end)


uws.listen(3000)
uws.run()