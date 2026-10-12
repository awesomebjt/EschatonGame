# src/core — Engine Core

Scope: app lifecycle, time/frame stepping, config loading. See root `CLAUDE.md` for
overall architecture; this file holds decisions and gotchas specific to this module only.

## Decisions & Notes

- **Data directories are found at runtime** (`resolve_data_dirs` in `main.cpp`):
  `--world` / `--shaders`, else `world/` and `shaders/` beside the executable, else the
  build tree the binary was compiled in (`WORLD_DIR`, `SHADER_DIR`, absolute). Both are
  printed at startup. `--server` loads neither.
- **`cmake --build build --target dist`** assembles `build/dist/eschaton/` (executable,
  all shader backends, and only the world files the loader reads, ~160 MB) plus a
  `.tar.gz` (~33 MB) for copying to other machines. The binary needs a glibc at least
  as new as the build machine's (2.43 from the Fedora 44 toolbox).
