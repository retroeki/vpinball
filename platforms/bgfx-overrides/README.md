# bgfx source overrides

The bgfx Vulkan backend is built from a pinned fork (`vbousquet/bgfx` @ `BGFX_PATCH_SHA`
in `platforms/config.sh`), downloaded as a tarball by each platform's `external.sh`,
compiled into `libbgfx.a`, and linked by the main `vpinball` build. That source lives
under the gitignored `external/` tree, so edits there are **not** version-controlled and
are wiped on a clean re-fetch.

This directory holds **tracked overrides** of specific bgfx source files. After
`external.sh` extracts the fork it copies this tree over `bgfx.cmake/bgfx/`, so the files
here win. **Edit the files here**, not the copies under `external/`.

Layout mirrors bgfx: `src/renderer_vk.cpp` -> `bgfx.cmake/bgfx/src/renderer_vk.cpp`.

## Currently overridden
Baseline = `vbousquet/bgfx` @ `a20c34bbe621cde25c0b5826d90ffec6b9f499d9` (verbatim copies):
- `src/renderer_vk.cpp` — Vulkan backend; target for **per-draw descriptor-set caching**
  (the ~30 us/draw CPU-submit cost on the Android/bgfx-Vulkan target lives here).
- `src/renderer_vk.h`

## Rebuild after editing
`external.sh` only rebuilds bgfx when its cache key changes. After editing a file here,
**bump the cache-buster suffix** (`_NNN` in `BGFX_EXPECTED_SHA` in `external.sh`), then
re-run `platforms/android-arm64-v8a/external.sh` and rebuild `vpinball`. Otherwise the
prebuilt `libbgfx.a` is reused and the edit is ignored.

## Keeping in sync
These are full-file copies pinned to the baseline SHA above. If `BGFX_PATCH_SHA` is bumped
in `config.sh`, re-copy/re-diff these files against the new fork source so the override
doesn't silently revert unrelated upstream fixes.
