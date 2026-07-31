<!-- neui reference. Extracted from CLAUDE.md - read when working on these topics. -->

## LVGL host (EXPERIMENTAL)

> **Status: experimental prototype. Not supported for product work, and not built by default.**
> It exists to answer one question - can neui render through LVGL on embedded hardware - and it
> answered it on a desktop stand-in. **Further work is paused until real target hardware is
> available**; the two things that matter next (a per-widget appearance cache and an embedded
> display driver / VGLite path) cannot be evaluated without a panel and an MCU in the loop.
> Design record, per-milestone detail and all measurements: `plans/lvgl-host-approach-c.md`.

**What it is**: a second rendering substrate for the *existing* crossplatform host, not a new host.
With `-DNEUI_WITH_LVGL=ON` the xpl host pairs with `backends/lvgl/` (`neui-backend-lvgl`) +
`platform_lvgl.cpp` instead of `neui-backend-d2d` + `platform_win32.cpp`. Client code is unchanged
and unaware: the public API surface is identical, so what is experimental is the *host feature set*
below, not the client contract.

**Build** (Windows only - configure `FATAL_ERROR`s elsewhere, because the prototype drives the LVGL
Windows display driver):

```bash
cmake -B out/build-lvgl -DNEUI_WITH_LVGL=ON [-DNEUI_LVGL_COLOR_DEPTH=32|16]
cmake --build out/build-lvgl --config Debug
```

LVGL is fetched from git via FetchContent and **pinned to commit `066d8db0`** (the revision every
measurement was taken against; move to a release tag once one carries the `lv_draw_vector_dsc_*`
API). `lv_conf.h` is generated from `backends/lvgl/lv_conf.h.in`; `NEUI_LVGL_COLOR_DEPTH` selects
the framebuffer depth (32 bpp default, 16 = RGB565). Adds one target, `neui_lvgl_example` (console
app; it prints frame timings to stdout).

**Retained vs whole-frame**: by default each neui widget gets a passive `lv_obj` mirror so a local
interaction repaints one widget rect instead of the screen ("Option C"). Set `NEUI_LVGL_RETAINED=0`
at runtime to fall back to the whole-frame walk in the same binary - the comparison the measurements
in the plan are built on.

**Works**: the full `neui_render_backend_t` (fills, borders, text through per-size Tiny TTF
instances, the whole path model through ThorVG, linear/radial gradients, clip stack, 2x3 CTM,
bitmaps), the retained mirror layer with per-widget invalidation and correct child paint order,
mouse / key / wheel input including **smooth-scroll kinetics** for the scrolling SECTION and the
GRID (the shared `scroll_kinetics` integrators on a 16 ms `lv_timer`), images through
`stb_image` (`hosts/shared/image_loader_stb.h`), and everything portable that rides on those:
compound / behavior / component assets, the GRID, and the client resource provider
(`docs/rendering-and-assets.md`).

**Stubbed or absent** (the honest list - none of these are bugs, they were out of the prototype's
scope):

- Clipboard, drag-and-drop, IME, native menubar, message boxes.
- Off-screen surfaces: `create_surface` returns `asset_none`, so SURFACE assets and the whole SVG
  filter graph are unreachable.
- Font registration (`create_font*`) returns false; family names resolve from `C:\Windows\Fonts`
  by family + weight instead.
- No DPI scaling: logical px == LVGL px == physical px.
- Closing a frame hides its window rather than destroying it (the LVGL Windows driver's display
  watchdog `exit(0)`s the process when the last display dies mid-loop). A production port runs its
  own display driver, which an embedded target does anyway.
- Dialogs are resizable; overlay changes (combo drop, popup menu, toast) still invalidate the whole
  frame.

**Known cost to be aware of on 565 targets**: LVGL's software vector path renders ThorVG only into
ARGB8888/XRGB8888, so on RGB565 every vector task round-trips through a temporary full-framebuffer
ARGB buffer. The backend mitigates this by coalescing consecutive path draws into one
`lv_draw_vector` task; the residue is the reason VGLite-class hardware is the production answer for
path chrome at 565. Numbers for both depths are in the plan.

**Test-automation note** (this bit tricks everyone): `WM_MOUSEWHEEL`'s `lParam` carries SCREEN
coordinates while the button messages carry CLIENT coordinates, and `PrintWindow` renders the whole
window (title bar included) into the target DC, so a client-sized bitmap is offset by the
non-client frame. Drive synthetic input from a widget's `create()` coordinates rather than from
measured screenshot pixels, and make the sending process Per-Monitor-V2 aware or Windows
DPI-virtualises the coordinates on the way in.
