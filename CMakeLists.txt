cmake_minimum_required(VERSION 3.20)
project(tracezl CXX)

set(CMAKE_CXX_STANDARD 17)

# We use the local openzl-src to ensure we have access to tools (training, io) 
# which might not be exported by standard install.
# Check if openzl-src exists
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/openzl-src/CMakeLists.txt")
    # Disable OpenZL options we don't need to speed up build
    set(OPENZL_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(OPENZL_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
    set(OPENZL_BUILD_PYTHON_EXT OFF CACHE BOOL "" FORCE)
    set(OPENZL_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    
    # We need tools for training
    set(OPENZL_BUILD_TOOLS ON CACHE BOOL "" FORCE)
    set(OPENZL_BUILD_LOGGER ON CACHE BOOL "" FORCE)
    set(OPENZL_BUILD_CLI OFF CACHE BOOL "" FORCE) # We don't need the CLI binary, just lib

    add_subdirectory(openzl-src)
else()
    message(FATAL_ERROR "openzl-src directory not found. Please clone OpenZL into openzl-src.")
endif()

add_executable(tracezl
    src/main.cpp
    src/compressor.cpp
)

# Link against OpenZL core and tools
target_link_libraries(tracezl PRIVATE 
    openzl 
    tools_training 
    tools_io
    logger # OpenZL logger
)

target_include_directories(tracezl PRIVATE 
    src
    "${CMAKE_CURRENT_SOURCE_DIR}/openzl-src/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/openzl-src" # For tools headers
)