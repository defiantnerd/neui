# WinUI3 Host Feasibility

## Context

neuilib already ships two hosts: a Win32 native-control host and a
crossplatform self-painted host (with a Direct2D backend). The
question is whether a third host backed by **WinUI3** would buy us
anything - specifically, can a WinUI3 host coexist with the existing
two, can it draw via Direct2D, and can it be built with our existing
MSVC + CMake + C++17 toolchain *without* C# or managed C++/CLI.

This document is purely an evaluation. It does not propose
implementation work; it documents what such work would entail and
ends with a recommendation.

## TL;DR

**Technically possible. Strategically poor fit. Recommend deferring
unless a specific Fluent-design or accessibility requirement
materialises.**

The three load-bearing reasons:

1. **The build cost is large and one-way.** WinUI3 forces a NuGet +
   `cppwinrt.exe` + XAML-compiler + Windows App SDK Bootstrapper
   pipeline. CMake support is unofficial and fragile - every other
   non-Windows-Forms framework that has tried (Qt, Skia/Slint, etc.)
   ends up either hand-rolling MSBuild props or maintaining a
   separate `.vcxproj`. We'd add ~150-300 lines of build
   infrastructure for one Win32-only host.
2. **Two of our 16 widget types fight WinUI3's model directly.**
   `PLUGWINDOW` (audio-plugin embed-in-foreign-HWND) has no
   supported path. `MENUBAR` becomes a XAML control instead of a
   real OS menu, breaking our `HACCEL` accelerator pipeline and
   Alt-key system mnemonics. Both are central to neuilib's
   audio-plugin and typed-shortcut design decisions.
3. **The Direct2D backend would have to be rewritten.** Today's
   backend uses `ID2D1HwndRenderTarget`. WinUI3's Direct2D seam is
   `SwapChainPanel`, which needs `ID2D1DeviceContext` over a DXGI
   swap chain. That's a parallel backend, not a tweak.

The *one* thing WinUI3 offers that the xpl host doesn't is native
Fluent UI + native UIAutomation. If those become hard requirements
(e.g. for an enterprise accessibility cert), revisit. Otherwise
ship.

## What gets built (if we did it)

### Three new build targets

| Target | Purpose |
|---|---|
| `neui-winui3host` | Static lib implementing the same `neui_api_t` contract as the other two hosts. Registers as `"neui.host.winui3"`. |
| `neui-backend-d2d-dx` | New Direct2D backend over `ID2D1DeviceContext` + DXGI swap chain (the existing `neui-backend-d2d` cannot drive a `SwapChainPanel`; see §3). |
| `neui-winui3host-deps` | Interface library that wires up Windows App SDK NuGet + cppwinrt projection headers + XAML compile step + Bootstrapper init. |

The example exe optionally links `neui-winui3host` and selects it via
the host registry.

### File layout

```
hosts/winui3/
  host.h            // Session, WidgetData hierarchy
  host.cpp          // session lifecycle, get_interface
  widgets.cpp       // widget/items/tree/attr/clipboard/commands
  window.cpp        // DispatcherQueue, DesktopWindowXamlSource glue
  bootstrap.cpp     // MddBootstrapInitialize2 / Shutdown
  app.manifest      // side-by-side WinAppSDK runtime declaration
  winui3.idl        // (only if we author custom WinRT types - likely not)
backends/d2d-dx/
  d2d_dx_backend.h
  d2d_dx_backend.cpp
```

## 1 - Compilation toolchain

### What WinUI3 actually requires

- **Windows App SDK 1.8+** via NuGet (`Microsoft.WindowsAppSDK`).
  This bundles WinUI3 + the Bootstrapper + supporting runtime. There
  is no standalone WinUI3 SDK.
- **C++/WinRT 2.0+** (`Microsoft.Windows.CppWinRT` NuGet). Pure
  native: it generates standard C++17 template headers (`winrt/...`)
  from `.winmd` metadata via `cppwinrt.exe`. No managed code, no
  C++/CX, no `/clr`. **Status: maintenance mode** - Microsoft has
  stated C++/WinRT is feature-complete; bug fixes only. Acceptable
  for a stable framework, but no new Modern-WinRT-on-C++ pattern is
  going to appear here.
- **XAML compiler.** WinUI3 expects at least one `.xaml` file
  (`App.xaml`) so the runtime resource map exists. "Imperative-only,
  no XAML" is documented as unsupported and degrades silently.
- **App manifest.** A `.manifest` file declaring side-by-side load
  of the Windows App SDK runtime.
- **Bootstrapper init.** Unpackaged apps must call
  `MddBootstrapInitialize2(0x00010008, ...)` at process start before
  any WinRT factory call, and `MddBootstrapShutdown` at exit.
- **Windows 10 build 17763+** as the minimum target (we currently
  build with no min version pinned).

### CMake reality

There is **no first-class CMake support** for WinUI3 from Microsoft.
The official toolchain is MSBuild + `.vcxproj` props/targets. To
make our CMake build work, we have to hand-roll:

1. **NuGet restore step.** Either via `nuget.exe restore` driven by
   `add_custom_command(... PRE_BUILD)` against a tiny helper
   `packages.config`, or via vcpkg manifests. NuGet route is closer
   to Microsoft's intended path.
2. **cppwinrt projection generation.** Run `cppwinrt.exe -input <sdk
   .winmd files> -output <gen dir>` once at configure time; the
   generated headers go on the include path of `neui-winui3host`.
3. **XAML compile.** Invoke the XAML compiler
   (`Microsoft.UI.Xaml.Markup.Compiler.dll` via MSBuild or directly)
   to produce `.g.h`/`.g.cpp` C++ stubs and the binary `.xbf`
   resource. This is the part with no clean CMake story -
   community workarounds use `add_custom_command` calling
   `msbuild.exe` on a minimal embedded `.vcxproj`. Roughly 80-150
   lines of CMake to get right.
4. **Manifest + runtime side-by-side declaration.** Embed
   `app.manifest` via `target_sources` so the linker picks it up.
5. **MakePri** for resource packaging if we ship images/strings
   through XAML resources. Avoidable if we keep all resources
   outside XAML (recommended).

**Estimate: 1–2 weeks of build infrastructure work, with ongoing
maintenance pain at every Windows App SDK upgrade.** Compare with
adding `imm32.lib` to the existing host (one line) - different
universe.

### Verification of "no managed code"

C++/WinRT compiles without `/clr` and without `<CLRSupport>`. The
generated projection (`winrt/Microsoft.UI.Xaml.h` etc.) is standard
C++ templates calling COM `IInspectable` ABI directly. **Confirmed
native.** The XAML compiler outputs C++ source, not IL.

## 2 - Architectural mapping to the host contract

The host contract is fixed (per CLAUDE.md and host inventory): all
function pointers in `neui_api_t`, `neui_widget_api_t`,
`neui_items_api_t`, `neui_tree_api_t`, `neui_attr_api_t`,
`neui_clipboard_api_t`, `neui_commands_api_t` are mandatory.
Optional client-side: `neui_clipboard_client_t`, `neui_menu_client_t`.

| Concept | Win32 host | xpl host | WinUI3 host |
|---|---|---|---|
| Top-level frame | `HWND` (CreateWindowExW) | `HWND` + D2D backend | `Microsoft.UI.Xaml.Window` (one DispatcherQueue) |
| Child widget identity | child `HWND` | logical idx + paint | XAML `UIElement` ref + logical idx |
| Event loop | `GetMessage` | `GetMessage` | `DispatcherQueueController` (still backed by Win32 message pump under the hood) |
| Focus | OS focus per widget | logical, frame-only OS focus | XAML focus engine (close to OS focus, but XAML-managed) |
| Accelerators | `HACCEL` + `TranslateAccelerator` | same | XAML `KeyboardAccelerator` per element - **incompatible with HACCEL** |
| Clipboard | `OpenClipboard` etc. | same | `Windows.ApplicationModel.DataTransfer.Clipboard` (different API; same effect) |
| HiDPI | per-monitor v2 | per-monitor v2 + D2D scale | XAML handles automatically |
| IME | native `Edit` does it | xpl on_composition | XAML handles automatically (loses our existing IME work - fine since it's an alternate host) |

**Single-HWND constraint.** WinUI3 internally creates one HWND per
`Window` and renders all controls into it via the visual-tree
compositor. Our `WidgetData::native_handle` semantics
(`HWND`-per-widget on win32, frame-`HWND` on xpl) becomes
"`UIElement*`-per-widget" in WinUI3. `get_native_handle` returns
the projected interface pointer - useful for clients but not an
HWND. Document it.

**`PLUGWINDOW` is a real problem.** Audio plugins receive a parent
HWND from the DAW and must root their UI inside it. WinUI3 creates
its own top-level Window - `Microsoft.UI.Xaml.Window` cannot be
parented to a foreign HWND. `DesktopWindowXamlSource` lets you embed
WinUI3 *inside* an HWND you own, which actually matches our
`PLUGWINDOW` use case. Path:

1. Receive parent HWND from DAW via the existing `PLUGWINDOW` x/y/w/h
   creation flow.
2. `DesktopWindowXamlSource source; source.Initialize(parent_hwnd);`
3. `source.Content(root_grid);` to set the XAML content tree.
4. Forward our existing event/command surface from the parent HWND
   into the xaml island.

Plausible - but `DesktopWindowXamlSource` from Windows App SDK has
known quirks around focus interop and per-monitor DPI. Allocate
buffer.

## 3 - Direct2D integration

The user explicitly asked about D2D drawing. Two options:

### Option A - `SwapChainPanel` + new `ID2D1DeviceContext` backend

`SwapChainPanel` is the documented seam.
`ISwapChainPanelNative::SetSwapChain` attaches a DXGI swap chain;
the panel sizes/clips the swap chain to the XAML layout rect. To
draw with D2D you go: `ID3D11Device → IDXGIDevice → ID2D1Device →
ID2D1DeviceContext → CreateBitmapFromDxgiSurface(back_buffer)` then
`SetTarget(bitmap)` and the same draw calls work.

**Cost:** the existing backend (`backends/d2d/d2d_backend.cpp`)
hardcodes `ID2D1HwndRenderTarget` and
`D2D1CreateFactory(SINGLE_THREADED)`. Migration is a parallel
rewrite, not an in-place edit, because:
- `HwndRenderTarget` and `DeviceContext` create-target paths differ
  fundamentally;
- DXGI swap chain creation requires a D3D11 device upfront;
- Resize semantics differ (`ResizeBuffers` on the swap chain, then
  recreate the bitmap target, vs. `Resize(SizeU)` on the
  HwndRenderTarget);
- Brushes/text formats live on the `ID2D1DeviceContext` - they need
  recreation when the device is lost (swap chain doesn't survive
  device-removed events; `HwndRenderTarget` does).

The render-API surface (`fill_rect`, `draw_rect`, `draw_text`,
`measure_text`, `push_clip`/`pop_clip`, the path API) translates
cleanly - same `ID2D1` calls under the hood.

**Estimate: 1 week** to bring up an `ID2D1DeviceContext` backend at
parity with the existing one.

### Option B - Reuse the WinUI3 visual tree, no Direct2D

Skip Direct2D entirely; map every widget to a XAML control and rely
on the XAML composition engine. Faster to ship, but loses the
ability to host the existing painted widgets (`KNOB`, future
audio-plugin Drawables) without a `SwapChainPanel`.

**Recommendation: Option A** if we go ahead - the whole point of
neuilib is uniform painted widgets across hosts, and audio-plugin
controls (knob today, Drawables tomorrow) are the differentiator.

### One subtle issue

`SwapChainPanel` does **not** support transparency against XAML
backdrops (`AcrylicBrush`, `CompositionBackdropBrush`). That's
fine for our use case - solid panels - but rules out one of the
"Fluent" reasons someone might want WinUI3 in the first place.

## 4 - Per-widget effort estimate

Notation: **S** = small (½–1 day glue code), **M** = medium (2–4
days), **L** = large (1+ week, design choices), **X** = blocker /
no clean path.

| Widget | WinUI3 control | Effort | Notes |
|---|---|---|---|
| `APPWINDOW` | `Microsoft.UI.Xaml.Window` | M | Window-per-frame, must wire `IWindowNative` to expose underlying HWND for our existing `WM_GETMINMAXINFO`-style clamps. `min/max_width/height` attrs route through `Window.AppWindow.Resize` constraints API. |
| `PLUGWINDOW` | DesktopWindowXamlSource embedded in foreign HWND | L | The hard one. See §2. Probably one full week including DAW-style verification. |
| `DIALOG` | `Microsoft.UI.Xaml.Window` + `IsModal` analog via owner.Disable | M | XAML has `ContentDialog` but it's a flyout over the parent window, not a real dialog window. To match our existing semantics we use `Window` + manual modal disable. |
| `LABEL` | `TextBlock` | S | Trivial. |
| `BUTTON` | `Button` | S | Trivial; `Click` event + `Content` for text. |
| `INPUTBOX` | `TextBox` | S | `SelectionStart` / `SelectionLength` / `Text`. Undo/redo built-in. Clipboard built-in. We lose our `EditHistory` + IME work but XAML's are equally good. |
| `CHECKBOX` | `CheckBox` | S | `IsChecked`. |
| `CHECKBOX3` | `CheckBox IsThreeState=true` | S | `IsChecked` is `bool?`. |
| `LISTBOX` | `ListView` (single-select mode) | M | Data binding from C++/WinRT. Our `items` API maps to `Items` collection or an `ObservableVector<IInspectable>`; no `ItemsSource`-MVVM machinery needed. |
| `COMBOBOX` | `ComboBox` | M | Same data shape as ListBox. Our hover≠selection model needs custom behaviour because XAML ComboBox commits hover-on-click only - actually that's already correct. |
| `MULTILINE` | `TextBox AcceptsReturn=true` (NOT `RichEditBox`) | M | `RichEditBox` is rich-text and has a different `Document` ITextDocument API; `TextBox` with `AcceptsReturn` and `TextWrapping` is the plain-text path. Soft-wrap deferred upstream - XAML wraps automatically, slight semantic mismatch with our explicit-newline-only design. |
| `TREEVIEW` | `Microsoft.UI.Xaml.Controls.TreeView` | L | Exists. Item model is `TreeViewNode` with `Content` + `Children`. We'd build a thin shim translating our `Tree<T>` into `TreeViewNode`s. Disabled-row gray-out (we just added) needs a custom item template - XAML doesn't expose `IsEnabled` per node out of the box. |
| `MENUBAR` | `Microsoft.UI.Xaml.Controls.MenuBar` + `MenuBarItem` + `MenuFlyoutItem` | L | **Core mismatch.** XAML MenuBar is a *visual control* in the layout, not an OS menu. Consequences: (a) no system Alt-key mnemonics out of the box, (b) `HACCEL` doesn't apply - we'd reimplement accelerator dispatch via `KeyboardAccelerator` per item, (c) `set_menu_cmd` routing still works (we'd handle it in our own `Click` handler). Not a blocker but ~1 week of porting the typed-shortcut + auto-validate machinery onto XAML primitives. |
| `IMAGE` | `Image` + `BitmapImage` | S | Trivial. We lose our manual D2D bitmap path on this widget (it goes through XAML), but the painted-widget #3 plan to fold IMAGE into the painted seam still works on the WinUI3 host via `SwapChainPanel`. |
| `SLIDER` | `Slider` | S | `Minimum` / `Maximum` / `StepFrequency` / `TickFrequency`. Tick-drift bug we just fixed doesn't recur - XAML places ticks correctly. |
| `KNOB` | custom on `SwapChainPanel` | M | No XAML rotary control exists. We host a `SwapChainPanel`, drive it with the new D2D-DX backend, and reuse the existing `paint_knob` math verbatim. Pointer events from XAML route into our existing on_mouse_event. |

**Sum: ~7 weeks of widget work** if D2D-DX backend is already in
place. Add 1–2 weeks for build infrastructure + 1 week for the new
D2D backend + 1 week for session/event/focus/clipboard plumbing.
**Realistic total: 10–12 weeks of focused work.**

## 5 - What we lose vs the existing two hosts

- **Painting parity.** WinUI3 controls render via the XAML
  compositor; pixel-for-pixel they will not match the xpl host.
  Multi-host UIs that try to look identical regress.
- **`HACCEL`.** Replaced with XAML `KeyboardAccelerator`.
  Functionally equivalent but a parallel implementation we have to
  maintain.
- **Audio-plugin reliability.** WinUI3's `DesktopWindowXamlSource`
  is not battle-tested in DAW-host scenarios. Crashes, focus bugs,
  multi-instance issues are likely. The xpl host on raw
  `HWND`+D2D is what plugin frameworks actually ship in 2026.
- **Build determinism.** NuGet+MSBuild interop adds non-trivial
  failure modes (missing SDK install, wrong arch, ARM64 vs x64
  mismatch).
- **Single-file installs.** WinUI3 unpackaged apps still need the
  Windows App SDK runtime DLLs alongside the exe.

## 6 - What we'd gain

- **Native UIAutomation accessibility** without the Tier B
  focus-proxy work currently deferred for the xpl host. A real win
  if accessibility certification matters.
- **Fluent Design polish** - Mica/Acrylic backdrops, consistent
  theme transitions, dark-mode tracking. Free with WinUI3.
- **Modern OS controls** - text input behaves identically to the
  rest of Windows 11 (touch keyboard, IME, autocomplete) without us
  maintaining it.
- **Future-proofing** if Microsoft's stated direction holds - WinUI3
  is the recommended stack for new Windows desktop apps.

## 7 - Recommendation

**Defer.**

Concretely: keep the door open, but do not invest. The xpl host
already covers the multi-platform story (macOS port is the next
priority per TODO.md), and the Win32 host covers native-control
fidelity. WinUI3 sits in an awkward middle: not as portable as xpl,
not as tight as Win32, expensive to build, with two architectural
mismatches (`PLUGWINDOW`, `MENUBAR`) against decisions central to
neuilib's audio-plugin focus.

**Re-evaluate when:**
- An accessibility cert (UIAutomation conformance) becomes a
  contract requirement *and* the deferred Tier B focus-proxy work
  proves too expensive in the xpl host. The cost comparison flips
  here.
- Microsoft ships first-class CMake templates for Windows App SDK.
  This eliminates ~half the build cost.
- A user asks specifically for Fluent Design with system theme
  tracking and is OK with desktop-only.

**If the user wants to proceed anyway:** start with build
infrastructure + a hello-world `Window` + `Button`, end-to-end,
linking against `imm32`-equivalent SDK libs. That milestone alone
will surface the build-system rough edges and inform whether to
continue.

## Critical files to reference (for future work)

- `include/neui/d/api.h` - host vtable contract (lines 47-66)
- `include/neui/d/widgets.h` - widget vtable + 16 widget types
- `include/neui/d/renderer.h` - D2D backend interface (must be
  reimplemented over `ID2D1DeviceContext` + DXGI swap chain)
- `backends/d2d/d2d_backend.cpp` - current
  `ID2D1HwndRenderTarget`-based backend; not reusable for
  `SwapChainPanel`
- `hosts/win32/host.cpp` (lines 157-175) - minimal host shape to
  mirror
- `hosts/crossplatform/host.cpp` (lines 3508-3528) -
  `register_host()` + `register_xplhost()` pattern to mirror as
  `register_winui3host()`
- `CMakeLists.txt` (root) and `hosts/win32/CMakeLists.txt` - how
  hosts are wired in today

## Verification

This plan produces a markdown document. The "verification" is the
user reading it and either accepting "defer" or directing
implementation. There is no code to run.

If the user authorises implementation later, end-to-end verification
of any WinUI3 host would be: build `neui_example.exe` linked against
`neui-winui3host` (with `ACTIVE_HOST = "neui.host.winui3"` in
`examples/main.cpp`), run it, walk through every widget in the
example app verifying parity with the existing two hosts. Same
verification protocol used for the IME work earlier this session.
