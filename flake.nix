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
      in
      {
        devShells.default = pkgs.mkShell {
          # zig is here for `zig cc`, which cross-compiles to musl and carries
          # its own libc, so a static build needs nothing else installed.
          packages = [
            pkgs.zig
            pkgs.clang
            pkgs.clang-tools
            pkgs.gnumake
            pkgs.pkg-config
            pkgs.git-cliff
            pkgs.valgrind
            pkgs.gdb

            # Benchmarking. Pinned here rather than taken from the machine so a
            # comparison is against a known starship, not whichever one happens
            # to be on PATH.
            pkgs.hyperfine
            pkgs.starship
          ];

          shellHook = ''
            export PIXY_LUA_SRC=${pkgs.lua5_4.src}
            export PIXY_BENCH_STARSHIP=${pkgs.starship}/bin/starship
            # A binary built here links this glibc, whose compiled-in TZDIR
            # holds no zoneinfo, so every zone would resolve to UTC. The static
            # musl builds that ship look in the standard paths and are fine.
            export TZDIR=''${TZDIR:-/usr/share/zoneinfo}
            echo "pixy: zig $(zig version), lua source at \$PIXY_LUA_SRC"
            echo "      starship $(starship --version | head -1 | cut -d' ' -f2) for comparison"
          '';
        };

        packages.default = pkgs.stdenv.mkDerivation {
          pname = "pixy";
          version = builtins.head (
            builtins.match ".*local PROJECT_VERSION = \"([0-9.]+)\".*" (builtins.readFile ./xmake.lua)
          );
          src = ./.;
          nativeBuildInputs = [ pkgs.zig ];
          buildPhase = ''
            export XDG_CACHE_HOME="$TMPDIR/zig-cache"
            make release-build CC="zig cc"
          '';
          installPhase = ''
            install -Dm755 build/pixy "$out/bin/pixy"
          '';
        };
      }
    );
}
