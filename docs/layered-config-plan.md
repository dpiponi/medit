# Layered Shared + Machine-Local Config for `medit`

## Summary

Add a first-class layered config model so most config stays shared and checked in, while machine-dependent values live in one local home-config overlay.

The design keeps current behavior working:

- Existing single-file `meditrc`, `lsp.json`, and `syntax.json` continue to load unchanged.
- Existing search order remains: `./.config/meditrc`, then `~/.config/meditrc`.
- The new recommended setup becomes:
  - shared base config in repo under `config/`
  - machine-local override entrypoint in `~/.config/meditrc`
  - machine-local JSON overlays in `~/.config/medit/`

This avoids hostname logic, avoids forcing env vars, and keeps the repo mostly shared while isolating per-machine paths and executable choices.

## Target UX

Recommended shared repo files:

- `config/meditrc`
- `config/medit/lsp.json`
- `config/medit/syntax.json`

Recommended machine-local files:

- `~/.config/meditrc`
- `~/.config/medit/lsp.local.json`
- `~/.config/medit/syntax.local.json`

Recommended user setup:

`~/.config/meditrc`
```ini
include = /absolute/path/to/repo/config/meditrc

lsp = lsp.local.json
syntax_config = syntax.local.json
```

`~/.config/medit/lsp.local.json`
```json
{
  "include": ["/absolute/path/to/repo/config/medit/lsp.json"],
  "servers": [
    {
      "name": "cpp",
      "command": "/opt/homebrew/opt/llvm/bin/clangd --background-index"
    }
  ]
}
```

`~/.config/medit/syntax.local.json`
```json
{
  "include": ["/absolute/path/to/repo/config/medit/syntax.json"],
  "languages": [
    {
      "name": "cpp",
      "grammar_path": "/Users/dan/.config/medit/grammars/libtree-sitter-cpp.dylib"
    }
  ]
}
```

That gives one stable local place for machine-specific overrides while the repo keeps the common defaults.

## Public Interface Changes

### `meditrc`

Add a new repeatable directive:

- `include = /path/to/other/meditrc`

Semantics:

- Includes are processed in file order.
- Included files load first.
- The current file overrides any values loaded from included files.
- Relative include paths are resolved relative to the including `meditrc` file's directory.
- Cycles are rejected with an error that includes the include chain.

No other `meditrc` syntax changes.

### `lsp.json`

Add optional top-level field:

- `"include": ["path1", "path2"]`

Semantics:

- Included files load first, in array order.
- Relative include paths are resolved relative to the JSON file that declares them.
- Resulting `servers` arrays are merged by `name`.
- For the same `name`, later files override earlier files field-by-field:
  - `command`: replace if present
  - `language_id`: replace if present
  - `patterns` or legacy `extensions`: replace if present
  - `workspace.markers`: replace if present
  - `workspace.fallback`: replace if present
- New server names append to the merged set.
- Duplicate pattern ownership is still validated after the final merge.
- Cycles are rejected with an error that includes the include chain.

### `syntax.json`

Add optional top-level field:

- `"include": ["path1", "path2"]`

Semantics:

- Included files load first, in array order.
- Relative include paths are resolved relative to the JSON file that declares them.
- Resulting `languages` arrays are merged by `name`.
- For the same `name`, later files override earlier files field-by-field:
  - `grammar_path`: replace if present
  - `symbol_name`: replace if present
  - `highlights_path`: replace if present
  - `patterns` or legacy `extensions`: replace if present
  - `editor.*`: merge by setting key
- New language names append to the merged set.
- Duplicate pattern ownership is still validated after the final merge.
- Cycles are rejected with an error that includes the include chain.

## Implementation Plan

### 1. Extend config data structures

In `config.cppm` / `EditorConfig`, add provenance fields:

- `std::vector<std::filesystem::path> meditrc_chain`
- `std::vector<std::filesystem::path> lsp_chain`
- `std::vector<std::filesystem::path> syntax_chain`

These should represent the fully resolved load order from base to final override.

No behavior should depend on these fields; they are for health/reporting and tests.

### 2. Refactor `meditrc` loading into layered parsing

In `src/core/config.cpp`:

- Replace single-file `load_editor_config_from_path()` parsing with a recursive loader.
- Introduce an internal helper like `load_editor_config_from_path_recursive(path, stack, config)`.
- Normalize included paths with `std::filesystem::weakly_canonical` when possible; otherwise use lexically normalized absolute paths for cycle detection.
- Apply current file values after included files.
- Preserve existing fallback filling:
  - default keybindings/colors/lsp/syntax/lua paths still resolve relative to the final top-level `meditrc`
  - if explicit paths are set in included files, they remain overridden by later files as usual

Important rule:

- `resolve_config_reference()` remains relative to the file where the setting appears, not the top-level file. This makes includes composable and predictable.

### 3. Add recursive JSON include loaders

Add separate internal loaders:

- `load_lsp_servers_from_path_recursive(path, stack, chain)`
- `load_syntax_languages_from_path_recursive(path, stack, chain)`

Each loader should:

- parse the current JSON object
- read optional `"include"` array
- recursively load includes first
- merge current file content onto accumulated data
- append current path to the chain once loaded successfully

Path resolution rules:

- `include` paths resolve relative to the declaring JSON file
- `grammar_path` and `highlights_path` continue to resolve relative to the file that declares them, not the top-level file

### 4. Define exact merge helpers

Add internal helpers for deterministic merges:

- `merge_lsp_server(base, override)`
- `merge_syntax_language(base, override)`

LSP merge details:

- Require `name` in all server objects
- Allow override objects to omit unchanged fields
- After merge, validate required fields are present:
  - `name`
  - `command`
  - `language_id`
  - at least one pattern

Syntax merge details:

- Require `name` in all language objects
- Allow override objects to omit unchanged fields
- After merge, validate required fields are present:
  - `name`
  - `grammar_path`
  - `symbol_name`
  - `highlights_path`
  - at least one pattern

Keep legacy `extensions` support exactly as today, but normalize to `patterns` before merge.

### 5. Preserve backward compatibility

Do not change:

- current `./.config/meditrc` then `~/.config/meditrc` discovery
- current single-file configs
- current default fallback when no `meditrc` exists
- current behavior for users who never use `include`

If a config has no includes, behavior should remain byte-for-byte equivalent except for new health output fields.

### 6. Improve `--health` reporting

Extend `src/app/main.cpp` health output to show the config chains, not only the final resolved file.

Add lines:

- `meditrc chain`
- `lsp chain`
- `syntax chain`

Format:
- single file: print the path
- layered: print `path1 -> path2 -> path3`
- absent: `(none)`

Keep existing final resolved file lines too, because they remain useful.

This makes it obvious which shared file and which local override were used.

### 7. Update docs and examples

Update:

- `README.md`
- `config/meditrc`

Documentation should explicitly recommend:

- shared checked-in base config under `config/`
- per-machine overrides under `~/.config`
- `include` for `meditrc`, `lsp.json`, and `syntax.json`
- example use cases:
  - alternate `clangd`
  - machine-specific tree-sitter `.dylib` / `.so`
  - alternate server executable names

Do not make repo-local `./.config` the recommended shared/multi-machine solution in the docs; keep it supported, but document `~/.config` as the default machine-local override location.

## Validation Rules and Errors

Add explicit errors for:

- include cycle in `meditrc`
- include cycle in `lsp.json`
- include cycle in `syntax.json`
- included file not found
- include field of wrong type
- merged server/language missing required fields after override merge
- duplicate final pattern ownership after all includes are merged

Error messages must include the file being processed and, for cycles, the include chain.

## Test Cases

### Config parsing

Add unit tests covering:

1. `meditrc` includes one base file and overrides scalar settings.
2. Multiple `meditrc` includes load in order; later current-file values win.
3. Relative `include` paths in `meditrc` resolve relative to including file.
4. `meditrc` include cycle is rejected.

### LSP config

5. `lsp.json` include merges base + local override by `name`.
6. Local override can replace only `command` and inherit the rest.
7. Relative include paths in `lsp.json` resolve relative to including file.
8. Duplicate final `patterns` across merged servers still fail.
9. `lsp.json` include cycle is rejected.

### Syntax config

10. `syntax.json` include merges base + local override by `name`.
11. Local override can replace only `grammar_path` and inherit the rest.
12. `editor` settings merge per key.
13. Relative paths for `grammar_path` and `highlights_path` remain relative to the file that declared them.
14. Duplicate final `patterns` across merged languages still fail.
15. `syntax.json` include cycle is rejected.

### Health output

16. `medit --health` reports `meditrc chain`, `lsp chain`, and `syntax chain` for layered configs.
17. Non-layered configs still report sensible single-path output.

## Acceptance Criteria

The feature is complete when all of the following are true:

- A user can keep shared config in repo `config/` and machine-local overrides in `~/.config`.
- A machine-local `~/.config/meditrc` can include the shared repo `config/meditrc`.
- A machine-local `lsp.local.json` can override only the `cpp` server `command` without duplicating the whole shared file.
- A machine-local `syntax.local.json` can override only grammar library paths without duplicating the whole shared file.
- Existing users with plain single-file configs see no behavior change.
- `medit --health` clearly shows the layered source chain.

## Assumptions and Defaults

- Default local override location: `~/.config`, not hostname-derived profiles.
- Shared config remains checked in under `config/`.
- Repo-local `./.config` remains supported for ad hoc local overrides but is not the primary documented multi-machine setup.
- No environment-variable interpolation is added in this change; the local override files are the sole machine-specific mechanism.
- Merge keys are `name` for both LSP servers and syntax languages.
