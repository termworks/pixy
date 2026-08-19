# Hexe and Oslo compatibility profile

Pixy ships a profile that reproduces the rendering configured by the reference
Hexe and Oslo Lua files without loading either project:

```sh
pixy init hexe-oslo > ~/.config/pixy/init.lua
pixy check
pixy init oslo
```

The last command only prints the future Oslo prompt assignments. It does not
edit Oslo configuration or replace its shell-to-mux hooks.

## Zones and segments

- `prompt.left` and `prompt.right`
- `status.left`, `status.center`, and `status.right`
- `status.left.spinner` for the spinner segment alone
- `overlay.sprite`
- `float.title` and `container.title`
- `float.frame` and `container.frame`
- `split.vertical` and `split.horizontal`
- `pop.notify`, `pop.confirm`, and `pop.choose`
- `oslo.direnv`

The profile uses inherited `SSH_CONNECTION`, `DIRENV_DIR`, `IN_NIX_SHELL`,
`SCRATCH`, `HOME`, `HOSTNAME`, and `USER` values. Oslo passes status, duration,
jobs, language, and vi mode through the generated prompt commands.

## Caller context

Future mux callers pass state in `context.values`:

| Value | Meaning |
|---|---|
| `pod_name`, `session` | Active pod and session labels |
| `pane` | Pane `cwd`, `shell_running`, `alt_screen`, and `adhoc_float` state |
| `tabs`, `active_tab` | Tab labels and active selection |
| `recording` | Recording indicator state |
| `shell_running`, `alt_screen`, `adhoc_float` | Spinner visibility |
| `time`, `randomdo`, `spinner` | Deterministic or caller-supplied status values |
| `sprite_name`, `sprite_shiny` | Pane Pokemon name and regular/shiny selection |
| `sprite_pack`, `sprite_item` | Pack and optional exact item override |
| `sprite_position` | `topleft`, `topright`, `bottomleft`, `bottomright`, or `center` |
| `sprite_frame`, `sprite_frames` | Inline SGR fixture or animated frame override |
| `sprite_visible` | Explicit `false` suppresses the sprite surface |
| `battery_percent`, `battery_status` | Caller-supplied battery state |
| `title`, `active` | Float, container, and split appearance |
| `message` | Notification or confirmation content |
| `choices`, `selected` | Chooser rows and selected index |
| `direnv` | Oslo direnv report state, owner, watched values, changes, and aliases |
| `hover_region` | Region receiving hover styling |
| `press_region`, `press_button` | Region and mouse button receiving pressed styling |

`hostname`, `username`, `distro`, `sudo`, `scratch_count`, `container`,
`git_branch`, `git_status`, and `git_status_text` override providers for tests
or callers that already own those values. Setting an override to `false`
suppresses the corresponding optional segment.

## Interaction contract

The `status.right.recording` segment reports `record.switch` for a left click and
`record.stop` for a right click. Chooser rows report `choose.N`. Pixy returns
identifiers and bounded cell geometry; the caller dispatches the operation.
This keeps multiplexer commands out of Pixy while preserving the configured
interaction.

`overlay.sprite` reads `regular/<pane-name>` or `shiny/<pane-name>` from the
built-in `pokemon` pack, parses its 24-bit SGR half-block art, leaves spaces
transparent, and falls back to Pikachu. A user-installed `pokemon.pixypack` can
override built-in items. `status.left.spinner` reproduces the configured
knight-rider glyphs, trail colors, timing, and endpoint holds.

Inspect the structured run result with:

```sh
pixy render status.left --config examples/hexe-oslo.lua --mode run \
  --width 100 --context-file tests/fixtures/contexts/hexe-oslo.json
pixy render status.center --config examples/hexe-oslo.lua --mode run \
  --width 100 --context-file tests/fixtures/contexts/hexe-oslo.json
pixy render status.right.recording --config examples/hexe-oslo.lua --mode run \
  --width 100 --context-file tests/fixtures/contexts/hexe-oslo.json
```
