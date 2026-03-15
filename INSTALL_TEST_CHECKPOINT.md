# Install Test Checkpoint

Date: 2026-03-14 UTC

Current state before install-script testing:

- `editor.cpp` was split so UI/rendering code now lives in [src/editor_ui.cpp](/home/dan/de/src/editor_ui.cpp).
- Shared internal declarations/state live in [src/editor_internal.hpp](/home/dan/de/src/editor_internal.hpp).
- [Makefile](/home/dan/de/Makefile) includes `src/editor_ui.cpp` in `APP_SOURCES`.
- `make -j2` completed successfully and linked `medit`.
- The legacy disabled UI copies in `src/editor.cpp` were removed after the split was validated.

Modified or added files relevant to this work:

- [Makefile](/home/dan/de/Makefile)
- [src/editor.cpp](/home/dan/de/src/editor.cpp)
- [src/editor_internal.hpp](/home/dan/de/src/editor_internal.hpp)
- [src/editor_ui.cpp](/home/dan/de/src/editor_ui.cpp)

Install test plan:

- Run `tools/install_local.sh` against a temporary prefix/config root under `/tmp`.
- Do not bootstrap tree-sitter on the first pass.
- Verify that `medit`, `medit-ctl`, config files, and support files are installed in the temporary target.

Suggested cleanup after the install test:

- Remove the temporary install roots under `/tmp`.
- Disabled legacy blocks in [src/editor.cpp](/home/dan/de/src/editor.cpp) have been removed.
