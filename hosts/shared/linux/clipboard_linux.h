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
// Scope: the CLIPBOARD selection (Ctrl+C/V semantics) AND the PRIMARY
// selection (select-to-copy / middle-click-paste, text-only - set_primary_text
// / get_primary_text). Text via UTF8_STRING/STRING; other MIMEs use the MIME
// string verbatim as the X target atom (matches GTK/Qt: "text/html",
// "text/uri-list", "image/png", arbitrary "x/y"). INCR (chunked transfers for
// payloads above the server's max-request size) is implemented both ways:
// serve_bytes streams large served payloads, and read_property/read_incr
// reassemble large incoming ones.

#include <X11/Xlib.h>
#include <X11/Xatom.h>

#include <time.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
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

      // PropertyNotify on the owner window drives the INCR receive pump, and on
      // a requestor window the INCR send pump.
      XSelectInput(_dpy, _owner, PropertyChangeMask);

      // Largest single-property payload we send before switching to INCR
      // (chunked) transfers - bounded by the server's max request size, with
      // headroom for the request header and capped so a chunk fits comfortably.
      long maxreq = XMaxRequestSize(_dpy);
      _max_chunk = (maxreq > 0) ? static_cast<size_t>(maxreq) * 4 : 65536;
      if (_max_chunk > 262144) _max_chunk = 262144;
      if (_max_chunk > 4096)   _max_chunk -= 4096;
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

    // Copy `bytes` into the caller buffer per the get_text/get_primary_text
    // seam contract: returns total bytes needed incl. NUL; buf=NULL queries
    // size; the copy is clamped to buflen-1 and always NUL-terminated.
    static int fill_text_out(const std::vector<uint8_t>& bytes,
                             char* buf, int buflen)
    {
      int need = static_cast<int>(bytes.size()) + 1;  // + NUL
      if (buf && buflen > 0) {
        int copy = (buflen - 1 < static_cast<int>(bytes.size()))
                     ? buflen - 1 : static_cast<int>(bytes.size());
        if (copy > 0) std::memcpy(buf, bytes.data(), copy);
        buf[copy] = '\0';
      }
      return need;
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
      return fill_text_out(bytes, buf, buflen);
    }

    // --- PRIMARY selection (text-only: select-to-copy / middle-click-paste) -

    // Own PRIMARY with the given text (call when a text selection is made).
    bool set_primary_text(const char* utf8, uint32_t length)
    {
      if (!_dpy || !utf8) return false;
      _own_primary = DataItem{};
      _own_primary.set_format("text/plain;charset=utf-8", utf8, length);
      XSetSelectionOwner(_dpy, XA_PRIMARY, _owner, CurrentTime);
      _owns_primary = (XGetSelectionOwner(_dpy, XA_PRIMARY) == _owner);
      XFlush(_dpy);
      return _owns_primary;
    }

    // Drop PRIMARY ownership (e.g. selection cleared). No-op if not owned.
    void clear_primary()
    {
      if (!_dpy || !_owns_primary) return;
      _owns_primary = false;
      _own_primary = DataItem{};
      XSetSelectionOwner(_dpy, XA_PRIMARY, None, CurrentTime);
      XFlush(_dpy);
    }

    // Read the current PRIMARY text (ours or another app's). Same return
    // contract as get_text: total bytes incl. NUL; buf=NULL queries size.
    int get_primary_text(char* buf, int buflen)
    {
      if (!_dpy) return 0;
      std::vector<uint8_t> bytes;
      if (_owns_primary) {
        if (!text_bytes(_own_primary, bytes)) return 0;
      } else {
        if (XGetSelectionOwner(_dpy, XA_PRIMARY) == None) return 0;
        Atom got = None;
        if ((!request_target_sel(XA_PRIMARY, _a_utf8, bytes, got) || bytes.empty()) &&
            (!request_target_sel(XA_PRIMARY, XA_STRING, bytes, got) || bytes.empty()))
          return 0;
      }
      return fill_text_out(bytes, buf, buflen);
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
            if (ev.xselectionclear.selection == XA_PRIMARY) {
              _owns_primary = false;
              _own_primary = DataItem{};
            } else {
              _owns = false;
              _own = DataItem{};
            }
            return true;
          }
          return false;
        case SelectionNotify:
          // A late notify (e.g. a timed-out request). Swallow if it's ours so
          // it doesn't reach a frame's dispatch.
          return ev.xselection.requestor == _owner;
        case PropertyNotify:
          // A requestor consumed a chunk of an INCR send (PropertyDelete) -
          // stream the next one.
          if (ev.xproperty.state == PropertyDelete)
            return handle_incr_send(ev.xproperty);
          return false;
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

    // Text-format helpers, parameterised on the served DataItem so they work
    // for both the CLIPBOARD (`_own`) and PRIMARY (`_own_primary`) copies.
    static bool have_text(const DataItem& d)
    {
      return d.has_format("text/plain;charset=utf-8") ||
             d.has_format("text/plain");
    }
    static bool text_bytes(const DataItem& d, std::vector<uint8_t>& out)
    {
      const char* keys[2] = { "text/plain;charset=utf-8", "text/plain" };
      for (const char* k : keys) {
        int n = d.get_format(k, nullptr, 0);
        if (n > 0) { out.resize(n); d.get_format(k, out.data(), n); return true; }
      }
      return false;
    }
    static std::vector<uint8_t> fmt_bytes(const DataItem& d, const std::string& mime)
    {
      std::vector<uint8_t> v;
      int n = d.get_format(mime, nullptr, 0);
      if (n > 0) { v.resize(n); d.get_format(mime, v.data(), n); }
      return v;
    }
    // CLIPBOARD convenience wrappers (kept for the existing call sites).
    bool have_own_text() const { return have_text(_own); }
    bool own_text_bytes(std::vector<uint8_t>& out) const { return text_bytes(_own, out); }

    // Serve a SelectionRequest from the matching owned item (PRIMARY -> the
    // text-only `_own_primary`, otherwise CLIPBOARD's `_own`).
    void serve_request(XSelectionRequestEvent& req)
    {
      const DataItem& src = (req.selection == XA_PRIMARY) ? _own_primary : _own;

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
        if (have_text(src)) {
          tg.push_back(_a_utf8);
          tg.push_back(XA_STRING);
          tg.push_back(_a_text);
          tg.push_back(_a_mime_text);
        }
        src.for_each_mime([&](const std::string& mime) {
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
        if (text_bytes(src, bytes)) {
          serve_bytes(req.requestor, prop, req.target, bytes);
          resp.property = prop;
        }
      }
      else {
        char* name = XGetAtomName(_dpy, req.target);
        if (name) {
          std::string mime = name; XFree(name);
          if (src.has_format(mime)) {
            std::vector<uint8_t> bytes = fmt_bytes(src, mime);
            serve_bytes(req.requestor, prop, req.target, bytes);
            resp.property = prop;
          }
        }
      }

      XSendEvent(_dpy, req.requestor, False, 0, reinterpret_cast<XEvent*>(&resp));
      XFlush(_dpy);
    }

    // Write `bytes` onto the requestor's property as `type`. Small payloads go
    // in one XChangeProperty; payloads over _max_chunk start an INCR transfer:
    // announce the total under the INCR type, watch the requestor for property
    // deletes, and stream chunks from handle_incr_send. The caller has already
    // decided resp.property = prop either way (the INCR property IS the reply).
    void serve_bytes(Window requestor, Atom prop, Atom type,
                     const std::vector<uint8_t>& bytes)
    {
      if (bytes.size() <= _max_chunk) {
        XChangeProperty(_dpy, requestor, prop, type, 8, PropModeReplace,
                        bytes.data(), static_cast<int>(bytes.size()));
        return;
      }
      // INCR: announce the size, then stream on each PropertyDelete.
      XSelectInput(_dpy, requestor, PropertyChangeMask);
      long total = static_cast<long>(bytes.size());
      XChangeProperty(_dpy, requestor, prop, _a_incr, 32, PropModeReplace,
                      reinterpret_cast<unsigned char*>(&total), 1);
      IncrSend s;
      s.requestor = requestor; s.property = prop; s.type = type;
      s.data = bytes; s.offset = 0;
      _incr_sends.push_back(std::move(s));
    }

    // Drive one step of any INCR send addressed by this PropertyDelete: append
    // the next chunk (PropModeAppend); when the data is exhausted, append a
    // final zero-length chunk to signal completion and retire the transfer.
    bool handle_incr_send(const XPropertyEvent& pe)
    {
      for (size_t i = 0; i < _incr_sends.size(); ++i) {
        IncrSend& s = _incr_sends[i];
        if (s.requestor != pe.window || s.property != pe.atom) continue;
        size_t remaining = s.data.size() - s.offset;
        size_t n = remaining < _max_chunk ? remaining : _max_chunk;
        XChangeProperty(_dpy, s.requestor, s.property, s.type, 8, PropModeAppend,
                        n ? s.data.data() + s.offset : reinterpret_cast<const unsigned char*>(""),
                        static_cast<int>(n));
        s.offset += n;
        if (n == 0) {
          // Terminating empty chunk sent: stop watching this requestor (unless
          // another transfer to the same window is still live) and retire.
          Window req = s.requestor;
          _incr_sends.erase(_incr_sends.begin() + i);
          bool still = false;
          for (auto& o : _incr_sends) if (o.requestor == req) { still = true; break; }
          if (!still) XSelectInput(_dpy, req, NoEventMask);
        }
        XFlush(_dpy);
        return true;
      }
      return false;
    }

    // Synchronous: ask the current owner to convert `target`, wait for the
    // SelectionNotify, read the property bytes. Leaves unrelated events queued.
    bool request_target(Atom target, std::vector<uint8_t>& out, Atom& type_out)
    { return request_target_sel(_a_clipboard, target, out, type_out); }

    bool request_target_sel(Atom selection, Atom target,
                            std::vector<uint8_t>& out, Atom& type_out)
    {
      out.clear();
      XDeleteProperty(_dpy, _owner, _a_recv);
      XConvertSelection(_dpy, selection, target, _a_recv, _owner, CurrentTime);
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
      if (type == _a_incr) {
        // Chunked transfer. The owner's setting of the INCR property already
        // queued a PropertyNewValue on our window; drain that (and any other
        // stale notify) BEFORE we delete + start, so read_incr doesn't mistake
        // the now-empty announce property for the terminating zero-length chunk
        // (real chunks are only generated after the delete below).
        XSync(_dpy, False);
        XEvent stale;
        while (XCheckTypedWindowEvent(_dpy, _owner, PropertyNotify, &stale)) { /* discard */ }
        // Deleting the INCR property acks the announcement and tells the owner
        // to start appending chunks. read_incr accumulates them (each
        // PropertyNewValue) until a zero-length chunk ends it.
        XDeleteProperty(_dpy, _owner, prop);
        return read_incr(prop, out, type_out);
      }
      long total_bytes = static_cast<long>(after);
      long longs = (total_bytes + 3) / 4;
      if (XGetWindowProperty(_dpy, _owner, prop, 0, longs ? longs : 1, False,
                             AnyPropertyType, &type, &fmt, &nitems, &after,
                             &data) == Success && data) {
        // Xlib widens format-32 properties to the C `long` type, which is 8
        // bytes on LP64 (not 4). Copying nitems*(fmt/8) would take only half
        // the buffer for a format-32 reply (e.g. the TARGETS atom list),
        // silently truncating it. Use sizeof(long) as the unit for fmt==32.
        size_t unit  = (fmt == 32) ? sizeof(long) : static_cast<size_t>(fmt / 8);
        size_t bytes = static_cast<size_t>(nitems) * unit;
        out.assign(data, data + bytes);
        XFree(data);
        XDeleteProperty(_dpy, _owner, prop);
        return true;
      }
      XDeleteProperty(_dpy, _owner, prop);
      return false;
    }

    // Wait (briefly) for a PropertyNotify of `state` for `prop` on our owner.
    bool wait_property_notify(Atom prop, int state, long timeout_ms)
    {
      struct timespec start; clock_gettime(CLOCK_MONOTONIC, &start);
      for (;;) {
        XEvent ev;
        if (XCheckTypedWindowEvent(_dpy, _owner, PropertyNotify, &ev)) {
          if (ev.xproperty.atom == prop && ev.xproperty.state == state)
            return true;
          continue;   // unrelated property change - keep waiting
        }
        struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
        long ms = (now.tv_sec - start.tv_sec) * 1000 +
                  (now.tv_nsec - start.tv_nsec) / 1000000;
        if (ms >= timeout_ms) return false;
        usleep(2000);
      }
    }

    // INCR receive pump: after we deleted the INCR property, the owner appends
    // chunks (each a PropertyNewValue); we read + delete each (the delete acks
    // and requests the next), until a zero-length chunk signals the end.
    bool read_incr(Atom prop, std::vector<uint8_t>& out, Atom& type_out)
    {
      out.clear();
      for (;;) {
        if (!wait_property_notify(prop, PropertyNewValue, 5000)) return false;
        Atom type = None; int fmt = 0;
        unsigned long nitems = 0, after = 0; unsigned char* data = nullptr;
        if (XGetWindowProperty(_dpy, _owner, prop, 0, 0, False, AnyPropertyType,
                               &type, &fmt, &nitems, &after, &data) != Success)
          return false;
        if (data) { XFree(data); data = nullptr; }
        if (after == 0) {                 // zero-length chunk == end of transfer
          XDeleteProperty(_dpy, _owner, prop);
          return true;
        }
        long longs = (static_cast<long>(after) + 3) / 4;
        if (XGetWindowProperty(_dpy, _owner, prop, 0, longs, False,
                               AnyPropertyType, &type, &fmt, &nitems, &after,
                               &data) == Success && data) {
          type_out = type;
          size_t unit  = (fmt == 32) ? sizeof(long) : static_cast<size_t>(fmt / 8);
          size_t bytes = static_cast<size_t>(nitems) * unit;
          out.insert(out.end(), data, data + bytes);
          XFree(data);
        }
        XDeleteProperty(_dpy, _owner, prop);   // ack -> owner appends next chunk
      }
    }

    Display* _dpy   = nullptr;
    Window   _owner = 0;
    bool     _owns  = false;   // do we currently own CLIPBOARD?
    DataItem _own;             // what we serve while we own it
    // PRIMARY selection (select-to-copy / middle-click-paste). Text-only; the
    // same owner window serves it. XA_PRIMARY is a predefined atom (no intern).
    bool     _owns_primary = false;
    DataItem _own_primary;

    Atom _a_clipboard = None, _a_targets = None, _a_incr = None;
    Atom _a_utf8 = None, _a_text = None, _a_mime_text = None, _a_recv = None;

    // INCR (chunked) send state. When a served payload exceeds _max_chunk we
    // announce INCR and stream chunks on each requestor PropertyDelete.
    size_t _max_chunk = 65536;
    struct IncrSend {
      Window               requestor = 0;
      Atom                 property  = None;
      Atom                 type      = None;   // the target atom (data type)
      std::vector<uint8_t> data;
      size_t               offset    = 0;
    };
    std::vector<IncrSend> _incr_sends;
  };

} // namespace neui_detail

#endif // linux
