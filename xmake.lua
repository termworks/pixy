-- The version lives here and nowhere else: `veri` reads and bumps this line,
-- the Makefile compiles it in as PIXY_VERSION_STRING, and the flake reads it
-- from here too, so a release never leaves two files disagreeing.
local PROJECT_VERSION = "0.1.4"

set_project("pixy")
set_version(PROJECT_VERSION)
set_languages("c11")

-- `make` is the build anyone should use; this target exists so the file is a
-- real build description rather than a place to keep a number.
target("pixy")
    set_kind("binary")
    add_files("src/*.c", "vendor/lua/src/*.c", "vendor/miniz/miniz*.c")
    add_files("build/lua_modules.c", "build/texts.c")
    add_includedirs("src", "vendor/lua/src", "vendor/miniz")
    add_defines("LUA_USE_POSIX", "_GNU_SOURCE", format('PIXY_VERSION_STRING="%s"', PROJECT_VERSION))
    add_syslinks("m")
    before_build(function ()
        os.exec("make build/lua_modules.c build/texts.c build/pokemon.pack")
    end)
