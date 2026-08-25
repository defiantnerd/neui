// Acceptance harness for the win32 DAW-embedding path (issue #29).
//
// Plays the role of a DAW: creates a foreign "host parent" HWND, embeds a neui
// PLUGWINDOW under it via the public NEUI_API_EMBED interface, then drives the
// UI purely from the DAW's own message pump (no neui-owned event loop - the
// win32 embedded frame is an ordinary WS_CHILD, so PeekMessage/DispatchMessage
// on this thread already services it).
//
// What it asserts:
//   1. NEUI_API_EMBED is exposed and set_parent accepts the foreign HWND.
//   2. The embedded child HWND is really parented under the DAW window.
//   3. The child's PHYSICAL client size matches the requested LOGICAL size
//      scaled by the frame's DPI - the "create() size is the client area"
//      contract.
//   4. **The first NEUI_EVENT_RESIZE reports LOGICAL pixels** (issue #29). On
//      win32, CreateWindowExW sends WM_SIZE for a WS_CHILD before it returns,
//      i.e. before the caller can read the new HWND's DPI back - so this is
//      the one event most likely to escape with physical pixels in it. A
//      client that lays out from the resize event would push its whole UI off
//      the visible area of an embedded editor.
//
// Checks 3 + 4 only differ from a trivial identity when the display is
// scaled (125% / 150% / ...): at 96 DPI logical == physical. Run it on a
// scaled display to exercise the regression it was written for.
//
// Windows-only; needs a GUI session, so it is built but NOT registered with
// ctest - run ./tests/<config>/neui_embed_smoke_win32 manually.

#include <neui/neui.h>

#include <windows.h>

#include <cstdio>
#include <cstring>

static const int k_plug_w = 940;   // logical, at 96 DPI
static const int k_plug_h = 400;

static int g_resize_count = 0;
static int g_first_w      = 0;
static int g_first_h      = 0;

static bool onevent(void*, neui_event_t* ev)
{
  if (ev && ev->type == NEUI_EVENT_RESIZE) {
    if (g_resize_count == 0) {
      g_first_w = ev->data.resize.width;
      g_first_h = ev->data.resize.height;
    }
    ++g_resize_count;
    std::printf("  RESIZE #%d: %d x %d\n", g_resize_count,
                ev->data.resize.width, ev->data.resize.height);
  }
  return false;
}

static neui_widget_client_t g_wc = { NEUI_VERSION, nullptr, onevent };
static void* iface(void*, const char* n)
{ return std::strcmp(n, NEUI_API_WIDGETS) ? nullptr : (void*)&g_wc; }
static neui_client_t g_client = { NEUI_VERSION, iface };

// A DAW is normally per-monitor-v2 aware and the plugin inherits that context,
// so opt in here too - without it the process is DPI-virtualised and every
// GetDpiForWindow reports 96, which would make the DPI checks vacuous.
static void opt_into_per_monitor_dpi()
{
  using SetCtxFn = BOOL (WINAPI*)(void*);
  HMODULE u32 = GetModuleHandleW(L"user32.dll");
  if (!u32) return;
  auto set_ctx = reinterpret_cast<SetCtxFn>(
    reinterpret_cast<void*>(
      GetProcAddress(u32, "SetProcessDpiAwarenessContext")));
  // -4 == DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
  if (set_ctx) set_ctx(reinterpret_cast<void*>(static_cast<INT_PTR>(-4)));
}

// Pump the DAW's message loop for `ms`, exactly as a plugin host would.
static void daw_pump(DWORD ms)
{
  DWORD end = GetTickCount() + ms;
  for (;;) {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
    if (GetTickCount() >= end) break;
    Sleep(10);
  }
}

int main()
{
  opt_into_per_monitor_dpi();

  // ---- Fake DAW host window -----------------------------------------------
  WNDCLASSEXW wc = {};
  wc.cbSize        = sizeof(wc);
  wc.lpfnWndProc   = DefWindowProcW;
  wc.hInstance     = GetModuleHandleW(nullptr);
  wc.hCursor       = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
  wc.hbrBackground = CreateSolidBrush(RGB(32, 32, 32));
  wc.lpszClassName = L"neui.fake.daw.host";
  RegisterClassExW(&wc);

  UINT sys_dpi = GetDpiForSystem();
  if (sys_dpi == 0) sys_dpi = 96;
  HWND parent = CreateWindowExW(
    0, wc.lpszClassName, L"fake daw host", WS_OVERLAPPEDWINDOW,
    40, 40,
    MulDiv(k_plug_w + 40, static_cast<int>(sys_dpi), 96),
    MulDiv(k_plug_h + 80, static_cast<int>(sys_dpi), 96),
    nullptr, nullptr, wc.hInstance, nullptr);
  if (!parent) { std::printf("FAIL: could not create the fake DAW window\n"); return 1; }
  ShowWindow(parent, SW_SHOWNORMAL);
  UpdateWindow(parent);
  daw_pump(200);

  UINT parent_dpi = GetDpiForWindow(parent);
  if (parent_dpi == 0) parent_dpi = 96;
  std::printf("fake DAW parent HWND=%p, dpi=%u (%.0f%% scaling)\n",
              (void*)parent, parent_dpi, parent_dpi * 100.0 / 96.0);

  // ---- Embed a neui PLUGWINDOW under it -----------------------------------
  neui_init();
  neui_api_t* api = neui_get_api("neui.host.crossplatform");
  if (!api) { std::printf("FAIL: no crossplatform host registered\n"); return 1; }
  neui_session_t sess = api->create_session(&g_client, nullptr);
  auto* w     = (neui_widget_api_t*)api->get_interface(sess, NEUI_API_WIDGETS);
  auto* embed = (neui_embed_api_t*)api->get_interface(sess, NEUI_API_EMBED);
  if (!w)     { std::printf("FAIL: no NEUI_API_WIDGETS\n"); return 1; }
  if (!embed) { std::printf("FAIL: no NEUI_API_EMBED\n"); return 1; }

  neui_widget_t plug = w->create(sess, widget_none, NEUI_W_PLUGWINDOW,
                                  0, 0, k_plug_w, k_plug_h, nullptr);
  neui_widget_t btn  = w->create(sess, plug, NEUI_W_BUTTON, 20, 20, 160, 32, nullptr);
  w->set_text(sess, btn, "Embedded!");

  if (!embed->set_parent(sess, plug, (void*)parent)) {
    std::printf("FAIL: set_parent rejected the DAW HWND\n");
    return 1;
  }

  w->show(sess, plug);   // embedded: never run() / pump_once()
  daw_pump(600);

  int failures = 0;

  // ---- 1. child really parented under the DAW window ----------------------
  HWND child = GetWindow(parent, GW_CHILD);
  if (!child) {
    std::printf("FAIL: no child HWND under the DAW parent\n");
    ++failures;
  } else {
    std::printf("PASS: embedded child HWND=%p under the DAW parent\n", (void*)child);
  }

  // ---- 2. client-area contract: physical client == logical * scale --------
  if (child) {
    RECT rc = {};
    GetClientRect(child, &rc);
    int want_w = MulDiv(k_plug_w, static_cast<int>(parent_dpi), 96);
    int want_h = MulDiv(k_plug_h, static_cast<int>(parent_dpi), 96);
    int got_w  = rc.right - rc.left;
    int got_h  = rc.bottom - rc.top;
    if (got_w != want_w || got_h != want_h) {
      std::printf("FAIL: child client is %dx%d physical, expected %dx%d "
                  "(%dx%d logical at %u dpi)\n",
                  got_w, got_h, want_w, want_h, k_plug_w, k_plug_h, parent_dpi);
      ++failures;
    } else {
      std::printf("PASS: child client is %dx%d physical = %dx%d logical at %u dpi\n",
                  got_w, got_h, k_plug_w, k_plug_h, parent_dpi);
    }
  }

  // ---- 3. issue #29: the first RESIZE must be in LOGICAL pixels -----------
  if (g_resize_count == 0) {
    std::printf("FAIL: no NEUI_EVENT_RESIZE reached the client\n");
    ++failures;
  } else if (g_first_w != k_plug_w || g_first_h != k_plug_h) {
    std::printf("FAIL: first RESIZE reported %dx%d, expected %dx%d logical"
                "%s\n", g_first_w, g_first_h, k_plug_w, k_plug_h,
                (g_first_w == MulDiv(k_plug_w, static_cast<int>(parent_dpi), 96))
                  ? " - those are PHYSICAL pixels (issue #29)" : "");
    ++failures;
  } else {
    std::printf("PASS: first RESIZE reported %dx%d logical\n", g_first_w, g_first_h);
  }

  w->destroy(sess, plug);
  api->destroy(sess);
  DestroyWindow(parent);

  std::printf(failures ? "\nFAILED (%d)\n" : "\nOK\n", failures);
  return failures ? 1 : 0;
}
