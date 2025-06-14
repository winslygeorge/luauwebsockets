-- uwebsockets-0.1.0-1.rockspec
-- The package name is "uwebsockets" to directly match the luaopen_uwebsockets function.

package = "uwebsockets"
version = "0.1.0-1" -- Format: <library_version>-<rockspec_revision>

-- The 'source' section tells LuaRocks where to download your project's source code.
source = {
   url = "https://github.com/winslygeorge/luauwebsockets/archive/master.zip",
   dir = "luauwebsockets-master" -- Adjust this if your archive extracts to a different directory name (e.g., based on a tag v0.1.0)
}

-- 'description' provides metadata about your library.
description = {
   summary = "Direct Lua binding for uWebSockets with WebSocket and HTTP capabilities.",
   detailed = [[
      This library provides a direct Lua binding for the high-performance uWebSockets
      library, enabling Lua applications to act as efficient WebSocket and HTTP
      servers or clients. It leverages uWebSockets' native C++ performance.
      Requires: require("uwebsockets")
   ]],
   homepage = "https://github.com/winslygeorge/luauwebsockets",
   license = "MIT" -- Ensure this matches the license of your project.
}

-- 'dependencies' lists other LuaRocks packages or system tools required to build or run your library.
dependencies = {
   "lua >= 5.1", -- Compatible with Lua 5.1 and newer versions (including LuaJIT).
}

-- The 'build' section defines how to compile and install your library.
build = {
   type = "command", -- Use "command" type for custom shell commands.

   -- 'build_command' contains the commands to execute during the build phase.
   -- **ALL COMMENTS REMOVED FROM THIS SECTION**
   build_command = [[
      chmod +x build_deps.sh
      ./build_deps.sh
      cmake .
      make
   ]],

   -- 'install_command' contains the commands to execute during the installation phase.
   -- **ALL COMMENTS REMOVED FROM THIS SECTION**
   install_command = [[
      mkdir -p "$(LIBDIR)"
      cp src/uwebsockets.so "$(LIBDIR)/uwebsockets.so"
   ]],

   modules = {}
}