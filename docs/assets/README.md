# Pokemon sprite source

`docs/assets/pokemon/` contains the regular and shiny ANSI sprites copied from the
read-only sibling Hexe checkout at commit `3ddc19b`. The imported collection
contains 1,017 regular and 1,017 shiny entries.

The source chain recorded by Hexe is:

- [krabby](https://github.com/yannjor/krabby)
- [PokéSprite](https://github.com/msikma/pokesprite)
- [pokemon-generator-scripts](https://gitlab.com/phoneybadger/pokemon-generator-scripts)
- [PokéAPI](https://github.com/PokeAPI/pokeapi)

Pixy packs the collection deterministically during the Cargo build and embeds
the resulting archive in the binary. Raw sprites are not read at runtime.

See `vendor/THIRD_PARTY.md` and `LICENSES/GPL-3.0-only.txt` for distribution notices.
