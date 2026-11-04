#pragma once

#if defined(__linux__) || (defined(__unix__) && !defined(__APPLE__))

#include "../../../include/neui/d/notify.h"
#include "../file_dialog_model.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// XDG desktop portal (org.freedesktop.portal.FileChooser) behind
// NEUI_API_NOTIFY::open_file / save_file on Linux.
//
// This is the OPTIONAL native path. It is tried first so a user on a normal
// desktop gets their own file chooser (with bookmarks, recent files, and
// sandbox-safe access), and it falls through to the neui-drawn browser in
// platform_linux.cpp on ANY failure - no libdbus at build time, no session
// bus at run time, no portal implementation installed, a malformed reply, or
// a timeout. What stops the fall-through is an answer from the portal itself,
// which is why the return is a tri-state rather than a count: re-opening a
// second dialog after someone pressed Cancel would be worse than having no
// portal support at all.
//
// "An answer" covers BOTH documented Response codes 1 (user cancelled) and 2
// (the portal failed for its own reasons) - see the wait loop for why, and for
// the case that trade-off gets wrong: a backend that answers 2 *before* ever
// putting a chooser on screen yields zero dialogs and a "cancelled" the user
// never performed. The alternative (falling through on 2) risks a second
// dialog stacked on a dismissed first, which is the more visible failure, so
// this is a judgement call rather than a clear win either way.
//
// The reply is asynchronous: the method call returns a Request object path
// and the answer arrives later as a Response signal on it. To avoid the race
// where the signal lands before the match rule is installed, we pass our own
// handle_token and precompute the object path (the documented approach), so
// the match rule is in place before the call goes out.
//
// The connection is PRIVATE (dbus_bus_get_private) rather than the shared
// session bus used by theme_provider_linux.h: a signal filter on the shared
// connection would have to coexist with the theme filter and outlive this
// call. A private connection closed at the end has neither problem.
//
// `pump` is called on every wait iteration so the caller can keep its own
// windows repainting and its X queue drained while the portal dialog (a
// different process) is up.

namespace neui_detail
{
  enum class PortalResult { unavailable, cancelled, ok };

  // Decode "file:///a/b%20c" to "/a/b c". Returns "" for a non-file URI -
  // the portal can hand back other schemes, and a client that asked for a
  // path cannot do anything with those.
  inline std::string portal_uri_to_path(const char* uri)
  {
    if (!uri) return std::string();
    const char* kFile = "file://";
    const size_t n = std::strlen(kFile);
    if (std::strncmp(uri, kFile, n) != 0) return std::string();
    const char* p = uri + n;
    std::string out;
    while (*p) {
      if (*p == '%' && p[1] && p[2]) {
        auto hex = [](char c) -> int {
          if (c >= '0' && c <= '9') return c - '0';
          if (c >= 'a' && c <= 'f') return c - 'a' + 10;
          if (c >= 'A' && c <= 'F') return c - 'A' + 10;
          return -1;
        };
        int hi = hex(p[1]), lo = hex(p[2]);
        if (hi >= 0 && lo >= 0) {
          out += static_cast<char>(hi * 16 + lo);
          p += 3;
          continue;
        }
      }
      out += *p++;
    }
    return out;
  }

} // namespace neui_detail

#ifdef NEUI_HAS_DBUS

#include <dbus/dbus.h>

namespace neui_detail
{
  // ---- small helpers for the a{sv} options dict ----------------------------

  inline void portal_dict_add_bool(DBusMessageIter* dict, const char* key, bool v)
  {
    DBusMessageIter e, var;
    dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, nullptr, &e);
    dbus_message_iter_append_basic(&e, DBUS_TYPE_STRING, &key);
    dbus_message_iter_open_container(&e, DBUS_TYPE_VARIANT, "b", &var);
    dbus_bool_t b = v ? TRUE : FALSE;
    dbus_message_iter_append_basic(&var, DBUS_TYPE_BOOLEAN, &b);
    dbus_message_iter_close_container(&e, &var);
    dbus_message_iter_close_container(dict, &e);
  }

  inline void portal_dict_add_string(DBusMessageIter* dict, const char* key,
                                     const char* v)
  {
    DBusMessageIter e, var;
    dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, nullptr, &e);
    dbus_message_iter_append_basic(&e, DBUS_TYPE_STRING, &key);
    dbus_message_iter_open_container(&e, DBUS_TYPE_VARIANT, "s", &var);
    dbus_message_iter_append_basic(&var, DBUS_TYPE_STRING, &v);
    dbus_message_iter_close_container(&e, &var);
    dbus_message_iter_close_container(dict, &e);
  }

  // current_folder / current_file are `ay`: raw bytes INCLUDING a trailing
  // NUL (the portal spec is explicit about the terminator).
  inline void portal_dict_add_path_bytes(DBusMessageIter* dict, const char* key,
                                         const std::string& path)
  {
    DBusMessageIter e, var, arr;
    dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, nullptr, &e);
    dbus_message_iter_append_basic(&e, DBUS_TYPE_STRING, &key);
    dbus_message_iter_open_container(&e, DBUS_TYPE_VARIANT, "ay", &var);
    dbus_message_iter_open_container(&var, DBUS_TYPE_ARRAY, "y", &arr);
    for (size_t i = 0; i <= path.size(); ++i) {   // <= : include the NUL
      unsigned char b = static_cast<unsigned char>(i < path.size() ? path[i] : '\0');
      dbus_message_iter_append_basic(&arr, DBUS_TYPE_BYTE, &b);
    }
    dbus_message_iter_close_container(&var, &arr);
    dbus_message_iter_close_container(&e, &var);
    dbus_message_iter_close_container(dict, &e);
  }

  // One filter: (s, a(us)) - label plus (kind, value) pairs where kind 0 is
  // a glob pattern and kind 1 a MIME type. neui filters are globs.
  inline void portal_append_one_filter(DBusMessageIter* parent,
                                       const FileFilter& f)
  {
    DBusMessageIter st, arr;
    dbus_message_iter_open_container(parent, DBUS_TYPE_STRUCT, nullptr, &st);
    const char* label = f.label.c_str();
    dbus_message_iter_append_basic(&st, DBUS_TYPE_STRING, &label);
    dbus_message_iter_open_container(&st, DBUS_TYPE_ARRAY, "(us)", &arr);
    for (const auto& pat : f.patterns) {
      DBusMessageIter pst;
      dbus_message_iter_open_container(&arr, DBUS_TYPE_STRUCT, nullptr, &pst);
      dbus_uint32_t kind = 0;   // 0 = glob
      const char* v = pat.c_str();
      dbus_message_iter_append_basic(&pst, DBUS_TYPE_UINT32, &kind);
      dbus_message_iter_append_basic(&pst, DBUS_TYPE_STRING, &v);
      dbus_message_iter_close_container(&arr, &pst);
    }
    dbus_message_iter_close_container(&st, &arr);
    dbus_message_iter_close_container(parent, &st);
  }

  inline void portal_dict_add_filters(DBusMessageIter* dict,
                                      const std::vector<FileFilter>& filters)
  {
    if (filters.empty()) return;
    DBusMessageIter e, var, arr;
    const char* key = "filters";
    dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, nullptr, &e);
    dbus_message_iter_append_basic(&e, DBUS_TYPE_STRING, &key);
    dbus_message_iter_open_container(&e, DBUS_TYPE_VARIANT, "a(sa(us))", &var);
    dbus_message_iter_open_container(&var, DBUS_TYPE_ARRAY, "(sa(us))", &arr);
    for (const auto& f : filters) portal_append_one_filter(&arr, f);
    dbus_message_iter_close_container(&var, &arr);
    dbus_message_iter_close_container(&e, &var);
    dbus_message_iter_close_container(dict, &e);
  }

  inline void portal_dict_add_current_filter(DBusMessageIter* dict,
                                             const FileFilter& f)
  {
    DBusMessageIter e, var;
    const char* key = "current_filter";
    dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, nullptr, &e);
    dbus_message_iter_append_basic(&e, DBUS_TYPE_STRING, &key);
    dbus_message_iter_open_container(&e, DBUS_TYPE_VARIANT, "(sa(us))", &var);
    portal_append_one_filter(&var, f);
    dbus_message_iter_close_container(&e, &var);
    dbus_message_iter_close_container(dict, &e);
  }

  // ---- reply parsing -------------------------------------------------------

  // Pull `uris` (as) out of the Response signal's a{sv} results dict.
  inline void portal_extract_uris(DBusMessageIter* results,
                                  std::vector<std::string>& out)
  {
    if (dbus_message_iter_get_arg_type(results) != DBUS_TYPE_ARRAY) return;
    DBusMessageIter dict;
    dbus_message_iter_recurse(results, &dict);
    while (dbus_message_iter_get_arg_type(&dict) == DBUS_TYPE_DICT_ENTRY) {
      DBusMessageIter e;
      dbus_message_iter_recurse(&dict, &e);
      const char* key = nullptr;
      if (dbus_message_iter_get_arg_type(&e) == DBUS_TYPE_STRING)
        dbus_message_iter_get_basic(&e, &key);
      dbus_message_iter_next(&e);
      if (key && std::strcmp(key, "uris") == 0 &&
          dbus_message_iter_get_arg_type(&e) == DBUS_TYPE_VARIANT) {
        DBusMessageIter var;
        dbus_message_iter_recurse(&e, &var);
        if (dbus_message_iter_get_arg_type(&var) == DBUS_TYPE_ARRAY) {
          DBusMessageIter list;
          dbus_message_iter_recurse(&var, &list);
          while (dbus_message_iter_get_arg_type(&list) == DBUS_TYPE_STRING) {
            const char* uri = nullptr;
            dbus_message_iter_get_basic(&list, &uri);
            std::string p = portal_uri_to_path(uri);
            if (!p.empty()) out.push_back(p);
            dbus_message_iter_next(&list);
          }
        }
      }
      dbus_message_iter_next(&dict);
    }
  }

  // Turn our unique bus name into the sender fragment the portal uses when
  // building a Request path: ":1.42" -> "1_42".
  inline std::string portal_sender_token(const char* unique_name)
  {
    std::string s = unique_name ? unique_name : "";
    if (!s.empty() && s[0] == ':') s.erase(0, 1);
    for (char& c : s)
      if (c == '.') c = '_';
    return s;
  }

  // ---- the call ------------------------------------------------------------

  inline PortalResult file_dialog_portal(bool save,
                                        const std::string& parent_window_handle,
                                        const neui_file_dialog_t* desc,
                                        std::vector<std::string>& out,
                                        void (*pump)(void*), void* pump_user)
  {
    DBusError err;
    dbus_error_init(&err);
    DBusConnection* conn = dbus_bus_get_private(DBUS_BUS_SESSION, &err);
    if (dbus_error_is_set(&err)) dbus_error_free(&err);
    if (!conn) return PortalResult::unavailable;
    dbus_connection_set_exit_on_disconnect(conn, FALSE);

    // Scope guard: every early return below must close the private bus.
    struct Closer {
      DBusConnection* c;
      ~Closer() { if (c) { dbus_connection_close(c); dbus_connection_unref(c); } }
    } closer{ conn };

    // Precompute the Request path so the match rule beats the signal.
    static unsigned s_token_counter = 0;
    char token[64];
    std::snprintf(token, sizeof token, "neui%u", ++s_token_counter);
    std::string sender = portal_sender_token(dbus_bus_get_unique_name(conn));
    if (sender.empty()) return PortalResult::unavailable;
    std::string request_path =
      "/org/freedesktop/portal/desktop/request/" + sender + "/" + token;

    std::string rule =
      "type='signal',interface='org.freedesktop.portal.Request',"
      "member='Response',path='" + request_path + "'";
    dbus_error_init(&err);
    dbus_bus_add_match(conn, rule.c_str(), &err);
    if (dbus_error_is_set(&err)) { dbus_error_free(&err); return PortalResult::unavailable; }
    dbus_connection_flush(conn);

    DBusMessage* msg = dbus_message_new_method_call(
        "org.freedesktop.portal.Desktop",
        "/org/freedesktop/portal/desktop",
        "org.freedesktop.portal.FileChooser",
        save ? "SaveFile" : "OpenFile");
    if (!msg) return PortalResult::unavailable;

    const uint32_t flags = desc ? desc->flags : 0u;
    std::vector<FileFilter> filters = parse_filters(desc);

    const char* parent = parent_window_handle.c_str();
    const char* title  = (desc && desc->title && *desc->title)
                         ? desc->title
                         : (save ? "Save File" : "Open File");
    DBusMessageIter args;
    dbus_message_iter_init_append(msg, &args);
    dbus_message_iter_append_basic(&args, DBUS_TYPE_STRING, &parent);
    dbus_message_iter_append_basic(&args, DBUS_TYPE_STRING, &title);

    DBusMessageIter dict;
    dbus_message_iter_open_container(&args, DBUS_TYPE_ARRAY, "{sv}", &dict);
    {
      const char* tok = token;
      portal_dict_add_string(&dict, "handle_token", tok);
      if (!save) {
        if (flags & NEUI_FD_MULTISELECT) portal_dict_add_bool(&dict, "multiple", true);
        if (flags & NEUI_FD_DIRECTORY)   portal_dict_add_bool(&dict, "directory", true);
      }
      // A directory picker has nothing to filter by extension.
      const bool want_filters = !(flags & NEUI_FD_DIRECTORY) && !filters.empty();
      if (want_filters) {
        portal_dict_add_filters(&dict, filters);
        portal_dict_add_current_filter(
            &dict, filters[clamp_default_filter(desc, filters)]);
      }
      if (desc && desc->initial_dir && *desc->initial_dir)
        portal_dict_add_path_bytes(&dict, "current_folder", desc->initial_dir);
      if (save && desc && desc->initial_name && *desc->initial_name)
        portal_dict_add_string(&dict, "current_name", desc->initial_name);
    }
    dbus_message_iter_close_container(&args, &dict);

    dbus_error_init(&err);
    DBusMessage* reply =
      dbus_connection_send_with_reply_and_block(conn, msg, 3000, &err);
    dbus_message_unref(msg);
    if (dbus_error_is_set(&err) || !reply) {
      // No portal implementation on the bus (or it refused) -> fall through
      // to the neui-drawn browser.
      if (dbus_error_is_set(&err)) dbus_error_free(&err);
      if (reply) dbus_message_unref(reply);
      return PortalResult::unavailable;
    }
    // The returned handle SHOULD equal request_path. A portal old enough to
    // ignore handle_token returns a different one, and our match rule would
    // never fire - detect that and fall through rather than hang.
    {
      const char* handle = nullptr;
      DBusMessageIter it;
      if (dbus_message_iter_init(reply, &it) &&
          dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_OBJECT_PATH)
        dbus_message_iter_get_basic(&it, &handle);
      bool matches = handle && request_path == handle;
      dbus_message_unref(reply);
      if (!matches) return PortalResult::unavailable;
    }

    // Wait for Response(u response, a{sv} results). 0 = ok, 1 = cancelled,
    // 2 = other error. The timeout is generous because a human is choosing a
    // file; it exists only so a portal that dies mid-dialog cannot hang the
    // app forever.
    const int  kPollMs      = 50;
    const long kTimeoutIter = (5 * 60 * 1000) / kPollMs;   // ~5 minutes
    for (long i = 0; i < kTimeoutIter; ++i) {
      if (pump) pump(pump_user);
      if (!dbus_connection_read_write(conn, kPollMs))
        return PortalResult::unavailable;          // bus died mid-dialog
      while (DBusMessage* m = dbus_connection_pop_message(conn)) {
        if (!dbus_message_is_signal(m, "org.freedesktop.portal.Request",
                                    "Response")) {
          dbus_message_unref(m);
          continue;
        }
        // Response(u response, a{sv} results)
        dbus_uint32_t  response = 2;
        DBusMessageIter it;
        bool have = dbus_message_iter_init(m, &it) &&
                    dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_UINT32;
        if (have) {
          dbus_message_iter_get_basic(&it, &response);
          if (response == 0) {
            dbus_message_iter_next(&it);
            portal_extract_uris(&it, out);
          }
        }
        dbus_message_unref(m);
        if (!have) return PortalResult::unavailable;   // malformed reply
        // response 1 = user cancelled, 2 = the portal itself failed. Both
        // report `cancelled` rather than `unavailable`, on the assumption that
        // a portal far enough along to answer had already shown its chooser -
        // so falling through would stack a second dialog on a dismissed first.
        // The spec does NOT guarantee that for code 2, so a backend that fails
        // before display gives the user no dialog and the client a "cancelled"
        // nobody performed. Chosen anyway: the failure mode is quiet, whereas
        // two stacked dialogs is not. Recorded in docs/deferred-issues.md.
        if (response != 0) return PortalResult::cancelled;
        // A "success" that yielded no usable path (every URI a non-file
        // scheme) is indistinguishable from a cancel to the caller.
        return out.empty() ? PortalResult::cancelled : PortalResult::ok;
      }
    }
    // Timed out waiting on a portal that never answered. Treat as
    // unavailable: nothing was picked, and the fallback is better than
    // silently returning "cancelled" for a dialog the user may never have
    // seen.
    return PortalResult::unavailable;
  }

} // namespace neui_detail

#else   // !NEUI_HAS_DBUS

namespace neui_detail
{
  // No libdbus at build time: the portal path does not exist and every call
  // goes straight to the neui-drawn browser.
  inline PortalResult file_dialog_portal(bool /*save*/,
                                        const std::string& /*parent*/,
                                        const neui_file_dialog_t* /*desc*/,
                                        std::vector<std::string>& /*out*/,
                                        void (* /*pump*/)(void*), void* /*pump_user*/)
  {
    return PortalResult::unavailable;
  }
}

#endif  // NEUI_HAS_DBUS

#endif  // linux
