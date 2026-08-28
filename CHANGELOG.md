# Changelog

## [0.2.2] - 2026-08-28

### <!-- 0 -->⛰️  Features

- --stdio, and a cadence that is a delay

### <!-- 1 -->🐛 Bug Fixes

- Dim the padding with the title
- Correct transcript row and improve clock fallback
- Hold a run's label for the whole run

### <!-- 7 -->⚙️ Miscellaneous Tasks

- Install the config with the binary

## [0.2.1] - 2026-08-25

### <!-- 7 -->⚙️ Miscellaneous Tasks

- A configs recipe for the config tree

## [0.2.0] - 2026-08-22

### <!-- 0 -->⛰️  Features

- Simplify direnv config and add install alias

### <!-- 1 -->🐛 Bug Fixes

- Build the static binary in the pinned shell
- Stage the static binary and rename it into place
- Bound a render by processor time, not wall time

### <!-- 2 -->🚜 Refactor

- Register zones by name and return nothing

### <!-- 7 -->⚙️ Miscellaneous Tasks

- Cleanup
- Replace the Makefile with .make.lua recipes
- Cleanup

### Build

- Report the binary, and quiet the cache's line markers

## [0.1.7] - 2026-08-21

### <!-- 0 -->⛰️  Features

- Read the have reply on ask --wait
- Colour namespaces over OSC 1330

### <!-- 1 -->🐛 Bug Fixes

- Render frames with monochrome fonts only
- Read the time in process, not from date
- Carry declared colours with the claim
- Terminate bash prompt sequences with BEL
- Refuse a slot instead of half-applying it
- Size the frame cell to the font's own box

### <!-- 4 -->⚡ Performance

- Precompile modules and cache compiled configs

### <!-- 6 -->🧪 Testing

- Stop asserting the environment's timezones and terminal

### <!-- 7 -->⚙️ Miscellaneous Tasks

- Add the clang-format the code was written in

### Build

- Keep the sanitized binary out of build/pixy
- Silence the noise and guard truncated paths
- Move to xmake, with make as a pass-through

## [0.1.6] - 2026-08-20

### <!-- 1 -->🐛 Bug Fixes

- Let cached providers expire on time

## [0.1.5] - 2026-08-20

## [0.1.5] - 2026-08-20

### <!-- 1 -->🐛 Bug Fixes

- Accept the joined --flag=value spelling

### <!-- 2 -->🚜 Refactor

- [**breaking**] Rewrite the host in C, keeping every config working

### <!-- 7 -->⚙️ Miscellaneous Tasks

- Keep sprite art under docs
- Tidy the tree and keep the version in one line
- Link release binaries statically

## [0.1.4] - 2026-08-20

## [0.1.4] - 2026-08-20

### <!-- 0 -->⛰️  Features

- Draw host progress in the status bar
- Names, progress segments and a real cli

## [0.1.3] - 2026-08-20

### <!-- 3 -->📚 Documentation

- Add a preset gallery with uniform frames

### <!-- 7 -->⚙️ Miscellaneous Tasks

- Widen bench budgets for shared runners
- Drop macos and pin HOME in compat fixtures

## [0.1.2] - 2026-08-20

### <!-- 0 -->⛰️  Features

- Add `pixy serve` command and `spacer` node
- Lua-powered terminal painter

### <!-- 1 -->🐛 Bug Fixes

- Stop keying the exec cache on ambient env
- Follow host active tab and pane
- Keep padding inside each badge
- Init

### <!-- 3 -->📚 Documentation

- Drop the fake terminal chrome from readme frames
- Show each readme claim with a rendered example
- Lead the readme with an animated demo
- Illustrate the readme with generated frames

### <!-- 7 -->⚙️ Miscellaneous Tasks

- Build macos releases with gnu make
- Release static musl linux artifacts

