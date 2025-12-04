cmake_minimum_required(VERSION 3.20)
project(tracezl CXX)

set(CMAKE_CXX_STANDARD 17)

find_package(OpenZL REQUIRED)

# If OpenZL was found, these should be available.
# However, our custom OpenZL package manually installed tools libraries.
# We need to find them.

find_path(OPENZL_TOOLS_INCLUDE_DIR tools/io/InputSetBuilder.h)
include_directories(${OPENZL_TOOLS_INCLUDE_DIR})

# Find static libraries for tools
find_library(LIB_TOOLS_TRAINING tools_training REQUIRED)
find_library(LIB_TOOLS_IO tools_io REQUIRED)
find_library(LIB_LOGGER logger REQUIRED)

find_package(CLI11 REQUIRED)

add_executable(tracezl
    src/main.cpp
    src/compressor.cpp
)

# Link against OpenZL core and tools
target_link_libraries(tracezl PRIVATE
    ${LIB_TOOLS_TRAINING}
    ${LIB_TOOLS_IO}
    ${LIB_LOGGER}
    OpenZL::openzl_cpp
    OpenZL::openzl
    CLI11::CLI11
)

target_include_directories(tracezl PRIVATE
    src
    "${CMAKE_CURRENT_SOURCE_DIR}/openzl-src/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/openzl-src" # For tools headers
)
target_include_directories(tracezl PRIVATE src)

install(TARGETS tracezl DESTINATION bin)
