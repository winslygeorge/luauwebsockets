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
static std::unordered_map<int, int> lua_callbacks;
static int callback_id_counter = 0;

struct DummyUserData {};

// Middleware structures
struct Middleware {
    int ref; // Lua function reference
    bool global; // true for global, false for route-specific
    std::string route; // Route for route-specific middleware
};

static std::vector<Middleware> middlewares;

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
    size_t len = lua_objlen(L, 2);
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
                     lua_pushlstring(main_L, data.data(), data.size());
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

// Assuming fs, app, and other necessary includes/definitions are present
// Assuming you have uWebSockets.h or similar for uWS::App and Response/Request types
// #include "uWebSockets.h"

// Assuming 'app' is globally available, e.g.:
// uWS::App* app = nullptr;
// namespace fs = std::filesystem; // Alias for std::filesystem

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
        {"listen", uw_listen},
        {"run", uw_run},
        {"use", uw_use},
        {"serve_static", uw_serve_static}, // Add the new function
        {nullptr, nullptr}
    };

    luaL_newlib(L, functions);
    return 1;
}