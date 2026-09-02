# Pokemon sprite archive

`assets/pokemon.hxsp` contains 1,017 regular and 1,017 shiny ANSI sprites.

The recorded source chain is:

- [krabby](https://github.com/yannjor/krabby)
- [PokéSprite](https://github.com/msikma/pokesprite)
- [pokemon-generator-scripts](https://gitlab.com/phoneybadger/pokemon-generator-scripts)
- [PokéAPI](https://github.com/PokeAPI/pokeapi)

Pixy embeds the archive directly in the binary. It inflates only the requested
sprite at runtime.

See `vendor/THIRD_PARTY.md` and `LICENSES/GPL-3.0-only.txt` for distribution notices.
