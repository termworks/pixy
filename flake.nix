{
  description = "pixy: a Lua terminal painter in C";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs?rev=4c1018dae018162ec878d42fec712642d214fdfa";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs =
    { nixpkgs, flake-utils, ... }:
    flake-utils.lib.eachDefaultSystem (
      system:
      let
        pkgs = import nixpkgs { inherit system; };

        # A single-word compiler that links musl statically. xmake wants one
        # program, not "zig cc", and nixpkgs' glibc ships no static libraries,
        # so `-static` against it cannot link at all.
        muslcc = pkgs.writeShellScriptBin "pixy-musl-cc" ''
          exec ${pkgs.zig}/bin/zig cc -target ${
            if pkgs.stdenv.hostPlatform.isAarch64 then "aarch64" else "x86_64"
          }-linux-musl "$@"
        '';
      in
      {
        devShells.default = pkgs.mkShell {
          # zig is here for `zig cc`, which cross-compiles to musl and carries
          # its own libc, so a static build needs nothing else installed.
          packages = [
            muslcc
            pkgs.xmake
            pkgs.zig
            pkgs.clang
            pkgs.clang-tools
            pkgs.gnumake
            pkgs.pkg-config
            pkgs.git-cliff
            pkgs.valgrind
            pkgs.gdb
            pkgs.hyperfine
            pkgs.starship
          ];

          shellHook = ''
            export PIXY_MUSL_CC=${muslcc}/bin/pixy-musl-cc
            export PIXY_BENCH_HYPERFINE=${pkgs.hyperfine}/bin/hyperfine
            export PIXY_BENCH_STARSHIP=${pkgs.starship}/bin/starship
            # A binary built here links this glibc, whose compiled-in TZDIR
            # holds no zoneinfo, so every zone would resolve to UTC. The static
            # musl builds that ship look in the standard paths and are fine.
            export TZDIR=''${TZDIR:-/usr/share/zoneinfo}
            echo "pixy: xmake $(xmake --version | head -n 1), zig $(zig version)"
          '';
        };

        packages.default = pkgs.stdenv.mkDerivation {
          pname = "pixy";
          version = builtins.head (
            builtins.match ".*local PROJECT_VERSION = \"([0-9.]+)\".*" (builtins.readFile ./xmake.lua)
          );
          src = ./.;
          nativeBuildInputs = [ pkgs.xmake ];
          buildPhase = ''
            export XDG_CACHE_HOME="$TMPDIR/xmake-cache"
            export XMAKE_ROOT=y
            # The source copy can carry a build directory from the working tree,
            # and the toolchain paths cached in it are this machine's, not the
            # sandbox's.
            rm -rf build .xmake
            # Absolute paths, not bare names: a bare `gcc` is resolved off PATH,
            # and picking up the host toolchain is how a build ends up linking
            # against a compiler that is not the one this derivation pins.
            export CC="${pkgs.stdenv.cc}/bin/cc"
            export CXX="${pkgs.stdenv.cc}/bin/c++"
            export AR="${pkgs.stdenv.cc.bintools.bintools}/bin/ar"
            xmake release-build
          '';
          installPhase = ''
            install -Dm755 build/pixy "$out/bin/pixy"
          '';
        };
      }
    );
}
