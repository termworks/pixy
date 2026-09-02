-- The version lives here and nowhere else: `veri` reads and bumps this line,
-- the build compiles it in as PIXY_VERSION_STRING, and the flake reads it from
-- here too, so a release never leaves two files disagreeing.
local PROJECT_VERSION = "0.3.0"

set_project("pixy")
set_version(PROJECT_VERSION)
set_xmakever("2.8.5")

set_languages("c11")
set_warnings("all", "extra", "pedantic")
set_config("builddir", "build/xmake")

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

-- Stable output location for tests and release workflows.
local OUTPUT = "build"

local LUA_SRC = "vendor/lua/src/*.c"
local MINIZ_SRC = {"vendor/miniz/miniz.c", "vendor/miniz/miniz_tdef.c", "vendor/miniz/miniz_tinfl.c"}
local EMBEDDED_TEXT = {
    {"PIXY_BASH_INIT", "examples/shell/init.bash"},
    {"PIXY_ZSH_INIT", "examples/shell/init.zsh"},
    {"PIXY_FISH_INIT", "examples/shell/init.fish"},
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

-- The build cache compiles a preprocessed copy of each source, and that copy
-- carries `# 1 "file"` line markers -- which `-Wpedantic` calls a GNU extension,
-- once per file, about a file nobody wrote. Asked of the compiler rather than
-- assumed, and in on_config because on_load runs before one is chosen.
local function quiet_line_markers(target)
    if target:has_cflags("-Wno-gnu-line-marker") then
        target:add("cflags", "-Wno-gnu-line-marker")
    end
end

-- ------------------------------------------------------------------- binary

target("pixy")
    set_kind("binary")
    set_default(true)
    set_targetdir(OUTPUT)
    add_rules("utils.bin2obj")
    add_files("assets/pokemon.hxsp", {rule = "utils.bin2obj"})
    add_files("src/*.c")
    -- Vendored code is not ours to hold to our warning settings; Lua's dispatch
    -- loop is a computed goto and miniz trips several on its own.
    add_files(LUA_SRC, {warnings = "none"})
    add_files(MINIZ_SRC, {warnings = "none"})
    -- Generated: one string literal per embedded file, longer than the standard
    -- obliges a compiler to support, which is the point of generating them.
    add_files(OUTPUT .. "/texts.c", {always_added = true, warnings = "none"})

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

    on_config(quiet_line_markers)

    -- The generated sources have to exist before the compiler is asked for them,
    -- and they are cheap enough to regenerate whenever their inputs move.
    before_build(function(target)
        import("core.project.depend")
        local texts = path.join(OUTPUT, "texts.c")
        local text_inputs = {}
        for _, entry in ipairs(EMBEDDED_TEXT) do table.insert(text_inputs, entry[2]) end
        os.mkdir(OUTPUT)
        depend.on_changed(function()
            local source = {"/* Generated by xmake.lua. Do not edit. */\n#include <stddef.h>\n\n"}
            for _, entry in ipairs(EMBEDDED_TEXT) do
                local value = io.readfile(path.join(os.projectdir(), entry[2]))
                value = value:gsub("\\", "\\\\"):gsub('"', '\\"'):gsub("\t", "\\t")
                value = value:gsub("\r", "\\r"):gsub("\n", '\\n"\n    "')
                table.insert(source, "const char " .. entry[1] .. "[] =\n    \"" .. value .. "\";\n\n")
            end
            io.writefile(texts, table.concat(source))
        end, {files = table.join(text_inputs, {"xmake.lua"}), dependfile = texts .. ".d"})

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
        -- Every configuration links to the same `build/pixy`, so a binary left by
        -- the previous one is newer than the objects this one just configured and
        -- xmake links nothing -- handing back the last build under the new name.
        process.tryrm(path.join(OUTPUT, "pixy"))
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

    local function bash()
        return os.isfile("/bin/bash") and "/bin/bash" or "bash"
    end

    local function command(arguments)
        local words = {}
        for _, argument in ipairs(arguments) do
            local word = tostring(argument):gsub("'", "'\\''")
            table.insert(words, "'" .. word .. "'")
        end
        return table.concat(words, " ")
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
        process.tryrm(path.join(OUTPUT, "pixy"))
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
        process.execv(bash(), {path.join(root, "tests/run.sh")}, {curdir = root})
    end)

    register("sanitize", "The suite under the address and UB sanitizers", function()
        configure("debug", {musl = false, sanitize = true})
        run_xmake({"build", "pixy"})
        process.execv(bash(), {path.join(root, "tests/run.sh")}, {curdir = root, envs = {
            PIXY = path.join(OUTPUT, "pixy-sanitize"),
            PIXY_BIN = path.join(OUTPUT, "pixy-sanitize"),
            ASAN_OPTIONS = process.getenv("ASAN_OPTIONS") or "detect_leaks=1",
            UBSAN_OPTIONS = "print_stacktrace=1:halt_on_error=1",
        }})
    end)

    register("fuzz", "Random input at the palette surface", function()
        configure("debug", {musl = false, sanitize = true})
        run_xmake({"build", "pixy"})
        process.execv(bash(), {path.join(root, "tests/fuzz.sh"), path.join(OUTPUT, "pixy-sanitize"),
                          process.getenv("ROUNDS") or "1000"},
                 {envs = {ASAN_OPTIONS = "detect_leaks=1", UBSAN_OPTIONS = "halt_on_error=1"}})
    end)

    register("bench", "Run release performance checks", function()
        release_build()
        process.execv(path.join(root, OUTPUT, "pixy"), {"__bench", "cold", "500"})
        process.execv(path.join(root, OUTPUT, "pixy"), {"__bench", "query", "10000"})
        process.execv(path.join(root, OUTPUT, "pixy"), {"__bench", "provider", "100"})
    end)

    register("bench-compare", "Compare matched providers against Starship", function()
        release_build()
        local hyperfine = process.getenv("PIXY_BENCH_HYPERFINE") or which("hyperfine")
        local starship = process.getenv("PIXY_BENCH_STARSHIP") or which("starship")
        if not hyperfine or not os.isfile(hyperfine) then fail("hyperfine is missing from the dev shell") end
        if not starship or not os.isfile(starship) then fail("starship is missing from the dev shell") end

        local runs = tonumber(process.getenv("PIXY_BENCH_RUNS") or "200")
        if not runs or runs < 1 or runs % 1 ~= 0 then fail("PIXY_BENCH_RUNS must be a positive integer") end

        local work = path.join(root, OUTPUT, "bench-compare")
        local plain = path.join(process.getenv("TMPDIR") or "/tmp", "pixy-bench-plain")
        process.mkdir(work)
        process.mkdir(plain)
        process.mkdir(path.join(work, "starship-cache"))
        process.mkdir(path.join(work, "pixy-cache"))

        local bare_config = path.join(root, "benchmarks/starship-bare.toml")
        local git_config = path.join(root, "benchmarks/starship-git-branch.toml")

        local binary = path.join(root, OUTPUT, "pixy")
        local config = path.join(root, "config/init.lua")
        local function compare(label, directory_path, selector, module, starship_config)
            local pixy = command({binary, "render", selector, "--config", config, "--target", "ansi",
                                  "--width", "120", "--set", "cwd=" .. directory_path,
                                  "--set", "status=0"})
            local reference = command({starship, "module", module})
            report("\n== " .. label)
            process.execv(hyperfine, {"--shell=none", "--warmup", "20", "--runs", tostring(runs),
                                      "--style", "basic", "--time-unit", "millisecond",
                                      "--command-name", "pixy " .. label, pixy,
                                      "--command-name", "starship " .. label, reference},
                          {curdir = directory_path, envs = {
                              PIXY_CACHE_DIR = path.join(work, "pixy-cache"),
                              STARSHIP_CACHE = path.join(work, "starship-cache"),
                              STARSHIP_CONFIG = starship_config,
                              PWD = directory_path,
                              TERM = "xterm-256color",
                          }})
        end

        compare("directory", plain, "prompt.left.directory", "directory", bare_config)
        compare("git-branch", root, "prompt.left.git", "git_branch", git_config)
    end)

    register("bench-phases", "Where a prompt spends its time", function()
        release_build()
        process.execv(path.join(root, OUTPUT, "pixy"), {"__bench", "phases", "400"})
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
        for _, file in ipairs({"README.md", "LICENSE", "vendor/THIRD_PARTY.md",
                               "assets/pokemon.hxsp", path.join(OUTPUT, "pixy")}) do
            if not os.isfile(file) then fail("package input is missing: " .. file) end
        end
        process.execv(path.join(root, OUTPUT, "pixy"),
                      {"pack", "check", path.join(OUTPUT, "pixy-example.pixypack")})
        local listing = process.iorunv(path.join(root, OUTPUT, "pixy"), {"pack", "list"})
        if not listing:find("pokemon\t2034\t", 1, true) then
            fail("the embedded Pokemon pack is incomplete")
        end
        report("package ok")
    end)

    -- The globs are expanded here: these run the program directly, with no shell
    -- to expand them, and clang-format given a literal `src/*.c` reports that no
    -- such file exists.
    local function c_sources()
        local found = {}
        for _, pattern in ipairs({"src/*.c", "src/*.h", "tests/*.c"}) do
            for _, file in ipairs(process.files(pattern)) do table.insert(found, file) end
        end
        return found
    end

    register("fmt", "Format the C sources", function()
        local formatter = which("clang-format")
        if not formatter then return report("clang-format is not here; nothing formatted") end
        process.execv(formatter, table.join({"-i"}, c_sources()))
    end)

    register("fmt-check", "Check C formatting", function()
        local formatter = which("clang-format")
        if not formatter then return report("clang-format is not here; nothing checked") end
        process.execv(formatter, table.join({"--dry-run", "--Werror"}, c_sources()))
    end)

    register("verify", "Format check plus the suite", function()
        run_xmake({"fmt-check"})
        run_xmake({"pixy-test"})
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
