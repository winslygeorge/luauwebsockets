#!/bin/bash

set -e

# Define destination directory
UWS_DIR="uWebSockets"

# === 0. Install Build Dependencies ===
echo "[0/6] Installing system dependencies..."
sudo apt update
sudo apt install -y git cmake python3 g++ libssl-dev libpq-dev zlib1g-dev lua5.1 luajit libluajit-5.1-dev


# Clone uWebSockets with submodules if not already present
if [ ! -d "$UWS_DIR" ]; then
    echo "Cloning uWebSockets with submodules..."
    git clone --recurse-submodules https://github.com/uNetworking/uWebSockets.git
else
    echo "uWebSockets directory already exists. Skipping clone."
fi

# Build uSockets with -fPIC
echo "Building uSockets..."
cd "$UWS_DIR"
make clean
WITH_OPENSSL=1 CFLAGS="-fPIC" CXXFLAGS="-fPIC" make

echo "✅ Dependencies built successfully."
