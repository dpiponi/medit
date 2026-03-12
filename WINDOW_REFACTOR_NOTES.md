# Window/Buffer Refactor Notes

## Goal

Keep windows lightweight.

A window should be a view onto a buffer, not a second home for buffer-derived state. If two windows show the same buffer, the expensive work should happen once per buffer whenever possible.

## Desired Ownership Model

### Buffer-owned state

This should live with the buffer or in a cache keyed by buffer id:

- text contents
- undo/redo history
- selection state
- diagnostics
- annotations
- syntax selection and syntax highlight results
- search pattern compilation and search match results
- document version / LSP state
- expensive derived data that depends only on buffer content

### Window-owned state

This should stay per view:

- which buffer the window shows
- scroll offsets
- cursor position, if independent per view is desired
- column goal / preferred x position for vertical movement
- any explicitly view-local navigation state

### Avoid in window state

The following are likely too heavy or too duplicated when kept per window:

- compiled search regex
- full search match lists
- syntax highlight caches
- annotation/visual row caches that depend only on buffer content
- diagnostic sorting data

## Current Issues

The code still keeps too much in `EditorState::WindowUiState` in [src/editor.cpp](/home/dan/de/src/editor.cpp).

Examples:

- search state is per window, including compiled regex and all matches
- visual row cache is per window
- diagnostic selection is per window
- syntax used to be per window and has now been moved to buffer-keyed state

This means split windows can still duplicate work for the same buffer.

## Good Direction Already Started

Syntax highlighting was moved from window-local state to buffer-keyed state in [src/editor.cpp](/home/dan/de/src/editor.cpp). That is the model to follow for other buffer-derived caches.

## Next Refactor Steps

### 1. Move search cache to buffer-owned state

Create a buffer-keyed search cache containing:

- active search pattern
- compiled regex
- match list
- version the matches were built from
- validity/error flag

Windows should only keep the minimal navigation state they need, for example:

- current search match index
- search origin if navigation should remain per window

### 2. Move visual row / annotation layout cache to buffer-owned state

Current visual row data is derived from:

- buffer text revision
- diagnostics revision
- annotations revision
- render width
- whether diagnostics are currently visible

That should be cached by buffer id, with width-sensitive entries if needed.

Windows should only use the computed rows, not own them.

### 3. Revisit diagnostic selection

Decide whether “selected diagnostic” is:

- per buffer, or
- per window

If it remains per window, keep only the selected index in window state. Sorting and projection should be shared per buffer.

### 4. Clarify cursor ownership

Right now the editor core owns the cursor, which means multiple windows onto the same buffer share one cursor.

That may be acceptable for now, but long term we should decide whether:

- cursor belongs to the buffer, or
- cursor belongs to the window/view

If independent view cursors are desired, this is a larger architectural change and should be treated separately from the lighter cache refactors above.

## Refactor Rule

When adding a new feature, ask:

"Does this depend on the document, or on the view?"

If it depends only on document content or document metadata, it should not default to window ownership.

## Performance Reason

Typing latency is affected most by work that repeats on every text revision. If that work is duplicated across windows showing the same buffer, split views become disproportionately expensive.

The intended model is:

- one buffer change
- one set of buffer-derived recomputations
- multiple cheap window renders over shared derived state
