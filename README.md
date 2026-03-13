# medit

`medit` is a minimal modal text editor written in C++ with `ncursesw`.

The repo vendors `nlohmann::json` under `third_party/nlohmann/`, so a fresh checkout does not need a separately installed JSON library.

## Layout

- Application sources live in `src/`
- Tests live in `tests/`
- Checked-in example config lives in `config/`
- Personal runtime config lives in `./.config/` or `~/.config/`

## Features

- Modal editing with `NORMAL`, `INSERT`, `VISUAL`, `VISUAL LINE`, and `COMMAND` modes
- UTF-8 file loading and saving
- Wide-character terminal input with `get_wch()`
- Small, function-oriented implementation intended to be scriptable later
- Core document/edit/history logic separated from the terminal UI for future protocol integrations like LSP
- Runtime service boundary for future background integrations like LSP clients
- Optional single-server LSP stdio transport configured through `meditrc`
- Clipboard integration that uses the system clipboard when available and otherwise shares clipboard contents between `medit` instances through a shared file

## Build

```sh
make
```

To provision the pinned local tree-sitter runtime and bundled grammars for a fresh checkout:

```sh
make bootstrap-tree-sitter
```

That builds local runtime assets under your live runtime config tree at `./.config/medit/` and regenerates `./.config/medit/syntax.json`. The checked-in `config/` tree remains just an example/template. To build only one bundled language, use a target like:

```sh
make bootstrap-tree-sitter-python
```

Some bundled grammars with extra prerequisites are optional in the default bootstrap. For example, Swift is skipped unless you request it explicitly:

```sh
make bootstrap-tree-sitter-swift
```

## Test

```sh
make test
```

## Run

```sh
./medit [file...]
```

Passing multiple files opens each one in its own buffer and leaves the first file active.
Starting `./medit` with no file arguments opens a project file picker first. `medit` prefers `fd`/`fdfind` when available and otherwise falls back to `rg`, then pipes the results through `fzf`. If the picker is canceled or unsupported, `medit` falls back to an empty buffer.

## Config

The repo ships an example config tree in `config/`. Copy that to `./.config/` or `~/.config/` before editing it for your own setup. `medit` does not read `config/` directly.

`medit` looks for a runtime config file using this lookup order:

1. `./.config/meditrc`
2. `~/.config/meditrc`

If `meditrc` exists, it selects the keybindings JSON, colors JSON, optional LSP rules JSON, and optional syntax rules JSON to load from `./.config/medit/` or `~/.config/medit/` unless you give absolute paths.

LSP can be configured per file extension through an LSP rules file:

```ini
lsp = lsp.json
log = debug.log
syntax_config = syntax.json
syntax = python
right_justify_diagnostics = true
clipboard = auto
clipboard_osc52 = true
shiftwidth = 4
autoindent = true
```

Example [lsp.json](/home/dan/de/config/medit/lsp.json):

```json
{
  "servers": [
    {
      "name": "cpp",
      "command": "clangd --background-index",
      "language_id": "cpp",
      "patterns": ["*.cpp", "*.hpp", "Makefile"],
      "workspace": {
        "markers": ["compile_commands.json", ".clangd", "CMakeLists.txt", ".git"],
        "fallback": "file_directory"
      }
    },
    {
      "name": "json",
      "command": "vscode-json-languageserver --stdio",
      "language_id": "json",
      "patterns": ["*.json", "*.jsonc"],
      "workspace": {
        "markers": ["package.json", ".git"],
        "fallback": "file_directory"
      }
    },
    {
      "name": "python",
      "command": "pyright-langserver --stdio",
      "language_id": "python",
      "patterns": ["*.py", "*.pyi", "*.pyw"],
      "workspace": {
        "markers": ["pyrightconfig.json", "pyproject.toml", "setup.py", "setup.cfg", ".git"],
        "fallback": "file_directory"
      }
    }
  ]
}
```

Each open buffer picks the first matching server by filename glob pattern and sends that server the configured language id. The old `lsp_command` and `lsp_language_id` keys still work as a single catch-all fallback.
The workspace root for each server is found by walking upward from the opened file and looking for the configured `workspace.markers`, then falling back according to `workspace.fallback`.

Tree-sitter syntax can be configured per language through a syntax rules file:

```json
{
  "languages": [
    {
      "name": "python",
      "patterns": ["*.py", "*.pyi", ".pyw"],
      "grammar_path": "/path/to/libtree-sitter-python.so",
      "symbol_name": "tree_sitter_python",
      "highlights_path": "/path/to/queries/highlights.scm"
    }
  ]
}
```

`syntax_config = syntax.json` points `medit` at that file. `syntax = python` forces a specific configured syntax language by name. If `syntax` is omitted, `medit` picks the first configured tree-sitter language whose filename glob pattern matches the file. If no configured tree-sitter language matches, `medit` falls back to the built-in lightweight C++ highlighter for C/C++ files and otherwise leaves syntax coloring off.

For reproducible fresh-checkout setup, the repo now ships a pinned tree-sitter manifest at [tools/tree_sitter_languages.json](/home/dan/de/tools/tree_sitter_languages.json) plus a bootstrap script at [tools/bootstrap_tree_sitter.py](/home/dan/de/tools/bootstrap_tree_sitter.py). Adding a bundled language means adding one manifest entry with:

- repo URL
- pinned commit
- filename glob patterns
- symbol name
- query path
- optional scanner path

Then run `make bootstrap-tree-sitter` to rebuild local grammar libraries, queries, and your live `./.config/medit/syntax.json`.

`right_justify_diagnostics = true` makes inline diagnostic annotations render against the right edge of the text area instead of starting from the left.

`show_diagnostics_in_insert_mode = false` hides diagnostic underlines and inline diagnostic annotations while you are in insert mode.

`shiftwidth = 4` sets how many spaces `>` and `<` add or remove when indenting and outdenting lines.

`tabstop = 8` sets the displayed width of tab characters.

`softtabstop = 0` makes insert-mode `Tab` and `Shift-Tab` use `shiftwidth`; set it to a positive number to use a different indentation step.

`expandtab = false` keeps literal tabs available; when true, insert-mode tabbing and indentation use spaces.

`autoindent = true` makes Enter and `o` copy the leading whitespace from the line above onto the new line.

Syntax language entries in `syntax.json` can also override editor behavior per filename pattern:

```json
{
  "name": "python",
  "patterns": ["*.py", "*.pyi", "*.pyw"],
  "grammar_path": "grammars/libtree-sitter-python.so",
  "symbol_name": "tree_sitter_python",
  "highlights_path": "queries/python/highlights.scm",
  "editor": {
    "shiftwidth": 4,
    "tabstop": 8,
    "softtabstop": 4,
    "expandtab": true,
    "autoindent": true,
    "show_diagnostics_in_insert_mode": false
  }
}
```

Those per-language editor settings override the global `meditrc` values for matching buffers.

`log = debug.log` enables append-only debug logging to `./.config/medit/debug.log` or `~/.config/medit/debug.log` relative to the loaded `meditrc` unless you use an absolute path. This is useful for debugging picker, file-open, and config reload issues without copying transient status messages.

Clipboard settings:

- `clipboard = auto` uses the native system clipboard when readable, mirrors writes through OSC52 when supported, and keeps a shared clipboard file for cross-instance fallback
- `clipboard = native` uses only native clipboard tools
- `clipboard = shared-file` uses only the shared clipboard file
- `clipboard = internal` keeps the clipboard inside one `medit` process
- `clipboard_file = /path/to/clipboard.json` overrides the shared clipboard file path
- `clipboard_osc52 = true|false` enables or disables OSC52 clipboard writes in `auto` mode

The checked-in example config is:

- [`config/meditrc`](/home/dan/de/config/meditrc)
- [`config/medit/keybindings.json`](/home/dan/de/config/medit/keybindings.json)
- [`config/medit/lsp.json`](/home/dan/de/config/medit/lsp.json)
- [`config/medit/syntax.json`](/home/dan/de/config/medit/syntax.json)
- [`config/medit/themes/default.json`](/home/dan/de/config/medit/themes/default.json)
- [`config/medit/themes/neon.json`](/home/dan/de/config/medit/themes/neon.json)
- [`config/medit/themes/forest.json`](/home/dan/de/config/medit/themes/forest.json)
- [`config/medit/themes/dragon.json`](/home/dan/de/config/medit/themes/dragon.json)
- [`config/medit/themes/tiger.json`](/home/dan/de/config/medit/themes/tiger.json)
- [`config/medit/themes/seaside.json`](/home/dan/de/config/medit/themes/seaside.json)
- [`config/medit/themes/luxor.json`](/home/dan/de/config/medit/themes/luxor.json)
- [`config/medit/themes/matrix.json`](/home/dan/de/config/medit/themes/matrix.json)

If `meditrc` is absent, `medit` falls back to default file names in `./.config/medit/` and then `~/.config/medit/`. If those files are also absent, it falls back to embedded defaults.

A typical first-time setup is:

```sh
cp -R config .config
```

Then edit `./.config/meditrc` and the JSON files under `./.config/medit/` for your own environment.

To switch themes, update `colors = ...` in your `meditrc`, for example:

```ini
colors = themes/neon.json
```

or:

```ini
colors = themes/forest.json
```

Theme color values can use:

- named ANSI colors like `red` or `cyan`
- bright ANSI names like `bright_red`
- numeric palette entries like `196`
- prefixed palette entries like `color196`

On terminals with extended color support, `medit` uses those palette entries directly. On terminals with fewer colors, it maps them down to the nearest supported palette color.

## Keys

- `i` enter insert mode
- `v` enter visual selection mode
- `V` enter linewise visual selection mode
- `%` select the entire buffer
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
- `Ctrl-G` toggle diagnostic display on and off
- Inline diagnostic annotations render below relevant code and are skipped by normal cursoring
- `gx` show a diagnostics summary
- `PageUp`, `PageDown` move by a screen
- `Ctrl-U`, `Ctrl-D` move by a half screen
- `Ctrl-W s` split the current window horizontally
- `Ctrl-W v` split the current window vertically
- `Ctrl-W c` close the current window
- `Ctrl-W o` close all other windows
- `Ctrl-W h`, `Ctrl-W j`, `Ctrl-W k`, `Ctrl-W l` move focus between windows
- `Ctrl-W` plus the arrow keys also moves focus between windows
- Split windows are independent views onto buffers, so editing in one view immediately updates every other window showing that buffer
- `Ctrl-Z` suspend to the shell and resume with `fg` on platforms with job control support
- Left mouse click moves the cursor to the clicked buffer position
- `gg` jump to the top of the file
- `G` jump to the bottom of the file
- `0`, `$` move to line start/end
- `>`, `<` indent or outdent the current line, or all selected lines in visual mode, by `shiftwidth`
- In insert or visual mode, `>` and `<` keep the visual selection active so you can repeat them
- In insert mode, `Tab` in leading indentation advances to the next `softtabstop` column
- In insert mode, `Shift-Tab` in leading indentation outdents to the previous `softtabstop` column
- `x` delete character under cursor
- `r<char>` replace the current character with `<char>`; counts work, for example `3ra`
- `.` repeat the last change command; counts work, for example `3.`
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
- In visual mode, `|` runs a Unix command on the selection and replaces it with stdout
- In visual mode, `S` runs `sed <script>` on the selection and replaces it with stdout, for example `%Ss/a/b/g`
- In visual mode, `o` moves the cursor to the start of the current selection and `O` moves it to the end
- In visual mode, `iw` extends the selection to include the inner word at the cursor
- In visual mode, `aw` extends the selection to include the word plus adjacent spaces at the cursor
- Search updates dynamically as you type and highlights all matches, with the current match highlighted separately
- `g d` jump to the LSP definition of the symbol under the cursor, switching to an open buffer or opening the target file in a new buffer
- `K` request LSP hover information for the symbol under the cursor and show it in a temporary popup
- `Ctrl-P` in insert mode requests LSP code completion at the cursor and shows it in a selectable popup
- `g f` opens the file reference under the cursor, first by direct relative path and then by searching from the workspace root with `fdfind` or `fd`
- `g o` jump back through definition history and `g i` jump forward again
- `u` undo last edit
- `Ctrl-R` redo last undone edit
- `:w` write
- `:w filename` write to a new path
- `:q` quit if clean
- `:q!` force quit
- `:wq` or `:x` write and quit
- `:42` jump to line 42
- `:s/pat/repl/`, `:s/pat/repl/g`, and `:%s/pat/repl/g` substitute on the current line or whole buffer
- `:e filename` open another file in a new buffer and switch to it
- `:buffers` list open buffers
- `:buffer N` switch to buffer `N`
- `:bnext` / `:bprev` switch buffers
- `:bd` close the current buffer if clean
- `:bd!` force close the current buffer
- `:find-file` open a project file via the external file picker (`fd`/`fdfind` or `rg`, then `fzf`)
- `:grep pattern` search with `rg` and jump via `fzf`
- `:pick-theme` choose a theme via `fzf`, update `meditrc`, and reload colors immediately
- `:reload-config` reload `meditrc`, keybindings, colors, and logging
- `:diagnostics` show a diagnostics summary
- `:lsp-status` show a popup with current LSP service state

## Keybinding Aliases

Keybindings may map a key either to an action name or to a token sequence alias. For example:

```json
"D": ["v", "$", "d"]
```

That replays the same internal key sequence as typing `v`, then `$`, then `d`.
