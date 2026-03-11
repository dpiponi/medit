# Tree-Sitter Bootstrap

Fresh checkout setup is now repo-managed instead of manual.

## Prerequisites

Linux:

```sh
sudo apt-get update
sudo apt-get install -y git python3 build-essential
```

macOS with Homebrew:

```sh
brew install git python
```

You do not need a system `tree-sitter` package for `medit` syntax support. The repo bootstrap builds the runtime locally.

## Bootstrap

From the repo root:

```sh
make bootstrap-tree-sitter
```

That will:

- fetch pinned tree-sitter sources into `build/tree-sitter/sources/`
- build a local runtime into `.config/medit/libtree-sitter.so` or `.dylib`
- build bundled grammar shared libraries into `.config/medit/grammars/`
- copy bundled highlight queries into `.config/medit/queries/`
- regenerate `.config/medit/syntax.json`

To build only one bundled language:

```sh
make bootstrap-tree-sitter-python
```

## Adding a New Bundled Language

Edit [tools/tree_sitter_languages.json](/home/dan/de/tools/tree_sitter_languages.json) and add a new language entry with:

- `name`
- `repo`
- pinned `ref`
- `extensions`
- `symbol_name`
- `query_path`
- optional `scanner_path`

Then rerun:

```sh
make bootstrap-tree-sitter
```

No C++ changes should be required for a normal tree-sitter language addition.

## Activate

Restart `medit` or run:

```sh
:reload-config
```
