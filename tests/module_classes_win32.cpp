// Win32: two DLLs built on neui in one process, as a host loads two plugins
// (or one plugin in two formats). Each must register its window class for its
// own module, with its own window procedure (hosts/shared/win32/
// window_class_win32.h). Registered for the exe, as before, the first DLL's
// class served both: the second's windows ran the first's code, and, once the
// first was unloaded, freed memory.
//
// Usage: neui_module_classes_win32 <path of DLL a> <path of DLL b>
// (tests/module_classes_dll_win32.cpp built twice).
//
// Checks, for a frame each DLL embeds in a window of this harness:
//   1. the frame's class is registered for that DLL (GCLP_HMODULE);
//   2. the class's window procedure is in that DLL;
//   3. with NEUI_WIN32_CLASS_SUFFIX, the class name ends with it;
//   4. with DLL a closed and unloaded, DLL b opens another frame, still of its
//      own class, and that frame handles messages and paints.
// Creates hidden windows only and posts no input.
#include <windows.h>

#include <cstdio>
#include <cwchar>

namespace
{
  using OpenFn  = bool (*)(HWND);
  using CloseFn = void (*)();

  struct Dll
  {
    const char* name   = nullptr;
    HMODULE     module = nullptr;
    OpenFn      open   = nullptr;
    CloseFn     close  = nullptr;
  };

  int g_failures = 0;

  void check(bool ok, const char* what, const char* dll)
  {
    std::printf("%s: %s (%s)\n", ok ? "PASS" : "FAIL", what, dll);
    if (!ok) ++g_failures;
  }

  void pump()
  {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
  }

  bool load(Dll& dll, const char* path)
  {
    dll.name   = path;
    dll.module = LoadLibraryA(path);
    if (!dll.module) return false;
    dll.open  = reinterpret_cast<OpenFn>(
      reinterpret_cast<void*>(GetProcAddress(dll.module, "neui_module_classes_open")));
    dll.close = reinterpret_cast<CloseFn>(
      reinterpret_cast<void*>(GetProcAddress(dll.module, "neui_module_classes_close")));
    return dll.open && dll.close;
  }

  HWND host_window()
  {
    static const wchar_t* k_class = L"neui.module.classes.host";
    static bool registered = false;
    if (!registered) {
      WNDCLASSEXW wc = {};
      wc.cbSize        = sizeof(wc);
      wc.lpfnWndProc   = DefWindowProcW;
      wc.hInstance     = GetModuleHandleW(nullptr);
      wc.lpszClassName = k_class;
      registered = RegisterClassExW(&wc) != 0;
    }
    return CreateWindowExW(0, k_class, L"host", WS_OVERLAPPEDWINDOW, 0, 0, 300, 200,
                           nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
  }

  HMODULE module_of(const void* address)
  {
    HMODULE m = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                         GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       static_cast<LPCWSTR>(address), &m);
    return m;
  }

  // Opens a frame of `dll` in a new host window and checks its class (1-3).
  // Returns the host window, or null when no frame opened.
  HWND open_and_check(const Dll& dll)
  {
    HWND host = host_window();
    if (!host || !dll.open(host)) {
      check(false, "the DLL opens an embedded frame", dll.name);
      return host;
    }
    pump();
    HWND frame = GetWindow(host, GW_CHILD);
    check(frame != nullptr, "the frame is the host window's child", dll.name);
    if (!frame) return host;

    const auto class_module =
      reinterpret_cast<HMODULE>(GetClassLongPtrW(frame, GCLP_HMODULE));
    check(class_module == dll.module, "the frame's class is registered for its DLL", dll.name);

    const auto proc = reinterpret_cast<const void*>(GetClassLongPtrW(frame, GCLP_WNDPROC));
    check(module_of(proc) == dll.module, "the class's window procedure is in its DLL", dll.name);

#if defined(NEUI_WIN32_CLASS_SUFFIX)
    wchar_t name[257] = {};
    GetClassNameW(frame, name, 257);
    const wchar_t suffix[] = L"" NEUI_WIN32_CLASS_SUFFIX;
    const size_t  n = std::wcslen(name), s = std::wcslen(suffix);
    check(n > s && std::wcscmp(name + n - s, suffix) == 0,
          "the class name ends with NEUI_WIN32_CLASS_SUFFIX", dll.name);
#endif
    return host;
  }
}  // namespace

int main(int argc, char** argv)
{
  if (argc < 3) {
    std::printf("usage: %s <DLL a> <DLL b>\n", argv[0]);
    return 2;
  }
  Dll a, b;
  if (!load(a, argv[1]) || !load(b, argv[2])) {
    std::printf("FAIL: could not load %s and %s\n", argv[1], argv[2]);
    return 1;
  }

  HWND host_a = open_and_check(a);
  HWND host_b = open_and_check(b);

  // 4. A gone: B's windows must not need anything of A's.
  b.close();
  DestroyWindow(host_b);
  a.close();
  DestroyWindow(host_a);
  pump();
  check(FreeLibrary(a.module) != 0, "DLL a is freed", a.name);
  a.module = nullptr;
  // Still mapped if something pinned it; then 4 proves less, but nothing wrong.
  std::printf("INFO: DLL a is %s\n", GetModuleHandleA(a.name) ? "still mapped" : "unloaded");

  HWND host = open_and_check(b);
  if (HWND frame = host ? GetWindow(host, GW_CHILD) : nullptr) {
    SendMessageW(frame, WM_NULL, 0, 0);
    RedrawWindow(frame, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
    pump();
    check(true, "with DLL a unloaded, DLL b's frame handles messages and paints", b.name);
  }
  b.close();
  if (host) DestroyWindow(host);
  pump();

  std::printf(g_failures ? "\nFAILED (%d)\n" : "\nOK\n", g_failures);
  return g_failures ? 1 : 0;
}
