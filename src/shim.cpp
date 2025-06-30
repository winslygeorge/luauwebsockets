#include <App.h>
#include <lua.hpp>
#include <iostream>
#include <unordered_map>
#include <memory>
#include <mutex>
#include <string_view>
#include <vector>
#include <functional>
#include <string> // For std::to_string
#include <sys/socket.h> // For sockaddr, sockaddr_storage
#include <netdb.h>
#include <random>     // Include for random number generation
#include <sstream>    // Include for stringstream
#include <iomanip>
#include <fstream>    // For file operations
#include <filesystem> // For path manipulation (C++17)


namespace fs = std::filesystem; // Alias for convenience

// For getnameinfo, NI_MAXHOST, NI_NUMERICHOST

static std::shared_ptr<uWS::App> app;
static lua_State *main_L = nullptr;
static std::mutex lua_mutex;
static std::unordered_map<int, int> lua_callbacks; // For general route callbacks
static int callback_id_counter = 0;

// --- SSE Specific Global State ---
// Map to store active SSE connections, identified by a unique ID
// Maps a generated SSE_ID to a shared_ptr to the HttpResponse object and the associated Lua callback ref
struct SseConnection {
    uWS::HttpResponse<false>* res; // Raw pointer, managed by uWS lifecycle
    int lua_ref; // Lua reference to the callback to send data
    bool is_aborted; // Flag to track if connection has been aborted
};
// Use a map to store active SSE connections for easy lookup and management
static std::unordered_map<std::string, std::shared_ptr<SseConnection>> active_sse_connections;
static std::mutex sse_connections_mutex; // Mutex for active_sse_connections map

// Function to generate a simple unique ID (re-using from original file)
std::string generate_unique_id(); // Forward declaration for use in uw_sse

struct DummyUserData {};

// Middleware structures
struct Middleware {
    int ref; // Lua function reference
    bool global; // true for global, false for route-specific
    std::string route; // Route for route-specific middleware
};

static std::vector<Middleware> middlewares;

struct LuaTimerData {
    lua_State* owner;
    lua_State* thread;
    int lua_ref;
    int timer_id;
    bool is_interval;
    bool is_cleared;
    std::vector<int> arg_refs; // <-- Add this line to store arguments
};



std::unordered_map<int, us_timer_t*> active_timers;
int timer_id_counter = 0;

// Helper to get app pointer for uWS::Loop
uWS::Loop *get_uWS_Loop() {
    // This is a bit of a hack, but uWS::Loop::get() returns the global loop instance.
    // The C++ uWS library is a wrapper around libusockets, and their Loop
    // object generally contains or is directly compatible with us_loop_t.
    // We assume the uWS::Loop* can be cast to us_loop_t* for libusockets functions.
    // This assumes uWS::Loop is essentially us_loop_t with C++ sugar.
    return uWS::Loop::get();
}


// Helper to push Lua table from C++ map
void push_map_to_lua(lua_State* L, const std::unordered_map<std::string, std::string>& m) {
    lua_newtable(L);
    for (const auto& pair : m) {
        lua_pushstring(L, pair.first.c_str());
        lua_pushstring(L, pair.second.c_str());
        lua_settable(L, -3);
    }
}


int uw_create_app(lua_State *L) {
    if (!app) {
        app = std::make_shared<uWS::App>();
        main_L = L;
    }
    lua_pushboolean(L, 1);
    return 1;
}

int create_req_userdata(lua_State *L, uWS::HttpRequest* req) {
    void *ud = lua_newuserdata(L, sizeof(uWS::HttpRequest*));
    uWS::HttpRequest** req_ptr = (uWS::HttpRequest**)ud;
    *req_ptr = req;

    luaL_getmetatable(L, "req");
    lua_setmetatable(L, -2);

    return 1;
}

int create_res_userdata(lua_State *L, uWS::HttpResponse<false>* res) {
    void *ud = lua_newuserdata(L, sizeof(uWS::HttpResponse<false>*));
    uWS::HttpResponse<false>** res_ptr = (uWS::HttpResponse<false>**)ud;
    *res_ptr = res;

    luaL_getmetatable(L, "res");
    lua_setmetatable(L, -2);

    return 1;
}

// New: create_sse_res_userdata - A distinct userdata for SSE responses
int create_sse_res_userdata(lua_State *L, uWS::HttpResponse<false>* res, const std::string& sse_id) {
    // We'll push a string ID to Lua, so Lua can call uw_sse_send with the ID
    lua_pushstring(L, sse_id.c_str());
    // Optionally, if you wanted a userdata for SSE, it would hold the ID, not the res pointer directly
    // For now, let's just pass the ID as a string, simpler for Lua to manage.
    return 1; // Pushed one value (the SSE ID string)
}


static int res_writeStatus(lua_State *L) {
    uWS::HttpResponse<false>** res = (uWS::HttpResponse<false>**)luaL_checkudata(L, 1, "res");
    int status = luaL_checkinteger(L, 2);
    (*res)->writeStatus(std::to_string(status).c_str());
    lua_pushvalue(L, 1); // Return self for chaining
    return 1;
}

static int res_getRemoteAddress(lua_State *L) {
    uWS::HttpResponse<false>** res = (uWS::HttpResponse<false>**)luaL_checkudata(L, 1, "res");
    std::string_view remoteAddress = (*res)->getRemoteAddress();
    lua_pushlstring(L, remoteAddress.data(), remoteAddress.length());
    return 1;
}

static int res_getProxiedRemoteAddress(lua_State *L) {
    uWS::HttpResponse<false>** res = (uWS::HttpResponse<false>**)luaL_checkudata(L, 1, "res");
    // In newer uWebSockets versions, you might need to check headers like X-Forwarded-For
    // For simplicity, let's just return the regular remote address for now.
    return res_getRemoteAddress(L);
}


static int res_closeConnection(lua_State *L) {
    uWS::HttpResponse<false>** res = (uWS::HttpResponse<false>**)luaL_checkudata(L, 1, "res");
    (*res)->close();
    return 0;
}

// Function to call a Lua callback with optional arguments
void call_lua_callback(int lua_ref, int num_args, std::function<void(lua_State*)> push_args) {
    std::lock_guard<std::mutex> lock(lua_mutex);
    lua_rawgeti(main_L, LUA_REGISTRYINDEX, lua_ref);
    if (lua_isfunction(main_L, -1)) {
        push_args(main_L);
        if (lua_pcall(main_L, num_args, 0, 0) != LUA_OK) {
            std::cerr << "Error calling Lua callback: " << lua_tostring(main_L, -1) << std::endl;
        }
    } else {
        std::cerr << "Lua callback reference is not a function." << std::endl;
        lua_pop(main_L, 1); // Pop the non-function value
    }
}


// --- Timer functions ---

// --- Timer functions ---

void libus_timer_callback(struct us_timer_t *timer) {
    LuaTimerData* data = (LuaTimerData*)us_timer_ext(timer);
    if (!data || data->is_cleared) {
        if (data) {
            // us_timer_close(timer);
            active_timers.erase(data->timer_id);
        }
        return;
    }

    std::lock_guard<std::mutex> lock(lua_mutex);

    lua_State* L = data->thread;

    lua_rawgeti(L, LUA_REGISTRYINDEX, data->lua_ref); // Push Lua function

    if (!lua_isfunction(L, -1)) {
        std::cerr << "[TimerCallback] Timer ID " << data->timer_id << ": not a function\n";
        lua_pop(L, 1);
        return;
    }

    // Push context table(s) from arg_refs
    int arg_count = 0;
    for (int ref : data->arg_refs) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, ref); // push context table or additional args
        ++arg_count;
    }

    int status = lua_resume(L, arg_count);

    if (status != LUA_OK && status != LUA_YIELD) {
        const char* err = lua_tostring(L, -1);
        std::cerr << "[TimerCallback] Timer ID " << data->timer_id << " error: "
                  << (err ? err : "unknown") << "\n";
        lua_pop(L, 1); // remove error message
    }

    if (!data->is_interval) {
        // One-time timeout: clean everything
        data->is_cleared = true;

        luaL_unref(data->owner, LUA_REGISTRYINDEX, data->lua_ref);
        for (int ref : data->arg_refs) {
            luaL_unref(data->owner, LUA_REGISTRYINDEX, ref);
        }

        // us_timer_close(timer);
        active_timers.erase(data->timer_id);
    }
}



// void libus_timer_callback(struct us_timer_t *timer) {
//     LuaTimerData* data = (LuaTimerData*)us_timer_ext(timer);
//     if (!data || data->is_cleared) {
//         if (data) {
//             us_timer_close(timer);
//             active_timers.erase(data->timer_id);
//         }
//         return;
//     }

//     std::lock_guard<std::mutex> lock(lua_mutex);

//     if (data->is_interval) {
//         // INTERVAL: create new coroutine, run stored function
//         lua_State* thread = lua_newthread(data->owner);
//         lua_rawgeti(thread, LUA_REGISTRYINDEX, data->lua_ref);
//         if (!lua_isfunction(thread, -1)) {
//             std::cerr << "[TimerCallback] Interval ref is not function\n";
//             lua_pop(thread, 1);
//             return;
//         }

//         int status = lua_resume(thread, 0);
//         if (status != LUA_OK && status != LUA_YIELD) {
//             std::cerr << "[TimerCallback] Interval error: " << (lua_isstring(thread, -1) ? lua_tostring(thread, -1) : "unknown") << "\n";
//             lua_pop(thread, 1);
//         }

//     } else {
//         // TIMEOUT: resume coroutine once
//         lua_rawgeti(data->owner, LUA_REGISTRYINDEX, data->lua_ref);

//         if (!lua_isfunction(data->owner, -1)) {
//             std::cerr << "[TimerCallback] Timeout ref is not function\n";
//             lua_pop(data->owner, 1);
//             return;
//         }
//         int status = lua_resume(data->owner, 0);
//         if (status != LUA_OK && status != LUA_YIELD) {
//             std::cerr << "[TimerCallback] Timeout error: " << (lua_isstring(data->owner, -1) ? lua_tostring(data->owner, -1) : "unknown") << "\n";
//             lua_pop(data->owner, 1);
//         }

//         data->is_cleared = true;
//         luaL_unref(data->owner, LUA_REGISTRYINDEX, data->lua_ref);
//         active_timers.erase(data->timer_id);
//     }
// }






// --- Timer functions ---



// --- Timer functions ---
int uw_setTimeout(lua_State *L) {
    luaL_checktype(L, 1, LUA_TFUNCTION);
    long long delay_ms = luaL_checkinteger(L, 2);
    int nargs = lua_gettop(L) - 2;

    std::lock_guard<std::mutex> lock(lua_mutex);

    int timer_id = timer_id_counter++;

    lua_State* thread = lua_newthread(L);
    lua_pushvalue(L, 1);           // Push the function
    lua_xmove(L, thread, 1);       // Move to new thread
    int fn_ref = luaL_ref(thread, LUA_REGISTRYINDEX);

    us_loop_t* loop = (us_loop_t *)uWS::Loop::get();
    us_timer_t* timer = us_create_timer(loop, 1, sizeof(LuaTimerData));
    if (!timer) {
        luaL_unref(thread, LUA_REGISTRYINDEX, fn_ref);
        lua_pushnil(L);
        return 1;
    }

    LuaTimerData* data = (LuaTimerData*)us_timer_ext(timer);
    memset(data, 0, sizeof(LuaTimerData));
    data->thread = thread;
    data->owner = thread;
    data->lua_ref = fn_ref;
    data->timer_id = timer_id;
    data->is_interval = false;
    data->is_cleared = false;

    // Create context table
    lua_newtable(L);                                  // ctx
    lua_pushstring(L, "id"); lua_pushinteger(L, timer_id); lua_settable(L, -3);
    lua_pushstring(L, "type"); lua_pushstring(L, "timeout"); lua_settable(L, -3);
    lua_pushstring(L, "delay_ms"); lua_pushinteger(L, delay_ms); lua_settable(L, -3);

    // Capture additional arguments
    if (nargs > 0) {
        lua_pushstring(L, "args");
        lua_newtable(L);  // args table
        for (int i = 0; i < nargs; i++) {
            lua_pushinteger(L, i + 1);
            lua_pushvalue(L, 3 + i);
            lua_settable(L, -3);
        }
        lua_settable(L, -3); // ctx.args = { ... }
    }

    data->arg_refs.push_back(luaL_ref(L, LUA_REGISTRYINDEX));  // ref to ctx table

    us_timer_set(timer, libus_timer_callback, delay_ms, 0);
    active_timers[timer_id] = timer;

    lua_pushinteger(L, timer_id);
    return 1;
}


int uw_setInterval(lua_State *L) {
    luaL_checktype(L, 1, LUA_TFUNCTION);
    long long interval_ms = luaL_checkinteger(L, 2);
    int nargs = lua_gettop(L) - 2;

    std::lock_guard<std::mutex> lock(lua_mutex);

    int timer_id = timer_id_counter++;

    lua_State* thread = lua_newthread(L); // Isolated coroutine
    lua_pushvalue(L, 1);                  // Push function
    lua_xmove(L, thread, 1);              // Move to new thread
    int fn_ref = luaL_ref(thread, LUA_REGISTRYINDEX); // Ref in new thread

    us_loop_t* loop = (us_loop_t *)uWS::Loop::get();
    us_timer_t* timer = us_create_timer(loop, 1, sizeof(LuaTimerData));
    if (!timer) {
        luaL_unref(thread, LUA_REGISTRYINDEX, fn_ref);
        lua_pushnil(L);
        return 1;
    }

    LuaTimerData* data = (LuaTimerData*)us_timer_ext(timer);
    memset(data, 0, sizeof(LuaTimerData));
    data->owner = thread;
    data->thread = thread;
    data->lua_ref = fn_ref;
    data->timer_id = timer_id;
    data->is_interval = true;
    data->is_cleared = false;

    // Build ctx table
    lua_newtable(L);                                  // ctx
    lua_pushstring(L, "id"); lua_pushinteger(L, timer_id); lua_settable(L, -3);
    lua_pushstring(L, "type"); lua_pushstring(L, "interval"); lua_settable(L, -3);
    lua_pushstring(L, "interval_ms"); lua_pushinteger(L, interval_ms); lua_settable(L, -3);

    if (nargs > 0) {
        lua_pushstring(L, "args");
        lua_newtable(L);
        for (int i = 0; i < nargs; i++) {
            lua_pushinteger(L, i + 1);
            lua_pushvalue(L, 3 + i);
            lua_settable(L, -3);
        }
        lua_settable(L, -3); // ctx.args = { ... }
    }

    data->arg_refs.push_back(luaL_ref(L, LUA_REGISTRYINDEX)); // ref ctx table in main state (L)

    us_timer_set(timer, libus_timer_callback, interval_ms, interval_ms);
    active_timers[timer_id] = timer;

    std::cout << "[setInterval] Created isolated interval ID: " << timer_id << "\n";

    lua_pushinteger(L, timer_id);
    return 1;
}





int uw_clearTimer(lua_State *L) {
    int timer_id = luaL_checkinteger(L, 1);

    std::lock_guard<std::mutex> lock(lua_mutex);
    auto it = active_timers.find(timer_id);
    if (it != active_timers.end()) {
        us_timer_t* timer = it->second;
        LuaTimerData *data = (LuaTimerData *)us_timer_ext(timer);
        if (data && !data->is_cleared) {
            data->is_cleared = true;
            luaL_unref(data->owner, LUA_REGISTRYINDEX, data->lua_ref);
            us_timer_set(timer, nullptr, 0, 0);  // Disarm before closing
            us_timer_close(timer);
        }
        active_timers.erase(it);
        std::cout << "[clearTimer] Cleared timer ID: " << timer_id << "\n";
        lua_pushboolean(L, 1);
    } else {
        std::cerr << "[clearTimer] Timer ID " << timer_id << " not found.\n";
        lua_pushboolean(L, 0);
    }
    return 1;
}



// User data structure for WebSocket
struct WebSocketUserData {
    std::string id;
};

static int websocket_send(lua_State *L) {
    void *ud = luaL_checkudata(L, 1, "websocket");
    if (!ud) {
        luaL_error(L, "Invalid WebSocket object");
        return 0;
    }
    uWS::WebSocket<false, true, WebSocketUserData>* ws = *(uWS::WebSocket<false, true, WebSocketUserData>**)ud;
    const char *message = luaL_checkstring(L, 2);
    size_t len = lua_objlen(L, 2); // Correct way to get length for string
    uWS::OpCode opCodeToSend = uWS::OpCode::TEXT; // Default to text

    if (lua_gettop(L) > 2 && lua_isstring(L, 3)) {
        const char *type = luaL_checkstring(L, 3);
        if (strcmp(type, "binary") == 0) {
            opCodeToSend = uWS::OpCode::BINARY;
        }
    }

    if (ws) {
        ws->send(std::string_view(message, len), opCodeToSend);
        lua_pushboolean(L, 1);
        return 1;
    } else {
        luaL_error(L, "Invalid WebSocket object");
        return 0;
    }
}

static int websocket_close(lua_State *L) {
    void *ud = luaL_checkudata(L, 1, "websocket");
    if (!ud) {
        luaL_error(L, "Invalid WebSocket object");
        return 0;
    }
    uWS::WebSocket<false, true, DummyUserData>* ws = *(uWS::WebSocket<false, true, DummyUserData>**)ud;
    if (ws) {
        ws->close(); // Call close with no arguments
        lua_pushboolean(L, 1);
        return 1;
    } else {
        luaL_error(L, "Invalid WebSocket object");
        return 0;
    }
}

static void create_websocket_metatable(lua_State *L) {
    luaL_newmetatable(L, "websocket");
    lua_pushstring(L, "__index");
    lua_newtable(L); // Metatable for methods
    lua_pushcfunction(L, websocket_send);
    lua_setfield(L, -2, "send");
    lua_pushcfunction(L, websocket_close);
    lua_setfield(L, -2, "close");
    lua_settable(L, -3); // Set __index to the methods table
    lua_pop(L, 1); // Pop the metatable
}

static void create_metatables(lua_State *L) {
    create_websocket_metatable(L);
    luaL_newmetatable(L, "req");
    lua_pushstring(L, "__index");
    lua_pushcfunction(L, [](lua_State *L) -> int {
        uWS::HttpRequest** req = (uWS::HttpRequest**)luaL_checkudata(L, 1, "req");
        const char *key = luaL_checkstring(L, 2);
        if (strcmp(key, "method") == 0) {
            lua_pushstring(L, (*req)->getMethod().data());
            return 1;
        } else if (strcmp(key, "url") == 0) {
            lua_pushstring(L, (*req)->getUrl().data());
            return 1;
        } else if (strcmp(key, "query") == 0) {
            lua_pushstring(L, (*req)->getQuery().data());
            return 1;
        } else if (strcmp(key, "getHeader") == 0) {
            lua_pushcclosure(L, [](lua_State *L) -> int {
                uWS::HttpRequest** req = (uWS::HttpRequest**)luaL_checkudata(L, 1, "req");
                const char *header_name = luaL_checkstring(L, 2);
                std::string_view header_value = (*req)->getHeader(header_name);
                lua_pushlstring(L, header_value.data(), header_value.length());
                return 1;
            }, 0);
            return 1;
        } // Inside the lambda for the '__index' metamethod of 'req'
        else if (strcmp(key, "getUrl") == 0) {
            lua_pushcfunction(L, [](lua_State *L) -> int {
                uWS::HttpRequest** req = (uWS::HttpRequest**)luaL_checkudata(L, 1, "req");
                std::string_view url = (*req)->getUrl();
                lua_pushlstring(L, url.data(), url.length());
                return 1;
            });
            return 1;
        }

        lua_pushnil(L);
        return 1;
    });
    lua_settable(L, -3);
    lua_pop(L, 1);

    luaL_newmetatable(L, "res");
    lua_pushstring(L, "__index");
    lua_pushcfunction(L, [](lua_State *L) -> int {
        uWS::HttpResponse<false>** res = (uWS::HttpResponse<false>**)luaL_checkudata(L, 1, "res");
        const char *key = luaL_checkstring(L, 2);
        if (strcmp(key, "send") == 0) {
            lua_pushcclosure(L, [](lua_State *L) -> int {
                uWS::HttpResponse<false>** res = (uWS::HttpResponse<false>**)luaL_checkudata(L, 1, "res");
                const char *response = luaL_checkstring(L, 2);
                (*res)->end(response);
                return 0;
            }, 0);
            return 1;
        } else if (strcmp(key, "writeHeader") == 0) {
            lua_pushcclosure(L, [](lua_State *L) -> int {
                uWS::HttpResponse<false>** res = (uWS::HttpResponse<false>**)luaL_checkudata(L, 1, "res");
                const char *header = luaL_checkstring(L, 2);
                const char *value = luaL_checkstring(L, 3);
                (*res)->writeHeader(header, value);
                lua_pushvalue(L, 1);
                return 1;
            }, 0);
            return 1;
        } else if (strcmp(key, "writeStatus") == 0) {
            lua_pushcfunction(L, res_writeStatus);
            return 1;
        } else if (strcmp(key, "getRemoteAddress") == 0) {
            lua_pushcfunction(L, res_getRemoteAddress);
            return 1;
        } else if (strcmp(key, "getProxiedRemoteAddress") == 0) {
            lua_pushcfunction(L, res_getProxiedRemoteAddress);
            return 1;
        } else if (strcmp(key, "closeConnection") == 0) {
            lua_pushcfunction(L, res_closeConnection);
            return 1;
        }
        lua_pushnil(L);
        return 1;
    });
    lua_settable(L, -3);
    lua_pop(L, 1);
}

// Function to execute middleware
bool execute_middleware(lua_State *L, uWS::HttpResponse<false> *res, uWS::HttpRequest *req, const std::string& route) {
    for (const auto& mw : middlewares) {
        if (mw.global || mw.route == route) {
            lua_rawgeti(L, LUA_REGISTRYINDEX, mw.ref);
            create_req_userdata(L, req);
            create_res_userdata(L, res);
            if (lua_pcall(L, 2, 1, 0) != LUA_OK) {
                std::cerr << "Lua middleware error: " << lua_tostring(L, -1) << std::endl;
                lua_pop(L, 1);
                return false; // Middleware error, stop processing
            }

            if (lua_isboolean(L, -1) && !lua_toboolean(L, -1)) {
                lua_pop(L, 1); // Remove boolean return
                return false; // Middleware returned false, stop processing
            }
            lua_pop(L, 1); // Remove boolean return
        }
    }
    return true; // Continue processing if no middleware returned false
}

// Function to add middleware
int uw_use(lua_State *L) {
    luaL_checktype(L, 1, LUA_TFUNCTION);
    lua_pushvalue(L, 1);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);

    Middleware mw;
    mw.ref = ref;
    mw.global = true;

    if (lua_gettop(L) > 1 && lua_isstring(L, 2)) {
        mw.global = false;
        mw.route = luaL_checkstring(L, 2);
    }
    middlewares.push_back(mw);
    lua_pushboolean(L, 1);
    return 1;
}

int uw_get(lua_State *L) {
    const char *route = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    lua_pushvalue(L, 2);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    int callback_id = callback_id_counter++;
    lua_callbacks[callback_id] = ref;

    app->get(route, [callback_id, route](auto *res, auto *req) {
        std::lock_guard<std::mutex> lock(lua_mutex);
        if (!execute_middleware(main_L, res, req, route)) return;

        lua_rawgeti(main_L, LUA_REGISTRYINDEX, lua_callbacks[callback_id]);
        create_req_userdata(main_L, req);
        create_res_userdata(main_L, res);

        if (lua_pcall(main_L, 2, 0, 0) != LUA_OK) {
            std::cerr << "Lua error: " << lua_tostring(main_L, -1) << std::endl;
            lua_pop(main_L, 1);
            res->writeHeader("Content-Type", "text/plain")->end("Internal Server Error");
        }
    });

    lua_pushboolean(L, 1);
    return 1;
}

int uw_post(lua_State *L) {
    const char *route = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    lua_pushvalue(L, 2);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    int callback_id = callback_id_counter++;
    lua_callbacks[callback_id] = ref;

    app->post(route, [callback_id, route](uWS::HttpResponse<false> *res_uws, uWS::HttpRequest *req_uws) {
        // std::cerr << "uw_post handler called. res_uws: " << res_uws << ", req_uws: " << req_uws << std::endl;
        if(res_uws){
            res_uws->onData([callback_id, res_uws, req_uws, route](std::string_view data, bool last) mutable {
                std::lock_guard<std::mutex> lock(lua_mutex);
                if (!execute_middleware(main_L, res_uws, req_uws, route)) return;

                lua_rawgeti(main_L, LUA_REGISTRYINDEX, lua_callbacks[callback_id]);
                create_req_userdata(main_L, req_uws);
                create_res_userdata(main_L, res_uws);
                lua_pushlstring(main_L, data.data(), data.size());
                lua_pushboolean(main_L, last);

                if (lua_pcall(main_L, 4, 0, 0) != LUA_OK) {
                    std::cerr << "Lua error in POST handler: " << lua_tostring(main_L, -1) << std::endl;
                    lua_pop(main_L, 1);
                    res_uws->writeHeader("Content-Type", "text/plain")->end("Internal Server Error");
                }
            });

            res_uws->onAborted([]() {
                std::cerr << "POST request aborted" << std::endl;
            });

        }else{
            std::cerr << "Error: res_uws is NULL in POST handler!" << std::endl;
        }
    });
    lua_pushboolean(L, 1);
    return 1;
}

int uw_put(lua_State *L) {
    const char *route = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    lua_pushvalue(L, 2);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    int callback_id = callback_id_counter++;
    lua_callbacks[callback_id] = ref;

    app->put(route, [callback_id, route](uWS::HttpResponse<false> *res_uws, uWS::HttpRequest *req_uws) {
        if (res_uws) {
            std::shared_ptr<std::string> body = std::make_shared<std::string>();

            res_uws->onData([callback_id, res_uws, req_uws, route, body](std::string_view data, bool last) mutable {
                body->append(data.data(), data.size());

                if (last) {
                    std::lock_guard<std::mutex> lock(lua_mutex);
                    if (!execute_middleware(main_L, res_uws, req_uws, route)) return;

                    lua_rawgeti(main_L, LUA_REGISTRYINDEX, lua_callbacks[callback_id]);
                    create_req_userdata(main_L, req_uws);
                    create_res_userdata(main_L, res_uws);
                    lua_pushlstring(main_L, data.data(), data.size()); // This passes the *last* chunk, not the full body
                    lua_pushboolean(main_L, last);

                    if (lua_pcall(main_L, 4, 0, 0) != LUA_OK) {
                        std::cerr << "Lua error in PUT handler: " << lua_tostring(main_L, -1) << std::endl;
                        lua_pop(main_L, 1);
                        res_uws->writeHeader("Content-Type", "text/plain")->end("Internal Server Error");
                    }
                }
            });

            res_uws->onAborted([]() {
                std::cerr << "PUT request aborted" << std::endl;
            });

        } else {
            std::cerr << "Error: res_uws is NULL in PUT handler!" << std::endl;
        }
    });

    lua_pushboolean(L, 1);
    return 1;
}


int uw_delete(lua_State *L) {
    const char *route = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    lua_pushvalue(L, 2);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    int callback_id = callback_id_counter++;
    lua_callbacks[callback_id] = ref;

    app->del(route, [callback_id, route](uWS::HttpResponse<false> *res_uws, uWS::HttpRequest *req_uws) {
        std::lock_guard<std::mutex> lock(lua_mutex);
        if (!execute_middleware(main_L, res_uws, req_uws, route)) return;

        lua_rawgeti(main_L, LUA_REGISTRYINDEX, lua_callbacks[callback_id]);
        create_req_userdata(main_L, req_uws);
        create_res_userdata(main_L, res_uws);

        if (lua_pcall(main_L, 2, 0, 0) != LUA_OK) {
            std::cerr << "Lua error in DELETE handler: " << lua_tostring(main_L, -1) << std::endl;
            lua_pop(main_L, 1);
            res_uws->writeHeader("Content-Type", "text/plain")->end("Internal Server Error");
        }
    });
    lua_pushboolean(L, 1);
    return 1;
}

int uw_patch(lua_State *L) {
    const char *route = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    lua_pushvalue(L, 2);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    int callback_id = callback_id_counter++;
    lua_callbacks[callback_id] = ref;

    app->patch(route, [callback_id, route](uWS::HttpResponse<false> *res_uws, uWS::HttpRequest *req_uws) {
        std::string body;
        res_uws->onData([callback_id, res_uws, &body, req_uws, route](std::string_view data, bool last) mutable {
            body.append(data.data(), data.size());
            if (last) {
                std::lock_guard<std::mutex> lock(lua_mutex);
                if (!execute_middleware(main_L, res_uws, req_uws, route)) return;

                lua_rawgeti(main_L, LUA_REGISTRYINDEX, lua_callbacks[callback_id]);
                create_req_userdata(main_L, req_uws);
                create_res_userdata(main_L, res_uws);
                lua_pushlstring(main_L, body.data(), body.size());

                if (lua_pcall(main_L, 3, 0, 0) != LUA_OK) {
                    std::cerr << "Lua error in PATCH handler: " << lua_tostring(main_L, -1) << std::endl;
                    lua_pop(main_L, 1);
                    res_uws->writeHeader("Content-Type", "text/plain")->end("Internal Server Error");
                }
            }
        });
        res_uws->onAborted([]() {
            std::cerr << "PATCH request aborted" << std::endl;
        });
    });
    lua_pushboolean(L, 1);
    return 1;
}

int uw_head(lua_State *L) {
    const char *route = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    lua_pushvalue(L, 2);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    int callback_id = callback_id_counter++;
    lua_callbacks[callback_id] = ref;

    app->head(route, [callback_id, route](uWS::HttpResponse<false> *res_uws, uWS::HttpRequest *req_uws) {
        std::lock_guard<std::mutex> lock(lua_mutex);
        if (!execute_middleware(main_L, res_uws, req_uws, route)) return;

        lua_rawgeti(main_L, LUA_REGISTRYINDEX, lua_callbacks[callback_id]);
        create_req_userdata(main_L, req_uws);
        create_res_userdata(main_L, res_uws);

        if (lua_pcall(main_L, 2, 0, 0) != LUA_OK) {
            std::cerr << "Lua error in HEAD handler: " << lua_tostring(main_L, -1) << std::endl;
            lua_pop(main_L, 1);
            res_uws->writeHeader("Content-Type", "text/plain")->end("Internal Server Error");
        }
    });
    lua_pushboolean(L, 1);
    return 1;
}

int uw_options(lua_State *L) {
    const char *route = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    lua_pushvalue(L, 2);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    int callback_id = callback_id_counter++;
    lua_callbacks[callback_id] = ref;

    app->options(route, [callback_id, route](uWS::HttpResponse<false> *res_uws, uWS::HttpRequest *req_uws) {
        std::lock_guard<std::mutex> lock(lua_mutex);
        if (!execute_middleware(main_L, res_uws, req_uws, route)) return;

        lua_rawgeti(main_L, LUA_REGISTRYINDEX, lua_callbacks[callback_id]);
        create_req_userdata(main_L, req_uws);
        create_res_userdata(main_L, res_uws);

        if (lua_pcall(main_L, 2, 0, 0) != LUA_OK) {
            std::cerr << "Lua error in OPTIONS handler: " << lua_tostring(main_L, -1) << std::endl;
            lua_pop(main_L, 1);
            res_uws->writeHeader("Content-Type", "text/plain")->end("Internal Server Error");
        }
    });
    lua_pushboolean(L, 1);
    return 1;
}

// Function to generate a simple unique ID
std::string generate_unique_id() {
    static std::random_device rd;
    static std::mt19937 gen(rd());
    static std::uniform_int_distribution<> distrib(0, 15);

    std::stringstream ss;
    for (int i = 0; i < 16; ++i) {
        int val = distrib(gen);
        ss << std::hex << val;
        if (i == 3 || i == 5 || i == 7 || i == 9) {
            ss << "-";
        }
    }
    return ss.str();
}

// Lua function to get the WebSocket ID
static int websocket_get_id(lua_State *L) {
    uWS::WebSocket<false, true, WebSocketUserData>** ws_ud = static_cast<uWS::WebSocket<false, true, WebSocketUserData>**>(luaL_checkudata(L, 1, "websocket"));
    if (!ws_ud || !*ws_ud) {
        luaL_error(L, "invalid websocket userdata");
        return 0;
    }

    lua_pushstring(L, (*ws_ud)->getUserData()->id.c_str());
    return 1;
}

int uw_ws(lua_State *L) {
    const char *route = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    lua_pushvalue(L, 2);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    int callback_id = callback_id_counter++;
    lua_callbacks[callback_id] = ref;

    app->ws<WebSocketUserData>(route, {
        .open = [callback_id, route](auto *ws) {
            std::lock_guard<std::mutex> lock(lua_mutex);
            lua_rawgeti(main_L, LUA_REGISTRYINDEX, lua_callbacks[callback_id]);

            // Push the WebSocket userdata and set its metatable
            uWS::WebSocket<false, true, WebSocketUserData>** ws_ud = static_cast<uWS::WebSocket<false, true, WebSocketUserData>**>(lua_newuserdata(main_L, sizeof(uWS::WebSocket<false, true, WebSocketUserData>*)));
            *ws_ud = ws;
            luaL_getmetatable(main_L, "websocket");
            lua_setmetatable(main_L, -2);

            // Generate and store the unique ID in the user data
            ws->getUserData()->id = generate_unique_id();

            lua_pushstring(main_L, "open");

            if (lua_pcall(main_L, 2, 0, 0) != LUA_OK) {
                std::cerr << "Lua error (open): " << lua_tostring(main_L, -1) << std::endl;
                lua_pop(main_L, 1);
            }
        },

        .message = [callback_id](auto *ws, std::string_view message, uWS::OpCode opCode) {
            std::cout << "uWS opcode enum value: " << static_cast<int>(opCode) << std::endl;
            std::lock_guard<std::mutex> lock(lua_mutex);
            lua_rawgeti(main_L, LUA_REGISTRYINDEX, lua_callbacks[callback_id]);

            // Push the WebSocket userdata with metatable
            uWS::WebSocket<false, true, WebSocketUserData>** ws_ud = static_cast<uWS::WebSocket<false, true, WebSocketUserData>**>(lua_newuserdata(main_L, sizeof(uWS::WebSocket<false, true, WebSocketUserData>*)));
            *ws_ud = ws;
            luaL_getmetatable(main_L, "websocket");
            lua_setmetatable(main_L, -2);

            lua_pushstring(main_L, "message");
            lua_pushlstring(main_L, message.data(), message.size());
            lua_pushinteger(main_L, static_cast<int>(opCode));


            if (lua_pcall(main_L, 4, 0, 0) != LUA_OK) {
                std::cerr << "Lua error (message): " << lua_tostring(main_L, -1) << std::endl;
                lua_pop(main_L, 1);
            }
        },

        .close = [callback_id](auto *ws, int code, std::string_view message) {
            std::lock_guard<std::mutex> lock(lua_mutex);
            lua_rawgeti(main_L, LUA_REGISTRYINDEX, lua_callbacks[callback_id]);

            // Push the WebSocket userdata with metatable
            uWS::WebSocket<false, true, WebSocketUserData>** ws_ud = static_cast<uWS::WebSocket<false, true, WebSocketUserData>**>(lua_newuserdata(main_L, sizeof(uWS::WebSocket<false, true, WebSocketUserData>*)));
            *ws_ud = ws;
            luaL_getmetatable(main_L, "websocket");
            lua_setmetatable(main_L, -2);

            lua_pushstring(main_L, "close");
            lua_pushinteger(main_L, code);
            lua_pushlstring(main_L, message.data(), message.size());

            if (lua_pcall(main_L, 4, 0, 0) != LUA_OK) {
                std::cerr << "Lua error (close): " << lua_tostring(main_L, -1) << std::endl;
                lua_pop(main_L, 1);
            }
        }
    });

    // Register the get_id function in the "websocket" metatable
    luaL_getmetatable(L, "websocket");
    lua_pushcfunction(L, websocket_get_id);
    lua_setfield(L, -2, "get_id");
    lua_pop(L, 1); // Pop the metatable

    lua_pushboolean(L, 1);
    return 1;
}

// Helper function to get MIME type (assuming it's correct)
std::string get_mime_type(const std::string& filepath) {
    fs::path p(filepath);
    std::string ext = p.extension().string();
    if (ext == ".html" || ext == ".htm") return "text/html";
    if (ext == ".css") return "text/css";
    if (ext == ".js") return "application/javascript";
    if (ext == ".json") return "application/json";
    if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
    if (ext == ".png") return "image/png";
    if (ext == ".gif") return "image/gif";
    if (ext == ".svg") return "image/svg+xml";
    if (ext == ".ico") return "image/x-icon";
    if (ext == ".pdf") return "application/pdf";
    if (ext == ".txt") return "text/plain";
    return "application/octet-stream";
}

// New function to serve static files
int uw_serve_static(lua_State *L) {
    const char *route_prefix = luaL_checkstring(L, 1);
    const char *dir_path = luaL_checkstring(L, 2);

    std::cout << "[uw_serve_static] Registering static route: " << route_prefix
              << " serving from directory: " << dir_path << std::endl;

    if (!fs::is_directory(dir_path)) {
        std::cerr << "ERROR: Static file directory '" << dir_path << "' does not exist or is not a directory." << std::endl;
        lua_pushboolean(L, 0);
        return 1;
    }

    std::string route_pattern = std::string(route_prefix) + "/*";

    app->get(route_pattern.c_str(), [dir_path_str = std::string(dir_path), route_prefix_str = std::string(route_prefix)](auto *res, auto *req) {
        std::string_view url = req->getUrl();
        std::cout << "[Request] Incoming URL: " << url << std::endl;

        std::string file_path_suffix = std::string(url.substr(route_prefix_str.length()));
        if (!file_path_suffix.empty() && file_path_suffix[0] == '/') {
            file_path_suffix.erase(0, 1);
        }
        std::cout << "[Request] Extracted file path suffix: " << file_path_suffix << std::endl;

        if (file_path_suffix.find("..") != std::string::npos) {
            std::cerr << "WARNING: Directory traversal attempt detected for: " << file_path_suffix << std::endl;
            res->writeStatus("403 Forbidden")->end("Forbidden");
            return;
        }

        fs::path full_path = fs::path(dir_path_str) / file_path_suffix;
        std::cout << "[File Path] Constructed full path: " << full_path.string() << std::endl;

        if (fs::is_directory(full_path)) {
            std::cout << "[File Path] Path is a directory, appending index.html." << std::endl;
            full_path /= "index.html";
        }

        if (fs::exists(full_path) && fs::is_regular_file(full_path)) {
            auto file_stream_ptr = std::make_shared<std::ifstream>(full_path, std::ios::binary);

            if (file_stream_ptr->is_open()) {
                file_stream_ptr->seekg(0, std::ios::end);
                size_t file_size = file_stream_ptr->tellg();
                file_stream_ptr->seekg(0, std::ios::beg);

                std::cout << "[File Open] File '" << full_path.string() << "' opened successfully. Size: " << file_size << " bytes." << std::endl;

                std::string mime_type = get_mime_type(full_path.string());
                res->writeHeader("Content-Type", mime_type);
                res->writeHeader("Content-Length", std::to_string(file_size));
                std::cout << "[Headers] Set Content-Type: " << mime_type << ", Content-Length: " << file_size << std::endl;

                // Determine a reasonable max chunk size for a single write to avoid onWritable for small files
                // UWS_MAX_SENDFILE_SIZE is not directly exposed to userland code like this.
                // A common buffer size or even the system's socket buffer size can be a guide.
                // Let's use 64KB as a heuristic for a "small" file that can be sent in one go.
                const size_t SMALL_FILE_THRESHOLD = 64 * 1024; // 64 KB

                if (file_size <= SMALL_FILE_THRESHOLD) {
                    // Read the entire small file into a buffer and send it all at once
                    std::vector<char> buffer(file_size);
                    if (!file_stream_ptr->read(buffer.data(), file_size)) {
                        std::cerr << "ERROR: Failed to read entire small file: " << full_path.string() << std::endl;
                        res->writeStatus("500 Internal Server Error")->end("File Read Error");
                        return;
                    }
                    std::cout << "[Small File] Sending entire file (" << file_size << " bytes) in one go." << std::endl;
                    res->end(std::string_view(buffer.data(), file_size));
                    // No need for onWritable or explicit res->write, res->end(data) sends it all.
                } else {
                    // File is large, use onWritable for chunking
                    auto buffer_ptr = std::make_shared<std::vector<char>>(16 * 1024); // 16KB buffer for chunks

                    res->onWritable([res, file_stream_ptr, buffer_ptr, remaining_bytes_captured = file_size](int offset) mutable {
                        std::cout << "[onWritable] Called. Offset: " << offset << ", Remaining bytes (before this chunk): " << remaining_bytes_captured << std::endl;

                        size_t chunk_to_read = std::min((size_t)offset, remaining_bytes_captured);

                        if (chunk_to_read == 0) {
                            std::cout << "[onWritable] No bytes to read in this chunk (chunk_to_read is 0)." << std::endl;
                            if (remaining_bytes_captured == 0) {
                                std::cout << "[onWritable] All bytes sent, calling res->end()." << std::endl;
                                res->end(); // Important: End the response here if all is sent
                            }
                            return true; // Indicate that no more data is immediately available to write
                        }

                        if (!file_stream_ptr->read(buffer_ptr->data(), chunk_to_read)) {
                            std::cerr << "ERROR: [onWritable] File read error or EOF unexpectedly! File: (Check if stream is still valid: " << file_stream_ptr->good() << ")" << std::endl;
                            res->end(); // Attempt to gracefully close the response on read error
                            return true; // Stop writing
                        }

                        remaining_bytes_captured -= chunk_to_read;
                        res->write(std::string_view(buffer_ptr->data(), chunk_to_read));
                        std::cout << "[onWritable] Wrote " << chunk_to_read << " bytes. Remaining: " << remaining_bytes_captured << std::endl;

                        if (remaining_bytes_captured == 0) {
                            std::cout << "[onWritable] Last chunk sent, calling res->end()." << std::endl;
                            res->end(); // Important: End the response once all data is sent
                        }
                        return remaining_bytes_captured == 0; // Return true if done, false otherwise
                    })->onAborted([file_stream_ptr, full_path_str = full_path.string()]() {
                        std::cerr << "WARNING: Static file transfer aborted for '" << full_path_str << "'." << std::endl;
                    });

                    // Send the first chunk for large files
                    size_t first_chunk_size = std::min(buffer_ptr->size(), file_size);
                    if (first_chunk_size > 0) {
                        std::cout << "[Initial Write] Sending first chunk of size: " << first_chunk_size << " for large file." << std::endl;
                        if (!file_stream_ptr->read(buffer_ptr->data(), first_chunk_size)) {
                             std::cerr << "ERROR: [Initial Write] File read error for first chunk of large file! File: " << full_path.string() << std::endl;
                             res->end();
                             return;
                        }
                        res->write(std::string_view(buffer_ptr->data(), first_chunk_size));
                    }
                    std::cout << "[Initial Write] More data to send. onWritable will continue for large file." << std::endl;
                }

            } else {
                std::cerr << "ERROR: Could not open file for reading: " << full_path.string() << std::endl;
                res->writeStatus("500 Internal Server Error")->end("Could not open file.");
            }
        } else {
            std::cout << "[File Check] File not found or not a regular file: " << full_path.string() << std::endl;
            res->writeStatus("404 Not Found")->end("Not Found");
        }
    });

    lua_pushboolean(L, 1);
    return 1;
}

// --- SSE Implementation ---

// Lua callable function to send an SSE event
// Expected usage: uwebsockets.sse_send(sse_id, data, event_name_optional, id_optional)
int uw_sse_send(lua_State *L) {
    const char* sse_id = luaL_checkstring(L, 1);
    const char* data = luaL_checkstring(L, 2);
    const char* event_name = lua_isstring(L, 3) ? luaL_checkstring(L, 3) : nullptr;
    const char* id = lua_isstring(L, 4) ? luaL_checkstring(L, 4) : nullptr;

    std::lock_guard<std::mutex> lock(sse_connections_mutex); // Lock access to the map

    auto it = active_sse_connections.find(sse_id);
    if (it == active_sse_connections.end() || it->second->is_aborted) {
        std::cerr << "SSE Connection with ID '" << sse_id << "' not found or aborted. Cannot send message." << std::endl;
        lua_pushboolean(L, 0); // Indicate failure
        lua_pushstring(L, "SSE connection not found or aborted.");
        return 2;
    }

    uWS::HttpResponse<false>* res = it->second->res;

    // Construct the SSE message
    std::string sse_message;
    if (id) {
        sse_message += "id: ";
        sse_message += id;
        sse_message += "\n";
    }
    if (event_name) {
        sse_message += "event: ";
        sse_message += event_name;
        sse_message += "\n";
    }
    sse_message += "data: ";
    sse_message += data;
    sse_message += "\n\n"; // Two newlines to terminate the event

    res->write(sse_message); // Send the data

    lua_pushboolean(L, 1); // Indicate success
    return 1;
}

// Lua callable function to close an SSE connection
// Expected usage: uwebsockets.sse_close(sse_id)
int uw_sse_close(lua_State *L) {
    const char* sse_id = luaL_checkstring(L, 1);

    std::lock_guard<std::mutex> lock(sse_connections_mutex);

    auto it = active_sse_connections.find(sse_id);
    if (it != active_sse_connections.end()) {
        if (!it->second->is_aborted) {
            it->second->res->end(); // Gracefully close the HTTP response
            it->second->is_aborted = true; // Mark as aborted
            // The onAborted callback will handle removal from the map
            std::cout << "SSE Connection with ID '" << sse_id << "' explicitly closed by Lua." << std::endl;
        } else {
            std::cout << "SSE Connection with ID '" << sse_id << "' already aborted/closed." << std::endl;
        }
        lua_pushboolean(L, 1); // Indicate success (or that it was already closed)
        return 1;
    } else {
        std::cerr << "SSE Connection with ID '" << sse_id << "' not found for closing." << std::endl;
        lua_pushboolean(L, 0); // Indicate failure
        lua_pushstring(L, "SSE connection not found.");
        return 2;
    }
}


// Lua callable function to set up an SSE route
// Expected usage: uwebsockets.sse("/my-events", function(req, sse_conn_id) ... end)
// The Lua callback receives req and the sse_conn_id (string) to manage the connection.
int uw_sse(lua_State *L) {
    const char *route = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    lua_pushvalue(L, 2); // Push the Lua callback function onto the stack
    int ref = luaL_ref(L, LUA_REGISTRYINDEX); // Get a reference to the Lua function

    app->get(route, [ref, route](uWS::HttpResponse<false> *res, uWS::HttpRequest *req) {
        std::lock_guard<std::mutex> lock(lua_mutex);
        if (!execute_middleware(main_L, res, req, route)) {
            // If middleware aborts, ensure the response is ended and headers not set for SSE
            res->writeStatus("403 Forbidden")->end("Forbidden by middleware");
            return;
        }

        // Generate a unique ID for this SSE connection
        std::string sse_id = generate_unique_id();
        std::cout << "New SSE connection established: " << sse_id << " for route: " << route << std::endl;

        // Set SSE specific headers
        res->writeHeader("Content-Type", "text/event-stream");
        res->writeHeader("Cache-Control", "no-cache");
        res->writeHeader("Connection", "keep-alive");
        // Optional: CORS headers if needed
        // res->writeHeader("Access-Control-Allow-Origin", "*");

        // Store the SseConnection in the global map
        auto sse_conn = std::make_shared<SseConnection>();
        sse_conn->res = res;
        sse_conn->lua_ref = ref; // The same Lua callback ref for all connections to this route
        sse_conn->is_aborted = false;

        {
            std::lock_guard<std::mutex> map_lock(sse_connections_mutex);
            active_sse_connections[sse_id] = sse_conn;
        }

        // Set up onAborted callback to clean up when client disconnects
        res->onAborted([sse_id, sse_conn]() {
            std::lock_guard<std::mutex> map_lock(sse_connections_mutex);
            std::cerr << "SSE connection aborted: " << sse_id << std::endl;
            sse_conn->is_aborted = true; // Mark as aborted
            active_sse_connections.erase(sse_id); // Remove from map
            // Note: The Lua ref (ref) is only removed when the module shuts down
            // or if we were to decrement its ref count here (luaL_unref)
            // For now, assume a single ref for the route's handler.
        });

        // Call the Lua callback to signal that a new SSE connection is ready
        lua_rawgeti(main_L, LUA_REGISTRYINDEX, ref); // Push the Lua callback
        create_req_userdata(main_L, req);            // Push req userdata
        create_sse_res_userdata(main_L, res, sse_id); // Push the SSE connection ID

        if (lua_pcall(main_L, 2, 0, 0) != LUA_OK) {
            std::cerr << "Lua error in SSE route handler (initial call): " << lua_tostring(main_L, -1) << std::endl;
            lua_pop(main_L, 1);
            // If the Lua handler fails, close the SSE connection
            res->end();
            {
                std::lock_guard<std::mutex> map_lock(sse_connections_mutex);
                active_sse_connections.erase(sse_id);
            }
        }
        // IMPORTANT: Do NOT call res->end() here. The connection must stay open for SSE.
    });

    lua_pushboolean(L, 1);
    return 1;
}

// file operation functions

// Global mutex to protect Lua state access from multiple threads
// IMPORTANT: In a real application, you might want a more sophisticated
// threading model or use Lua's built-in coroutines for asynchronous operations
// if direct OS threads are not strictly necessary or if you need to
// avoid locking the entire Lua state for extended periods.
// static std::mutex lua_mutex;

// Pointer to the main Lua state. This should be set once at initialization.
// Ensure main_L is valid and accessible from the worker threads.
// For example, you might set it in your Lua initialization function:
// extern lua_State* main_L; // Declare it somewhere accessible
// lua_State* main_L = nullptr; // Initialize it appropriately

// Helper function to push error messages to Lua
static void push_error_to_lua(lua_State* L, const std::string& message) {
    lua_pushnil(L); // First return value is nil for error
    lua_pushstring(L, message.c_str()); // Second return value is the error message
}

// Helper function to push success results to Lua
static void push_success_to_lua(lua_State* L, const std::string& content) {
    lua_pushlstring(L, content.data(), content.size()); // The content
    lua_pushnil(L); // No error message
}

// Helper function to push boolean success results to Lua
static void push_bool_result_to_lua(lua_State* L, bool success) {
    lua_pushboolean(L, success); // True for success, false for failure
    lua_pushnil(L); // No error message initially
}

// file operation functions

int uw_async_read_file(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);

    lua_pushvalue(L, 2); // Push the callback function
    int cb_ref = luaL_ref(L, LUA_REGISTRYINDEX); // Store a reference to the callback

    // Create copies of data needed in the thread to avoid dangling pointers
    std::string path_copy = path;

    // Detach the thread to run independently.
    // Consider using a thread pool or managing threads if you expect many concurrent operations
    // to avoid resource exhaustion from too many detached threads.
    std::thread([path_copy, cb_ref]() {
        std::string content;
        std::string error_message; // To store any error during file operation

        // Use std::ios::ate to seek to the end and get file size, then seek back
        // This is a common way to pre-allocate buffer for efficiency with known file size.
        // Or simply read byte by byte if file size is unknown or can be very large.
        std::ifstream file(path_copy, std::ios::binary | std::ios::ate);

        if (file.is_open()) {
            std::streampos file_size = file.tellg(); // Get file size
            file.seekg(0, std::ios::beg); // Seek back to beginning

            // Read the entire file content into the string
            // For very large files, consider reading in chunks to avoid excessive memory usage.
            try {
                content.reserve(file_size); // Pre-allocate memory
                content.assign((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            } catch (const std::bad_alloc& e) {
                error_message = "Memory allocation failed for file content: " + std::string(e.what());
            } catch (const std::exception& e) {
                error_message = "Error reading file content: " + std::string(e.what());
            }

            file.close(); // Explicitly close the file
        } else {
            error_message = "Failed to open file for reading: " + path_copy;
        }

        // Acquire lock before interacting with the Lua state
        std::lock_guard<std::mutex> lock(lua_mutex);

        lua_rawgeti(main_L, LUA_REGISTRYINDEX, cb_ref); // Push the callback function onto the stack

        // Push results based on whether an error occurred
        if (error_message.empty()) {
            // Success: push content and nil for error
            lua_pushlstring(main_L, content.data(), content.size());
            lua_pushnil(main_L); // No error
        } else {
            // Error: push nil for content and error message
            lua_pushnil(main_L);
            lua_pushstring(main_L, error_message.c_str());
        }

        // Call the Lua callback function with 2 return values (content/nil, nil/error_message)
        if (lua_pcall(main_L, 2, 0, 0) != LUA_OK) { // 2 arguments, 0 results, no error handler func
            std::cerr << "Async read callback error: " << lua_tostring(main_L, -1) << std::endl;
            lua_pop(main_L, 1); // Pop the error message from the stack
        }

        luaL_unref(main_L, LUA_REGISTRYINDEX, cb_ref); // Release the callback reference
    }).detach();

    return 0; // Lua function returns 0 results
}

int uw_async_write_file(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);
    size_t len;
    const char *data = luaL_checklstring(L, 2, &len);
    luaL_checktype(L, 3, LUA_TFUNCTION);

    lua_pushvalue(L, 3); // Push the callback function
    int cb_ref = luaL_ref(L, LUA_REGISTRYINDEX); // Store a reference to the callback

    // Create copies of data needed in the thread to avoid dangling pointers
    std::string path_copy = path;
    std::string data_copy(data, len); // Create a copy of the data

    std::thread([path_copy, data_copy, cb_ref]() {
        bool success = false;
        std::string error_message; // To store any error during file operation

        // Open file for writing in binary mode, truncating existing content
        std::ofstream file(path_copy, std::ios::binary | std::ios::trunc);

        if (file.is_open()) {
            try {
                file.write(data_copy.data(), data_copy.size());
                if (file.good()) {
                    success = true; // Check if the write operation was successful
                } else {
                    error_message = "Error writing data to file: " + path_copy;
                }
            } catch (const std::exception& e) {
                error_message = "Exception during file write: " + std::string(e.what());
            }
            file.close(); // Explicitly close the file
        } else {
            error_message = "Failed to open file for writing: " + path_copy;
        }

        // Acquire lock before interacting with the Lua state
        std::lock_guard<std::mutex> lock(lua_mutex);

        lua_rawgeti(main_L, LUA_REGISTRYINDEX, cb_ref); // Push the callback function onto the stack

        // Push results based on whether an error occurred
        if (error_message.empty()) {
            // Success: push true and nil for error
            lua_pushboolean(main_L, success);
            lua_pushnil(main_L); // No error
        } else {
            // Error: push false for success status and error message
            lua_pushboolean(main_L, false); // Indicate failure
            lua_pushstring(main_L, error_message.c_str());
        }

        // Call the Lua callback function with 2 return values (success_bool, nil/error_message)
        if (lua_pcall(main_L, 2, 0, 0) != LUA_OK) { // 2 arguments, 0 results, no error handler func
            std::cerr << "Async write callback error: " << lua_tostring(main_L, -1) << std::endl;
            lua_pop(main_L, 1); // Pop the error message from the stack
        }

        luaL_unref(main_L, LUA_REGISTRYINDEX, cb_ref); // Release the callback reference
    }).detach();

    return 0; // Lua function returns 0 results
}

// --- Synchronous File Operations ---

// Reads the content of a file synchronously.
// Lua Usage: content, err_msg = read_file(path)
// Returns:
//   - On success: string content, nil
//   - On error: nil, string error_message
int uw_sync_read_file(lua_State *L) {
    const char *path = luaL_checkstring(L, 1); // Get the file path from Lua

    std::string content;
    std::string error_message;

    std::ifstream file(path, std::ios::binary | std::ios::ate); // Open in binary mode, seek to end

    if (file.is_open()) {
        std::streampos file_size = file.tellg(); // Get file size
        file.seekg(0, std::ios::beg);            // Seek back to beginning

        try {
            // Read the entire file content into the string
            content.reserve(file_size); // Pre-allocate memory for efficiency
            content.assign((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

            if (file.bad()) { // Check for errors during reading after assignment
                error_message = "Error during file read operation: " + std::string(path);
            }
        } catch (const std::bad_alloc& e) {
            error_message = "Memory allocation failed for file content: " + std::string(e.what());
        } catch (const std::exception& e) {
            error_message = "Unexpected error reading file: " + std::string(e.what());
        }

        file.close(); // Explicitly close the file
    } else {
        error_message = "Failed to open file for reading: " + std::string(path);
    }

    // Push results to Lua
    if (error_message.empty()) {
        lua_pushlstring(L, content.data(), content.size()); // Push content
        lua_pushnil(L);                                     // No error message
        return 2; // Return 2 values
    } else {
        lua_pushnil(L);              // No content
        lua_pushstring(L, error_message.c_str()); // Push error message
        return 2; // Return 2 values
    }
}

// Writes data to a file synchronously.
// Lua Usage: success, err_msg = write_file(path, data)
// Returns:
//   - On success: true, nil
//   - On error: false, string error_message
int uw_sync_write_file(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);    // Get the file path
    size_t len;
    const char *data = luaL_checklstring(L, 2, &len); // Get the data and its length

    bool success = false;
    std::string error_message;

    // Open file for writing in binary mode, truncating existing content
    std::ofstream file(path, std::ios::binary | std::ios::trunc);

    if (file.is_open()) {
        try {
            file.write(data, len); // Write the data

            if (file.good()) { // Check if the write operation was successful
                success = true;
            } else {
                error_message = "Error writing data to file: " + std::string(path);
            }
        } catch (const std::exception& e) {
            error_message = "Exception during file write: " + std::string(e.what());
        }
        file.close(); // Explicitly close the file
    } else {
        error_message = "Failed to open file for writing: " + std::string(path);
    }

    // Push results to Lua
    lua_pushboolean(L, success); // Push true/false for success
    if (error_message.empty()) {
        lua_pushnil(L); // No error message
    } else {
        lua_pushstring(L, error_message.c_str()); // Push error message
    }
    return 2; // Return 2 values
}


int uw_listen(lua_State *L) {
    if (!app) {
        std::cerr << "Error: uWS::App not initialized." << std::endl;
        lua_pushboolean(L, 0);
        return 1;
    }

    int port = luaL_checkinteger(L, 1);
    app->listen(port, [L, port](auto *token) {
        std::lock_guard<std::mutex> lock(lua_mutex);
        if (token) {
            std::cout << "Listening on port " << port << std::endl;
        } else {
            std::cerr << "Failed to listen on port " << port << std::endl;
        }
    });

    return 0;
}

int uw_run(lua_State *L) {
    if (!app) {
        std::cerr << "Error: uWS::App not initialized. Call create_app first." << std::endl;
        return 0;
    }
    app->run();
    return 0;
}

int uw_cleanup_app(lua_State *L) {
    if (app) {
        std::cout << "Explicitly resetting uWS::App shared_ptr.\n";
        app.reset(); // This will call the destructor of uWS::App
    }
    main_L = nullptr; // Reset main_L if needed
    return 0;
}

extern "C" int luaopen_uwebsockets(lua_State *L) {
    create_metatables(L);

    luaL_Reg functions[] = {
        {"create_app", uw_create_app},
        {"get", uw_get},
        {"post", uw_post},
        {"put", uw_put},
        {"delete", uw_delete},
        {"patch", uw_patch},
        {"head", uw_head},
        {"options", uw_options},
        {"ws", uw_ws},
        {"sse", uw_sse},             // New SSE route function
        {"sse_send", uw_sse_send},   // New function to send SSE data
        {"sse_close", uw_sse_close}, // New function to close SSE connection
        {"listen", uw_listen},
        {"run", uw_run},
        {"use", uw_use},
        {"serve_static", uw_serve_static},
        {"setTimeout", uw_setTimeout},
        {"setInterval", uw_setInterval},
        {"clearTimer", uw_clearTimer},
        {"cleanup_app", uw_cleanup_app}, // Add this new function
        {"async_read_file", uw_async_read_file},
        {"async_write_file", uw_async_write_file},
        {"sync_read_file", uw_sync_read_file},
        {"sync_write_file", uw_sync_write_file},

        // {"add_timer", uw_add_timer}, // This function was not defined in the provided shim.cpp
        {nullptr, nullptr}
    };

    luaL_newlib(L, functions);
    return 1;
}
