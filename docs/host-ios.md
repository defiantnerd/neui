<!-- neui reference. Extracted from CLAUDE.md - read when working on these topics. -->

## iOS / iPadOS hosts

> **Implemented and shipping.** Anything in this repo still calling iOS unported is stale.
> CI builds both hosts and runs the unit suite on a simulator on every push.

### Two hosts

| | `neui.host.ios` | `neui.host.crossplatform` on iOS |
|---|---|---|
| Where | `hosts/ios/` (`neui-ioshost`) | `platform_ios.mm` (`neui-xplhost`) |
| Widgets | native UIKit controls, one `UIView` each | painted into one `NEUIView` per frame |
| Pick it when | it should feel like an iOS app | it must match the desktop build pixel for pixel |

Both use `neui-backend-cg`. `neui_init()` registers **ios first**, so `neui_get_api(NULL)`
returns the native one; ask by id and fall back to `"neui.host.crossplatform"`. Both can be
linked into one binary — `examples/ios` does — which is what the seam rule below is about.

Object graph, same shape on both: `UIWindowScene` → `UIWindow` (owned by the frame's
`WidgetData`, `+1` retained) → `NEUINativeIOSViewController` / `NEUIViewController` →
content view → per-widget `UIView`s (native host only). `get_native_handle` gives the
`UIWindow*` for a frame, the `UIView*` for a child.

**Units: logical pixels are UIKit points, 1:1.** No conversion; backing scale reaches the
backend separately as `wd.dpi = 96 * screen.scale`.

### Already handled — do not reimplement

- **Safe-area insets** — `metrics->safe_area_insets`, real on both hosts. `get_client_rect`
  already subtracts the **top** inset (safe area + the 44 px hamburger band when a MENUBAR is
  present). *Left, right and bottom are reported but not subtracted — do that yourself.*
- **Dynamic Type** — `metrics->ui_scale` and every `NEUI_METRIC_*` are already scaled, and an
  explicit `NEUI_ATTR_FONT_SIZE` is routed through `UIFontMetrics`. **Do not scale again.**
- **`NEUI_EVENT_METRICS_CHANGED`** on Dynamic Type, rotation and safe-area change — a change
  confined to the left, right or bottom inset moves no bounds and raises no `RESIZE`, so this
  is the only notice a client laying out inside the safe area gets.
- **Dark mode** via `NEUI_ATTR_FOLLOW_SYSTEM_THEME`; `@2x`/`@3x` asset selection.

### Not available here

- No menu bar on iPhone (a MENUBAR becomes the hamburger button; iPad 26+ gets a real one).
- `notify->message_box` returns `NEUI_MB_IOS_PENDING` — `UIAlertController` is async.
- `UIApplicationMain` owns the run loop: `run()` returns immediately, never call `pump_once()`.
  Build the UI from `scene:willConnectToSession:`.
- `dnd->begin_drag` is a no-op; attach a `DRAG_SOURCE` behavior asset.
- Nothing moves content out from under the keyboard — ask `NEUI_API_IOS::keyboard_inset`.

### `NEUI_API_IOS`

Reference: **`include/neui/d/ios.h`**. Idle timer, screen-edge gesture deferral, status bar,
orientation, forced appearance, Dynamic Type *category*, accessibility switches, device /
battery / thermal / Low Power Mode, haptics, keyboard inset.

```c
neui_ios_api_t* ios = (neui_ios_api_t*)api->get_interface(sess, NEUI_API_IOS);
if (ios) ios->set_idle_timer_disabled(sess, 1);   // NULL on every non-iOS host
```

Optional by contract, like `NEUI_API_EMBED` and `NEUI_API_METRICS`, but never inert: a host
that returns it implements every method, and a call that cannot be answered says so in its
return value. Changes arrive as one `NEUI_EVENT_IOS_ENVIRONMENT_CHANGED` carrying a
`NEUI_IOS_ENV_*` mask; Dynamic Type stays on `METRICS_CHANGED`, where clients already handle it.

Implementation is shared in `hosts/shared/ios/ios_api.h` — UIKit's globals are the same on both
hosts. Only two things are per-host, and they are seams: resolving a frame to its
`UIViewController`, and walking the host's own registry to deliver the event. Frame chrome
(status bar, home indicator, deferred edges, orientations) lives in a shared table keyed by
widget id; each host's view controller reads it back in its `preferredStatusBarStyle` and
friends, falling through to `super` when the client set nothing.

### Seams must be ADDED, not assigned

**The trap.** `neui_init()` registers the native host and *then* xpl, whose `platform_init()`
runs last. An assigned single-slot seam therefore always ends up holding **xpl's** — and for a
native-host client it fails silently: the frame resolves to nothing, so `safe_area_insets`
reports zeros and the environment event reaches nobody. Both were real bugs, both measured in
`examples/ios`.

So `NEUI_API_IOS` and `NEUI_API_METRICS` both keep **lists**. Each host calls
`ios_add_*_seam` / `metrics_add_*_seam`; the frame lookups try each until one claims the frame
(a frame belongs to exactly one host) and the event broadcast runs all of them. Adding the same
pointer twice is a no-op, so a repeated `register_host()` is safe. Covered by
`tests/test_metrics.cpp`.

### Build and test

```sh
cmake -B out/ios -G Xcode -DCMAKE_SYSTEM_NAME=iOS \
      -DCMAKE_OSX_SYSROOT=iphonesimulator -DCMAKE_OSX_ARCHITECTURES=arm64
cmake --build out/ios --config Debug
xcrun simctl install booted out/ios/tests/Debug-iphonesimulator/neui_tests.app
xcrun simctl launch --console-pty booted org.neui.tests
```

`NEUI_IOS` is a **CMake variable, not a preprocessor define** — sources are selected by CMake.
The exception is `hosts/crossplatform/host.cpp`, compiled everywhere, which needs a real macro
for its `NEUI_API_IOS` lines: `NEUI_PLATFORM_IOS=1`, minted in the xpl host's `elseif(NEUI_IOS)`
branch (mirroring `NEUI_PLATFORM_LVGL=1`).

`tests/test_ios_api.cpp` pins the enum and bit-flag ABI and runs on all four CI jobs.
`tests/test_ios_api_device.mm` is iOS-only and exercises the real implementation on the
simulator — the process-global queries plus the session and frame bookkeeping. It has a plain
`main()` and no `UIApplication`, so anything frame-scoped (status bar, orientation, keyboard) is
exercised by `examples/ios` instead.
