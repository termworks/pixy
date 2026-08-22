-- The version lives here and nowhere else: `veri` reads and bumps this line,
-- the build compiles it in as PIXY_VERSION_STRING, and the flake reads it from
-- here too, so a release never leaves two files disagreeing.
local PROJECT_VERSION = "0.1.7"

set_project("pixy")
set_version(PROJECT_VERSION)
set_xmakever("2.8.5")

set_languages("c11")
set_warnings("all", "extra", "pedantic")
set_config("builddir", "build/xmake")

-- The generators have to finish before the binary compiles: `pokemon_pack.c`
-- carries the sprite archive in with `.incbin`, so the pack must be a file on
-- disk by the time the assembler reads it, and the module tables are sources
-- the compiler is handed. Overlapping targets would race all three.
set_policy("build.fence", true)

option("musl")
    set_default(false)
    set_showmenu(true)
    set_description("Link a static binary that needs nothing on the target machine")
option_end()

option("sanitize")
    set_default(false)
    set_showmenu(true)
    set_description("Build with the address and undefined-behaviour sanitizers")
option_end()

-- Everything lands here rather than under the mode-specific xmake tree, because
-- the scripts, the tests and the release workflow all reach for `build/pixy`.
local OUTPUT = "build"

local LUA_SRC = "vendor/lua/src/*.c"
local MINIZ_SRC = {"vendor/miniz/miniz.c", "vendor/miniz/miniz_tdef.c", "vendor/miniz/miniz_tinfl.c"}
local LUA_MODULES = {
    "lua/pixy/style.lua", "lua/pixy/nodes.lua", "lua/pixy/layout.lua", "lua/pixy/ansi.lua",
    "lua/pixy/encode.lua", "lua/pixy/animate.lua", "lua/pixy/sprite.lua",
    "lua/pixy/segments/shell.lua", "lua/pixy/segments/git.lua", "lua/pixy/segments/system.lua",
    "lua/pixy/segments/progress.lua", "lua/pixy/init.lua",
}

local function common(target)
    target:add("includedirs", "src")
    -- As system headers: miniz defines a handful of functions we never call,
    -- and the warning belongs to whoever vendored it, not to every file that
    -- includes it.
    target:add("sysincludedirs", "vendor/lua/src", "vendor/miniz")
    target:add("defines", "LUA_USE_POSIX", "_GNU_SOURCE",
               'PIXY_VERSION_STRING="' .. PROJECT_VERSION .. '"')
    target:add("syslinks", "m")
end

-- ---------------------------------------------------------------- generators

-- Each generator produces its output as soon as it is built, rather than the
-- binary reaching for it: `add_deps` then guarantees the order, where a hook on
-- the consumer would run before its dependencies exist.

-- Compiles the bundled Lua to bytecode, which is otherwise parsed at every
-- prompt to reach the same functions. Runs on the machine doing the build.
target("lua_precompile")
    set_kind("binary")
    set_default(false)
    set_targetdir(OUTPUT)
    add_files("scripts/lua_precompile.c")
    add_files(LUA_SRC, {warnings = "none"})
    on_load(common)
    after_build(function(target)
        import("core.project.depend")
        local modules = path.join(OUTPUT, "lua_modules.c")
        depend.on_changed(function()
            os.vrunv(target:targetfile(), table.join({modules}, LUA_MODULES))
        end, {files = table.join(LUA_MODULES, {target:targetfile()}),
              dependfile = modules .. ".d"})
    end)
target_end()

target("pack_sprites")
    set_kind("binary")
    set_default(false)
    set_targetdir(OUTPUT)
    add_files("scripts/pack_sprites.c")
    add_files(MINIZ_SRC, {warnings = "none"})
    add_sysincludedirs("vendor/miniz")
    -- `-std=c11` alone hides `lstat`; the old build got it from the compiler's
    -- gnu default rather than by asking.
    add_defines("_GNU_SOURCE")
    add_syslinks("m")
    after_build(function(target)
        local pack = path.join(OUTPUT, "pokemon.pack")
        if not os.isfile(pack) then
            os.vrunv(target:targetfile(), {"docs/assets/pokemon", pack})
        end
    end)
target_end()

-- ------------------------------------------------------------------- binary

target("pixy")
    set_kind("binary")
    set_default(true)
    set_targetdir(OUTPUT)
    add_deps("lua_precompile", "pack_sprites")
    add_files("src/*.c")
    -- Vendored code is not ours to hold to our warning settings; Lua's dispatch
    -- loop is a computed goto and miniz trips several on its own.
    add_files(LUA_SRC, {warnings = "none"})
    add_files(MINIZ_SRC, {warnings = "none"})
    -- Generated: one string literal per embedded file, longer than the standard
    -- obliges a compiler to support, which is the point of generating them.
    add_files(OUTPUT .. "/lua_modules.c", OUTPUT .. "/texts.c",
              {always_added = true, warnings = "none"})

    on_load(function(target)
        common(target)
        if is_mode("release") then
            target:set("optimize", "fastest")
            target:add("defines", "NDEBUG")
            target:set("strip", "all")
        else
            target:set("symbols", "debug")
            -- `-Og`, not `-O0`: a hardened toolchain defines _FORTIFY_SOURCE,
            -- which warns once per file when nothing is optimized.
            target:set("optimize", "smallest")
            target:add("cflags", "-Og", {force = true})
        end
        if has_config("sanitize") then
            target:add("cflags", "-fsanitize=address,undefined", "-fno-omit-frame-pointer")
            target:add("ldflags", "-fsanitize=address,undefined", {force = true})
            target:set("strip", "none")
            target:set("symbols", "debug")
            -- Its own file. Sharing `build/pixy` with the ordinary build means
            -- the last one written wins, and the next build finds a binary
            -- newer than its objects and relinks nothing -- so `bench` after
            -- `sanitize` measured the sanitized binary and called it a
            -- regression.
            target:set("basename", "pixy-sanitize")
            -- `-O1`, not the mode's `-O0`: the suite holds the binary to the
            -- 100ms render deadline, and unoptimized the largest sprite takes
            -- 110ms, so it would fail a bound it was never held to.
            target:set("optimize", "fast")
        end
        if has_config("musl") then
            -- `-no-pie` as well: a position-independent executable is the
            -- default here, and it quietly wins over `-static`, which is how a
            -- "static" build comes out dynamically linked.
            target:add("ldflags", "-static", "-no-pie", {force = true})
        end
    end)

    -- The generated sources have to exist before the compiler is asked for them,
    -- and they are cheap enough to regenerate whenever their inputs move.
    before_build(function(target)
        import("core.project.depend")
        local texts = path.join(OUTPUT, "texts.c")
        local text_inputs = {
            "lua/pixy/default.lua", "examples/hexe-oslo.lua", "examples/shell/init.bash",
            "examples/shell/init.zsh", "examples/shell/init.fish", "examples/shell/init.oslo",
        }
        os.mkdir(OUTPUT)
        depend.on_changed(function()
            -- Run from the project root: a hook's working directory is not
            -- guaranteed to be it, and every path here is relative to it.
            os.vrunv("bash", {
                "scripts/embed_text.sh", "text", texts,
                "PIXY_DEFAULT_CONFIG=lua/pixy/default.lua",
                "PIXY_HEXE_OSLO_CONFIG=examples/hexe-oslo.lua",
                "PIXY_BASH_INIT=examples/shell/init.bash",
                "PIXY_ZSH_INIT=examples/shell/init.zsh",
                "PIXY_FISH_INIT=examples/shell/init.fish",
                "PIXY_OSLO_INIT=examples/shell/init.oslo",
            }, {curdir = os.projectdir()})
        end, {files = table.join(text_inputs, {"scripts/embed_text.sh"}), dependfile = texts .. ".d"})

    end)
target_end()

-- -------------------------------------------------------------------- tasks
--
-- Everything the Makefile used to drive. `make` still works -- it is a
-- pass-through now -- so `make test` and `xmake test` are the same thing.

do
    local root = path.normalize(path.absolute(os.scriptdir()))

    -- `os` at script load is a smaller sandbox than the one a task body gets:
    -- `execv` only exists inside `on_run`. So the running task hands its own
    -- back here, and everything below goes through these.
    local process
    local fail
    local report

    local function run_xmake(arguments)
        process.execv("xmake", arguments)
    end

    -- Walking PATH by hand: the helpers that would do this live in the task
    -- sandbox, and these functions were closed over the load-time one.
    local function which(program)
        for _, directory in ipairs(path.splitenv(process.getenv("PATH") or "")) do
            local candidate = path.join(directory, program)
            if os.isfile(candidate) then
                return candidate
            end
        end
        return nil
    end

    local function configure(mode, options)
        local arguments = {"f", "-c", "-y", "-m", mode}
        for name, value in pairs(options or {}) do
            table.insert(arguments, "--" .. name .. "=" .. (value and "y" or "n"))
        end
        -- An environment that has already chosen a toolchain gets to keep it:
        -- a Nix build sets these to its wrapped tools, and letting xmake detect
        -- its own instead is how a sandboxed build reaches for a compiler from
        -- outside the sandbox.
        -- Not LD: a compiler driver does the linking, and `$LD` is often the
        -- bare linker, which does not understand the flags it would be given.
        for flag, variable in pairs({cc = "CC", cxx = "CXX", ar = "AR"}) do
            local tool = process.getenv(variable)
            if tool and tool ~= "" then
                table.insert(arguments, "--" .. flag .. "=" .. tool)
            end
        end
        run_xmake(arguments)
    end

    -- The suite asserts production guarantees: the largest sprite renders in
    -- 10ms optimized and 110ms unoptimized, either side of the 100ms render
    -- deadline, so a debug build fails a bound it was never held to.
    local function release_build()
        configure("release", {musl = false, sanitize = false})
        run_xmake({"build", "pixy"})
    end

    -- Only for packaging, which inspects the artifact that is there rather than
    -- deciding how it was built: `package-check` after `release-musl` would
    -- otherwise rebuild it dynamically linked and check that instead, which is
    -- how a release ends up shipping the wrong file.
    --
    -- Everything that measures or tests calls `release_build()` instead. Reusing
    -- whatever was configured last meant `bench` after `sanitize` measured the
    -- sanitized binary and reported it as a regression.
    local function ensure_binary()
        if os.isfile(path.join(root, OUTPUT, "pixy")) then
            run_xmake({"build", "pixy"})
        else
            release_build()
        end
    end

    local function script(name, arguments, environment)
        process.execv("bash", table.join({path.join(root, "scripts", name)}, arguments or {}),
                 {envs = environment})
    end

    local function register(name, description, action)
        task(name)
            set_category("pixy")
            set_menu({usage = "xmake " .. name, description = description, options = {}})
            on_run(function()
                process = os
                fail = raise
                report = print
                action()
            end)
        task_end()
    end

    register("pixy-build", "Build the debug binary", function()
        configure("debug", {musl = false, sanitize = false})
        run_xmake({"build", "pixy"})
    end)

    register("release-build", "Build the optimized binary", function()
        release_build()
    end)

    register("release-musl", "Build a static binary for this machine", function()
        -- musl for preference: a statically linked glibc still warns about
        -- getaddrinfo and friends, and the release artifacts are musl.
        local compiler = process.getenv("PIXY_MUSL_CC") or which("musl-gcc")

        local arguments = {"f", "-c", "-y", "-m", "release", "--musl=y", "--sanitize=n"}
        if compiler then
            table.insert(arguments, "--cc=" .. compiler)
            table.insert(arguments, "--ld=" .. compiler)
        end
        run_xmake(arguments)
        run_xmake({"build", "pixy"})

        -- A release binary should need nothing on the machine it lands on, and
        -- "static" silently coming out dynamic is exactly the sort of thing
        -- that is only noticed by whoever the binary fails for.
        local kind = process.iorunv("file", {path.join(OUTPUT, "pixy")})
        if not kind:find("statically linked", 1, true) then
            fail("release-musl produced a dynamically linked binary: " .. kind:trim())
        end
        report(compiler and ("static, built with " .. compiler) or "static, built with the default cc")
    end)

    register("pixy-test", "Run the test suite", function()
        release_build()
        process.execv("bash", {path.join(root, "tests/run.sh")})
    end)

    register("sanitize", "The suite under the address and UB sanitizers", function()
        configure("debug", {musl = false, sanitize = true})
        run_xmake({"build", "pixy"})
        process.execv("bash", {path.join(root, "tests/run.sh")}, {envs = {
            PIXY = path.join(OUTPUT, "pixy-sanitize"),
            PIXY_BIN = path.join(OUTPUT, "pixy-sanitize"),
            ASAN_OPTIONS = "detect_leaks=1",
            UBSAN_OPTIONS = "print_stacktrace=1:halt_on_error=1",
        }})
    end)

    register("fuzz", "Random input at the palette surface", function()
        configure("debug", {musl = false, sanitize = true})
        run_xmake({"build", "pixy"})
        process.execv("bash", {path.join(root, "tests/fuzz.sh"), path.join(OUTPUT, "pixy-sanitize"),
                          process.getenv("ROUNDS") or "1000"},
                 {envs = {ASAN_OPTIONS = "detect_leaks=1", UBSAN_OPTIONS = "halt_on_error=1"}})
    end)

    register("smoke", "Run the CLI smoke", function()
        release_build()
        script("smoke.sh")
    end)

    register("smoke-shell", "Run the shell integration smoke", function()
        release_build()
        script("smoke_shell.sh")
    end)

    register("bench", "Run release performance checks", function()
        release_build()
        script("bench.sh")
    end)

    register("bench-compare", "Compare against starship (needs the dev shell)", function()
        release_build()
        script("bench_compare.sh")
    end)

    register("bench-phases", "Where a prompt spends its time", function()
        release_build()
        process.execv(path.join(root, OUTPUT, "pixy"), {"__bench", "phases", "400"})
        process.execv(path.join(root, OUTPUT, "pixy"), {"__bench", "compat-phases", "400"})
    end)

    register("example-pack", "Build the example sprite pack", function()
        ensure_binary()
        process.execv(path.join(root, OUTPUT, "pixy"), {
            "pack", "build", "examples/pack", "--output", path.join(OUTPUT, "pixy-example.pixypack"),
            "--source", "pixy", "--license", "MIT", "--attribution", "Pixy contributors",
        })
    end)

    register("package-check", "Check release artifact contents", function()
        run_xmake({"example-pack"})
        script("package_check.sh", {}, {RELEASE_DIR = OUTPUT})
    end)

    register("docs-images", "Regenerate the README frames from live output", function()
        release_build()
        script("docs_images.sh", {}, {PIXY = path.join(OUTPUT, "pixy")})
    end)

    register("fmt", "Format the C sources", function()
        local formatter = which("clang-format")
        if not formatter then return report("clang-format is not here; nothing formatted") end
        process.execv(formatter, {"-i", "src/*.c", "src/*.h", "scripts/*.c", "tests/*.c"},
                      {shell = true})
    end)

    register("fmt-check", "Check C formatting", function()
        local formatter = which("clang-format")
        if not formatter then return report("clang-format is not here; nothing checked") end
        process.exec(formatter .. " --dry-run --Werror src/*.c src/*.h scripts/*.c tests/*.c")
    end)

    register("verify", "Format check plus the suite", function()
        run_xmake({"fmt-check"})
        run_xmake({"pixy-test"})
    end)

    register("pixy-install", "Install into PREFIX/bin", function()
        -- The static build: what gets installed should need nothing on the
        -- machine it runs on, and a glibc build from a Nix shell cannot even
        -- resolve a timezone without TZDIR set.
        run_xmake({"release-musl"})
        local prefix = process.getenv("PREFIX") or path.join(process.getenv("HOME"), ".local")
        local destination = path.join(prefix, "bin")
        process.mkdir(destination)
        -- Renamed into place rather than copied over: a prompt runs this binary
        -- constantly, and writing to one that is executing is "text file busy".
        -- A rename swaps the name atomically and leaves the running copy alone.
        local installed = path.join(destination, "pixy")
        local staged = installed .. ".new"
        process.cp(path.join(OUTPUT, "pixy"), staged)
        process.mv(staged, installed)
        report("installed " .. installed)
    end)

    register("release", "Release a new version", function()
        local kind = process.getenv("TYPE")
        if not kind or kind == "" then
            fail("release type not specified: make release TYPE=[patch|minor|major|M.m.p]")
        end
        process.execv("git", {"rel", kind})
    end)

    register("clean-all", "Remove every build output", function()
        process.tryrm(path.join(root, OUTPUT))
    end)
end
