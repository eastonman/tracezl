#!/bin/bash
set -e

# Ensure we are in a nix environment or have cmake
if ! command -v cmake &> /dev/null; then
    echo "CMake not found. Please run 'nix develop' first."
    exit 1
fi

mkdir -p build
cd build
cmake ..
make -j$(nproc)

echo "Build complete. Binary is at build/tracezl"
