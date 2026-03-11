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

To provision the pinned local tree-sitter runtime and bundled grammars for a fresh checkout:

```sh
make bootstrap-tree-sitter
```

That builds local assets under `.config/medit/` and regenerates `.config/medit/syntax.json`. To build only one bundled language, use a target like:

```sh
make bootstrap-tree-sitter-python
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
Starting `./medit` with no file arguments opens an `rg --files | fzf` picker first; if the picker is canceled or unsupported, `medit` falls back to an empty buffer.

## Config

`medit` looks for a runtime config file using this lookup order:

1. `./.config/meditrc`
2. `~/.config/meditrc`

If `meditrc` exists, it selects the keybindings JSON, colors JSON, optional LSP rules JSON, and optional syntax rules JSON to load from `.config/medit/` unless you give absolute paths.

LSP can be configured per file extension through an LSP rules file:

```ini
lsp = lsp.json
log = debug.log
syntax_config = syntax.json
syntax = python
right_justify_diagnostics = true
```

Example [lsp.json](/home/dan/de/.config/medit/lsp.json):

```json
{
  "servers": [
    {
      "name": "cpp",
      "command": "clangd --background-index",
      "language_id": "cpp",
      "extensions": [".cpp", ".hpp"],
      "workspace": {
        "markers": ["compile_commands.json", ".clangd", "CMakeLists.txt", ".git"],
        "fallback": "file_directory"
      }
    },
    {
      "name": "json",
      "command": "vscode-json-languageserver --stdio",
      "language_id": "json",
      "extensions": [".json", ".jsonc"],
      "workspace": {
        "markers": ["package.json", ".git"],
        "fallback": "file_directory"
      }
    },
    {
      "name": "python",
      "command": "pyright-langserver --stdio",
      "language_id": "python",
      "extensions": [".py", ".pyi", ".pyw"],
      "workspace": {
        "markers": ["pyrightconfig.json", "pyproject.toml", "setup.py", "setup.cfg", ".git"],
        "fallback": "file_directory"
      }
    }
  ]
}
```

Each open buffer picks the first matching server by file extension and sends that server the configured language id. The old `lsp_command` and `lsp_language_id` keys still work as a single catch-all fallback.
The workspace root for each server is found by walking upward from the opened file and looking for the configured `workspace.markers`, then falling back according to `workspace.fallback`.

Tree-sitter syntax can be configured per language through a syntax rules file:

```json
{
  "languages": [
    {
      "name": "python",
      "extensions": [".py", ".pyi", ".pyw"],
      "grammar_path": "/path/to/libtree-sitter-python.so",
      "symbol_name": "tree_sitter_python",
      "highlights_path": "/path/to/queries/highlights.scm"
    }
  ]
}
```

`syntax_config = syntax.json` points `medit` at that file. `syntax = python` forces a specific configured syntax language by name. If `syntax` is omitted, `medit` picks the first configured tree-sitter language whose extension matches the file. If no configured tree-sitter language matches, `medit` falls back to the built-in lightweight C++ highlighter for C/C++ files and otherwise leaves syntax coloring off.

For reproducible fresh-checkout setup, the repo now ships a pinned tree-sitter manifest at [tools/tree_sitter_languages.json](/home/dan/de/tools/tree_sitter_languages.json) plus a bootstrap script at [tools/bootstrap_tree_sitter.py](/home/dan/de/tools/bootstrap_tree_sitter.py). Adding a bundled language means adding one manifest entry with:

- repo URL
- pinned commit
- file extensions
- symbol name
- query path
- optional scanner path

Then run `make bootstrap-tree-sitter` to rebuild local grammar libraries, queries, and `syntax.json`.

`right_justify_diagnostics = true` makes inline diagnostic annotations render against the right edge of the text area instead of starting from the left.

`log = debug.log` enables append-only debug logging to `.config/medit/debug.log` relative to the loaded `meditrc` unless you use an absolute path. This is useful for debugging picker, file-open, and config reload issues without copying transient status messages.

The checked-in local config is:

- [`.config/meditrc`](/home/dan/de/.config/meditrc)
- [`.config/medit/keybindings.json`](/home/dan/de/.config/medit/keybindings.json)
- [`.config/medit/colors.json`](/home/dan/de/.config/medit/colors.json)
- [`.config/medit/lsp.json`](/home/dan/de/.config/medit/lsp.json)
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
- `g d` jump to the LSP definition of the symbol under the cursor, switching to an open buffer or opening the target file in a new buffer
- `g f` opens the file reference under the cursor, first by direct relative path and then by searching from the workspace root with `fdfind` or `fd`
- `g o` jump back through definition history and `g i` jump forward again
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
- `:find-file` open a project file via `rg --files | fzf`
- `:grep pattern` search with `rg` and jump via `fzf`
- `:reload-config` reload `meditrc`, keybindings, colors, and logging
- `:diagnostics` show a diagnostics summary

## Keybinding Aliases

Keybindings may map a key either to an action name or to a token sequence alias. For example:

```json
"D": ["v", "$", "d"]
```

That replays the same internal key sequence as typing `v`, then `$`, then `d`.
