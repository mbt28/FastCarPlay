# Vendored LVGL 9.3.0

This is a **trimmed, vendored copy** of LVGL, not a git submodule.

- Upstream: https://github.com/lvgl/lvgl  (tag `v9.3.0`, commit `c033a98`)
- License: MIT (see `LICENCE.txt`)

## Why vendored and not a submodule

The FastCarPlay Buildroot package fetches the repo as a GitHub source archive
(`$(call github,...)`), and GitHub archives never contain submodule contents.
A submodule would arrive empty in the image build and break `USE_LVGL=1`.
LVGL is also compiled *into* the FastCarPlay binary (there is no system
`liblvgl` -- Buildroot does not package LVGL, by design, because `lv_conf.h`
is a compile-time configuration). LVGL's own `lv_port_linux` template does the
same: build LVGL from source into the app.

## What was kept

Only what the build needs: `src/`, `lvgl.h`, `lvgl_private.h`, `lv_version.h`,
plus the license files and `lv_conf_template.h` for reference. The upstream
`demos/`, `examples/`, `tests/`, `docs/`, `scripts/` etc. (~210 MB) were
removed. The active configuration is `src/ui/lv_conf.h` (via
`LV_CONF_INCLUDE_SIMPLE`), not the template here.

## Updating

Re-download the target tag from upstream, replace `src/` and the three
top-level headers, keep this file, and rebuild `make USE_LVGL=1`.
