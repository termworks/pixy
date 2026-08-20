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
          ];

          shellHook = ''
            export PIXY_LUA_SRC=${pkgs.lua5_4.src}
            echo "pixy: zig $(zig version), lua source at \$PIXY_LUA_SRC"
          '';
        };

        packages.default = pkgs.stdenv.mkDerivation {
          pname = "pixy";
          version = builtins.head (
            builtins.match ".*\n([0-9.]+)\n?" (builtins.readFile ./PROJECT)
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
