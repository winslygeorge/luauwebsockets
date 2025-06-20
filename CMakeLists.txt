cmake_minimum_required(VERSION 3.12)
project(uwebsockets_lua_binding LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Set debug flags for C++ compiler
# This adds -g to CXX_FLAGS when building in Debug mode
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -g")

# Add your source
add_library(uwebsockets SHARED src/shim.cpp)

# Include paths
target_include_directories(uwebsockets PRIVATE
    ${CMAKE_SOURCE_DIR}/uWebSockets/src
    ${CMAKE_SOURCE_DIR}/uWebSockets/uSockets/src
    /usr/include/luajit-2.1
)

# Link static uSockets
target_link_libraries(uwebsockets
    ${CMAKE_SOURCE_DIR}/uWebSockets/uSockets/uSockets.a
    ssl crypto z pthread
)

# Output to src/
set_target_properties(uwebsockets PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/src
    PREFIX ""
    SUFFIX ".so"
)