{
  description = "ChampSim Trace Compression with OpenZL";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-25.11";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { self, nixpkgs, flake-utils }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = nixpkgs.legacyPackages.${system};

        # OpenZL Derivation
        openzl = pkgs.stdenv.mkDerivation {
          pname = "openzl";
          version = "0.1.0-unstable-2025-11-30";

          src = pkgs.fetchFromGitHub {
            owner = "facebook";
            repo = "openzl";
            rev = "cf58428033d111df9862792d1111714880129a89";
            sha256 = "sha256-YdZC2r7SToJIp5y/1XXIZXKy32zg96AnFG0FOr4aGxs=";
          };

          # We need zstd source for OpenZL's internal build process
          zstdSrc = pkgs.fetchurl {
            url = "https://github.com/facebook/zstd/releases/download/v1.5.7/zstd-1.5.7.tar.gz";
            sha256 = "sha256-6zPlH0mhXgI5UM14Jcp0pKK0Pbg1SCWsJPwbfuCeb6M=";
          };

          nativeBuildInputs = [ pkgs.cmake ];
          propagatedBuildInputs = [ pkgs.zstd ]; # System zstd might be useful, but OpenZL insists on its own

          # Prepare zstd source in the expected location
          preConfigure = ''
            mkdir -p deps/zstd
            tar --strip-components=1 -xf $zstdSrc -C deps/zstd
          '';

          # Fix mismatched variable names in openzl-config.cmake.in
          postPatch = ''
            substituteInPlace build/cmake/openzl-config.cmake.in \
              --replace "@PACKAGE_INCLUDE_INSTALL_DIR@" "@PACKAGE_OPENZL_INSTALL_INCLUDEDIR@" \
              --replace "@PACKAGE_CMAKE_INSTALL_DIR@" "@PACKAGE_OPENZL_INSTALL_CMAKEDIR@" \
              --replace "include(CMakeFindDependencyMacro)" "include(CMakeFindDependencyMacro)
find_dependency(zstd)"
          '';

          cmakeFlags = [
            "-DOPENZL_BUILD_TOOLS=ON"
            "-DOPENZL_BUILD_CLI=OFF"
            "-DOPENZL_BUILD_TESTS=OFF"
            "-DOPENZL_BUILD_BENCHMARKS=OFF"
            "-DOPENZL_BUILD_PYTHON_EXT=OFF"
            "-DOPENZL_BUILD_EXAMPLES=OFF"
            "-DOPENZL_BUILD_LOGGER=ON" # We enabled this in our manual build
          ];

          # OpenZL doesn't install internal tools by default. We need them.
          postInstall = ''
            # Install tool libraries
            cp tools/training/libtools_training.a $out/lib/
            cp tools/io/libtools_io.a $out/lib/
            cp tools/logger/liblogger.a $out/lib/

            # Install tool headers (preserving structure)
            mkdir -p $out/include/tools
            cp -r ../tools/* $out/include/tools/

            # Clean up source files from include if copied
            find $out/include/tools -name "*.cpp" -delete
            find $out/include/tools -name "*.c" -delete
            find $out/include/tools -name "CMakeLists.txt" -delete
          '';
        };
      in
      {
        packages.openzl = openzl;
        packages.default = pkgs.stdenv.mkDerivation {
          pname = "tracezl";
          version = "0.1.0";

          src = ./.;

          nativeBuildInputs = [ pkgs.cmake ];
          buildInputs = [ openzl pkgs.zstd ];

          cmakeFlags = [
            "-DOPENZL_ROOT=${openzl}"
          ];
        };

        devShells.default = pkgs.mkShell {
          buildInputs = [
            pkgs.cmake
            pkgs.gcc
            pkgs.git
            openzl
          ];
        };
      }
    );
}
