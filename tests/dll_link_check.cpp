// Linked into a DLL by tests/CMakeLists.txt (neui_dll_link_check): an audio
// plugin links the hosts the same way, and must not need an application's
// main(). Building it is the check; nothing loads it.
#include <neui/neui.h>

extern "C" __declspec(dllexport) int neui_dll_link_check(void)
{
  neui_init();
  return neui_get_api("neui.host.crossplatform") != nullptr;
}
