-- uwebsockets-0.1.0-1.rockspec
-- The package name is now "uwebsockets" to directly match the luaopen_uwebsockets function.

package = "uwebsockets" -- **CHANGED: Name now matches the C module's 'luaopen_uwebsockets' function**
version = "0.1.0-1" -- Format: <library_version>-<rockspec_revision>

-- The 'source' section tells LuaRocks where to download your project's source code.
-- **IMPORTANT:** This URL must point to a downloadable archive (e.g., .zip, .tar.gz)
-- that contains ALL your project files:
-- - build_deps.sh
-- - CMakeLists.txt
-- - src/shim.cpp
-- - The `src` directory containing your C++ source files.
source = {
   url = "https://github.com/yourusername/your-luauwebsockets-repo/archive/v0.1.0.zip", -- **REPLACE THIS WITH YOUR ACTUAL PROJECT'S RELEASE/SOURCE URL**
   dir = "your-luauwebsockets-repo-0.1.0" -- Adjust this if your archive extracts to a different directory name.
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
   homepage = "https://github.com/yourusername/your-luauwebsockets-repo", -- **REPLACE WITH YOUR ACTUAL PROJECT'S HOMEPAGE/REPOSITORY URL**
   license = "MIT" -- Ensure this matches the license of your project.
}

-- 'dependencies' lists other LuaRocks packages or system tools required to build or run your library.
dependencies = {
   "lua >= 5.1", -- Compatible with Lua 5.1 and newer versions (including LuaJIT).
   "cmake",      -- CMake is required to build the C++ binding.
   "git",        -- Git is required by 'build_deps.sh' to clone the uWebSockets repository.
   "unzip",      -- Or "tar" if your `source.url` uses a .tar.gz archive.

   -- System-level development dependencies that LuaRocks does not manage directly.
   -- Users installing your rock will need to ensure these are present on their system.
   -- Example package names: 'libssl-dev', 'zlib1g-dev' (Debian/Ubuntu); 'openssl-devel', 'zlib-devel' (RHEL/CentOS).
}

-- The 'build' section defines how to compile and install your library.
build = {
   type = "command", -- Use "command" type for custom shell commands.

   -- 'build_command' contains the commands to execute during the build phase.
   build_command = [[
      -- Ensure the build_deps.sh script is executable
      chmod +x build_deps.sh

      -- Execute build_deps.sh to clone uWebSockets and build the uSockets static library (.a)
      ./build_deps.sh

      -- Configure and build your Lua binding using CMake.
      -- IMPORTANT NOTE ON CMAKE PATH: Your CMakeLists.txt hardcodes '/usr/include/luajit-2.1'.
      -- For broader compatibility, you might need to adjust CMakeLists.txt to dynamically
      -- find LuaJIT headers or accept the path as a CMake argument.
      cmake .
      make
   ]],

   -- 'install_command' contains the commands to execute during the installation phase.
   install_command = [[
      -- Create the target directory for C modules if it doesn't already exist
      mkdir -p "$(LIBDIR)"
      -- Copy the compiled shared library (from 'src/uwebsockets.so' as per CMakeLists.txt)
      -- to the LuaRocks C module installation directory ($(LIBDIR)).
      -- It MUST be named 'uwebsockets.so' to match the 'luaopen_uwebsockets' function in shim.cpp.
      cp src/uwebsockets.so "$(LIBDIR)/uwebsockets.so"
   ]],

   -- When installing directly without an interface file, there are no separate Lua modules to list here.
   -- The C module itself is the primary component of the package.
   modules = {} -- Keep empty if no other pure Lua files are part of the direct install.
}