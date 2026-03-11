# medit

`medit` is a minimal modal text editor written in C++ with `ncursesw`.

## Layout

- Application sources live in `src/`
- Tests live in `tests/`
- Local project config lives in `.config/`

## Features

- Modal editing with `NORMAL`, `INSERT`, `VISUAL`, `VISUAL LINE`, and `COMMAND` modes
- UTF-8 file loading and saving
- Wide-character terminal input with `get_wch()`
- Small, function-oriented implementation intended to be scriptable later
- Core document/edit/history logic separated from the terminal UI for future protocol integrations like LSP
- Runtime service boundary for future background integrations like LSP clients
- Optional single-server LSP stdio transport configured through `meditrc`

## Build

```sh
make
```

## Test

```sh
make test
```

## Run

```sh
./medit [file]
```

## Config

`medit` looks for a runtime config file using this lookup order:

1. `./.config/meditrc`
2. `~/.config/meditrc`

If `meditrc` exists, it selects the keybindings JSON and colors JSON to load from `.config/medit/` unless you give absolute paths.

It can also configure one LSP server command and language id, for example:

```ini
lsp_command = clangd --background-index
lsp_language_id = cpp
syntax = cpp
right_justify_diagnostics = true
```

`syntax = cpp` forces the lightweight C++ syntax highlighter. If `syntax` is omitted, `medit` auto-detects C/C++ files by extension and otherwise leaves syntax coloring off.

`right_justify_diagnostics = true` makes inline diagnostic annotations render against the right edge of the text area instead of starting from the left.

The checked-in local config is:

- [`.config/meditrc`](/home/dan/de/.config/meditrc)
- [`.config/medit/keybindings.json`](/home/dan/de/.config/medit/keybindings.json)
- [`.config/medit/colors.json`](/home/dan/de/.config/medit/colors.json)
- [`.config/medit/neon.json`](/home/dan/de/.config/medit/neon.json)
- [`.config/medit/forest.json`](/home/dan/de/.config/medit/forest.json)

If `meditrc` is absent, `medit` falls back to default file names in `./.config/medit/` and then `~/.config/medit/`. If those files are also absent, it falls back to embedded defaults.

To switch themes, update `colors = ...` in [`.config/meditrc`](/home/dan/de/.config/meditrc), for example:

```ini
colors = neon.json
```

or:

```ini
colors = forest.json
```

## Keys

- `i` enter insert mode
- `v` enter visual selection mode
- `V` enter linewise visual selection mode
- `/` enter regex search mode
- `a` append after cursor
- `o` open line below
- `O` open line above
- `Esc` return to normal mode
- `h`, `j`, `k`, `l` move
- `f<char>` move to the next matching character on the line
- `F<char>` move backward to the previous matching character on the line
- `t<char>` move up to the next matching character on the line
- `T<char>` move backward up to the previous matching character on the line
- `n` jump to the next search match
- `b` jump to the previous search match
- `]d`, `[d` jump to the next and previous diagnostic
- Inline diagnostic annotations render below relevant code and are skipped by normal cursoring
- `gx` show a diagnostics summary
- `PageUp`, `PageDown` move by a screen
- `Ctrl-U`, `Ctrl-D` move by a half screen
- `Ctrl-Z` suspend to the shell and resume with `fg` on platforms with job control support
- Left mouse click moves the cursor to the clicked buffer position
- `gg` jump to the top of the file
- `G` jump to the bottom of the file
- `0`, `$` move to line start/end
- `x` delete character under cursor
- `D` delete from the cursor to the end of the line through the configurable key-sequence alias system
- `p` paste after the cursor
- `P` paste before the cursor
- Numeric prefixes in normal and visual mode repeat repeatable commands, for example `10j` or `3x`
- Parenthesized command groups execute live and can be repeated as one unit, for example `3(aX<esc>)`
- `dd` delete current line
- In visual mode, `d` deletes the selection
- In visual mode, `c` deletes the selection and enters insert mode to replace it
- In visual mode, `y` yanks the selection
- In visual mode, `p` or `P` replaces the selection with the yanked text
- In visual mode, `iw` extends the selection to include the inner word at the cursor
- In visual mode, `aw` extends the selection to include the word plus adjacent spaces at the cursor
- Search updates dynamically as you type and highlights all matches, with the current match highlighted separately
- `u` undo last edit
- `r` redo last undone edit
- `:w` write
- `:w filename` write to a new path
- `:q` quit if clean
- `:q!` force quit
- `:wq` or `:x` write and quit
- `:e filename` open another file in a new buffer and switch to it
- `:buffers` list open buffers
- `:buffer N` switch to buffer `N`
- `:bnext` / `:bprev` switch buffers
- `:bd` close the current buffer if clean
- `:bd!` force close the current buffer
- `:diagnostics` show a diagnostics summary

## Keybinding Aliases

Keybindings may map a key either to an action name or to a token sequence alias. For example:

```json
"D": ["v", "$", "d"]
```

That replays the same internal key sequence as typing `v`, then `$`, then `d`.
