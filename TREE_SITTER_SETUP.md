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
- optional `source_subdir`
- optional `scanner_path`

`ref` is the exact git commit hash to build from. It keeps fresh-checkout setup reproducible by avoiding whatever the grammar repo happens to have on its default branch later.

To get a `ref` for a new language:

```sh
git clone https://github.com/tree-sitter/tree-sitter-rust.git /tmp/tree-sitter-rust
git -C /tmp/tree-sitter-rust rev-parse HEAD
```

That prints the full commit SHA you should copy into the manifest. If you want a different revision than the current `HEAD`, inspect the history first:

```sh
git -C /tmp/tree-sitter-rust log --oneline
```

Then rerun:

```sh
make bootstrap-tree-sitter
```

No C++ changes should be required for a normal tree-sitter language addition.

If a grammar repo contains the actual parser in a nested subdirectory, set `source_subdir`. For example, a repo layout like:

- `tree-sitter-markdown/tree-sitter-markdown/`
- `tree-sitter-markdown/tree-sitter-markdown-inline/`

would need entries that point at one of those subdirectories explicitly.

## Activate

Restart `medit` or run:

```sh
:reload-config
```
