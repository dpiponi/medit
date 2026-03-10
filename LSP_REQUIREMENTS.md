# LSP Requirements

This file tracks the outstanding work required before `medit` is ready for Language Server Protocol integration.

Items are ordered roughly by dependency and architectural leverage. Update this file whenever a requirement is completed, split, or superseded.

## Status

- Done: editor core separated from the terminal UI
- Done: first-class selection/range groundwork in the core with visual selection in the terminal UI
- Done: generic range edit API in the core
- Done: atomic multi-edit transactions with single-step undo/redo
- Done: explicit document version tracking in the core
- Done: UTF-8 and UTF-16 position conversion utilities in the core
- In progress: building a reusable editor core API around stable document semantics and protocol-facing tests
- Not started: everything else below

## Outstanding Requirements

1. Generic range edit API
   Status: done in [editor_core.hpp](/home/dan/de/src/editor_core.hpp) and [editor_core.cpp](/home/dan/de/src/editor_core.cpp)
   The core needs first-class `insert`, `delete`, and `replace` operations over arbitrary `Range` values, not only vi-driven point edits.

2. Edit transactions
   Status: done in [editor_core.hpp](/home/dan/de/src/editor_core.hpp) and [editor_core.cpp](/home/dan/de/src/editor_core.cpp)
   The core needs a way to apply multiple text edits atomically as one undoable operation. LSP responses often contain many edits that must be applied together.

3. Stable document versioning semantics
   Status: done in [editor_core.hpp](/home/dan/de/src/editor_core.hpp), [editor_core.cpp](/home/dan/de/src/editor_core.cpp), and [test_editor_core.cpp](/home/dan/de/tests/test_editor_core.cpp)
   The core already tracks revisions, but this needs to become an explicit document version model suitable for `didOpen`, `didChange`, `didSave`, and conflict handling.

4. Text offset conversion utilities
   Status: done in [editor_core.hpp](/home/dan/de/src/editor_core.hpp), [editor_core.cpp](/home/dan/de/src/editor_core.cpp), and [test_editor_core.cpp](/home/dan/de/tests/test_editor_core.cpp)
   Add explicit conversions between:
   editor positions
   UTF-8 byte offsets
   UTF-16 LSP positions
   This is required for correct diagnostics, edits, hover ranges, and completion insertions.

5. Clear document identity model
   Add durable document identifiers and URI handling so buffers can be addressed the way LSP clients expect.

6. Change event pipeline
   The core should emit structured events for:
   document opened
   document changed
   document saved
   document closed
   cursor moved
   These events should be consumable without depending on `ncurses`.

7. External command/service boundary
   Introduce a clean interface for long-lived background services so an LSP client can be attached without coupling protocol code to the editor core.

8. Asynchronous event loop
   The frontend/runtime needs a non-blocking way to process editor input, background responses, and UI updates together.

9. Diagnostics data model
   Add core-level storage and querying for diagnostics with severity, range, message, and source fields.

10. UI surfaces for LSP results
    The frontend needs reusable presentation primitives for:
    diagnostics
    completion menus
    hover popups
    signature help
    location lists

11. Multi-range and richer selection model
    Basic single-range selection is now implemented. The remaining work is support for richer selection shapes and multiple selections where future features need them.

12. Buffer capability model
    Add a way to declare buffer language/filetype and per-buffer capabilities so the editor can decide which servers and features apply.

13. Root/workspace model
    LSP needs project/workspace awareness, not just single-file editing. The editor needs a concept of workspace root and opened documents within it.

14. Process and transport management
    Implement a managed JSON-RPC transport layer over stdio for language server processes, including startup, shutdown, restart, and failure handling.

15. Request/notification routing
    Add client-side handling for:
    initialize
    initialized
    didOpen
    didChange
    didSave
    didClose
    publishDiagnostics
    completion
    hover
    definition
    references
    signatureHelp

16. Config and capability negotiation
    Add editor-side configuration and a way to advertise supported client capabilities during server initialization.

17. Logging and protocol tracing
    LSP integration will need structured logs and optional protocol tracing for debugging correctness issues.

18. Automated core tests
    Status: in progress in [test_editor_core.cpp](/home/dan/de/tests/test_editor_core.cpp)
    The core now exists as a separate layer, but it needs direct tests for position math, edits, undo/redo, and file behavior before protocol integration starts.

19. Automated protocol-facing tests
    Status: in progress in [test_editor_core.cpp](/home/dan/de/tests/test_editor_core.cpp)
    Add tests around versioning, edit application ordering, and position conversion so LSP-specific regressions are caught early.

20. Command entry points for LSP-driven actions
    Expose core/editor commands for "apply diagnostics", "jump to definition result", "show completions", and similar actions so protocol features do not bypass the command model.

## Maintenance Rules

- When a requirement is completed, mark it done and keep the completed item in this file until the next major cleanup.
- When new architectural work reveals missing prerequisites, insert them at the correct dependency point rather than appending blindly.
- If an item becomes too broad, split it into smaller ordered items.
- If implementation starts on any item, note the affected files beside that item.
