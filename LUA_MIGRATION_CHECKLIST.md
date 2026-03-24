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

## Concrete Next Plan

### Phase 1: Unify command ownership

Objective: stop splitting command behavior across a C++ built-in table and a Lua-only name registry.

Current constraints:

- C++ owns built-in command metadata and palette entries.
- Lua commands register only `name -> function`.
- Command completion shows Lua entries as generic "Lua command" items.

Deliverables:

- Extend `medit.register_command(...)` to optionally accept metadata:
  - `detail`
  - `completion_text`
  - optional `aliases`
  - optional `category`
- Replace the separate C++/Lua completion assembly with one merged command registry.
- Keep hard engine commands in C++, but register their palette metadata through the same internal shape Lua uses.

Suggested C++ changes:

- Update `src/platform/lua_runtime.cpp` command registration storage to hold a metadata struct, not only a Lua ref.
- Refactor `src/editor/editor_command_palette.cpp` and `src/editor/editor_ex_commands.cpp` to build completion items from one command source.
- Preserve direct C++ dispatch for engine-only commands such as `:w`, `:q`, panel controls, and config reload.

Success criteria:

- Palette entries for Lua and C++ commands share the same detail/completion path.
- `config/medit/init.lua` can describe commands without any C++ command-palette edits.

### Phase 2: Move startup picker policy to Lua

Objective: make startup behavior for directory arguments user-scriptable.

Current constraints:

- Directory startup detection lives in `open_startup_files(...)`.
- Startup picker flow is still hardwired in C++.

Deliverables:

- Add a startup hook or startup-command delegation API in Lua.
- Pass startup arguments and the detected directory/root into Lua.
- Reimplement the "open directory -> run picker -> open selected file" policy in `config/medit/init.lua`.

Suggested API shapes:

- `medit.on_startup(function(args) ... end)`
- or `medit.register_startup_handler(fn)`

Success criteria:

- Launching `medit some-directory` no longer requires C++ picker policy.
- Alternative startup workflows become possible in Lua:
  - open recent file
  - show project dashboard
  - auto-open README / todo file

### Phase 3: Add diagnostics and menu APIs

Objective: expose enough editor state for Lua to own summaries and quickfix workflows.

Missing Lua APIs:

- `medit.get_diagnostics(buffer_id?)`
- `medit.show_menu(title, items, opts?)`
- optional helper: `medit.list_buffers()`
- optional helper: `medit.active_buffer_id()`

Recommended menu item shape:

- `label`
- `detail`
- `position`
- `path`
- `buffer_id`
- `payload`

Suggested diagnostic shape:

- `severity`
- `message`
- `source`
- `code`
- `range`
- `buffer_id`
- `document_uri`
- `file_path`

Success criteria:

- Lua can compute error/warning counts without calling back into C++ status helpers.
- Lua can present a pickable diagnostics list and jump to the selected location.

### Phase 4: Migrate diagnostic and status commands

Objective: move shallow status formatting and quickfix policy out of C++.

Good candidates:

- `:diagnostics`
- custom quickfix lists
- richer LSP status summaries
- tree-sitter status formatting

Recommended split:

- Keep raw status producers in C++:
  - LSP runtime status
  - tree-sitter runtime status
  - editor event transport
- Move formatting and workflow decisions to Lua:
  - how to summarize
  - whether to show popup vs. menu vs. panel
  - project-specific filtering and grouping

Success criteria:

- `:diagnostics` becomes a Lua command.
- Custom workflows like "errors only", "current file only", or "group by source" can be added without C++ edits.

### Phase 5: Push more project workflows to Lua

Objective: keep C++ focused on primitives while optional workflows live in script.

Best candidates:

- build / test / run commands
- project dashboards
- external text transforms
- notebook / REPL helpers
- custom panels fed by async jobs or processes

Keep in C++:

- buffer creation and mutation
- async process plumbing
- panel/window mechanics
- text edits and cursor semantics

Success criteria:

- New workflow commands are usually implemented entirely in `config/medit/init.lua`.
- C++ additions are mostly primitive APIs, not feature-specific orchestration.

## Priority Order

1. Command metadata registration and unified palette model
2. Startup hook for directory-open policy
3. Diagnostics query and menu APIs
4. Lua migration of `:diagnostics` and related quickfix workflows
5. Ongoing migration of project-specific workflows

## Design Guardrails

- Do not move text editing semantics, undo/redo, rendering, curses lifecycle, LSP transport, tree-sitter runtime, or input decoding into Lua.
- Prefer adding narrow primitives to Lua instead of exporting large mutable editor internals.
- If a feature is optional, shell-driven, project-specific, or mostly formatting, bias toward Lua.
- If a feature is latency-sensitive, correctness-critical, or central to modal editing semantics, keep it in C++.
