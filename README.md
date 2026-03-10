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
- `a` append after cursor
- `o` open line below
- `O` open line above
- `Esc` return to normal mode
- `h`, `j`, `k`, `l` move
- `f<char>` move to the next matching character on the line
- `F<char>` move backward to the previous matching character on the line
- `t<char>` move up to the next matching character on the line
- `T<char>` move backward up to the previous matching character on the line
- `PageUp`, `PageDown` move by a screen
- `Ctrl-U`, `Ctrl-D` move by a half screen
- Left mouse click moves the cursor to the clicked buffer position
- `gg` jump to the top of the file
- `G` jump to the bottom of the file
- `0`, `$` move to line start/end
- `x` delete character under cursor
- `p` paste after the cursor
- `P` paste before the cursor
- `dd` delete current line
- In visual mode, `d` deletes the selection
- In visual mode, `c` deletes the selection and enters insert mode to replace it
- In visual mode, `y` yanks the selection
- In visual mode, `p` or `P` replaces the selection with the yanked text
- In visual mode, `iw` extends the selection to include the inner word at the cursor
- In visual mode, `aw` extends the selection to include the word plus adjacent spaces at the cursor
- `u` undo last edit
- `r` redo last undone edit
- `:w` write
- `:w filename` write to a new path
- `:q` quit if clean
- `:q!` force quit
- `:wq` or `:x` write and quit
- `:e filename` open another file
