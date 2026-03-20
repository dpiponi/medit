# Lua Migration Checklist

Goal: shrink the C++ core down to editor-engine responsibilities and move command orchestration, external-tool workflows, and user-specific policy into Lua wherever that boundary stays clean.

## Principles

- Keep in C++:
  - text model and edits
  - undo/redo
  - cursor and selection semantics
  - rendering and curses lifecycle
  - input decoding and modal state machine
  - LSP transport
  - tree-sitter runtime and highlighting
  - control socket and low-level protocol glue
- Prefer Lua for:
  - shell-driven commands
  - project-specific behavior
  - command orchestration
  - summary formatting
  - optional integrations and health checks

## Tier 1

- [x] Move `find-file` to Lua
- [x] Move `grep` to Lua
- [x] Move `pick-theme` to Lua
- [ ] Move startup file picker policy to Lua
  - Needed API: startup hook or startup-command delegation
- [~] Move optional dependency health checks toward Lua ownership
  - Done: Lua health checks are registered and shown by `--health`
  - Next: migrate more command-specific checks into Lua

## Tier 2

- [x] Move AI command orchestration to Lua
  - Done: `:ai` and `:ai-popup` now use Lua-owned text/process APIs
- [x] Move file-under-cursor policy to Lua
  - Done: `go to file under cursor` is now owned by the Lua `open-file-under-cursor` command
- [ ] Move diagnostics summaries and custom quickfix workflows to Lua
  - Needed API: diagnostics query, popup/menu APIs, location jumps
- [ ] Move command metadata registration to Lua
  - Needed API: command registration with detail and completion metadata

## Tier 3

- [ ] Move theme switching completely to Lua
- [ ] Move project build/test/run workflows to Lua
- [ ] Move session helpers and project dashboards to Lua
- [ ] Move external text transforms and custom filters to Lua

## Lua API Backlog

- [x] `medit.executable_exists(name)`
- [x] `medit.run_picker(command)`
- [x] `medit.register_health_check(name, fn)`
- [x] `medit.get_cursor()`
- [x] `medit.set_cursor(position)`
- [x] `medit.open_location(path, position)`
- [x] `medit.show_popup(title, text)`
- [x] `medit.shell_quote(text)`
- [x] `medit.get_selection()`
- [x] `medit.get_selection_text()`
- [x] `medit.get_buffer_text()`
- [x] `medit.replace_selection(text)`
- [x] `medit.replace_buffer(text)`
- [x] `medit.run_filter(command, input)`
- [x] `medit.resolve_ai_command()`
- [x] `medit.resolve_ai_provider()`
- [x] `medit.resolve_ai_model(provider?)`
- [x] `medit.reload_config()`
- [ ] `medit.read_file(path)` / `medit.write_file(path, text)` or a narrower config helper
- [ ] `medit.get_diagnostics()`
- [ ] `medit.show_menu(...)`
- [ ] Lua command registration with palette metadata

## Current Focus

- [x] Add buffer/selection text APIs for AI-command migration
- [x] Add config mutation or reload helpers for Lua-owned workflows
