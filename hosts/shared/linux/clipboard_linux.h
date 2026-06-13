#pragma once

#if defined(__linux__) || (defined(__unix__) && !defined(__APPLE__))

// X11 CLIPBOARD-selection implementation, mirror of the role
// hosts/shared/macos/clipboard_macos.h plays for NSPasteboard.
//
// X11 has no central clipboard store: the *selection owner* holds the bytes
// and answers SelectionRequest events from pasters. So this class needs three
// things the macOS/Win32 helpers don't:
//   1. a dedicated owner window + a DataItem holding what we currently serve,
//   2. a SelectionRequest/SelectionClear handler driven from the platform
//      event loop (handle_event), and
//   3. a short synchronous request-pump for reads (request_*), which waits for
//      the owner's SelectionNotify without freezing the UI (other events stay
//      queued for the main loop).
//
// Scope: the CLIPBOARD selection (Ctrl+C/V semantics; PRIMARY/select-to-paste
// is a later add). Text via UTF8_STRING/STRING; other MIMEs use the MIME
// string verbatim as the X target atom (matches GTK/Qt: "text/html",
// "text/uri-list", "image/png", arbitrary "x/y"). INCR (chunked transfers for
// payloads above the server's max-request size) is not implemented - very
// large clipboard items silently fall back to "no data".

#include <X11/Xlib.h>
#include <X11/Xatom.h>

#include <time.h>
#include <unistd.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "../clipboard_item.h"

namespace neui_detail
{
  class ClipboardX11
  {
  public:
    void init(Display* dpy)
    {
      _dpy = dpy;
      if (!_dpy) return;
      int scr = DefaultScreen(_dpy);
      // Unmapped InputOnly window: owns the selection + receives the
      // SelectionRequest / SelectionNotify / SelectionClear events.
      _owner = XCreateWindow(_dpy, RootWindow(_dpy, scr), -10, -10, 1, 1, 0,
                             0, InputOnly, CopyFromParent, 0, nullptr);

      _a_clipboard = XInternAtom(_dpy, "CLIPBOARD",   False);
      _a_targets   = XInternAtom(_dpy, "TARGETS",     False);
      _a_incr      = XInternAtom(_dpy, "INCR",        False);
      _a_utf8      = XInternAtom(_dpy, "UTF8_STRING", False);
      _a_text      = XInternAtom(_dpy, "TEXT",        False);
      _a_mime_text = XInternAtom(_dpy, "text/plain;charset=utf-8", False);
      _a_recv      = XInternAtom(_dpy, "NEUI_CLIP_RECV", False);
    }

    // --- writers ------------------------------------------------------------

    bool set_text(const char* utf8, uint32_t length)
    {
      if (!_dpy || !utf8) return false;
      _own = DataItem{};
      _own.set_format("text/plain;charset=utf-8", utf8, length);
      return take_ownership();
    }

    bool write_item(const DataItem& item)
    {
      if (!_dpy) return false;
      _own = DataItem{};
      bool any = false;
      item.for_each_format([&](const std::string& mime,
                               const std::vector<uint8_t>& bytes) {
        _own.set_format(mime, bytes.data(), static_cast<uint32_t>(bytes.size()));
        any = true;
      });
      if (!any) return false;
      return take_ownership();
    }

    // --- readers ------------------------------------------------------------

    bool has_text()
    {
      if (!_dpy) return false;
      if (_owns) return have_own_text();
      if (XGetSelectionOwner(_dpy, _a_clipboard) == None) return false;
      std::vector<Atom> tgts;
      if (!request_targets(tgts)) return false;
      for (Atom t : tgts)
        if (t == _a_utf8 || t == XA_STRING || t == _a_text || t == _a_mime_text)
          return true;
      return false;
    }

    // Returns total bytes needed incl. NUL terminator (matches the seam
    // contract). buf=NULL queries size. 0 = no text.
    int get_text(char* buf, int buflen)
    {
      if (!_dpy) return 0;
      std::vector<uint8_t> bytes;
      if (_owns) {
        if (!own_text_bytes(bytes)) return 0;
      } else {
        if (XGetSelectionOwner(_dpy, _a_clipboard) == None) return 0;
        Atom got = None;
        if (!request_target(_a_utf8, bytes, got) || bytes.empty())
          if (!request_target(XA_STRING, bytes, got) || bytes.empty())
            return 0;
      }
      int need = static_cast<int>(bytes.size()) + 1;  // + NUL
      if (buf && buflen > 0) {
        int copy = (buflen - 1 < static_cast<int>(bytes.size()))
                     ? buflen - 1 : static_cast<int>(bytes.size());
        if (copy > 0) std::memcpy(buf, bytes.data(), copy);
        buf[copy] = '\0';
      }
      return need;
    }

    bool read_item(DataItem& out)
    {
      if (!_dpy) return false;
      if (_owns) {  // serve our own copy directly (no self-request deadlock)
        bool any = false;
        _own.for_each_format([&](const std::string& mime,
                                 const std::vector<uint8_t>& b) {
          out.set_format(mime, b.data(), static_cast<uint32_t>(b.size()));
          any = true;
        });
        return any;
      }
      if (XGetSelectionOwner(_dpy, _a_clipboard) == None) return false;
      std::vector<Atom> tgts;
      if (!request_targets(tgts)) return false;

      bool any = false, got_text = false;
      for (Atom t : tgts) {
        // Text: prefer UTF8_STRING, accept STRING; store canonical MIME once.
        if ((t == _a_utf8 || t == XA_STRING || t == _a_text) && !got_text) {
          std::vector<uint8_t> bytes; Atom rt = None;
          if (request_target(t, bytes, rt) && !bytes.empty()) {
            out.set_format("text/plain;charset=utf-8", bytes.data(),
                           static_cast<uint32_t>(bytes.size()));
            got_text = any = true;
          }
          continue;
        }
        // Anything whose atom name looks like a MIME ("x/y") is fetched as-is.
        char* name = XGetAtomName(_dpy, t);
        if (name) {
          std::string mime = name;
          XFree(name);
          if (mime.find('/') != std::string::npos && mime != "text/plain") {
            std::vector<uint8_t> bytes; Atom rt = None;
            if (request_target(t, bytes, rt) && !bytes.empty()) {
              out.set_format(mime, bytes.data(),
                             static_cast<uint32_t>(bytes.size()));
              any = true;
            }
          }
        }
      }
      return any;
    }

    // --- event-loop hook ----------------------------------------------------

    // Returns true if the event was a clipboard selection event for our owner
    // window (and was consumed). Called at the top of the platform's
    // dispatch_x_event.
    bool handle_event(XEvent& ev)
    {
      if (!_dpy) return false;
      switch (ev.type) {
        case SelectionRequest:
          if (ev.xselectionrequest.owner == _owner) {
            serve_request(ev.xselectionrequest);
            return true;
          }
          return false;
        case SelectionClear:
          if (ev.xselectionclear.window == _owner) {
            _owns = false;
            _own = DataItem{};
            return true;
          }
          return false;
        case SelectionNotify:
          // A late notify (e.g. a timed-out request). Swallow if it's ours so
          // it doesn't reach a frame's dispatch.
          return ev.xselection.requestor == _owner;
        default:
          return false;
      }
    }

  private:
    bool take_ownership()
    {
      XSetSelectionOwner(_dpy, _a_clipboard, _owner, CurrentTime);
      _owns = (XGetSelectionOwner(_dpy, _a_clipboard) == _owner);
      XFlush(_dpy);
      return _owns;
    }

    bool have_own_text() const
    {
      return _own.has_format("text/plain;charset=utf-8") ||
             _own.has_format("text/plain");
    }
    bool own_text_bytes(std::vector<uint8_t>& out) const
    {
      const char* keys[2] = { "text/plain;charset=utf-8", "text/plain" };
      for (const char* k : keys) {
        int n = _own.get_format(k, nullptr, 0);
        if (n > 0) { out.resize(n); _own.get_format(k, out.data(), n); return true; }
      }
      return false;
    }
    std::vector<uint8_t> fmt_bytes(const std::string& mime) const
    {
      std::vector<uint8_t> v;
      int n = _own.get_format(mime, nullptr, 0);
      if (n > 0) { v.resize(n); _own.get_format(mime, v.data(), n); }
      return v;
    }

    // Serve a SelectionRequest from `_own`.
    void serve_request(XSelectionRequestEvent& req)
    {
      XSelectionEvent resp;
      resp.type      = SelectionNotify;
      resp.display   = req.display;
      resp.requestor = req.requestor;
      resp.selection = req.selection;
      resp.target    = req.target;
      resp.time      = req.time;
      resp.property  = None;  // None == refused, until we fill it in
      // Obsolete clients send property == None; reply on the target atom.
      Atom prop = req.property ? req.property : req.target;

      if (req.target == _a_targets) {
        std::vector<Atom> tg;
        tg.push_back(_a_targets);
        if (have_own_text()) {
          tg.push_back(_a_utf8);
          tg.push_back(XA_STRING);
          tg.push_back(_a_text);
          tg.push_back(_a_mime_text);
        }
        _own.for_each_mime([&](const std::string& mime) {
          if (mime == "text/plain" || mime == "text/plain;charset=utf-8") return;
          tg.push_back(XInternAtom(_dpy, mime.c_str(), False));
        });
        XChangeProperty(_dpy, req.requestor, prop, XA_ATOM, 32, PropModeReplace,
                        reinterpret_cast<unsigned char*>(tg.data()),
                        static_cast<int>(tg.size()));
        resp.property = prop;
      }
      else if (req.target == _a_utf8 || req.target == XA_STRING ||
               req.target == _a_text || req.target == _a_mime_text) {
        std::vector<uint8_t> bytes;
        if (own_text_bytes(bytes)) {
          XChangeProperty(_dpy, req.requestor, prop, req.target, 8,
                          PropModeReplace, bytes.data(),
                          static_cast<int>(bytes.size()));
          resp.property = prop;
        }
      }
      else {
        char* name = XGetAtomName(_dpy, req.target);
        if (name) {
          std::string mime = name; XFree(name);
          if (_own.has_format(mime)) {
            std::vector<uint8_t> bytes = fmt_bytes(mime);
            XChangeProperty(_dpy, req.requestor, prop, req.target, 8,
                            PropModeReplace, bytes.data(),
                            static_cast<int>(bytes.size()));
            resp.property = prop;
          }
        }
      }

      XSendEvent(_dpy, req.requestor, False, 0, reinterpret_cast<XEvent*>(&resp));
      XFlush(_dpy);
    }

    // Synchronous: ask the current owner to convert `target`, wait for the
    // SelectionNotify, read the property bytes. Leaves unrelated events queued.
    bool request_target(Atom target, std::vector<uint8_t>& out, Atom& type_out)
    {
      out.clear();
      XDeleteProperty(_dpy, _owner, _a_recv);
      XConvertSelection(_dpy, _a_clipboard, target, _a_recv, _owner, CurrentTime);
      XFlush(_dpy);

      Atom prop = None;
      if (!wait_selection_notify(prop, 2000)) return false;
      if (prop == None) return false;   // owner refused this target
      return read_property(prop, out, type_out);
    }

    bool request_targets(std::vector<Atom>& out)
    {
      out.clear();
      std::vector<uint8_t> bytes; Atom type = None;
      if (!request_target(_a_targets, bytes, type)) return false;
      // TARGETS comes back as 32-bit atoms.
      size_t count = bytes.size() / sizeof(Atom);
      const Atom* a = reinterpret_cast<const Atom*>(bytes.data());
      out.assign(a, a + count);
      return !out.empty();
    }

    bool wait_selection_notify(Atom& prop_out, long timeout_ms)
    {
      struct timespec start; clock_gettime(CLOCK_MONOTONIC, &start);
      for (;;) {
        XEvent ev;
        if (XCheckTypedWindowEvent(_dpy, _owner, SelectionNotify, &ev)) {
          prop_out = ev.xselection.property;
          return true;
        }
        struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
        long ms = (now.tv_sec - start.tv_sec) * 1000 +
                  (now.tv_nsec - start.tv_nsec) / 1000000;
        if (ms >= timeout_ms) return false;
        usleep(2000);
      }
    }

    // Read the whole property on our owner window into out (handles the
    // 32-bit length unit; bails on INCR). Deletes the property afterward.
    bool read_property(Atom prop, std::vector<uint8_t>& out, Atom& type_out)
    {
      Atom type = None; int fmt = 0;
      unsigned long nitems = 0, after = 0; unsigned char* data = nullptr;
      // Peek to learn the size + type.
      if (XGetWindowProperty(_dpy, _owner, prop, 0, 0, False, AnyPropertyType,
                             &type, &fmt, &nitems, &after, &data) != Success)
        return false;
      if (data) { XFree(data); data = nullptr; }
      type_out = type;
      if (type == _a_incr) {           // chunked transfer - unsupported
        XDeleteProperty(_dpy, _owner, prop);
        return false;
      }
      long total_bytes = static_cast<long>(after);
      long longs = (total_bytes + 3) / 4;
      if (XGetWindowProperty(_dpy, _owner, prop, 0, longs ? longs : 1, False,
                             AnyPropertyType, &type, &fmt, &nitems, &after,
                             &data) == Success && data) {
        size_t bytes = static_cast<size_t>(nitems) * (fmt / 8);
        out.assign(data, data + bytes);
        XFree(data);
        XDeleteProperty(_dpy, _owner, prop);
        return true;
      }
      XDeleteProperty(_dpy, _owner, prop);
      return false;
    }

    Display* _dpy   = nullptr;
    Window   _owner = 0;
    bool     _owns  = false;   // do we currently own CLIPBOARD?
    DataItem _own;             // what we serve while we own it

    Atom _a_clipboard = None, _a_targets = None, _a_incr = None;
    Atom _a_utf8 = None, _a_text = None, _a_mime_text = None, _a_recv = None;
  };

} // namespace neui_detail

#endif // linux
