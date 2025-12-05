# TraceZL

This project is a compression tool written in C++ for very large ChampSim traces.
It uses [OpenZL](https://github.com/facebook/openzl) to automatically train a compression algorithm, that suite [ChampSim](https://github.com/ChampSim/ChampSim) trace format.

Source code are placed under src/ directory.

This project uses nix flakes to manage dependencies and uses CMake as build system.
Run `nix develop` if you found any tools or dependencies missing.

## Coding conventions

Do not use `using namespace xxx`, explicitly write the namespace when used.
Comply with C++17, do not use any newer C++ features, and do not use legacy C++ features.

## Before finalizing a Git commit

Please run formatter before forming a commit.
This project uses clang-format as formatter and the format is specified in `.clang-format` file.
