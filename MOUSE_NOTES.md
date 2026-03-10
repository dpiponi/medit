# Mouse Notes

Status: on hold

## Current Behavior

- Mouse click-to-cursor movement works when tested on Windows.
- Mouse click-to-cursor movement does not work reliably on iPad when using Shellfish to SSH into a remote Linux box.
- On the iPad/Shellfish path, the first click may work, but subsequent clicks are ignored.

## Important Evidence

- The same editor binary behaves differently across terminal environments.
- Other terminal editors on the iPad path, including `vim` and `hx`, do handle mouse input correctly.
- That strongly suggests the problem is terminal mouse protocol handling or `ncurses` event interpretation, not the basic cursor-move implementation itself.

## Likely Causes

- Shellfish may be sending mouse events in a form different from what the current `ncurses` setup expects.
- The active `$TERM` on the remote Linux box may not match what Shellfish actually supports.
- The editor may need different mouse tracking flags or broader event handling for that terminal path.

## Current Implementation Notes

- Mouse handling is in [editor.cpp](/home/dan/de/src/editor.cpp).
- The editor currently enables `ncurses` mouse support and maps button-1 click events to buffer positions.
- Buffer position mapping already accounts for:
  - line number gutter
  - scroll offsets
  - wide-character display widths

## Next Debugging Step

Add temporary logging around `getmouse()` and `MEVENT.bstate` so the event stream can be compared between:

- Windows terminal where it works
- iPad Shellfish session where it fails

Record at least:

- `getmouse()` result
- `MEVENT.bstate`
- `MEVENT.x`
- `MEVENT.y`
- `$TERM`

## Follow-Up Questions

- Does Shellfish send double-click, drag, or SGR-style mouse events instead of plain click events?
- Does `ncurses` need different mousemask flags for that terminal?
- Does the remote environment need a different `TERM` value for Shellfish?
