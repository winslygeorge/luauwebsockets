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

#include <chrono>
#include <sys/stat.h> // Added for fstat

#include <system_error>
#ifdef __linux__
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#elif _WIN32
#include <windows.h>
#endif

namespace fs = std::filesystem;


namespace fs = std::filesystem; // Alias for convenience

// For getnameinfo, NI_MAXHOST, NI_NUMERICHOST

static std::shared_ptr<uWS::App> app;
static us_listen_socket_t *listen_socket = nullptr;
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

int timer_id = 0;
us_loop_t *main_loop = nullptr;

struct LuaTimerData {
    lua_State* owner;
    lua_State* thread;
    int lua_ref;
    int thread_ref;
    int timer_id;
    bool is_interval;
    bool is_cleared;
    std::vector<int> arg_refs;
};



// std::unordered_map<int, us_timer_t*> active_timers;
// int timer_id_counter = 0;

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


// User data structure for WebSocket
struct WebSocketUserData {
    std::string id;
    bool is_closed = false;
    uWS::WebSocket<false, true, WebSocketUserData>* socket = nullptr;
     // Store additional user data if needed
    std::unordered_map<std::string, std::string> metadata;
};

// Fully corrected create_zombie_websocket
static int create_zombie_websocket(lua_State *L, const std::string& id) {
    using WebSocketPtr = uWS::WebSocket<false, true, WebSocketUserData>*;
    
    WebSocketPtr* ws_ud = static_cast<WebSocketPtr*>(
        lua_newuserdata(L, sizeof(WebSocketPtr)));
    *ws_ud = nullptr;
    
    lua_newtable(L);
    lua_pushstring(L, id.c_str());
    lua_setfield(L, -2, "id");
    lua_pushboolean(L, true);
    lua_setfield(L, -2, "closed");
    
    luaL_getmetatable(L, "websocket");
    lua_setmetatable(L, -2);
    
    return 1;
}


// static int websocket_send(lua_State *L) {
//     void *ud = luaL_checkudata(L, 1, "websocket");
//     if (!ud) {
//         lua_pushboolean(L, 0);
//         lua_pushstring(L, "Invalid userdata");
//         return 2;
//     }

//     auto ws_ptr = *(uWS::WebSocket<false, true, WebSocketUserData>**)ud;
//     if (!ws_ptr) {
//         lua_pushboolean(L, 0);
//         lua_pushstring(L, "Socket pointer is null");
//         return 2;
//     }

//     auto *userdata = ws_ptr->getUserData();
//     if (!userdata || userdata->is_closed || userdata->socket != ws_ptr) {
//         lua_pushboolean(L, 0);
//         lua_pushstring(L, "Socket has been closed or is invalid");
//         return 2;
//     }

//     const char *message = luaL_checkstring(L, 2);
//     size_t len = lua_objlen(L, 2);
//     uWS::OpCode opcode = uWS::OpCode::TEXT;

//     if (lua_gettop(L) > 2 && lua_isstring(L, 3)) {
//         const char *type = luaL_checkstring(L, 3);
//         if (strcmp(type, "binary") == 0) {
//             opcode = uWS::OpCode::BINARY;
//         }
//     }

//     userdata->socket->send(std::string_view(message, len), opcode);
//     lua_pushboolean(L, 1);
//     return 1;
// }

// Update websocket_send to handle zombie sockets
// Fully corrected websocket_send
static int websocket_send(lua_State *L) {
    using WebSocketPtr = uWS::WebSocket<false, true, WebSocketUserData>*;
    
    WebSocketPtr* ws_ptr = static_cast<WebSocketPtr*>(luaL_checkudata(L, 1, "websocket"));
    if (!ws_ptr) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, "Invalid userdata");
        return 2;
    }

    if (!*ws_ptr) {
        if (lua_getmetatable(L, 1)) {
            lua_getfield(L, -1, "id");
            if (lua_isstring(L, -1)) {
                const char* id = lua_tostring(L, -1);
                lua_pushboolean(L, 0);
                lua_pushfstring(L, "Socket %s is closed", id);
                lua_remove(L, -3);
                return 2;
            }
            lua_pop(L, 2);
        }
        lua_pushboolean(L, 0);
        lua_pushstring(L, "Socket is closed");
        return 2;
    }

    auto *userdata = (*ws_ptr)->getUserData();
    if (!userdata || userdata->is_closed || userdata->socket != *ws_ptr) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, "Socket has been closed or is invalid");
        return 2;
    }

    const char *message = luaL_checkstring(L, 2);
    size_t len = lua_objlen(L, 2);
    uWS::OpCode opcode = uWS::OpCode::TEXT;

    if (lua_gettop(L) > 2 && lua_isstring(L, 3)) {
        const char *type = luaL_checkstring(L, 3);
        if (strcmp(type, "binary") == 0) {
            opcode = uWS::OpCode::BINARY;
        }
    }

    userdata->socket->send(std::string_view(message, len), opcode);
    lua_pushboolean(L, 1);
    return 1;
}


static int websocket_close(lua_State *L) {
    void *ud = luaL_checkudata(L, 1, "websocket");
    if (!ud) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, "Invalid userdata");
        return 2;
    }

    auto ws = *(uWS::WebSocket<false, true, WebSocketUserData>**)ud;
    if (!ws) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, "Socket pointer is null");
        return 2;
    }

    auto *userdata = ws->getUserData();
    if (!userdata || userdata->is_closed || userdata->socket != ws) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, "Socket already closed or invalid");
        return 2;
    }

    userdata->is_closed = true;
    userdata->socket = nullptr;
    ws->close();

    lua_pushboolean(L, 1);
    return 1;
}

// Function to create a WebSocket userdata

// Update websocket_get_id to work with zombie sockets
static int websocket_get_id(lua_State *L) {
    uWS::WebSocket<false, true, WebSocketUserData>** ws_ud = 
        static_cast<uWS::WebSocket<false, true, WebSocketUserData>**>(luaL_checkudata(L, 1, "websocket"));
    
    // Handle zombie sockets
    if (!*ws_ud) {
        if (lua_getmetatable(L, 1)) {
            lua_getfield(L, -1, "id");
            if (lua_isstring(L, -1)) {
                // Return the ID from the environment table
                return 1;
            }
            lua_pop(L, 2); // pop string and metatable
        }
        lua_pushnil(L);
        return 1;
    }

    if (!*ws_ud || !(*ws_ud)->getUserData()) {
        luaL_error(L, "invalid websocket userdata");
        return 0;
    }

    lua_pushstring(L, (*ws_ud)->getUserData()->id.c_str());
    return 1;
}

// Add metadata access methods to websocket metatable
static void create_websocket_metatable(lua_State *L) {
    luaL_newmetatable(L, "websocket");
    lua_pushstring(L, "__index");
    lua_newtable(L);
    
    // Existing methods
    lua_pushcfunction(L, websocket_send);
    lua_setfield(L, -2, "send");
    lua_pushcfunction(L, websocket_close);
    lua_setfield(L, -2, "close");
    lua_pushcfunction(L, websocket_get_id);
    lua_setfield(L, -2, "get_id");
    
    // New metadata methods
    lua_pushcfunction(L, [](lua_State *L) -> int {
        void *ud = luaL_checkudata(L, 1, "websocket");
        auto ws_ptr = *(uWS::WebSocket<false, true, WebSocketUserData>**)ud;
        
        WebSocketUserData* userdata = nullptr;
        if (ws_ptr) {
            userdata = ws_ptr->getUserData();
        } else {
            // For zombie sockets, we can't access userdata
            lua_pushnil(L);
            return 1;
        }

        if (!userdata) {
            lua_pushnil(L);
            return 1;
        }

        const char *key = luaL_checkstring(L, 2);
        auto it = userdata->metadata.find(key);
        if (it != userdata->metadata.end()) {
            lua_pushstring(L, it->second.c_str());
        } else {
            lua_pushnil(L);
        }
        return 1;
    });
    lua_setfield(L, -2, "get_metadata");
    
    lua_pushcfunction(L, [](lua_State *L) -> int {
        void *ud = luaL_checkudata(L, 1, "websocket");
        auto ws_ptr = *(uWS::WebSocket<false, true, WebSocketUserData>**)ud;
        
        if (!ws_ptr) {
            lua_pushboolean(L, 0);
            lua_pushstring(L, "Cannot set metadata on closed socket");
            return 2;
        }

        WebSocketUserData* userdata = ws_ptr->getUserData();
        if (!userdata) {
            lua_pushboolean(L, 0);
            lua_pushstring(L, "Invalid userdata");
            return 2;
        }

        const char *key = luaL_checkstring(L, 2);
        const char *value = luaL_checkstring(L, 3);
        userdata->metadata[key] = value;
        
        lua_pushboolean(L, 1);
        return 1;
    });
    lua_setfield(L, -2, "set_metadata");
    
    lua_settable(L, -3);
    lua_pop(L, 1);
}


// static void create_websocket_metatable(lua_State *L) {
//     luaL_newmetatable(L, "websocket");
//     lua_pushstring(L, "__index");
//     lua_newtable(L); // Metatable for methods
//     lua_pushcfunction(L, websocket_send);
//     lua_setfield(L, -2, "send");
//     lua_pushcfunction(L, websocket_close);
//     lua_setfield(L, -2, "close");
//     lua_settable(L, -3); // Set __index to the methods table
//     lua_pop(L, 1); // Pop the metatable
// }

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
// static int websocket_get_id(lua_State *L) {
//     uWS::WebSocket<false, true, WebSocketUserData>** ws_ud = static_cast<uWS::WebSocket<false, true, WebSocketUserData>**>(luaL_checkudata(L, 1, "websocket"));
//     if (!ws_ud || !*ws_ud) {
//         luaL_error(L, "invalid websocket userdata");
//         return 0;
//     }

//     lua_pushstring(L, (*ws_ud)->getUserData()->id.c_str());
//     return 1;
// }

int uw_ws(lua_State *L) {
    const char *route_c_str = luaL_checkstring(L, 1);
    std::string route = route_c_str; // Explicitly convert to std::string
    luaL_checktype(L, 2, LUA_TFUNCTION);
    lua_pushvalue(L, 2);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    int callback_id = callback_id_counter++;
    lua_callbacks[callback_id] = ref;

    app->ws<WebSocketUserData>(route, {
        .open = [callback_id, route](auto *ws) {
            std::lock_guard<std::mutex> lock(lua_mutex);
            lua_rawgeti(main_L, LUA_REGISTRYINDEX, lua_callbacks[callback_id]);

            // Set WebSocket pointer in userdata
            WebSocketUserData* data = ws->getUserData();
            data->id = generate_unique_id();
            data->socket = ws;
            data->is_closed = false;

            // Create Lua userdata and store ws pointer
            auto **ws_ud = static_cast<uWS::WebSocket<false, true, WebSocketUserData>**>(
                lua_newuserdata(main_L, sizeof(*ws_ud)));
            *ws_ud = ws;
            luaL_getmetatable(main_L, "websocket");
            lua_setmetatable(main_L, -2);

            lua_pushstring(main_L, "open");

            if (lua_pcall(main_L, 2, 0, 0) != LUA_OK) {
                std::cerr << "Lua error (open): " << lua_tostring(main_L, -1) << std::endl;
                lua_pop(main_L, 1);
            }
        },

        .message = [callback_id](auto *ws, std::string_view message, uWS::OpCode opCode) {
            std::lock_guard<std::mutex> lock(lua_mutex);
            lua_rawgeti(main_L, LUA_REGISTRYINDEX, lua_callbacks[callback_id]);

            // Push Lua userdata
            auto **ws_ud = static_cast<uWS::WebSocket<false, true, WebSocketUserData>**>(
                lua_newuserdata(main_L, sizeof(*ws_ud)));
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

        // .close = [callback_id](auto *ws, int code, std::string_view message) {
        //     std::lock_guard<std::mutex> lock(lua_mutex);
        //     WebSocketUserData* data = ws->getUserData();
        //     if (data) {
        //         data->is_closed = true;
        //         data->socket = nullptr;
        //     }

        //     lua_rawgeti(main_L, LUA_REGISTRYINDEX, lua_callbacks[callback_id]);

        //     auto **ws_ud = static_cast<uWS::WebSocket<false, true, WebSocketUserData>**>(
        //         lua_newuserdata(main_L, sizeof(*ws_ud)));
        //     *ws_ud = nullptr;   // prevent reuse
        //     luaL_getmetatable(main_L, "websocket");
        //     lua_setmetatable(main_L, -2);

        //     lua_pushstring(main_L, "close");
        //     lua_pushinteger(main_L, code);
        //     lua_pushlstring(main_L, message.data(), message.size());

        //     if (lua_pcall(main_L, 4, 0, 0) != LUA_OK) {
        //         std::cerr << "Lua error (close): " << lua_tostring(main_L, -1) << std::endl;
        //         lua_pop(main_L, 1);
        //     }
        // }
        // Modify the close handler in uw_ws
        .close = [callback_id](auto *ws, int code, std::string_view message) {
    std::lock_guard<std::mutex> lock(lua_mutex);
    WebSocketUserData* data = ws ? ws->getUserData() : nullptr;
    std::string id;
    
    if (data) {
        data->is_closed = true;
        data->socket = nullptr;
        id = data->id;
    }

    lua_rawgeti(main_L, LUA_REGISTRYINDEX, lua_callbacks[callback_id]);
    
    // Create zombie userdata with the ID
    create_zombie_websocket(main_L, id);
    
    lua_pushstring(main_L, "close");
    lua_pushinteger(main_L, code);
    lua_pushlstring(main_L, message.data(), message.size());

    if (lua_pcall(main_L, 4, 0, 0) != LUA_OK) {
        std::cerr << "Lua error (close): " << lua_tostring(main_L, -1) << std::endl;
        lua_pop(main_L, 1);
    }
}
        // Removed .autoUpgrade here
    });

    // Register the get_id method in the websocket metatable
    luaL_getmetatable(L, "websocket");
    lua_pushcfunction(L, websocket_get_id);
    lua_setfield(L, -2, "get_id");
    lua_pop(L, 1); // Pop the metatable

    lua_pushboolean(L, 1);
    return 1;
}


std::string get_mime_type(const std::string& filepath) {
    static const std::unordered_map<std::string, std::string> mime_types = {
        {".html", "text/html"},
        {".htm", "text/html"},
        {".css", "text/css"},
        {".js", "application/javascript"},
        {".json", "application/json"},
        {".jpg", "image/jpeg"},
        {".jpeg", "image/jpeg"},
        {".png", "image/png"},
        {".gif", "image/gif"},
        {".svg", "image/svg+xml"},
        {".ico", "image/x-icon"},
        {".pdf", "application/pdf"},
        {".txt", "text/plain"},
        {".mp4", "video/mp4"},
        {".webm", "video/webm"},
        {".mp3", "audio/mpeg"},
        {".woff2", "font/woff2"}
    };

    fs::path p(filepath);
    std::string ext = p.extension().string();
    auto it = mime_types.find(ext);
    return it != mime_types.end() ? it->second : "application/octet-stream";
}

// Configuration constants
namespace {
    const size_t SMALL_FILE_THRESHOLD = 64 * 1024; // 64KB
    const size_t LARGE_FILE_CHUNK_SIZE = 128 * 1024; // 128KB
    const size_t MMAP_THRESHOLD = 10 * 1024 * 1024; // 10MB
    const unsigned int TRANSFER_TIMEOUT_MS = 30000; // 30 seconds
}

// Memory-mapped file wrapper
class MappedFile {
    int fd = -1;
    void* mapping = MAP_FAILED;
    size_t size = 0;
    const char* data = nullptr;

public:
    MappedFile(const std::string& path) {
        fd = open(path.c_str(), O_RDONLY);
        if (fd == -1) {
            throw std::system_error(errno, std::system_category(), "Failed to open file");
        }

        struct stat st;
        if (fstat(fd, &st) == -1) {
            close(fd);
            throw std::system_error(errno, std::system_category(), "Failed to get file size");
        }
        size = st.st_size;

        mapping = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
        if (mapping == MAP_FAILED) {
            close(fd);
            throw std::system_error(errno, std::system_category(), "Failed to map file");
        }
        data = static_cast<const char*>(mapping);
    }

    ~MappedFile() {
        if (mapping != MAP_FAILED) munmap(mapping, size);
        if (fd != -1) close(fd);
    }

    const char* getData() const { return data; }
    size_t getSize() const { return size; }
};

// Helper function to clean path and prevent directory traversal
std::string sanitize_path(const std::string& base, const std::string& path) {
    fs::path full_path = fs::path(base) / path;
    full_path = fs::canonical(full_path);
    
    // Verify the path is within the base directory
    fs::path base_path = fs::canonical(base);
    if (full_path.string().rfind(base_path.string(), 0) != 0) {
        throw std::runtime_error("Directory traversal attempt detected");
    }
    
    return full_path.string();
}

// Enhanced static file serving function with proper completion handling
int uw_serve_static(lua_State *L) {
    const char *route_prefix = luaL_checkstring(L, 1);
    const char *dir_path = luaL_checkstring(L, 2);

    if (!fs::is_directory(dir_path)) {
        std::cerr << "ERROR: Static file directory '" << dir_path 
                  << "' does not exist or is not a directory." << std::endl;
        lua_pushboolean(L, 0);
        return 1;
    }

    std::string route_pattern = std::string(route_prefix) + "/*";

    app->get(route_pattern.c_str(), [dir_path_str = std::string(dir_path), 
                                   route_prefix_str = std::string(route_prefix)](auto *res, auto *req) {
        try {
            std::string_view url = req->getUrl();
            std::string file_path_suffix = std::string(url.substr(route_prefix_str.length()));
            
            // Remove leading slash if present
            if (!file_path_suffix.empty() && file_path_suffix[0] == '/') {
                file_path_suffix.erase(0, 1);
            }

            // Handle directory requests by appending index.html
            fs::path full_path;
            try {
                full_path = sanitize_path(dir_path_str, file_path_suffix);
                if (fs::is_directory(full_path)) {
                    full_path /= "index.html";
                }
            } catch (const std::exception& e) {
                std::cerr << "SECURITY: " << e.what() << " for path: " << file_path_suffix << std::endl;
                res->writeStatus("403 Forbidden")->end("Forbidden");
                return;
            }

            // Check if file exists and is regular file
            if (!fs::exists(full_path) || !fs::is_regular_file(full_path)) {
                res->writeStatus("404 Not Found")->end("Not Found");
                return;
            }

            // Get file size
            size_t file_size = fs::file_size(full_path);

            // Set headers
            std::string mime_type = get_mime_type(full_path.string());
            res->writeHeader("Content-Type", mime_type);
            res->writeHeader("Content-Length", std::to_string(file_size));
            
            // For small files, send everything at once
            if (file_size <= SMALL_FILE_THRESHOLD) {
                std::ifstream file(full_path, std::ios::binary);
                if (!file) {
                    std::cerr << "ERROR: Failed to open small file: " << full_path.string() << std::endl;
                    res->writeStatus("500 Internal Server Error")->end("File Read Error");
                    return;
                }

                std::vector<char> buffer(file_size);
                if (!file.read(buffer.data(), file_size)) {
                    std::cerr << "ERROR: Failed to read small file: " << full_path.string() << std::endl;
                    res->writeStatus("500 Internal Server Error")->end("File Read Error");
                    return;
                }
                res->end(std::string_view(buffer.data(), file_size));
                return;
            }

            // For medium files, use buffered chunked transfer
            if (file_size <= MMAP_THRESHOLD) {
                auto file_stream_ptr = std::make_shared<std::ifstream>(
                    full_path, std::ios::binary
                );
                
                if (!file_stream_ptr->is_open()) {
                    std::cerr << "ERROR: Could not open medium file: " << full_path.string() << std::endl;
                    res->writeStatus("500 Internal Server Error")->end("Could not open file");
                    return;
                }

                auto buffer_ptr = std::make_shared<std::vector<char>>(LARGE_FILE_CHUNK_SIZE);
                auto remaining_bytes = std::make_shared<size_t>(file_size);
                auto transfer_start = std::chrono::steady_clock::now();

                // Abort handler
                res->onAborted([file_stream_ptr, full_path_str = full_path.string()]() {
                    std::cerr << "WARNING: Transfer aborted for file: " << full_path_str << std::endl;
                });

                // Chunked transfer handler
                res->onWritable([res, file_stream_ptr, buffer_ptr, remaining_bytes, 
                                transfer_start, full_path_str = full_path.string()](int /* offset */) mutable {
                    // Check timeout
                    auto now = std::chrono::steady_clock::now();
                    if (std::chrono::duration_cast<std::chrono::milliseconds>(
                        now - transfer_start).count() > TRANSFER_TIMEOUT_MS) {
                        std::cerr << "ERROR: Transfer timeout reached for file: " << full_path_str << std::endl;
                        res->writeStatus("500 Internal Server Error")->end("Transfer Timeout");
                        return true;
                    }

                    if (*remaining_bytes == 0) {
                        // All data sent - end the response
                        res->end();
                        return true;
                    }

                    size_t chunk_size = std::min(buffer_ptr->size(), *remaining_bytes);
                    if (!file_stream_ptr->read(buffer_ptr->data(), chunk_size)) {
                        std::cerr << "ERROR: Failed to read chunk from file: " << full_path_str << std::endl;
                        res->writeStatus("500 Internal Server Error")->end("File Read Error");
                        return true;
                    }

                    bool write_success = res->write(std::string_view(buffer_ptr->data(), chunk_size));
                    *remaining_bytes -= chunk_size;

                    if (!write_success) {
                        std::cerr << "WARNING: Write failed for file: " << full_path_str << std::endl;
                        return true;
                    }

                    // Return true if we're done, false if more to send
                    return *remaining_bytes == 0;
                });

                // Send first chunk
                size_t first_chunk = std::min(buffer_ptr->size(), file_size);
                if (first_chunk > 0) {
                    if (!file_stream_ptr->read(buffer_ptr->data(), first_chunk)) {
                        std::cerr << "ERROR: Failed to read first chunk from file: " << full_path.string() << std::endl;
                        res->writeStatus("500 Internal Server Error")->end("File Read Error");
                        return;
                    }
                    res->write(std::string_view(buffer_ptr->data(), first_chunk));
                }
            }
            // For very large files (above MMAP_THRESHOLD), use memory-mapped I/O
            else {
                try {
                    auto mapped_file = std::make_shared<MappedFile>(full_path.string());
                    auto remaining_bytes = std::make_shared<size_t>(mapped_file->getSize());
                    auto transfer_start = std::chrono::steady_clock::now();

                    // Abort handler
                    res->onAborted([mapped_file, full_path_str = full_path.string()]() {
                        std::cerr << "WARNING: Transfer aborted for file: " << full_path_str << std::endl;
                    });

                    // Chunked transfer handler using memory-mapped file
                    res->onWritable([res, mapped_file, remaining_bytes, 
                                    transfer_start, full_path_str = full_path.string()](int /* offset */) mutable {
                        // Check timeout
                        auto now = std::chrono::steady_clock::now();
                        if (std::chrono::duration_cast<std::chrono::milliseconds>(
                            now - transfer_start).count() > TRANSFER_TIMEOUT_MS) {
                            std::cerr << "ERROR: Transfer timeout reached for file: " << full_path_str << std::endl;
                            res->writeStatus("500 Internal Server Error")->end("Transfer Timeout");
                            return true;
                        }

                        if (*remaining_bytes == 0) {
                            // All data sent - end the response
                            res->end();
                            return true;
                        }

                        size_t chunk_size = std::min(LARGE_FILE_CHUNK_SIZE, *remaining_bytes);
                        const char* chunk_start = mapped_file->getData() + (mapped_file->getSize() - *remaining_bytes);

                        bool write_success = res->write(std::string_view(chunk_start, chunk_size));
                        *remaining_bytes -= chunk_size;

                        if (!write_success) {
                            std::cerr << "WARNING: Write failed for file: " << full_path_str << std::endl;
                            return true;
                        }

                        // Return true if we're done, false if more to send
                        return *remaining_bytes == 0;
                    });

                    // Send first chunk
                    size_t first_chunk = std::min(LARGE_FILE_CHUNK_SIZE, mapped_file->getSize());
                    if (first_chunk > 0) {
                        res->write(std::string_view(mapped_file->getData(), first_chunk));
                    }
                } catch (const std::exception& e) {
                    std::cerr << "ERROR: Failed to memory-map file " << full_path.string() 
                              << ": " << e.what() << std::endl;
                    res->writeStatus("500 Internal Server Error")->end("File Read Error");
                }
            }

        } catch (const std::exception& e) {
            std::cerr << "ERROR: Exception in static file handler: " << e.what() << std::endl;
            if (!res->hasResponded()) {
                res->writeStatus("500 Internal Server Error")->end("Internal Server Error");
            }
        }
    });

    lua_pushboolean(L, 1);
    return 1;
}


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


// Add near top with other declarations
struct LuaTimer {
    int timer_id;
    bool is_interval;
    int callback_ref;
    std::vector<int> arg_refs;
    bool active;
    std::chrono::milliseconds interval;
    std::chrono::steady_clock::time_point next_execution;
};

static std::unordered_map<int, LuaTimer> active_timers;
static std::mutex timers_mutex;
static int next_timer_id = 1;
static void* timer_handler_key = nullptr;
static bool timers_initialized = false;
static int cleanup_callback_ref = LUA_NOREF;

// Helper to call Lua timer callbacks
static void call_timer_callback(int timer_id) {
    std::lock_guard<std::mutex> lua_lock(lua_mutex);
    std::lock_guard<std::mutex> timer_lock(timers_mutex);
    
    auto it = active_timers.find(timer_id);
    if (it == active_timers.end() || !it->second.active) {
        return;
    }
    
    LuaTimer& timer = it->second;
    
    lua_rawgeti(main_L, LUA_REGISTRYINDEX, timer.callback_ref);
    
    for (int arg_ref : timer.arg_refs) {
        lua_rawgeti(main_L, LUA_REGISTRYINDEX, arg_ref);
    }
    
    if (lua_pcall(main_L, timer.arg_refs.size(), 0, 0) != LUA_OK) {
        std::cerr << "Timer callback error: " << lua_tostring(main_L, -1) << std::endl;
        lua_pop(main_L, 1);
    }
    
    if (timer.is_interval) {
        timer.next_execution = std::chrono::steady_clock::now() + timer.interval;
    } else {
        timer.active = false;
    }
}

// Timer check function
static void check_timers() {
    auto now = std::chrono::steady_clock::now();
    std::vector<int> timers_to_execute;
    
    {
        std::lock_guard<std::mutex> lock(timers_mutex);
        for (auto& pair : active_timers) {
            if (pair.second.active && now >= pair.second.next_execution) {
                timers_to_execute.push_back(pair.first);
            }
        }
    }
    
    for (int timer_id : timers_to_execute) {
        call_timer_callback(timer_id);
    }
    
    // Clean up inactive timers
    {
        std::lock_guard<std::mutex> lock(timers_mutex);
        for (auto it = active_timers.begin(); it != active_timers.end(); ) {
            if (!it->second.active) {
                luaL_unref(main_L, LUA_REGISTRYINDEX, it->second.callback_ref);
                for (int arg_ref : it->second.arg_refs) {
                    luaL_unref(main_L, LUA_REGISTRYINDEX, arg_ref);
                }
                it = active_timers.erase(it);
            } else {
                ++it;
            }
        }
    }
}

// Initialize timer system
static void init_timer_system() {
    if (!timers_initialized && uWS::Loop::get()) {
        timer_handler_key = new int(0);
        uWS::Loop::get()->addPostHandler(timer_handler_key, [](uWS::Loop* loop) {
            check_timers();
        });
        timers_initialized = true;
    }
}

// Shutdown timer system
static void shutdown_timer_system() {
    if (timers_initialized) {
        if (uWS::Loop::get() && timer_handler_key) {
            uWS::Loop::get()->removePostHandler(timer_handler_key);
        }
        
        // Clean up any remaining timers
        {
            std::lock_guard<std::mutex> lock(timers_mutex);
            for (auto& pair : active_timers) {
                luaL_unref(main_L, LUA_REGISTRYINDEX, pair.second.callback_ref);
                for (int arg_ref : pair.second.arg_refs) {
                    luaL_unref(main_L, LUA_REGISTRYINDEX, arg_ref);
                }
            }
            active_timers.clear();
        }
        
        delete static_cast<int*>(timer_handler_key);
        timer_handler_key = nullptr;
        timers_initialized = false;
    }
}

// Common function to create timers
static int create_timer(lua_State* L, bool is_interval) {
    if (!app) {
        luaL_error(L, "uWS::App not initialized. Call create_app first.");
        return 0;
    }
    
    luaL_checktype(L, 1, LUA_TFUNCTION);
    int delay = luaL_checkinteger(L, 2);
    
    lua_pushvalue(L, 1);
    int callback_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    
    std::vector<int> arg_refs;
    int num_args = lua_gettop(L);
    for (int i = 3; i <= num_args; i++) {
        lua_pushvalue(L, i);
        arg_refs.push_back(luaL_ref(L, LUA_REGISTRYINDEX));
    }
    
    LuaTimer timer;
    timer.timer_id = next_timer_id++;
    timer.is_interval = is_interval;
    timer.callback_ref = callback_ref;
    timer.arg_refs = std::move(arg_refs);
    timer.active = true;
    timer.interval = std::chrono::milliseconds(delay);
    timer.next_execution = std::chrono::steady_clock::now() + timer.interval;
    
    {
        std::lock_guard<std::mutex> lock(timers_mutex);
        active_timers[timer.timer_id] = timer;
    }
    
    // Ensure timer system is initialized
    init_timer_system();
    
    lua_pushinteger(L, timer.timer_id);
    return 1;
}

// Clear a timer
static int clear_timer(lua_State* L) {
    int timer_id = luaL_checkinteger(L, 1);
    
    std::lock_guard<std::mutex> lock(timers_mutex);
    auto it = active_timers.find(timer_id);
    if (it != active_timers.end()) {
        it->second.active = false;
    }
    
    return 0;
}

// Lua interface functions
int uw_setTimeout(lua_State *L) {
    return create_timer(L, false);
}

int uw_setInterval(lua_State *L) {
    return create_timer(L, true);
}

int uw_clearTimer(lua_State *L) {
    return clear_timer(L);
}

int uw_cleanup_app(lua_State *L) {
    std::cout << "Cleaning up the uWS app instance..." << std::endl;

    // Explicitly close the listening socket if active
    if (listen_socket) {
        us_listen_socket_close(0, listen_socket);
        listen_socket = nullptr;
        std::cout << "🔒 Listening socket closed" << std::endl;
    }

    // Destroy the uWS::App instance
    if (app) {
        app.reset();
        std::cout << "🗑️ uWS::App destroyed" << std::endl;
    }

    return 0;
}







// int uw_listen(lua_State *L) {
//     if (!app) {
//         std::cerr << "Error: uWS::App not initialized." << std::endl;
//         lua_pushboolean(L, 0);
//         return 1;
//     }

//     int port = luaL_checkinteger(L, 1);
//     app->listen(port, [L, port](auto *token) {
//         std::lock_guard<std::mutex> lock(lua_mutex);
//         if (token) {
//             std::cout << "Listening on port " << port << std::endl;
//         } else {
//             std::cerr << "Failed to listen on port " << port << std::endl;
//         }
//     });

//     return 0;
// }

int uw_run(lua_State *L) {
    if (!app) {
        std::cerr << "Error: uWS::App not initialized. Call create_app first." << std::endl;
        return 0;
    }
     // Initialize timer system if not already done
    init_timer_system();
    
    app->run();
    
    // Call cleanup callback if set
    if (cleanup_callback_ref != LUA_NOREF) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, cleanup_callback_ref);
        if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
            std::cerr << "Cleanup callback error: " << lua_tostring(L, -1) << std::endl;
            lua_pop(L, 1);
        }
        luaL_unref(L, LUA_REGISTRYINDEX, cleanup_callback_ref);
        cleanup_callback_ref = LUA_NOREF;
    }
    
    // Clean up everything
    shutdown_timer_system();
    if (app) {
        app.reset();
    }
    
    return 0;
}

int uw_listen(lua_State *L) {
    if (!app) {
        luaL_error(L, "uWS::App not initialized. Call create_app first.");
        return 0;
    }

    int port = luaL_checkinteger(L, 1);

    app->listen(port, [L, port](auto *token) {
        std::lock_guard<std::mutex> lock(lua_mutex);
        listen_socket = token;

        if (token) {
            std::cout << "✅ Listening on port " << port << std::endl;

            if (lua_gettop(L) > 1 && lua_isfunction(L, 2)) {
                lua_pushvalue(L, 2);
                if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
                    std::cerr << "Listen callback error: " << lua_tostring(L, -1) << std::endl;
                    lua_pop(L, 1);
                }
            }
        } else {
            std::cerr << "❌ Failed to listen on port " << port << std::endl;
        }
    });

    // ⚠️ DO NOT run loop here — defer it to uw_run()
    return 0;
}

int uw_restart_cleanup(lua_State *L) {
    uWS::Loop::get()->defer([]() {
        std::cout << "[restart_cleanup] Cleaning up server..." << std::endl;

        shutdown_timer_system();

        {
            std::lock_guard<std::mutex> lock(sse_connections_mutex);
            for (auto &p : active_sse_connections) {
                auto &conn = p.second;
                if (conn && !conn->is_aborted && conn->res) {
                    conn->res->end();
                    conn->is_aborted = true;
                }
            }
            active_sse_connections.clear();
        }

        if (listen_socket) {
            us_listen_socket_close(0, listen_socket);
            listen_socket = nullptr;
        }

        if (app) {
            app.reset();
        }
    });

    return 0; // tell Lua "no return values"

}
// declare uw_restart_reregister before ninitalization
   static int uw_restart_reregister(lua_State *L);

// Forward decl from uw_create_app
static int app_userdata_gc(lua_State *L);

static void create_app_metatable(lua_State *L) {
    luaL_newmetatable(L, "uWS.App");

    // Methods table
    lua_newtable(L);

    // Register methods on the App object
    lua_pushcfunction(L, uw_get);           lua_setfield(L, -2, "get");
    lua_pushcfunction(L, uw_post);          lua_setfield(L, -2, "post");
    lua_pushcfunction(L, uw_put);           lua_setfield(L, -2, "put");
    lua_pushcfunction(L, uw_delete);        lua_setfield(L, -2, "delete");
    lua_pushcfunction(L, uw_patch);         lua_setfield(L, -2, "patch");
    lua_pushcfunction(L, uw_head);          lua_setfield(L, -2, "head");
    lua_pushcfunction(L, uw_options);       lua_setfield(L, -2, "options");
    lua_pushcfunction(L, uw_ws);            lua_setfield(L, -2, "ws");

    lua_pushcfunction(L, uw_sse);           lua_setfield(L, -2, "sse");
    lua_pushcfunction(L, uw_sse_send);      lua_setfield(L, -2, "sse_send");
    lua_pushcfunction(L, uw_sse_close);     lua_setfield(L, -2, "sse_close");

    lua_pushcfunction(L, uw_use);           lua_setfield(L, -2, "use");
    lua_pushcfunction(L, uw_serve_static);  lua_setfield(L, -2, "serve_static");

    lua_pushcfunction(L, uw_setTimeout);    lua_setfield(L, -2, "setTimeout");
    lua_pushcfunction(L, uw_setInterval);   lua_setfield(L, -2, "setInterval");
    lua_pushcfunction(L, uw_clearTimer);    lua_setfield(L, -2, "clearTimer");

    lua_pushcfunction(L, uw_listen);        lua_setfield(L, -2, "listen");
    lua_pushcfunction(L, uw_run);           lua_setfield(L, -2, "run");
    lua_pushcfunction(L, uw_cleanup_app);   lua_setfield(L, -2, "cleanup_app");
    // lua_pushcfunction(L, uw_force_restart);      lua_setfield(L, -2, "restart");
    lua_pushcfunction(L, uw_restart_cleanup); lua_setfield(L, -2, "restart_cleanup");
    lua_pushcfunction(L, uw_restart_reregister); lua_setfield(L, -2, "restart_reregister");

    // File I/O methods too if you want them on app
    lua_pushcfunction(L, uw_async_read_file);  lua_setfield(L, -2, "async_read_file");
    lua_pushcfunction(L, uw_async_write_file); lua_setfield(L, -2, "async_write_file");
    lua_pushcfunction(L, uw_sync_read_file);   lua_setfield(L, -2, "sync_read_file");
    lua_pushcfunction(L, uw_sync_write_file);  lua_setfield(L, -2, "sync_write_file");

    // set __index = methods table
    lua_setfield(L, -2, "__index");

    // Add __gc metamethod so shared_ptr is destroyed when Lua collects
    lua_pushcfunction(L, app_userdata_gc);
    lua_setfield(L, -2, "__gc");

    lua_pop(L, 1); // pop metatable
}

// Helper: destructor for the app userdata (called by Lua GC)
static int app_userdata_gc(lua_State *L) {
    void* ud = lua_touserdata(L, 1);
    if (!ud) return 0;
    // Call destructor explicitly
    std::shared_ptr<uWS::App>* ptr = static_cast<std::shared_ptr<uWS::App>*>(ud);
    ptr->~shared_ptr<uWS::App>();
    return 0;
}

int uw_create_app(lua_State *L) {
    if (!app) {
        app = std::make_shared<uWS::App>();
        main_L = L;
    }

    // Allocate userdata sized for a shared_ptr
    void* ud = lua_newuserdata(L, sizeof(std::shared_ptr<uWS::App>));
    new (ud) std::shared_ptr<uWS::App>(app); // placement-new construct shared_ptr

    // Ensure the metatable exists
    luaL_getmetatable(L, "uWS.App");
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);            // remove nil
        create_app_metatable(L);  // define it
        luaL_getmetatable(L, "uWS.App"); // push it again
    }

    // Set the metatable on userdata
    lua_setmetatable(L, -2);

    return 1;
}



int uw_restart_reregister(lua_State *L) {
    int port = luaL_checkinteger(L, 1);

    int cb_ref = LUA_NOREF;
    if (lua_gettop(L) >= 2 && lua_isfunction(L, 2)) {
        lua_pushvalue(L, 2);
        cb_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    }

    uWS::Loop::get()->defer([port, cb_ref]() {
        app = std::make_shared<uWS::App>();
        init_timer_system();

        if (main_L) {
            std::lock_guard<std::mutex> lock(lua_mutex);
            lua_getglobal(main_L, "on_restart_register");
            if (lua_isfunction(main_L, -1)) {
                // Create a proper app userdata with the same metatable as uw_create_app
                void* ud = lua_newuserdata(main_L, sizeof(std::shared_ptr<uWS::App>));
                new (ud) std::shared_ptr<uWS::App>(app); // placement-new construct shared_ptr
                
                // Set the same metatable used by uw_create_app
                luaL_getmetatable(main_L, "uWS.App");
                lua_setmetatable(main_L, -2);
                
                if (lua_pcall(main_L, 1, 0, 0) != LUA_OK) {
                    std::cerr << "[restart_reregister] Lua error: "
                              << lua_tostring(main_L, -1) << std::endl;
                    lua_pop(main_L, 1);
                }
            } else {
                lua_pop(main_L, 1);
                std::cerr << "[restart_reregister] No Lua on_restart_register() found" << std::endl;
            }
        }

        // Now listen
        app->listen(port, [port, cb_ref](auto *token) {
            std::lock_guard<std::mutex> lock(lua_mutex);

            if (token) {
                listen_socket = token;
                std::cout << "[restart_reregister] Listening on port " << port << std::endl;

                if (cb_ref != LUA_NOREF && main_L) {
                    lua_rawgeti(main_L, LUA_REGISTRYINDEX, cb_ref);
                    lua_pushboolean(main_L, 1);
                    lua_pushnil(main_L);
                    lua_pcall(main_L, 2, 0, 0);
                    luaL_unref(main_L, LUA_REGISTRYINDEX, cb_ref);
                }
            } else {
                std::cerr << "[restart_reregister] Failed to bind" << std::endl;
                if (cb_ref != LUA_NOREF && main_L) {
                    lua_rawgeti(main_L, LUA_REGISTRYINDEX, cb_ref);
                    lua_pushboolean(main_L, 0);
                    lua_pushstring(main_L, "bind failed");
                    lua_pcall(main_L, 2, 0, 0);
                    luaL_unref(main_L, LUA_REGISTRYINDEX, cb_ref);
                }
            }
        });
    });

    lua_pushboolean(L, 1);
    return 1;
}


extern "C" int luaopen_uwebsockets(lua_State *L) {
    create_metatables(L);     // req, res, websocket
    create_app_metatable(L);  // app

    luaL_Reg functions[] = {
        {"create_app", uw_create_app},
        {nullptr, nullptr}
    };

    luaL_newlib(L, functions);
    return 1;
}

