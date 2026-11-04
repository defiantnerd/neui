// The Win32 host's entry point for a GUI application (the WIN32 subsystem):
// it sets the process up for the host and calls the client's main().
//
// An object of its own, so the linker takes it out of neui-win32host.lib only
// where the C runtime's startup asks for WinMain - in an application. A DLL
// never does: an audio plugin that embeds a frame in its host's window
// (NEUI_API_EMBED) links the host libraries into a DLL, and while WinMain sat
// in window.cpp - which every use of the host pulls in - the DLL failed to
// link on the main() it calls ("unresolved external symbol main referenced in
// function WinMain").
#define NOMINMAX
#include "window.h"
#include <objbase.h>
#include <shellapi.h>   // CommandLineToArgvW (WIN32_LEAN_AND_MEAN leaves it out)

#include <string>
#include <vector>

// The client's main, called from WinMain below. Clients write the standard
// `int main(int, char**)` (MSVC does not mangle main, so a mismatch would
// still link and hand them UTF-16), so it gets UTF-8 arguments.
int main(int argc, char** argv);

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
{
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);  // required for WIC image loading
  win32_host::set_hinstance(hInstance);
  win32_host::register_classes();
  win32_host::register_host();
  int wargc = 0;
  wchar_t** wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
  std::vector<std::string> args;
  for (int i = 0; wargv && i < wargc; ++i) {
    const int wl = static_cast<int>(wcslen(wargv[i]));
    const int n = WideCharToMultiByte(CP_UTF8, 0, wargv[i], wl, nullptr, 0, nullptr, nullptr);
    std::string a(n > 0 ? static_cast<size_t>(n) : 0u, '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, wargv[i], wl, &a[0], n, nullptr, nullptr);
    args.push_back(std::move(a));
  }
  if (wargv) LocalFree(wargv);
  std::vector<char*> argv;
  for (auto& a : args) argv.push_back(&a[0]);
  argv.push_back(nullptr);
  return main(static_cast<int>(args.size()), argv.data());
}
