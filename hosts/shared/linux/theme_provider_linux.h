#pragma once

#if defined(__linux__) || (defined(__unix__) && !defined(__APPLE__))

// Linux system dark/light tracking via the XDG desktop portal's
// `org.freedesktop.appearance` / `color-scheme` setting (the cross-desktop
// standard GNOME / KDE / etc. expose). Mirrors theme_provider_win32.h /
// _macos.h: on init it reads the current scheme into the process-wide palette
// (mutable_current_palette), and a D-Bus signal filter updates it live and
// calls broadcast_theme_change() so each Session::on_theme_changed repaints.
//
// D-Bus is an OPTIONAL dependency: with libdbus-1 (NEUI_HAS_DBUS) this tracks
// the system theme; without it the whole thing compiles to no-ops and the host
// keeps its built-in default palette. The platform layer integrates
// theme_dbus_fd() into its select() loop and calls theme_dbus_dispatch() when
// it is readable.

#include "../theme_palette.h"

#ifdef NEUI_HAS_DBUS
#include <dbus/dbus.h>
#include <cstdint>
#include <string>
#endif

namespace neui_detail
{
#ifdef NEUI_HAS_DBUS

  inline DBusConnection*& theme_dbus_conn()
  {
    static DBusConnection* c = nullptr;
    return c;
  }

  // Map the portal color-scheme (0 = no preference, 1 = prefer dark,
  // 2 = prefer light) onto the process palette. "No preference" keeps neui's
  // historical default (dark) so existing apps don't flip unexpectedly.
  inline void theme_apply_color_scheme(uint32_t scheme)
  {
    bool dark = (scheme != 2);   // light only when explicitly "prefer light"
    Palette& p = mutable_current_palette();
    uint32_t prev_version = p.version;
    p = dark ? default_dark_palette() : default_light_palette();
    p.version = prev_version + 1;   // bump so caches / sessions notice the change
  }

  // Recurse through nested VARIANT containers to the first UINT32 (the portal
  // double-wraps the value: Read returns v(v(u)) on some versions, v(u) on
  // others). Returns true + the value if found.
  inline bool theme_iter_to_u32(DBusMessageIter* it, uint32_t* out)
  {
    int t = dbus_message_iter_get_arg_type(it);
    if (t == DBUS_TYPE_VARIANT) {
      DBusMessageIter sub;
      dbus_message_iter_recurse(it, &sub);
      return theme_iter_to_u32(&sub, out);
    }
    if (t == DBUS_TYPE_UINT32) {
      dbus_uint32_t v = 0;
      dbus_message_iter_get_basic(it, &v);
      *out = static_cast<uint32_t>(v);
      return true;
    }
    return false;
  }

  // Synchronous portal read of org.freedesktop.appearance/color-scheme.
  // Returns the scheme (0/1/2), or 0 if unavailable.
  inline uint32_t theme_query_color_scheme(DBusConnection* conn)
  {
    if (!conn) return 0;
    DBusMessage* msg = dbus_message_new_method_call(
        "org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop",
        "org.freedesktop.portal.Settings", "Read");
    if (!msg) return 0;
    const char* ns  = "org.freedesktop.appearance";
    const char* key = "color-scheme";
    dbus_message_append_args(msg, DBUS_TYPE_STRING, &ns,
                             DBUS_TYPE_STRING, &key, DBUS_TYPE_INVALID);

    DBusError err; dbus_error_init(&err);
    DBusMessage* reply =
        dbus_connection_send_with_reply_and_block(conn, msg, 1000, &err);
    dbus_message_unref(msg);
    uint32_t scheme = 0;
    if (reply) {
      DBusMessageIter it;
      if (dbus_message_iter_init(reply, &it))
        theme_iter_to_u32(&it, &scheme);
      dbus_message_unref(reply);
    }
    if (dbus_error_is_set(&err)) dbus_error_free(&err);
    return scheme;
  }

  // Filter for the portal's SettingChanged(s namespace, s key, v value) signal.
  inline DBusHandlerResult theme_signal_filter(DBusConnection*, DBusMessage* msg, void*)
  {
    if (!dbus_message_is_signal(msg, "org.freedesktop.portal.Settings",
                                "SettingChanged"))
      return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    DBusMessageIter it;
    if (!dbus_message_iter_init(msg, &it)) return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    const char* ns = nullptr;
    if (dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_STRING)
      return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    dbus_message_iter_get_basic(&it, &ns);
    dbus_message_iter_next(&it);
    const char* key = nullptr;
    if (dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_STRING)
      return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    dbus_message_iter_get_basic(&it, &key);
    dbus_message_iter_next(&it);
    if (ns && key &&
        std::string(ns) == "org.freedesktop.appearance" &&
        std::string(key) == "color-scheme") {
      uint32_t scheme = 0;
      if (theme_iter_to_u32(&it, &scheme)) {
        theme_apply_color_scheme(scheme);
        broadcast_theme_change();
      }
    }
    return DBUS_HANDLER_RESULT_HANDLED;
  }

  // Connect to the session bus once, read the initial scheme, and subscribe to
  // SettingChanged. Safe to call repeatedly (no-op after the first).
  inline void ensure_theme_provider_linux()
  {
    static bool done = false;
    if (done) return;
    done = true;

    DBusError err; dbus_error_init(&err);
    DBusConnection* conn = dbus_bus_get(DBUS_BUS_SESSION, &err);
    if (dbus_error_is_set(&err)) dbus_error_free(&err);
    if (!conn) return;
    dbus_connection_set_exit_on_disconnect(conn, FALSE);
    theme_dbus_conn() = conn;

    theme_apply_color_scheme(theme_query_color_scheme(conn));

    dbus_bus_add_match(conn,
        "type='signal',interface='org.freedesktop.portal.Settings',"
        "member='SettingChanged'", &err);
    if (dbus_error_is_set(&err)) dbus_error_free(&err);
    dbus_connection_add_filter(conn, theme_signal_filter, nullptr, nullptr);
    dbus_connection_flush(conn);
  }

  // The session-bus socket fd, for the platform select() loop (-1 if absent).
  inline int theme_dbus_fd()
  {
    DBusConnection* c = theme_dbus_conn();
    int fd = -1;
    if (c) dbus_connection_get_unix_fd(c, &fd);
    return fd;
  }

  // Drain + dispatch pending D-Bus messages (drives theme_signal_filter).
  inline void theme_dbus_dispatch()
  {
    DBusConnection* c = theme_dbus_conn();
    if (!c) return;
    dbus_connection_read_write(c, 0);
    while (dbus_connection_dispatch(c) == DBUS_DISPATCH_DATA_REMAINS) {}
  }

#else   // !NEUI_HAS_DBUS - no system-theme tracking, keep the default palette.

  inline void ensure_theme_provider_linux() {}
  inline int  theme_dbus_fd() { return -1; }
  inline void theme_dbus_dispatch() {}

#endif  // NEUI_HAS_DBUS
} // namespace neui_detail

#endif  // linux
