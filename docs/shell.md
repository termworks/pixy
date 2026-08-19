# Shell integration

`pixy init bash`, `zsh`, `fish`, or `oslo` prints integration text and never
edits configuration. Bash and Zsh request their prompt-safe encodings; Fish and
the generated Oslo command use raw ANSI. The reference prompt accepts cwd,
status, duration, jobs, language, and vi-mode context.

The Oslo output assigns both `oslo.prompt.left` and `oslo.prompt.right`, uses
Oslo's `$status`, `$duration_ms`, `$jobs`, `$language`, and `$vimode`
substitutions, and enables asynchronous rendering with a 10-millisecond
deadline. It does not replace shell-to-multiplexer hooks or exit policy.

The Bash, Zsh, and Fish generators install pre-execution hooks so status and
duration describe the command that just completed. Existing Bash prompt hooks
run while Pixy's timing guard remains active.

Review generated output before adding it to shell configuration:

```sh
pixy init zsh
pixy init oslo
pixy init hexe-oslo
```
