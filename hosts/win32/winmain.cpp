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

int main(int argc, wchar_t** argv);

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
{
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);  // required for WIC image loading
  win32_host::set_hinstance(hInstance);
  win32_host::register_classes();
  win32_host::register_host();
  int argc;
  wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  return main(argc, argv);
}
