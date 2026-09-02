local make = oslo.make

local NAME = "pixy"
local BIN = "build/pixy"
local PREFIX = os.getenv("PREFIX") or (os.getenv("HOME") .. "/.local")
local VERSION = assert(oslo.fs.read("xmake.lua"):match('local PROJECT_VERSION = "([%d%.]+)"'))

local HAVE_XMAKE = oslo.run{"sh", "-c", "command -v xmake", capture = true}.ok
local HAVE_NIX = oslo.run{"sh", "-c", "command -v nix", capture = true}.ok

local function xmake(...)
  if HAVE_XMAKE then return sh.xmake(...) end
  if HAVE_NIX then return sh.nix("develop", "--command", "xmake", ...) end
  error("install xmake, or install nix and use nix develop")
end

local function xmake_pinned(...)
  if HAVE_XMAKE and (os.getenv("PIXY_MUSL_CC") or "") ~= "" then
    return sh.xmake(...)
  end
  if HAVE_NIX then return sh.nix("develop", "--command", "xmake", ...) end
  return xmake(...)
end

local function xmake_developed(...)
  if HAVE_NIX and
      (not os.getenv("PIXY_BENCH_HYPERFINE") or not os.getenv("PIXY_BENCH_STARSHIP")) then
    return sh.nix("develop", "--command", "xmake", ...)
  end
  return xmake(...)
end

local function report()
  local stat = oslo.fs.stat(BIN)
  if stat then print(('%s %s: %d bytes'):format(NAME, VERSION, stat.size)) end
end

make.recipe({name = "version", desc = "print the project version", run = function()
  print(('%s v%s'):format(NAME, VERSION))
end})

make.recipe({name = "build", desc = "build the debug binary", run = function()
  xmake("pixy-build")
  report()
end})
make.alias("b", "build")

make.recipe({name = "release-build", desc = "build the optimized binary", run = function()
  xmake("release-build")
  report()
end})

make.recipe({name = "release-musl", desc = "build the static release binary", run = function()
  xmake_pinned("release-musl")
  report()
end})

make.recipe({name = "clean", desc = "remove build output", run = function()
  xmake("clean-all")
end})
make.recipe({name = "compile", desc = "clean and build", deps = {"clean", "build"}})
make.alias("c", "compile")

make.recipe({name = "test", desc = "run the optimized test suite", run = function()
  xmake("pixy-test")
end})
make.alias("t", "test")

make.recipe({name = "sanitize", desc = "run tests with ASan and UBSan", run = function()
  xmake("sanitize")
end})

make.recipe({
  name = "fuzz",
  desc = "fuzz palette arguments",
  params = {{"--rounds", desc = "number of rounds"}},
  run = function(args)
    if args.rounds then oslo.env.set("ROUNDS", tostring(args.rounds)) end
    xmake("fuzz")
  end,
})

make.recipe({name = "bench", desc = "run performance checks", run = function()
  xmake("bench")
end})
make.recipe({
  name = "bench-compare",
  desc = "compare matched providers against starship",
  params = {{"--runs", desc = "number of runs"}},
  run = function(args)
    if args.runs then oslo.env.set("PIXY_BENCH_RUNS", tostring(args.runs)) end
    xmake_developed("bench-compare")
  end,
})
make.recipe({name = "bench-phases", desc = "measure render phases", run = function()
  xmake("bench-phases")
end})
make.recipe({name = "example-pack", desc = "build the example sprite pack", run = function()
  xmake("example-pack")
end})
make.recipe({name = "package-check", desc = "check release inputs", run = function()
  xmake("package-check")
end})
make.recipe({name = "fmt", desc = "format C sources", run = function()
  xmake("fmt")
end})
make.recipe({name = "fmt-check", desc = "check C formatting", run = function()
  xmake("fmt-check")
end})

make.recipe({
  name = "verify",
  desc = "run all local checks",
  deps = {"fmt-check", "test", "bench", "package-check"},
})
make.alias("v", "verify")

make.recipe({
  name = "configs",
  desc = "install config into the user config directory",
  params = {{"--dest", desc = "override destination"}},
  run = function(args)
    local base = os.getenv("XDG_CONFIG_HOME") or (os.getenv("HOME") .. "/.config")
    local destination = args.dest or (base .. "/" .. NAME)
    sh.mkdir("-p", destination)
    for _, path in ipairs(oslo.fs.glob("config/*")) do
      sh.rsync("-a", "--ignore-existing", path, destination .. "/" .. oslo.path.name(path))
    end
    print("config -> " .. destination)
  end,
})

make.recipe({
  name = "install",
  desc = "install the static binary and starter config",
  deps = {"release-musl"},
  run = function()
    local destination = (os.getenv("DESTDIR") or "") .. PREFIX .. "/bin"
    sh.install("-d", destination)
    sh.install("-m", "755", BIN, destination .. "/" .. NAME)
    make.run("configs")
  end,
})

make.recipe({name = "uninstall", desc = "remove the installed binary", run = function()
  sh.rm("-f", PREFIX .. "/bin/" .. NAME)
end})

make.recipe({
  name = "release",
  desc = "cut a project version",
  params = {{"--type", desc = "patch, minor, major, or version"}},
  run = function(args)
    assert(type(args.type) == "string", "release requires --type")
    sh.git("rel", args.type)
  end,
})
