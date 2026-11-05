// Linked into two DLLs by tests/CMakeLists.txt (neui_module_classes_a and
// neui_module_classes_b), as a host loads two plugins built on neui, or one
// plugin in two formats. Each opens a frame embedded in a window of the
// harness (tests/module_classes_win32.cpp), which checks whose window class
// the frame got.
#include <neui/neui.h>

#include <windows.h>

#include <cstring>

namespace
{
  bool onevent(void*, neui_event_t*) { return false; }
  neui_widget_client_t g_widget_client = { NEUI_VERSION, nullptr, onevent };
  void* iface(void*, const char* name)
  {
    return std::strcmp(name, NEUI_API_WIDGETS) ? nullptr : static_cast<void*>(&g_widget_client);
  }
  neui_client_t g_client = { NEUI_VERSION, iface };

  neui_api_t*    g_api     = nullptr;
  neui_session_t g_session = {};
  bool           g_open    = false;
}  // namespace

// Opens a 200 x 100 frame embedded in `parent`; the frame's window is
// `parent`'s child. False on failure.
extern "C" __declspec(dllexport) bool neui_module_classes_open(HWND parent)
{
  neui_init();
  g_api = neui_get_api("neui.host.crossplatform");
  if (!g_api) return false;
  g_session  = g_api->create_session(&g_client, nullptr);
  g_open     = true;
  auto* w     = static_cast<neui_widget_api_t*>(g_api->get_interface(g_session, NEUI_API_WIDGETS));
  auto* embed = static_cast<neui_embed_api_t*>(g_api->get_interface(g_session, NEUI_API_EMBED));
  if (!w || !embed) return false;
  neui_widget_t frame = w->create(g_session, widget_none, NEUI_W_PLUGWINDOW, 0, 0, 200, 100, nullptr);
  if (!embed->set_parent(g_session, frame, parent)) return false;
  w->show(g_session, frame);
  return true;
}

// Closes the frame and the session.
extern "C" __declspec(dllexport) void neui_module_classes_close()
{
  if (g_api && g_open) g_api->destroy(g_session);
  g_open = false;
}
