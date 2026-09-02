# Shell integration

Pixy prints integration source for Bash, Zsh, and Fish:

```sh
pixy init bash
pixy init zsh
pixy init fish
```

The command writes source to stdout and never edits shell configuration. Source
the output from the corresponding shell startup file.

Each integration renders `prompt.left` and passes conventional values including
`cwd`, `status`, `duration_ms`, `jobs`, `language`, and `vimode`. These names are
not reserved by Pixy; the selected Lua configuration decides whether to use
them.

Bash output uses `--target bash`, Zsh uses `--target zsh`, and Fish uses ANSI.
Prompt targets mark control sequences as zero-width where the shell requires it.

The integration expects a configuration to be discoverable through
`PIXY_CONFIG` or the normal configuration directory. Install the starter with:

```sh
oslo make configs
```
