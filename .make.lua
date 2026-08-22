-- pixy's build, as recipes.
--
--   make                 the recipes, with what each of them says it does
--   make build           the debug binary
--   make test            the suite
--   make install         the static binary, into $PREFIX/bin
--   make verify          the whole local gate
--
-- At an oslo prompt in this directory `make` is enough; everywhere else it is `oslo make`.
--
-- `xmake.lua` is still the build — this drives it, the way hexe's recipes drive zig. What lives
-- here is everything the old Makefile could only say awkwardly: which recipes need an optimised
-- binary and which must not touch one, where xmake comes from when it is not installed, and what
-- each gate is actually for.

local make = oslo.make

---------------------------------------------------------------------------- what the build is

-- One place holds the version: `veri` reads and bumps this line, the build compiles it in, and the
-- flake reads it from there too.
local VERSION = oslo.fs.read("xmake.lua"):match('local PROJECT_VERSION = "([%d%.]+)"')
assert(VERSION, "xmake.lua is missing its PROJECT_VERSION line")

local NAME = "pixy"
local BIN = "build/pixy"
local PREFIX = os.getenv("PREFIX") or (os.getenv("HOME") .. "/.local")

-- xmake is not something to have installed before a clone will build. If it is missing and nix is
-- here, borrow it from the dev shell so a fresh checkout still works without being told to enter
-- anything first.
local HAVE_XMAKE = oslo.run{ "sh", "-c", "command -v xmake", capture = true }.ok
local HAVE_NIX = oslo.run{ "sh", "-c", "command -v nix", capture = true }.ok

local function xmake(...)
  if HAVE_XMAKE then
    sh.xmake(...)
  elseif HAVE_NIX then
    sh.nix("develop", "--command", "xmake", ...)
  else
    error("pixy builds with xmake: install it, or install nix and use `nix develop`")
  end
end

---------------------------------------------------------------------------- building

make.recipe{ name = "version", desc = "what this checkout calls itself",
             run = function() print(("%s v%s"):format(NAME, VERSION)) end }

make.recipe{
  name = "build",
  desc = "the debug binary",
  run = function() xmake("pixy-build") end,
}

make.alias("b", "build")

make.recipe{
  name = "release-build",
  desc = "the optimised binary",
  run = function() xmake("release-build") end,
}

-- The one that gets installed and shipped. It refuses to finish unless the result really is
-- static, because "static" quietly coming out dynamic is only ever noticed by whoever the binary
-- fails for -- and a glibc build from a Nix shell cannot even resolve a timezone.
make.recipe{
  name = "release-musl",
  desc = "a static binary that needs nothing on the target machine",
  run = function() xmake("release-musl") end,
}

make.recipe{ name = "clean", desc = "remove every build output",
             run = function() xmake("clean-all") end }

make.recipe{ name = "compile", desc = "clean, then build", deps = { "clean", "build" } }
make.alias("c", "compile")

---------------------------------------------------------------------------- checking

-- Against the optimised binary, because the suite asserts production guarantees: the largest
-- sprite renders in 10ms optimised and 110ms unoptimised, either side of the 100ms render
-- deadline, so a debug build fails a bound it was never held to.
make.recipe{
  name = "test",
  desc = "the suite, against the optimised binary",
  run = function() xmake("pixy-test") end,
}

make.alias("t", "test")

-- Its own binary, at `build/pixy-sanitize`. Sharing `build/pixy` meant the last build written won
-- and the next one relinked nothing, so `bench` after `sanitize` measured the sanitized binary and
-- reported it as a regression.
make.recipe{
  name = "sanitize",
  desc = "the suite under the address and UB sanitizers",
  run = function() xmake("sanitize") end,
}

make.recipe{
  name = "fuzz",
  desc = "random input at the palette surface: --rounds N",
  params = { { "--rounds", desc = "argument rounds, 1000 by default" } },
  run = function(a)
    if a.rounds then oslo.env.set("ROUNDS", tostring(a.rounds)) end
    xmake("fuzz")
  end,
}

make.recipe{ name = "smoke", desc = "the CLI smoke",
             run = function() xmake("smoke") end }

make.recipe{ name = "smoke-shell", desc = "the shell integration smoke",
             run = function() xmake("smoke-shell") end }

make.recipe{ name = "package-check", desc = "check the release artifact is complete",
             run = function() xmake("package-check") end }

make.recipe{ name = "example-pack", desc = "build the example sprite pack",
             run = function() xmake("example-pack") end }

make.recipe{ name = "fmt", desc = "format the C sources",
             run = function() xmake("fmt") end }

make.recipe{ name = "fmt-check", desc = "fail if anything is unformatted",
             run = function() xmake("fmt-check") end }

make.recipe{
  name = "verify",
  desc = "the whole local gate",
  deps = { "fmt-check", "test", "smoke", "smoke-shell", "bench", "package-check" },
}

make.alias("v", "verify")

---------------------------------------------------------------------------- measuring

-- A prompt is a process: what it costs is paid on every command, before the shell gives the
-- terminal back. These measure that, not what happens once a runtime is warm.
make.recipe{
  name = "bench",
  desc = "the performance budgets: --scale N widens every one",
  params = { { "--scale", desc = "widen each time budget, for a slower machine" } },
  run = function(a)
    if a.scale then oslo.env.set("PIXY_BENCH_SCALE", tostring(a.scale)) end
    xmake("bench")
  end,
}

make.recipe{
  name = "bench-compare",
  desc = "against starship, both pinned by the flake",
  run = function() xmake("bench-compare") end,
}

make.recipe{
  name = "bench-phases",
  desc = "where a prompt spends its time, so an optimisation can be aimed",
  run = function() xmake("bench-phases") end,
}

---------------------------------------------------------------------------- shipping

make.recipe{
  name = "docs-images",
  desc = "regenerate the README frames from live output",
  run = function() xmake("docs-images") end,
}

-- The static build, staged and renamed into place. A prompt runs this binary constantly, and
-- writing over one that is executing is "text file busy"; a rename swaps the name atomically and
-- leaves the running copy alone.
make.recipe{
  name = "install",
  desc = "the static binary, into $PREFIX/bin",
  run = function()
    xmake("pixy-install")
    local installed = PREFIX .. "/bin/pixy"
    local reported = oslo.run{ installed, "--version", capture = true }
    print(("installed %s (%s)"):format(installed, (reported.out or "?"):gsub("%s+$", "")))
  end,
}

make.recipe{
  name = "uninstall",
  desc = "take it back out of $PREFIX/bin",
  run = function() sh.rm("-f", PREFIX .. "/bin/pixy") end,
}

make.recipe{
  name = "release",
  desc = "cut a version: --type patch | minor | major | M.m.p",
  params = { { "--type", desc = "patch | minor | major | M.m.p" } },
  run = function(a)
    assert(oslo.run{ "sh", "-c", "command -v git-rel", capture = true }.ok,
           "git-rel is not installed; install it first")
    assert(type(a.type) == "string",
           "which release? make release --type patch|minor|major|M.m.p")
    sh.git("rel", a.type)
  end,
}
