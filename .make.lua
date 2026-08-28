-- pixy's build, as recipes.
--
--   make                 the recipes, with what each of them says it does
--   make build           the debug binary
--   make test            the suite
--   make install         the static binary, into $PREFIX/bin
--   make configs         config/ into $XDG_CONFIG_HOME/pixy
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

-- For the recipes that need a tool the flake pins rather than one the machine
-- happens to have. The static build wants a musl compiler; without one, `-static`
-- is handed to a glibc that ships no `libc.a` and the link fails on `-lc`.
local function xmake_pinned(...)
  local have_musl = (os.getenv("PIXY_MUSL_CC") or "") ~= ""
  if HAVE_XMAKE and have_musl then
    sh.xmake(...)
  elseif HAVE_NIX then
    sh.nix("develop", "--command", "xmake", ...)
  else
    xmake(...)
  end
end

local function absolute(path)
  if oslo.path.is_absolute(path) then return oslo.path.normalize(path) end
  return oslo.path.normalize(oslo.path.join(oslo.fs.cwd(), path))
end

-- Whether `dir` is somewhere `$PATH` already looks. Compared as absolute paths,
-- because `$PATH` carries whatever spelling was put in it.
local function on_path(dir)
  local want = absolute(dir)
  for entry in ((os.getenv("PATH") or "") .. ":"):gmatch("([^:]*):") do
    if entry ~= "" and absolute(entry) == want then return true end
  end
  return false
end

---------------------------------------------------------------------------- saying what was built

local function dim(text)
  return oslo.ui.style(text, { dim = true })
end

local function line(label, value)
  print(dim(oslo.ui.pad(label, 8)) .. value)
end

-- `2242256` → `2,242,256`. A number this long is read in groups or not at all.
local function grouped(n)
  local text = tostring(math.floor(n))
  local out = text:sub(-3)
  local at = #text - 3
  while at > 0 do
    out = text:sub(math.max(1, at - 2), at) .. "," .. out
    at = at - 3
  end
  return out
end

-- Asked of the ELF, not assumed. pixy has three builds and only one of them is
-- static, so a report that always claimed "static" would be wrong twice out of
-- three -- and "static" quietly coming out dynamic is the exact thing the
-- release build refuses to ship.
local function linkage(path)
  local segments = oslo.run{ "readelf", "-l", path, capture = true }
  local dynamic = oslo.run{ "readelf", "-d", path, capture = true }
  if not segments.ok then return nil end
  local interpreted = (segments.out or ""):find("program interpreter")
  local needed = (dynamic.out or ""):find("NEEDED")
  if interpreted or needed then return "dynamic" end
  return "static"
end

local function report(path)
  local stat = oslo.fs.stat(path)
  if not stat then return end
  local dir = oslo.path.parent(path)
  local megabytes = ("%.2f MB"):format(stat.size / 1048576)

  print("")
  print(oslo.ui.title(("%s %s   %s"):format(NAME, VERSION, megabytes)))
  line("binary", path)
  -- Bytes beside megabytes: `2.14 MB` cannot be subtracted from last week's
  -- `2.11 MB` to get one.
  line("size", megabytes .. dim("   " .. grouped(stat.size) .. " bytes"))

  local kind = linkage(path)
  if kind == "static" then
    line("linking", oslo.ui.style("✓ static", { fg = "green" }) ..
                    dim("   no runtime dependencies"))
  elseif kind == "dynamic" then
    line("linking", oslo.ui.style("dynamic", { fg = "yellow" }) ..
                    dim("   make release-musl for the one that ships"))
  end

  if on_path(dir) then
    line("path", oslo.ui.style("✓ on $PATH", { fg = "green" }) .. dim("  " .. dir))
  else
    line("path", oslo.ui.style("✗ not on $PATH", { fg = "yellow" }))
    print(oslo.ui.subtitle(('         add to .env.lua:  oslo.direnv.path_add("%s")'):format(dir)))
  end
  print("")
end

---------------------------------------------------------------------------- building

make.recipe{ name = "version", desc = "what this checkout calls itself",
             run = function() print(("%s v%s"):format(NAME, VERSION)) end }

make.recipe{
  name = "build",
  desc = "the debug binary",
  run = function()
    xmake("pixy-build")
    report(BIN)
  end,
}

make.alias("b", "build")

make.recipe{
  name = "release-build",
  desc = "the optimised binary",
  run = function()
    xmake("release-build")
    report(BIN)
  end,
}

-- The one that gets installed and shipped. It refuses to finish unless the result really is
-- static, because "static" quietly coming out dynamic is only ever noticed by whoever the binary
-- fails for -- and a glibc build from a Nix shell cannot even resolve a timezone.
make.recipe{
  name = "release-musl",
  desc = "a static binary that needs nothing on the target machine",
  run = function()
    xmake_pinned("release-musl")
    report(BIN)
  end,
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

make.recipe{
  name = "install",
  desc = "put the static binary in $PREFIX/bin, and config/ where it reads it",
  deps = { "release-musl" },
  run = function()
    local dest = (os.getenv("DESTDIR") or "") .. PREFIX .. "/bin"
    local target = dest .. "/" .. NAME
    sh.install("-d", dest)
    -- Staged, then renamed. A prompt runs this binary on every command, and
    -- `install` writes the destination in place -- which on a file that is
    -- executing is "text file busy". A rename swaps the name atomically and
    -- leaves whatever is still running alone.
    sh.install("-m", "755", BIN, target .. ".new")
    os.rename(target .. ".new", target)

    local reported = oslo.run{ target, "--version", capture = true }
    print(oslo.ui.style("✓ ", { fg = "green" }) .. target ..
          "  " .. ((reported.out or ""):gsub("%s+$", "")))
    if not on_path(dest) then
      print(oslo.ui.subtitle(("  %s is not on $PATH, so `%s` still finds something else")
        :format(dest, NAME)))
    end
    -- Last, and part of the install rather than a step to remember: a binary newer than
    -- the config it reads is how a setting that shipped together with it silently does
    -- nothing. Run alone, `configs` still installs only the config.
    make.run("configs")
  end,
}

-- pixy's own configuration lives in `config/`, and this installs it: `config/*` becomes
-- `$XDG_CONFIG_HOME/pixy/*`. The repository is the copy that is edited and reviewed; the one under
-- `~/.config` is a deployment of it.
make.recipe{
  name = "configs",
  desc = "install config/ into $XDG_CONFIG_HOME/pixy",
  params = { { "--dest", desc = "somewhere other than the config directory" } },
  run = function(a)
    assert(oslo.run{ "sh", "-c", "command -v rsync", capture = true }.ok,
           "rsync is not installed; install it first")
    -- Asked of git rather than assumed from the working directory, so this works from anywhere in
    -- the tree. Outside a repository, where the command was run is the best answer available.
    local top = oslo.run{ "git", "rev-parse", "--show-toplevel", capture = true }
    local root = top.ok and (top.out or ""):match("^%s*(.-)%s*$") or ""
    if root == "" then root = oslo.fs.cwd() end
    local source = root .. "/config"
    assert(oslo.fs.stat(source .. "/"), "there is no config/ directory in " .. root)

    local dest = a.dest
    if not dest then
      local config = os.getenv("XDG_CONFIG_HOME")
      if not config or config == "" then config = os.getenv("HOME") .. "/.config" end
      dest = config .. "/" .. NAME
    end
    sh.mkdir("-p", dest)

    -- One entry at a time, each mirrored with --delete, rather than one --delete over the whole
    -- tree: the destination is where anything else you keep beside init.lua lives -- a sprite pack,
    -- a theme you are trying out -- and a tree-wide mirror would take it with it.
    local synced = 0
    for _, path in ipairs(oslo.fs.glob(source .. "/*")) do
      local name = oslo.path.name(path)
      if oslo.fs.stat(path .. "/") then
        sh.mkdir("-p", dest .. "/" .. name)
        sh.rsync("-a", "--delete", path .. "/", dest .. "/" .. name .. "/")
      else
        sh.rsync("-a", path, dest .. "/" .. name)
      end
      synced = synced + 1
    end
    print(oslo.ui.style("✓ ", { fg = "green" }) ..
          ("%d entr%s -> %s"):format(synced, synced == 1 and "y" or "ies", dest))
    print(oslo.ui.subtitle("  anything else in that directory is left alone"))
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
