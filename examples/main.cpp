#include "neui/neui.h"
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

// Select which host to use. Change this define to switch at compile time:
//   "neui.host.win32"          - native Win32 controls (Windows only)
//   "neui.host.macos"          - native AppKit controls (macOS only)
//   "neui.host.crossplatform"  - pluggable-backend host (D2D on Windows, CG on macOS)
//
// Flip the x-suffix below to switch hosts. Both the native win32 / macOS
// hosts and the xpl host fully implement NEUI_W_CUSTOMDRAW + compound
// drawables, so the same example renders identically on either path.
#ifdef _WIN32
#define ACTIVE_HOST  "neui.host.crossplatform"
#define ACTIVE_HOSTx "neui.host.win32"
#elif defined(__APPLE__)
#define ACTIVE_HOSTx "neui.host.crossplatform"
#define ACTIVE_HOST "neui.host.macos"
#else
// Other platforms only have the crossplatform host today.
#define ACTIVE_HOST "neui.host.crossplatform"
#endif


static void dbglog(const char* fmt, ...) {
  char buf[1024];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
#ifdef _WIN32
  OutputDebugStringA(buf);
#else
  fputs(buf, stderr);
#endif
}

// App context passed as token through the session so callbacks can identify widgets.
// Session + widget handles are plain id structs; stored by value, treated read-only.
struct AppState {
  neui_api_t*           neui      = nullptr;
  neui_widget_api_t*    widgets   = nullptr;
  neui_items_api_t*     items     = nullptr;
  neui_tree_api_t*      tree      = nullptr;
  neui_attr_api_t*      attrs     = nullptr;
  neui_asset_api_t*     assets    = nullptr;
  neui_compound_api_t*  compound  = nullptr;
  neui_behavior_api_t*  behavior  = nullptr;
  neui_notify_api_t*    notify    = nullptr;
  neui_session_t     session   = {0};
  uint32_t           win_id    = 0;
  uint32_t           input_id  = 0;
  uint32_t           button_id = 0;
  uint32_t           toast_button_id = 0;
  uint32_t           msgbox_button_id = 0;
  uint32_t           list_id   = 0;
  uint32_t           combo_id  = 0;
  uint32_t           check_id  = 0;
  uint32_t           check3_id = 0;
  uint32_t           menubar_id = 0;
  uint32_t           treev_id  = 0;
  uint32_t           slider_id  = 0;
  uint32_t           slider2_id = 0;   // 16-step variant
  uint32_t           knob_id    = 0;
  uint32_t           knob_b_id  = 0;   // second continuous knob, drives compound B
  uint32_t           knob2_id   = 0;   // 16-step variant
  uint32_t           rot_slider_id  = 0;   // controls the rotating image below
  uint32_t           rot_image_id   = 0;
  uint32_t           value_label_id = 0;
  // CUSTOMDRAW demo: a client-painted meter that tracks the continuous
  // knob's value. The client handles NEUI_EVENT_WIDGET_PAINT to draw, and
  // calls widgets->invalidate(...) from NEUI_EVENT_VALUE_CHANGED so the
  // meter repaints as the knob moves.
  uint32_t           customdraw_id  = 0;
  float              customdraw_v   = 0.5f;  // last-known knob value (drawn each paint)
  uint32_t           customdraw_frames = 0;  // frame counter overlaid as text
  // Asset handle for the meter's background. Loaded once at startup via
  // NEUI_API_ASSETS, drawn via painter->draw_asset on every paint. The
  // host owns the CPU pixels + per-ctx GPU cache; the client only sees
  // the handle. asset_none.id sentinels "not loaded".
  neui_asset_t       customdraw_bg  = asset_none;
  // Asset handle for the side-by-side IMAGE smoke test. Demonstrates
  // feeding NEUI_W_IMAGE via the public asset API (widgets->set_asset)
  // rather than the legacy path source (widgets->set_text). Cleaned up
  // alongside customdraw_bg on shutdown.
  neui_asset_t       image_via_asset = asset_none;
  // Modal "About" dialog (created on demand by Help > About).
  uint32_t           about_dlg_id = 0;
  uint32_t           about_ok_id  = 0;
  // SECTION demo: a non-interactive coloured backdrop with an optional
  // header label; children paint on top via normal tree traversal.
  uint32_t           section_id    = 0;
  uint32_t           section_btn_id = 0;
  uint32_t           section_input_id = 0;  // QR-source input inside the section
  // Combobox that drives the section's NEUI_ATTR_ALIGN_TEXT attribute,
  // letting the user pick which side the title chip sits on.
  uint32_t           align_combo_id = 0;
  // Combobox above the rotation slider that swaps the rotating IMAGE's
  // source file at runtime (Lemur / Lion / Panda).
  uint32_t           image_combo_id = 0;
  // Compound-drawable demo: a CUSTOMDRAW widget whose visuals are
  // declared as a compound asset rather than painted from a WIDGET_PAINT
  // callback. Driven by the same knob's value; the framework reads the
  // widget's attr bag at paint time. Compound is shared between two
  // CUSTOMDRAW widgets to show that one shape can back many instances.
  uint32_t           compound_widget_a = 0;
  uint32_t           compound_widget_b = 0;
  neui_asset_t       compound_shape    = asset_none;
  // Behavior demo: a third CUSTOMDRAW sharing the same compound visual
  // but with an attached NEUI_ASSET_KIND_BEHAVIOR for input. Drag,
  // wheel, arrow keys, and right-click "Reset to default" all route
  // through the behavior asset and mutate the widget's "value" attr -
  // no client onevent plumbing required. The compound's existing
  // rotation binding picks up the new value at paint time.
  uint32_t           behavior_widget_id = 0;
  neui_asset_t       behavior_asset_h   = asset_none;
  // Image-knob demo: CUSTOMDRAW driven by two bitmap layers (a static
  // background shell + a moving overlay rotated by NEUI_PARAM_VALUE) plus
  // a behavior asset for input. Sweep is 270deg total, 135deg to either side
  // of the artwork's resting pose: value 0 -> rotation 0 (rest = -135deg
  // visually), value 1 -> rotation 1.5pi (rotated to +135deg), value 0.5 ->
  // rotation 0.75pi (12 o'clock). Same input handler shape as cw_c.
  uint32_t           img_knob_widget_id = 0;
  neui_asset_t       img_knob_compound  = asset_none;
  neui_asset_t       img_knob_bg        = asset_none;
  neui_asset_t       img_knob_move      = asset_none;
  neui_asset_t       img_knob_behavior  = asset_none;
  // Compound layer-kinds demo: a CUSTOMDRAW that exercises the three
  // newer kinds together - a rounded RECT backplate (theme-accent fill,
  // accent border), a TINTED asset overlay (myimage.png multiplied by
  // the theme accent so a single source bitmap recolours with the
  // session theme), and a PATH chevron drawn on top via the painter's
  // path API (MOVE_TO / LINE_TO / CLOSE).
  uint32_t           features_widget_id = 0;
  neui_asset_t       features_compound  = asset_none;
  // QR compound-layer demo: a CUSTOMDRAW backed by a single
  // NEUI_COMPOUND_LAYER_QR layer that encodes the neui repo URL (supplied
  // via the NEUI_ATTR_QRCODE string attr on the widget).
  uint32_t           qr_widget_id       = 0;
  neui_asset_t       qr_compound        = asset_none;
};

// Open a modal "About" dialog owned by the main window. The dialog has a
// short label and a single OK button that destroys the dialog (which
// re-enables the owner). Idempotent - does nothing if already open.
static void open_about_dialog(AppState* app)
{
  if (!app || app->about_dlg_id != 0) return;  // already open
  neui_session_t sess  = app->session;
  neui_widget_t  owner = { app->win_id };

  auto dlg = app->widgets->create(sess, widget_none, NEUI_W_DIALOG,
                                   240, 240, 336, 140, nullptr);
  app->widgets->set_text  (sess, dlg, "About");
  app->widgets->set_owner (sess, dlg, owner);

  auto lbl = app->widgets->create(sess, dlg, NEUI_W_LABEL, 16, 16, 290, 40, nullptr);
  app->widgets->set_text(sess, lbl, "neui example\nmodal dialog demo");

  auto ok  = app->widgets->create(sess, dlg, NEUI_W_BUTTON, 116, 64, 80, 28, nullptr);
  app->widgets->set_text(sess, ok, "OK");
  // Win32 host: events default to off; the xpl host auto-enables for interactive
  // widgets but calling set_emit_events on both is harmless and keeps parity.
  app->widgets->set_emit_events(sess, ok, true);

  app->about_dlg_id = dlg.id;
  app->about_ok_id  = ok.id;

  app->widgets->show(sess, dlg);
}

// Widget client - callbacks the host invokes for widget lifecycle and input events.
// token is the AppState* passed to create_session.
static neui_widget_client_t widget_client = {
  NEUI_VERSION,

  /* ondestroy: called when a widget is destroyed */
  [](void* token, neui_widget_t widget, void* userdata) -> void {
    dbglog("widget destroyed: id=0x%08x\n", widget.id);
    // If our modal dialog goes away (closed via X or via OK), forget the
    // cached widget IDs so the next "About" pick reopens it.
    AppState* app = static_cast<AppState*>(token);
    if (app && widget.id == app->about_dlg_id) {
      app->about_dlg_id = 0;
      app->about_ok_id  = 0;
    }
  },

  /* onevent: called for all input and focus events on widgets with emit_events enabled */
  [](void* token, neui_event_t* event) -> bool {
    AppState* app = static_cast<AppState*>(token);

    switch (event->type) {

    case NEUI_EVENT_MOUSE_BUTTON_CLICK:
      if (app && event->data.mouse.widget.id == app->section_btn_id) {
        // Read the section's input field and re-encode it into the QR widget
        // via NEUI_ATTR_QRCODE; the compound's QR layer regenerates next paint.
        neui_session_t sess = app->session;
        neui_widget_t  in   = { app->section_input_id };
        char buf[1024] = {0};
        app->widgets->get_text(sess, in, buf, (int)sizeof(buf));
        dbglog("section: encoding QR from input: %s\n", buf);
        if (app->attrs && app->qr_widget_id != 0) {
          neui_widget_t qw = { app->qr_widget_id };
          app->attrs->set_string(sess, qw, NEUI_ATTR_QRCODE, buf);
          app->widgets->invalidate(sess, qw);
        }
        return true;
      }
      if (app && event->data.mouse.widget.id == app->button_id) {
        neui_session_t sess  = app->session;
        neui_widget_t  input = { app->input_id };
        // Read current input box content (syncs from Win32 control)
        int needed = app->widgets->get_text(sess, input, nullptr, 0);
        if (needed > 1) {
          char buf[512];
          app->widgets->get_text(sess, input, buf, (int)sizeof(buf));
          dbglog("input text: %s\n", buf);
        } else {
          dbglog("input is empty\n");
        }
      }
      // Toast demo: a multi-line message anchored to the main window
      // that fades / slides in from above, holds, fades / slides out.
      if (app && app->notify && event->data.mouse.widget.id == app->toast_button_id) {
        neui_session_t sess = app->session;
        neui_widget_t  win  = { app->win_id };
        app->notify->toast(sess, win,
          "Settings saved.\nThis is a multi-line toast.");
      }
      // Message box demo: a MessageBoxEx-shaped modal alert; the chosen
      // button is echoed back as a toast.
      if (app && app->notify && event->data.mouse.widget.id == app->msgbox_button_id) {
        neui_session_t sess = app->session;
        neui_widget_t  win  = { app->win_id };
        int r = app->notify->message_box(sess, win,
          "Save changes before closing?", "neui example",
          NEUI_MB_YESNOCANCEL | NEUI_MB_ICONWARNING | NEUI_MB_DEFBUTTON1);
        const char* picked = (r == NEUI_ID_YES)    ? "You picked: Yes"
                           : (r == NEUI_ID_NO)     ? "You picked: No"
                           : (r == NEUI_ID_CANCEL) ? "You picked: Cancel"
                                                   : "Message box failed";
        app->notify->toast(sess, win, picked);
      }
      // OK button inside the modal "About" dialog: dismiss the dialog.
      if (app && app->about_dlg_id != 0 &&
          event->data.mouse.widget.id == app->about_ok_id) {
        neui_session_t sess = app->session;
        neui_widget_t  dlg  = { app->about_dlg_id };
        app->widgets->destroy(sess, dlg);
        app->about_dlg_id = 0;
        app->about_ok_id  = 0;
      }
      return true;

    case NEUI_EVENT_ITEM_SELECTED: {
      uint32_t idx = event->data.item.index;
      if (idx == NEUI_ITEM_NONE) {
        dbglog("selection cleared\n");
        return true;
      }
      neui_session_t sess   = app->session;
      neui_widget_t  widget = event->data.item.widget;
      // Image combo: userdata is the literal source filename. Push it
      // onto the rotating IMAGE widget; xpl repaints on next paint and
      // win32's widget_set_text reloads the bitmap immediately.
      if (app && widget.id == app->image_combo_id && app->rot_image_id != 0) {
        const char* filename = static_cast<const char*>(
          app->items->get_userdata(sess, widget, idx));
        if (filename) {
          neui_widget_t img = { app->rot_image_id };
          app->widgets->set_text(sess, img, filename);
          dbglog("image source -> %s\n", filename);
        }
        return true;
      }
      // Alignment combo: userdata is the literal NEUI_ATTR_ALIGN_TEXT
      // value ("left" / "center" / "right"). Push it onto the section
      // and let the xpl host's a_set_string invalidate the repaint.
      if (app && widget.id == app->align_combo_id && app->section_id != 0) {
        const char* attr_val = static_cast<const char*>(
          app->items->get_userdata(sess, widget, idx));
        if (attr_val) {
          neui_widget_t section = { app->section_id };
          app->attrs->set_string(sess, section, NEUI_ATTR_ALIGN_TEXT, attr_val);
          dbglog("section align -> %s\n", attr_val);
        }
        return true;
      }
      char buf[256];
      app->items->get_text(sess, widget, idx, buf, (int)sizeof(buf));
      void* udata = app->items->get_userdata(sess, widget, idx);
      dbglog("item selected: [%u] \"%s\" userdata=%p\n", idx, buf, udata);
      return true;
    }

    case NEUI_EVENT_CHECKBOX_CHANGED: {
      const char* states[] = { "unchecked", "checked", "indeterminate" };
      int s = (int)event->data.checkbox.state;
      dbglog("checkbox changed: widget=0x%08x state=%s\n",
        event->data.checkbox.widget.id,
        (s >= 0 && s <= 2) ? states[s] : "?");
      // Mirror the "Enable combobox above" checkbox into the combobox's
      // enabled state. Demonstrates widgets->set_enabled on both hosts:
      // win32 calls EnableWindow on the native COMBOBOX, xpl dims the
      // painted combo and gates hit-test + tab traversal.
      if (app && app->widgets &&
          event->data.checkbox.widget.id == app->check_id &&
          app->combo_id != 0) {
        neui_session_t sess = app->session;
        neui_widget_t  combo = { app->combo_id };
        app->widgets->set_enabled(sess, combo,
                                    event->data.checkbox.state == NEUI_CHECK_CHECKED);
      }
      return true;
    }

    case NEUI_EVENT_WIDGET_FOCUS:
      dbglog("focus %s: widget=0x%08x\n",
        event->data.focus.focused ? "gained" : "lost",
        event->data.focus.widget.id);
      return true;

    case NEUI_EVENT_KEYCHAR:
      //if (event->data.key.keycode == 67)
      //{
      //  return true;
      //}
      dbglog("keychar: 0x%04x\n", event->data.key.keycode);
      return false;

    case NEUI_EVENT_WIDGET_PAINT: {
      // Client-side draw for NEUI_W_CUSTOMDRAW. The event hands us a
      // curated painter API + opaque handle, the widget-local size in
      // logical px, and the focus state. Origin (0, 0) is the widget's
      // top-left - the framework has already pushed a translate + clip
      // for us, so we can't accidentally overdraw siblings.
      if (!app) return false;
      if (event->data.paint.widget.id != app->customdraw_id) return false;

      neui_painter_api_t* pa = event->data.paint.painter_api;
      neui_painter_t*     p  = event->data.paint.p;
      float w       = event->data.paint.width;
      float h       = event->data.paint.height;
      bool  focused = event->data.paint.focused;
      float v       = app->customdraw_v;

      // Backdrop: draw the preloaded bitmap asset if available, else a
      // flat dark fill. This is the proof point for NEUI_API_ASSETS -
      // the asset was loaded once at app startup; we're just handing the
      // host a handle each frame.
      if (app->customdraw_bg.id != asset_none.id)
        pa->draw_asset(p, app->customdraw_bg, 0.0f, 0.0f, w, h);
      else
        pa->fill_rect(p, 0.0f, 0.0f, w, h, 0xFF1B2230u);

      // 1-px border so the widget's bounds are visible.
      uint32_t border = focused ? 0xFF66CCFFu : 0xFF3A4255u;
      pa->draw_rect(p, 0.5f, 0.5f, w - 1.0f, h - 1.0f, 1.0f, border);

      // Meter bar: vertical level proportional to v. Anchored at the
      // bottom so it grows upward like a VU meter.
      const float pad     = 8.0f;
      float meter_x  = pad;
      float meter_y  = pad;
      float meter_w  = w - 2.0f * pad;
      float meter_h  = h - 2.0f * pad - 22.0f;  // leave 22px for caption
      if (meter_h > 0.0f && meter_w > 0.0f) {
        // Semi-transparent trough so the asset background reads through.
        pa->fill_rect(p, meter_x, meter_y, meter_w, meter_h, 0xA00E1420u);
        float fill_h = meter_h * v;
        if (fill_h > 0.0f) {
          // Green-to-red shift past 0.75 so the demo shows colour logic
          // driven by the same state the framework just handed back.
          uint32_t fill = (v > 0.85f) ? 0xFFE25555u
                       : (v > 0.65f) ? 0xFFE2C055u
                                     : 0xFF55C26Cu;
          pa->fill_rect(p, meter_x,
                            meter_y + (meter_h - fill_h),
                            meter_w, fill_h, fill);
        }
      }

      // Caption: value + frame counter (the counter proves invalidate()
      // actually re-fires the paint).
      char buf[64];
      snprintf(buf, sizeof(buf), "%.2f  (frame %u)", v, app->customdraw_frames);
      pa->draw_text(p, pad, h - 20.0f, w - 2.0f * pad, 18.0f,
                     buf, 12.0f, 0xFFE0E0E0u);

      ++app->customdraw_frames;
      return true;
    }

    case NEUI_EVENT_WIDGET_PREUPDATE: {
      // Refresh the knob's NEUI_ATTR_VALUE_TEXT just before paint reads it.
      // This is the recommended pattern: the framework "pulls" updated
      // display text from the client right before drawing, so the overlay
      // never lags the value.
      if (!app || !app->attrs) return false;
      uint32_t wid = event->data.preupdate.widget.id;
      if (wid == app->knob_id || wid == app->knob2_id) {
        neui_session_t sess = app->session;
        neui_widget_t  w    = { wid };
        float v = app->attrs->get_float(sess, w, NEUI_PARAM_VALUE, 0.0f);
        char buf[32];
        snprintf(buf, sizeof(buf), "%.2f", v);
        app->attrs->set_string(sess, w, NEUI_ATTR_VALUE_TEXT, buf);
      }
      return false;
    }

    case NEUI_EVENT_VALUE_CHANGED: {
      // Slider / knob value moved by the user. Mirror the value to the
      // label next to the knob so we can see the live update.
      if (!app) return true;
      uint32_t wid = event->data.value.widget.id;
      float    v   = event->data.value.value;
      const char* which = (wid == app->slider_id)     ? "slider"
                       : (wid == app->slider2_id)    ? "slider16"
                       : (wid == app->knob_id)       ? "knob"
                       : (wid == app->knob_b_id)     ? "knobB"
                       : (wid == app->knob2_id)      ? "knob16"
                       : (wid == app->rot_slider_id) ? "rotation"
                                                     : "?";
      dbglog("value changed: widget=0x%08x (%s) value=%.3f\n",
             wid, which, v);
      if (app->value_label_id != 0 && app->widgets) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%s: %.2f", which, v);
        neui_session_t sess  = app->session;
        neui_widget_t  label = { app->value_label_id };
        app->widgets->set_text(sess, label, buf);
      }
      // Mirror the continuous knob's value into the CUSTOMDRAW meter and
      // ask the framework to repaint it. This is the canonical "client
      // drives custom widget" loop: state change -> invalidate -> paint.
      if (wid == app->knob_id && app->customdraw_id != 0 && app->widgets) {
        app->customdraw_v = v;
        neui_session_t sess = app->session;
        neui_widget_t  cd   = { app->customdraw_id };
        app->widgets->invalidate(sess, cd);
      }
      // Compound demo: the continuous knob drives compound widget A's
      // value attr; the rotation slider drives B's. Both widgets share
      // ONE compound asset; the framework re-resolves each widget's
      // template + bindings against its own attrbag and the win32 host
      // (xpl too) invalidates the widget on attr touch when a compound
      // is attached. So no explicit invalidate() is needed here.
      if (wid == app->knob_id && app->compound_widget_a != 0 && app->attrs) {
        neui_session_t sess = app->session;
        neui_widget_t  cw   = { app->compound_widget_a };
        app->attrs->set_float(sess, cw, NEUI_PARAM_VALUE, v);
      }
      if (wid == app->knob_b_id && app->compound_widget_b != 0 && app->attrs) {
        neui_session_t sess = app->session;
        neui_widget_t  cw   = { app->compound_widget_b };
        app->attrs->set_float(sess, cw, NEUI_PARAM_VALUE, v);
      }
      // Rotation slider drives the image's NEUI_ATTR_ROTATION attribute.
      // The framework's transform stack picks it up on the next paint.
      if (wid == app->rot_slider_id && app->rot_image_id != 0 && app->attrs) {
        neui_session_t sess  = app->session;
        neui_widget_t  image = { app->rot_image_id };
        const float two_pi = 6.28318530717958647692f;
        app->attrs->set_float(sess, image, NEUI_ATTR_ROTATION, v * two_pi);
      }
      return true;
    }

    case NEUI_EVENT_ATTR_CHANGED: {
      // User-driven attr write from a behavior asset (CUSTOMDRAW C or
      // the image-knob). Mirror to the value label so the info string
      // above the knobs / sliders reflects behavior-driven changes too.
      // Native sliders / knobs still emit NEUI_EVENT_VALUE_CHANGED via
      // the case above; the two event channels are complementary.
      if (!app) return false;
      uint32_t wid = event->data.attr.widget.id;
      const char* tag = (wid == app->behavior_widget_id) ? "C"
                      : (wid == app->img_knob_widget_id) ? "imgknob"
                                                         : nullptr;
      if (tag && app->value_label_id != 0
          && app->widgets && event->data.attr.attr_key) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%s %s: %.2f", tag,
                  event->data.attr.attr_key, event->data.attr.value);
        neui_session_t sess  = app->session;
        neui_widget_t  label = { app->value_label_id };
        app->widgets->set_text(sess, label, buf);
      }
      return true;
    }

    case NEUI_EVENT_TREE_ITEM_SELECTED: {
      if (!app) return true;
      neui_session_t sess   = app->session;
      neui_widget_t  widget = event->data.tree.widget;
      neui_item_t    item   = event->data.tree.item;
      char buf[256] = {};
      app->tree->get_text(sess, widget, item, buf, (int)sizeof(buf));
      void* udata = app->tree->get_userdata(sess, widget, item);
      dbglog("tree item selected: \"%s\" userdata=%p\n", buf, udata);
      return true;
    }

    case NEUI_EVENT_TREE_ITEM_ACTIVATED: {
      if (!app) return true;
      neui_session_t sess   = app->session;
      neui_widget_t  widget = event->data.tree.widget;
      neui_item_t    item   = event->data.tree.item;
      char buf[256] = {};
      app->tree->get_text(sess, widget, item, buf, (int)sizeof(buf));
      void* ud = app->tree->get_userdata(sess, widget, item);
      dbglog("tree item activated: \"%s\" -> %p\n", buf, ud);
      if (ud == (void*)4 && app->neui)  // Exit
        app->neui->endsession(sess);
      if (ud == (void*)20)              // Help > About -> open modal dialog
        open_about_dialog(app);
      return true;
    }

    case NEUI_EVENT_APP_QUIT:
      // Allow close. ondestroy clears the modal dialog's cached IDs.
      dbglog("app quit requested - allowing\n");
      return true;

    default:
      return false;
    }
  }
};

// Host client - the host calls get_interface to ask whether this client
// implements a given extension. We advertise the widget event client.
static neui_client_t host_client = {
  NEUI_VERSION,
  [](void* /*token*/, const char* iface) -> void* {
    if (!strcmp(iface, NEUI_API_WIDGETS))
      return &widget_client;
    return nullptr;
  }
};

// On Win32 this is also reachable as the entry point - the WIN32 subsystem
// links to mainCRTStartup which calls main(), so the wWinMain alternative
// isn't needed and we get the same signature on every platform.
int main(int argc, char** argv) {
  (void)argc; (void)argv;
  AppState app;

  // One call registers every host the linked neuilib has compiled in.
  // The per-host wrappers (neui_register_xplhost, neui_register_win32host,
  // neui_register_macoshost) remain available for fine-grained control.
  neui_init();
  auto neui = neui_get_api(ACTIVE_HOST);

  // Pass &app as token so callbacks can access widget handles and the widget API
  auto sess = neui->create_session(&host_client, &app);
  app.neui    = neui;
  app.session = sess;
  app.widgets = (neui_widget_api_t*)neui->get_interface(sess, NEUI_API_WIDGETS);
  app.items   = (neui_items_api_t*) neui->get_interface(sess, NEUI_API_ITEMS);
  app.tree    = (neui_tree_api_t*)  neui->get_interface(sess, NEUI_API_TREE);
  app.attrs   = (neui_attr_api_t*)  neui->get_interface(sess, NEUI_API_ATTRS);
  app.assets  = (neui_asset_api_t*) neui->get_interface(sess, NEUI_API_ASSETS);
  app.compound = (neui_compound_api_t*) neui->get_interface(sess, NEUI_API_COMPOUND);
  app.behavior = (neui_behavior_api_t*) neui->get_interface(sess, NEUI_API_BEHAVIOR);
  app.notify   = (neui_notify_api_t*)   neui->get_interface(sess, NEUI_API_NOTIFY);

  // Window: four columns - left = input + listbox, middle = combobox + checkboxes,
  //   right = treeview, far-right = slider + knob (continuous + 16-step variants).
  // Outer window dimensions sized so the client area has roughly the same
  // ~5px margin on right and bottom as it has on left and top:
  //   rightmost widget right edge: x=865  -> client width ~= 870
  //   bottommost widget bottom    : y=580  -> client height ~= 585
  // Win11 non-client chrome (resize borders + title bar + menu bar) is
  // about 16 horizontal + 58 vertical, so ~= 890 x 645 outer.
  auto win = app.widgets->create(sess, widget_none, NEUI_W_APPWINDOW, 100, 100, 1010, 645, nullptr);
  app.win_id = win.id;
  app.widgets->set_text(sess, win, "neui example");
  // Opt this frame into system-theme tracking on both hosts: title bar +
  // (win32) native controls + painted widgets follow OS light/dark and
  // accent, and the frame invalidates on every system theme flip.
  // Without this attr, the frame keeps OS-default chrome and (xpl) a
  // frozen palette captured at session creation.
  app.attrs->set_int(sess, win, NEUI_ATTR_FOLLOW_SYSTEM_THEME, 1);
  // Window icon. On Windows the icon loader checks the embedded "PNG"
  // resource first (same resource pool as the IMAGE widget), then falls
  // back to a file on disk. On macOS the image loader falls back to the
  // app bundle's Resources/. Either way `"myimage.png"` resolves without
  // a runtime file dependency.
  app.attrs->set_string(sess, win, NEUI_ATTR_ICON_PATH, "myimage.png");

  // --- Menu bar ---

  auto menubar = app.widgets->create(sess, win, NEUI_W_MENUBAR, 0, 0, 0, 0, nullptr);
  app.menubar_id = menubar.id;

  auto file_menu = app.tree->add(sess, menubar, tree_item_root, "File", nullptr);
  auto new_item  = app.tree->add(sess, menubar, file_menu,      "New",  (void*)1);
  auto open_item = app.tree->add(sess, menubar, file_menu,      "Open", (void*)2);
  auto save_item = app.tree->add(sess, menubar, file_menu,      "Save", (void*)3);
                   app.tree->add(sess, menubar, file_menu,      "-",    nullptr);
                   app.tree->add(sess, menubar, file_menu,      "Exit", (void*)4);
  app.tree->set_shortcut(sess, menubar, new_item,  NEUI_KMOD_CTRL, NEUI_KEY_N);
  app.tree->set_shortcut(sess, menubar, open_item, NEUI_KMOD_CTRL, NEUI_KEY_O);
  app.tree->set_shortcut(sess, menubar, save_item, NEUI_KMOD_CTRL, NEUI_KEY_S);

  auto edit_menu = app.tree->add(sess, menubar, tree_item_root, "Edit", nullptr);
  auto undo_item = app.tree->add(sess, menubar, edit_menu,      "Undo", (void*)10);
  auto redo_item = app.tree->add(sess, menubar, edit_menu,      "Redo", (void*)11);
  app.tree->set_shortcut(sess, menubar, undo_item, NEUI_KMOD_CTRL, NEUI_KEY_Z);
  app.tree->set_shortcut(sess, menubar, redo_item, NEUI_KMOD_CTRL, NEUI_KEY_Y);
  // Bind to built-in commands so the focused text widget handles activation.
  app.tree->set_menu_cmd (sess, menubar, undo_item, NEUI_CMD_UNDO);
  app.tree->set_menu_cmd (sess, menubar, redo_item, NEUI_CMD_REDO);
  // app.tree->set_enabled (sess, menubar, redo_item, false);  // Redo is grayed

  auto help_menu  = app.tree->add(sess, menubar, tree_item_root, "Help",  nullptr);
                    app.tree->add(sess, menubar, help_menu,      "About", (void*)20);

  // --- Left column (x=5, width=195) ---

  auto label  = app.widgets->create(sess, win, NEUI_W_LABEL,     5,   5, 195,  20, nullptr);
  auto input  = app.widgets->create(sess, win, NEUI_W_INPUTBOX,  5,  30, 195,  24, nullptr);
  auto button = app.widgets->create(sess, win, NEUI_W_BUTTON,    5,  60, 100,  28, nullptr);
  auto toast_btn = app.widgets->create(sess, win, NEUI_W_BUTTON, 110, 60,  90,  28, nullptr);
  app.input_id  = input.id;
  app.button_id = button.id;
  app.toast_button_id = toast_btn.id;

  app.widgets->set_text(sess, label,  "Type something below:");
  app.widgets->set_text(sess, button, "Read input");
  app.widgets->set_text(sess, toast_btn, "Toast!");
  app.widgets->set_emit_events(sess, toast_btn, true);

  app.attrs->set_string(sess, label, NEUI_ATTR_FONT_FAMILY, "Consolas");
  app.attrs->set_int(sess, label, NEUI_ATTR_FONT_WEIGHT, 700);
  app.attrs->set_float(sess, label, NEUI_ATTR_FONT_SIZE, 20.f);

  auto list_label = app.widgets->create(sess, win, NEUI_W_LABEL,   5, 100, 195,  20, nullptr);
  auto list       = app.widgets->create(sess, win, NEUI_W_LISTBOX, 5, 125, 195, 160, nullptr);
  app.list_id = list.id;

  app.widgets->set_text(sess, list_label, "Listbox:");
  app.items->add(sess, list, "Alpha",   (void*)1);
  app.items->add(sess, list, "Beta",    (void*)2);
  app.items->add(sess, list, "Gamma",   (void*)3);
  app.items->add(sess, list, "Delta",   (void*)4);
  app.items->add(sess, list, "Epsilon", (void*)5);
  app.items->add(sess, list, "Zeta",    (void*)6);
  app.items->add(sess, list, "Eta",     (void*)7);
  app.items->add(sess, list, "Theta",   (void*)8);
  app.items->add(sess, list, "Iota",    (void*)9);
  app.items->add(sess, list, "Kappa",   (void*)10);
  app.items->add(sess, list, "Lambda",  (void*)11);
  app.items->add(sess, list, "Mu",      (void*)12);
  app.items->add(sess, list, "Nu",      (void*)13);
  app.items->set_selected(sess, list, 0);

  // --- Left column, lower section: image-source combo + rotation slider +
  // rotating image. Demonstrates two live attribute paths on the IMAGE
  // widget: NEUI_ATTR_ROTATION (driven by the slider) and the widget's
  // text source filename (driven by the image combo). Both update the
  // same widget on each paint.
  auto image_label = app.widgets->create(sess, win, NEUI_W_LABEL,
                                          5, 290, 195, 20, nullptr);
  app.widgets->set_text(sess, image_label, "Image:");

  // Combo coords describe just the collapsed bar; the drop list sizes
  // itself from the item count when opened.
  auto image_combo = app.widgets->create(sess, win, NEUI_W_COMBOBOX,
                                          5, 313, 195, 24, nullptr);
  app.image_combo_id = image_combo.id;
  // Userdata is the literal filename so the ITEM_SELECTED handler can
  // pass it straight to widgets->set_text(rot_image, ...).
  app.items->add(sess, image_combo, "Lemur", (void*)"lemur.jpg");
  app.items->add(sess, image_combo, "Lion",  (void*)"lion.jpg");
  app.items->add(sess, image_combo, "Panda", (void*)"panda.jpg");
  app.items->set_selected(sess, image_combo, 0);  // default: Lemur

  auto rot_slider = app.widgets->create(sess, win, NEUI_W_SLIDER,
                                         5, 345, 195, 28, nullptr);
  app.rot_slider_id = rot_slider.id;
  if (app.attrs)
    app.attrs->set_float(sess, rot_slider, NEUI_PARAM_VALUE, 0.0f);

  auto rot_image = app.widgets->create(sess, win, NEUI_W_IMAGE,
                                        5, 380, 195, 200, nullptr);
  app.widgets->set_text(sess, rot_image, "lemur.jpg");
  app.rot_image_id = rot_image.id;
  if (app.attrs)
    app.attrs->set_float(sess, rot_image, NEUI_ATTR_ROTATION, 0.0f);

  // --- Middle column (x=215, width=190) ---

  auto combo_label = app.widgets->create(sess, win, NEUI_W_LABEL,   215,  5, 190,  20, nullptr);
  auto combo       = app.widgets->create(sess, win, NEUI_W_COMBOBOX,215, 30, 190,  24, nullptr);
  app.combo_id = combo.id;

  app.widgets->set_text(sess, combo_label, "Combobox:");
  app.items->add(sess, combo, "Option A", (void*)'A');
  app.items->add(sess, combo, "Option B", (void*)'B');
  app.items->add(sess, combo, "Option C", (void*)'C');
  app.items->set_selected(sess, combo, 0);

  // Checkboxes - 2-state and 3-state, below the combobox
  auto check  = app.widgets->create(sess, win, NEUI_W_CHECKBOX,  215,  65, 185, 22, nullptr);
  auto check3 = app.widgets->create(sess, win, NEUI_W_CHECKBOX3, 215,  95, 185, 22, nullptr);
  app.check_id  = check.id;
  app.check3_id = check3.id;

  app.widgets->set_text(sess, check,  "Enable combobox above");
  app.widgets->set_text(sess, check3, "Allow override");

  // Message box demo button - sits in the gap between the checkboxes and
  // the asset-fed IMAGE below.
  auto msgbox_btn = app.widgets->create(sess, win, NEUI_W_BUTTON, 215, 125, 185, 28, nullptr);
  app.msgbox_button_id = msgbox_btn.id;
  app.widgets->set_text(sess, msgbox_btn, "Message box...");
  app.widgets->set_emit_events(sess, msgbox_btn, true);
  // Default the checkbox to checked so the combobox starts enabled. The
  // CHECKBOX_CHANGED handler below mirrors the state into the combo via
  // widgets->set_enabled.
  app.widgets->set_check(sess, check, NEUI_CHECK_CHECKED);

  // --- Right column (x=420, width=200) - treeview ---

  auto tree_label = app.widgets->create(sess, win, NEUI_W_LABEL,    420,   5, 200,  20, nullptr);
  auto treev      = app.widgets->create(sess, win, NEUI_W_TREEVIEW, 420,  30, 200, 255, nullptr);
  app.treev_id = treev.id;

  app.widgets->set_text(sess, tree_label, "Treeview:");

  // Build a small hierarchy
  auto fruits = app.tree->add(sess, treev, tree_item_root, "Fruits",     (void*)100);
  auto vegs   = app.tree->add(sess, treev, tree_item_root, "Vegetables", (void*)200);
  auto grains = app.tree->add(sess, treev, tree_item_root, "Grains",     (void*)300);

  app.tree->add(sess, treev, fruits, "Apple",   (void*)101);
  app.tree->add(sess, treev, fruits, "Banana",  (void*)102);
  app.tree->add(sess, treev, fruits, "Cherry",  (void*)103);

  auto root_veg = app.tree->add(sess, treev, vegs, "Root",  (void*)201);
  app.tree->add(sess, treev, vegs, "Leafy",     (void*)202);
  app.tree->add(sess, treev, root_veg, "Carrot", (void*)211);
  app.tree->add(sess, treev, root_veg, "Beet",   (void*)212);

  app.tree->add(sess, treev, grains, "Wheat", (void*)301);
  app.tree->add(sess, treev, grains, "Rice",  (void*)302);

  // Disabled item example
  auto disabled = app.tree->add(sess, treev, tree_item_root, "Unavailable", (void*)400);
  app.tree->set_enabled(sess, treev, disabled, false);

  auto img = app.widgets->create(sess, win, NEUI_W_IMAGE, 215, 160, 200, 150, nullptr);
  app.widgets->set_text(sess, img, "myimage.png");

  // Asset-driven IMAGE smoke test: pre-load via NEUI_API_ASSETS and bind
  // the handle with widgets->set_asset. Visually identical to the
  // path-driven IMAGE on the left; same aspect-fit, same rotation
  // honouring NEUI_ATTR_ROTATION. The widget does not retain a refcount,
  // so the asset stays alive on `app.image_via_asset` until shutdown.
  auto img_asset_demo = app.widgets->create(sess, win, NEUI_W_IMAGE,
                                             430, 160, 200, 150, nullptr);
  app.image_via_asset = app.assets->create_from_file(sess, "myimage.png");
  app.widgets->set_asset(sess, img_asset_demo, app.image_via_asset);

  // --- SECTION demo: visual grouping container ---------------------------
  // A non-interactive coloured backdrop with an optional header label.
  // Children (the label + button below) are created with the section as
  // their parent so they belong to the section in the widget tree, and
  // paint on top of it via normal depth-first traversal. The section has
  // emit_events=false so clicks on its bare area pass through to anything
  // beneath. By default the background is a theme-derived shade that sits
  // lighter than the frame's clear colour, and the header text is centred
  // within a band at the top. To override either, set NEUI_ATTR_BACKGROUND
  // (int ARGB) or NEUI_ATTR_ALIGN_TEXT ("left"|"center"|"right").
  //
  // Alignment combobox: sits above the section and drives its
  // NEUI_ATTR_ALIGN_TEXT live. Userdata on each item is the literal
  // attribute string so the ITEM_SELECTED handler can apply it directly.
  // Combo coords describe just the collapsed bar; the drop list sizes
  // itself from the item count when opened.
  auto align_combo = app.widgets->create(sess, win, NEUI_W_COMBOBOX,
                                          215, 320, 200, 24, nullptr);
  app.align_combo_id = align_combo.id;
  app.items->add(sess, align_combo, "left",   (void*)"left");
  app.items->add(sess, align_combo, "middle", (void*)"center");
  app.items->add(sess, align_combo, "right",  (void*)"right");
  app.items->set_selected(sess, align_combo, 0);  // matches section's initial "left"

  auto section = app.widgets->create(sess, win, NEUI_W_SECTION,
                                      215, 350, 200, 230, nullptr);
  app.section_id = section.id;
  app.widgets->set_text(sess, section, "Section widget");
  app.attrs->set_string(sess, section, NEUI_ATTR_ALIGN_TEXT, "left");
  // app.attrs->set_int   (sess, section, NEUI_ATTR_BACKGROUND, 0xFF2A3340);

  // Children of the section. Coordinates are relative to the section's
  // BODY top-left - i.e. just below the chip band when `align_text` is
  // "left" / "center" / "right", and the section's top edge when it's
  // "none" or there is no text. So (10, 6) is 6 px below the band's
  // bottom edge when the band is present, or 6 px below the section's
  // top when "none" hides the band.
  // Upper child: a text INPUTBOX whose content the button below encodes
  // into the QR widget at the bottom of the section.
  auto sec_input = app.widgets->create(sess, section, NEUI_W_INPUTBOX,
                                        10, 6, 180, 24, nullptr);
  app.widgets->set_text(sess, sec_input, "https://github.com/defiantnerd/neui");
  app.section_input_id = sec_input.id;

  auto sec_btn = app.widgets->create(sess, section, NEUI_W_BUTTON,
                                      10, 36, 180, 28, nullptr);
  app.widgets->set_text(sess, sec_btn, "Set QR code");
  app.section_btn_id = sec_btn.id;
  app.widgets->set_emit_events(sess, sec_btn, true);

  // Bottom child: the QR compound-layer widget. A white rounded card sits
  // behind a NEUI_COMPOUND_LAYER_QR layer; the button above re-encodes the
  // input text into it via NEUI_ATTR_QRCODE. The symbol is generated by the
  // vendored qrcodegen library and rasterised into an internally-held bitmap.
  if (app.assets && app.compound) {
    app.qr_compound = app.assets->create_compound(sess);
    if (app.qr_compound.id != asset_none.id) {
      neui_asset_t cs = app.qr_compound;

      // White rounded card behind the symbol so it scans on any theme
      // background, with a subtle border that delineates it from the section
      // fill. The border is built from two FILLS (a grey rounded rect with a
      // slightly inset white one on top) rather than a 1 px STROKE: a stroke
      // sits on the widget boundary inside the bounds clip, so at fractional
      // DPI (150%) the clip keeps a different fraction of each edge and the
      // border looks uneven (heavier on one side). Fill edges anti-alias
      // uniformly, so the ring is even all the way around.
      auto border = app.compound->add_layer(sess, cs, NEUI_COMPOUND_LAYER_RECT, -2);
      app.compound->set_anchor(sess, cs, border,
                                 NEUI_ANCHOR_TOP_LEFT, NEUI_ANCHOR_TOP_LEFT);
      app.compound->set_int  (sess, cs, border, "width",  NEUI_COMPOUND_FILL);
      app.compound->set_int  (sess, cs, border, "height", NEUI_COMPOUND_FILL);
      app.compound->set_int  (sess, cs, border, "fill_color",    (int)0xFFB0B0B0);
      app.compound->set_float(sess, cs, border, "corner_radius", 8.0f);

      auto card = app.compound->add_layer(sess, cs, NEUI_COMPOUND_LAYER_RECT, -1);
      app.compound->set_anchor(sess, cs, card,
                                 NEUI_ANCHOR_CENTER, NEUI_ANCHOR_CENTER);
      app.compound->set_int  (sess, cs, card, "width",  116);   // 2 px inset each side
      app.compound->set_int  (sess, cs, card, "height", 116);
      app.compound->set_int  (sess, cs, card, "fill_color",    (int)0xFFFFFFFF);
      app.compound->set_float(sess, cs, card, "corner_radius", 7.0f);

      // QR layer: black modules on a transparent background (the white card
      // shows through), QUARTILE ecc. Fills the widget; the symbol's own quiet
      // zone (plus the small letterbox gap from integer module sizing) gives an
      // even white margin inside the rounded card, so the card hugs it tidily.
      auto qr = app.compound->add_layer(sess, cs, NEUI_COMPOUND_LAYER_QR, 0);
      app.compound->set_anchor(sess, cs, qr,
                                 NEUI_ANCHOR_CENTER, NEUI_ANCHOR_CENTER);
      app.compound->set_int(sess, cs, qr, "width",      NEUI_COMPOUND_FILL);
      app.compound->set_int(sess, cs, qr, "height",     NEUI_COMPOUND_FILL);
      app.compound->set_int(sess, cs, qr, "fill_color", (int)0xFF000000);
      app.compound->set_int(sess, cs, qr, "ecc",        NEUI_QR_ECC_QUARTILE);

      auto qw = app.widgets->create(sess, section, NEUI_W_CUSTOMDRAW,
                                      40, 72, 120, 120, nullptr);
      app.qr_widget_id = qw.id;
      app.widgets->set_asset(sess, qw, app.qr_compound);
      if (app.attrs)
        app.attrs->set_string(sess, qw, NEUI_ATTR_QRCODE,
                              "https://github.com/defiantnerd/neui");
    }
  }

  // --- Far-right column (x=635, width=230): slider + knob -----------------

  auto slider_label = app.widgets->create(sess, win, NEUI_W_LABEL,
                                            635,   5, 230, 20, nullptr);
  app.widgets->set_text(sess, slider_label, "Slider (continuous):");

  auto slider = app.widgets->create(sess, win, NEUI_W_SLIDER,
                                     635, 28, 230, 28, nullptr);
  app.slider_id = slider.id;
  if (app.attrs)
    app.attrs->set_float(sess, slider, NEUI_PARAM_VALUE, 0.5f);

  auto slider2_label = app.widgets->create(sess, win, NEUI_W_LABEL,
                                             635,  62, 230, 20, nullptr);
  app.widgets->set_text(sess, slider2_label, "Slider (16 steps):");

  auto slider2 = app.widgets->create(sess, win, NEUI_W_SLIDER,
                                      635,  85, 230, 28, nullptr);
  app.slider2_id = slider2.id;
  if (app.attrs) {
    app.attrs->set_int  (sess, slider2, NEUI_ATTR_STEPS,   16);
    app.attrs->set_float(sess, slider2, NEUI_PARAM_VALUE,  0.5f);
  }

  auto knob_label = app.widgets->create(sess, win, NEUI_W_LABEL,
                                          635, 122, 230, 20, nullptr);
  app.widgets->set_text(sess, knob_label, "Knob (continuous):");

  // Knob bounds include extra height for the NEUI_ATTR_VALUE_TEXT overlay
  // beneath the disc (~18 logical px reserved by paint_knob).
  auto knob = app.widgets->create(sess, win, NEUI_W_KNOB,
                                   635, 145, 70, 95, nullptr);
  app.knob_id = knob.id;
  if (app.attrs) {
    app.attrs->set_float(sess, knob, NEUI_PARAM_DEFAULT, 0.5f);
    app.attrs->set_float(sess, knob, NEUI_PARAM_VALUE,   0.5f);
    // Active-arc polarity: "min" (default), "center" (bipolar), or "max".
    // app.attrs->set_string(sess, knob, NEUI_ATTR_POLARITY, "center");
  }

  // Second continuous knob, sits right of the first. Drives compound
  // widget B's value attr; widget B reads it as its rotation binding.
  auto knob_b = app.widgets->create(sess, win, NEUI_W_KNOB,
                                     715, 145, 70, 95, nullptr);
  app.knob_b_id = knob_b.id;
  if (app.attrs) {
    app.attrs->set_float(sess, knob_b, NEUI_PARAM_DEFAULT, 0.5f);
    app.attrs->set_float(sess, knob_b, NEUI_PARAM_VALUE,   0.5f);
  }

  auto knob2_label = app.widgets->create(sess, win, NEUI_W_LABEL,
                                           635, 250, 230, 20, nullptr);
  app.widgets->set_text(sess, knob2_label, "Knob (16 steps):");

  auto knob2 = app.widgets->create(sess, win, NEUI_W_KNOB,
                                    635, 273, 70, 95, nullptr);
  app.knob2_id = knob2.id;
  if (app.attrs) {
    app.attrs->set_int  (sess, knob2, NEUI_ATTR_STEPS,    16);
    app.attrs->set_float(sess, knob2, NEUI_PARAM_DEFAULT, 0.5f);
    app.attrs->set_float(sess, knob2, NEUI_PARAM_VALUE,   0.5f);
  }

  auto value_label = app.widgets->create(sess, win, NEUI_W_LABEL,
                                          720, 380, 145, 22, nullptr);
  app.value_label_id = value_label.id;
  app.widgets->set_text(sess, value_label, "(drag to change)");

  // CUSTOMDRAW demo: a client-painted meter tied to the continuous knob.
  // Placed under the treeview (x=420, w=200) with a 10 px gap above and
  // ~10 px padding to the client-area bottom (the rotating image at y=580
  // is the existing layout's lower waterline). The framework hands us
  // the render backend + ctx each paint; we draw a vertical level bar
  // plus a frame counter. invalidate() is fired from the knob's
  // VALUE_CHANGED handler above.
  auto customdraw = app.widgets->create(sess, win, NEUI_W_CUSTOMDRAW,
                                         420, 295, 200, 275, nullptr);
  app.customdraw_id = customdraw.id;
  app.customdraw_v  = 0.5f;

  // Preload the meter background bitmap via NEUI_API_ASSETS. Note: this
  // happens BEFORE any paint - clients no longer need a render context
  // to obtain a usable asset handle. The host owns CPU pixels + per-ctx
  // GPU upload cache behind the opaque neui_asset_t.
  if (app.assets)
    app.customdraw_bg = app.assets->create_from_file(sess, "myimage.png");

  // Image-knob assets: bg shell + rotating moving part. Both files exist
  // as 1x and @2x; the asset loader auto-picks @2x on HiDPI displays.
  if (app.assets) {
    app.img_knob_bg   = app.assets->create_from_file(sess, "knob_bg.png");
    app.img_knob_move = app.assets->create_from_file(sess, "knob_move.png");
  }

  // -------------------------------------------------------------------------
  // Compound-drawable demo. Two CUSTOMDRAW widgets share ONE compound
  // asset (the visual shape) but carry their own attribute values; the
  // framework reads each widget's AttrBag at paint time. Demonstrates:
  //   - one shape, many widgets (no per-widget paint code)
  //   - text templates ({value} substitution)
  //   - numeric bindings (rotation = scale * value + offset)
  //   - alpha multiplier on layers
  if (app.assets && app.compound) {
    app.compound_shape = app.assets->create_compound(sess);
    if (app.compound_shape.id != asset_none.id) {
      neui_asset_t cs = app.compound_shape;

      // Layer z=-1: aspect-fitted bitmap behind the text, rotated by value.
      // Scale 2pi so value 0..1 maps to a full rotation. offset = 0.
      auto bg_layer = app.compound->add_layer(sess, cs,
                                                NEUI_COMPOUND_LAYER_ASSET, -1);
      app.compound->set_anchor(sess, cs, bg_layer,
                                 NEUI_ANCHOR_CENTER, NEUI_ANCHOR_CENTER);
      app.compound->set_int(sess, cs, bg_layer, "width",  80);
      app.compound->set_int(sess, cs, bg_layer, "height", 80);
      app.compound->set_float(sess, cs, bg_layer, "alpha", 0.6f);
      if (app.customdraw_bg.id != asset_none.id)
        app.compound->set_asset(sess, cs, bg_layer, "asset", app.customdraw_bg);
      app.compound->bind(sess, cs, bg_layer, "rotation",
                          NEUI_PARAM_VALUE, 6.2831853f, 0.0f);

      // Layer z=+1: centered label showing "<name>: <value>" via template
      // substitution. {name} and {value} resolve against the widget's
      // AttrBag at paint time - each widget supplies its own.
      auto txt_layer = app.compound->add_layer(sess, cs,
                                                 NEUI_COMPOUND_LAYER_TEXT, 1);
      app.compound->set_anchor(sess, cs, txt_layer,
                                 NEUI_ANCHOR_BOTTOM, NEUI_ANCHOR_BOTTOM);
      app.compound->set_int(sess, cs, txt_layer, "width",  NEUI_COMPOUND_FILL);
      app.compound->set_int(sess, cs, txt_layer, "height", 22);
      app.compound->set_string(sess, cs, txt_layer, "text", "{name}: {value}");
      app.compound->set_float(sess, cs, txt_layer, "size", 14.0f);
      // Leave "color" unset - the text layer falls back to the active
      // theme's text_primary colour, so the label stays legible if the
      // user flips the system theme at runtime.

      // State-filtered layers: each "show_when" bitmask gates the layer
      // behind the widget's internal hover / press state with no client
      // onevent plumbing. Driven by the framework's hover / press / enabled
      // detection; independent of event emission. The two layers are
      // mutually exclusive at the same screen position: HOVER appears
      // while the cursor is in but the button isn't held; PRESS takes
      // over while the button is held (capture-style - stays even if
      // the cursor drags out). HOVERED | NOT_PRESSED is the AND-match
      // expressing "hovered but not currently pressed".
      auto hover_layer = app.compound->add_layer(sess, cs,
                                                   NEUI_COMPOUND_LAYER_TEXT, 2);
      app.compound->set_anchor(sess, cs, hover_layer,
                                 NEUI_ANCHOR_TOP, NEUI_ANCHOR_TOP);
      app.compound->set_int(sess, cs, hover_layer, "width",  NEUI_COMPOUND_FILL);
      app.compound->set_int(sess, cs, hover_layer, "height", 18);
      app.compound->set_string(sess, cs, hover_layer, "text", "HOVER");
      app.compound->set_float (sess, cs, hover_layer, "size", 11.0f);
      app.compound->set_int   (sess, cs, hover_layer, NEUI_PROP_SHOW_WHEN,
                                 NEUI_LAYER_STATE_HOVERED
                                 | NEUI_LAYER_STATE_NOT_PRESSED);

      auto press_layer = app.compound->add_layer(sess, cs,
                                                   NEUI_COMPOUND_LAYER_TEXT, 3);
      app.compound->set_anchor(sess, cs, press_layer,
                                 NEUI_ANCHOR_TOP, NEUI_ANCHOR_TOP);
      app.compound->set_int(sess, cs, press_layer, "width",  NEUI_COMPOUND_FILL);
      app.compound->set_int(sess, cs, press_layer, "height", 18);
      app.compound->set_string(sess, cs, press_layer, "text", "PRESS");
      app.compound->set_float (sess, cs, press_layer, "size", 11.0f);
      app.compound->set_int   (sess, cs, press_layer, NEUI_PROP_SHOW_WHEN,
                                 NEUI_LAYER_STATE_PRESSED);
    }

    // Two CUSTOMDRAW widgets pointing at the same compound asset. Each
    // has its own "name" and "value" attr so the shared compound renders
    // distinct text and (when value differs) distinct rotation.
    auto cw_a = app.widgets->create(sess, win, NEUI_W_CUSTOMDRAW,
                                      635, 410, 110, 110, nullptr);
    auto cw_b = app.widgets->create(sess, win, NEUI_W_CUSTOMDRAW,
                                      755, 410, 110, 110, nullptr);
    app.compound_widget_a = cw_a.id;
    app.compound_widget_b = cw_b.id;
    app.widgets->set_asset(sess, cw_a, app.compound_shape);
    app.widgets->set_asset(sess, cw_b, app.compound_shape);
    if (app.attrs) {
      app.attrs->set_string(sess, cw_a, "name",  "A");
      app.attrs->set_float (sess, cw_a, NEUI_PARAM_VALUE, 0.3f);
      app.attrs->set_string(sess, cw_b, "name",  "Bub");
      app.attrs->set_float (sess, cw_b, NEUI_PARAM_VALUE, 0.7f);
    }

    // ----------------------------------------------------------------
    // Interactive-behavior demo: a third CUSTOMDRAW sharing the same
    // compound visual, with an attached NEUI_ASSET_KIND_BEHAVIOR that
    // wires rotational drag + wheel + arrow keys + right-click reset.
    // No client onevent plumbing: the framework writes `value` on the
    // widget's AttrBag and the compound's rotation binding picks it up.
    if (app.behavior) {
      auto cw_c = app.widgets->create(sess, win, NEUI_W_CUSTOMDRAW,
                                        875, 410, 110, 110, nullptr);
      app.behavior_widget_id = cw_c.id;
      app.widgets->set_asset(sess, cw_c, app.compound_shape);
      app.attrs->set_string(sess, cw_c, "name",  "C");
      app.attrs->set_float (sess, cw_c, NEUI_PARAM_VALUE,   0.5f);
      app.attrs->set_float (sess, cw_c, NEUI_PARAM_DEFAULT, 0.5f);

      // Four handlers on one behavior asset bundle the full rotary-knob
      // input feel: rotational drag (primary gesture), wheel (fine
      // adjust), arrow keys (keyboard tweak when focused), and right-
      // click "Reset to default" (the CONTEXT_RESET handler reads
      // target_default and writes it back). The handlers run in
      // dispatch order; mouse / wheel / key events each match the first
      // applicable handler.
      neui_asset_t ba = app.assets->create_behavior(sess);
      app.behavior_asset_h = ba;
      neui_behavior_handler_t hdrag =
        app.behavior->add_handler(sess, ba, NEUI_BEHAVIOR_KIND_DRAG_ROTATIONAL);
      neui_behavior_handler_t hwheel =
        app.behavior->add_handler(sess, ba, NEUI_BEHAVIOR_KIND_WHEEL);
      neui_behavior_handler_t hkeys =
        app.behavior->add_handler(sess, ba, NEUI_BEHAVIOR_KIND_KEY_STEP);
      neui_behavior_handler_t hreset =
        app.behavior->add_handler(sess, ba, NEUI_BEHAVIOR_KIND_CONTEXT_RESET);

      // Shared per-handler config: all four manipulate the same float
      // attr (NEUI_PARAM_VALUE) clamped to [0, 1]. target_default is
      // only consulted by CONTEXT_RESET but is harmless on the others.
      // No client onevent plumbing: the framework writes the attr and
      // the compound's rotation binding re-renders.
      neui_behavior_handler_t hs[] = { hdrag, hwheel, hkeys, hreset };
      for (auto h : hs) {
        app.behavior->set_string(sess, ba, h, "target",         NEUI_PARAM_VALUE);
        app.behavior->set_string(sess, ba, h, "target_default", NEUI_PARAM_DEFAULT);
        app.behavior->set_float (sess, ba, h, "min", 0.0f);
        app.behavior->set_float (sess, ba, h, "max", 1.0f);
      }
      // Per-handler tuning. Wheel `step` is per-notch (Win32 maps
      // WHEEL_DELTA via SPI_GETWHEELSCROLLLINES, so one physical notch
      // advances by step * lines_per_notch). Key `step` is per arrow
      // tap; `coarse` is the Shift+arrow step. Values picked to feel
      // similar to the native KNOB widget's drag sensitivity.
      app.behavior->set_float(sess, ba, hwheel, "step",   0.02f);
      app.behavior->set_float(sess, ba, hkeys,  "step",   0.01f);
      app.behavior->set_float(sess, ba, hkeys,  "coarse", 0.10f);

      // Kind-routed: set_asset with a BEHAVIOR handle lands in the
      // widget's behavior slot, leaving the compound visual intact.
      app.widgets->set_asset(sess, cw_c, ba);
    }

    // ----------------------------------------------------------------
    // Image-knob demo: a fourth CUSTOMDRAW with its own compound built
    // from two bitmap layers (no shared visual with A / B / C). Layer 0
    // is the static knob shell; layer 1 is the moving overlay rotated by
    // a binding on NEUI_PARAM_VALUE. The artwork's resting pose is the
    // value=0 position (135deg CCW from neutral), so the rotation goes
    // 0 -> 1.5pi as value goes 0 -> 1 (270deg total sweep, value 0.5 lands
    // straight up). Input handler shape mirrors cw_c.
    if (app.behavior
        && app.img_knob_bg.id   != asset_none.id
        && app.img_knob_move.id != asset_none.id) {
      app.img_knob_compound = app.assets->create_compound(sess);
      if (app.img_knob_compound.id != asset_none.id) {
        neui_asset_t cs = app.img_knob_compound;

        // Background shell. FILL spans the whole widget; the asset draws
        // aspect-preserving inside that rect.
        auto bg_layer = app.compound->add_layer(sess, cs,
                                                  NEUI_COMPOUND_LAYER_ASSET, 0);
        app.compound->set_anchor(sess, cs, bg_layer,
                                   NEUI_ANCHOR_CENTER, NEUI_ANCHOR_CENTER);
        app.compound->set_int  (sess, cs, bg_layer, "width",  NEUI_COMPOUND_FILL);
        app.compound->set_int  (sess, cs, bg_layer, "height", NEUI_COMPOUND_FILL);
        app.compound->set_asset(sess, cs, bg_layer, "asset",  app.img_knob_bg);

        // Moving overlay, sized below the shell so it fits inside the dial.
        // The compound rotation runs around the layer's destination centre,
        // and CENTER-CENTER anchoring on a square layer puts that pivot at
        // the widget's centre. The shell bitmap carries bottom-edge shadow
        // padding (122x134), so its optical centre sits above the geometric
        // centre - shift the move layer up to land on the dial.
        auto mv_layer = app.compound->add_layer(sess, cs,
                                                  NEUI_COMPOUND_LAYER_ASSET, 1);
        app.compound->set_anchor(sess, cs, mv_layer,
                                   NEUI_ANCHOR_CENTER, NEUI_ANCHOR_CENTER);
        app.compound->set_int  (sess, cs, mv_layer, "width",  70);
        app.compound->set_int  (sess, cs, mv_layer, "height", 70);
        app.compound->set_int  (sess, cs, mv_layer, "offset_y", -15);
        app.compound->set_asset(sess, cs, mv_layer, "asset",  app.img_knob_move);
        // 270deg sweep: scale = 3pi/2, offset = 0. Positive rotation is CW
        // (renderer Y-down convention), matching "1.0 = 135deg to the right".
        const float three_half_pi = 4.71238898038f;
        app.compound->bind(sess, cs, mv_layer, "rotation",
                             NEUI_PARAM_VALUE, three_half_pi, 0.0f);
      }

      auto ik = app.widgets->create(sess, win, NEUI_W_CUSTOMDRAW,
                                      875, 273, 110, 110, nullptr);
      app.img_knob_widget_id = ik.id;
      app.widgets->set_asset(sess, ik, app.img_knob_compound);
      if (app.attrs) {
        app.attrs->set_float(sess, ik, NEUI_PARAM_VALUE,   0.5f);
        app.attrs->set_float(sess, ik, NEUI_PARAM_DEFAULT, 0.5f);
      }

      // Behavior: same four-handler bundle as cw_c (drag, wheel, keys,
      // right-click reset), writing NEUI_PARAM_VALUE clamped to [0, 1].
      neui_asset_t bk = app.assets->create_behavior(sess);
      app.img_knob_behavior = bk;
      neui_behavior_handler_t hdrag2 =
        app.behavior->add_handler(sess, bk, NEUI_BEHAVIOR_KIND_DRAG_ROTATIONAL);
      neui_behavior_handler_t hwheel2 =
        app.behavior->add_handler(sess, bk, NEUI_BEHAVIOR_KIND_WHEEL);
      neui_behavior_handler_t hkeys2 =
        app.behavior->add_handler(sess, bk, NEUI_BEHAVIOR_KIND_KEY_STEP);
      neui_behavior_handler_t hreset2 =
        app.behavior->add_handler(sess, bk, NEUI_BEHAVIOR_KIND_CONTEXT_RESET);
      neui_behavior_handler_t hs2[] = { hdrag2, hwheel2, hkeys2, hreset2 };
      for (auto h : hs2) {
        app.behavior->set_string(sess, bk, h, "target",         NEUI_PARAM_VALUE);
        app.behavior->set_string(sess, bk, h, "target_default", NEUI_PARAM_DEFAULT);
        app.behavior->set_float (sess, bk, h, "min", 0.0f);
        app.behavior->set_float (sess, bk, h, "max", 1.0f);
      }
      app.behavior->set_float(sess, bk, hwheel2, "step",   0.02f);
      app.behavior->set_float(sess, bk, hkeys2,  "step",   0.01f);
      app.behavior->set_float(sess, bk, hkeys2,  "coarse", 0.10f);
      app.widgets->set_asset(sess, ik, bk);
    }
  }

  // -------------------------------------------------------------------------
  // Compound layer-kinds demo. A single CUSTOMDRAW whose compound bundles
  // the three layer kinds added after the initial text + asset pair:
  //
  //   z=-1  RECT   - rounded backplate. Two-tone (fill + stroke) with a
  //                  corner_radius > 0 so the painter takes the rounded
  //                  4-arc path that fill+stroke'd in one go.
  //   z= 0  ASSET  - the same myimage.png used elsewhere, drawn with a
  //                  backend-level multiplicative tint so the source
  //                  pixels are recoloured at draw time (here a soft
  //                  green - any single bitmap can be recoloured this
  //                  way without a per-recolour source file).
  //   z=+1  PATH   - a chevron icon built with MOVE_TO / LINE_TO /
  //                  CLOSE in layer-local coordinates, fill + stroke.
  //                  Coords are in logical pixels with (0, 0) at the
  //                  layer rect's top-left.
  //
  // Static demo (no behavior / no rotation binding) - the focus is on
  // showing what each layer kind looks like, not on interactivity.
  if (app.assets && app.compound) {
    app.features_compound = app.assets->create_compound(sess);
    if (app.features_compound.id != asset_none.id) {
      neui_asset_t cs = app.features_compound;

      // Layer 1: rounded RECT backplate. fill_color = soft accent blue;
      // stroke_color = brighter accent; corner_radius = 14 px. Static
      // (no bindings); fills the widget via NEUI_COMPOUND_FILL.
      auto rect_layer = app.compound->add_layer(sess, cs,
                                                  NEUI_COMPOUND_LAYER_RECT, -1);
      app.compound->set_anchor(sess, cs, rect_layer,
                                 NEUI_ANCHOR_TOP_LEFT, NEUI_ANCHOR_TOP_LEFT);
      app.compound->set_int  (sess, cs, rect_layer, "width",         NEUI_COMPOUND_FILL);
      app.compound->set_int  (sess, cs, rect_layer, "height",        NEUI_COMPOUND_FILL);
      app.compound->set_int  (sess, cs, rect_layer, "fill_color",   (int)0x33336699);
      app.compound->set_int  (sess, cs, rect_layer, "stroke_color", (int)0xFF6699CC);
      app.compound->set_float(sess, cs, rect_layer, "stroke_width",  1.5f);
      app.compound->set_float(sess, cs, rect_layer, "corner_radius", 14.0f);

      // Layer 2: TINTED bitmap. Same myimage.png handle the customdraw
      // and the other compound use, but multiplied by a translucent
      // green here so the source pixels look completely different. The
      // backend runs its native multiplicative-tint primitive at draw
      // time (D2D1Tint effect on Windows, blend-mode multiply on macOS)
      // against the single per-(asset, ctx) GPU upload - changing the
      // tint colour costs nothing extra beyond the per-draw effect call.
      if (app.customdraw_bg.id != asset_none.id) {
        auto img_layer = app.compound->add_layer(sess, cs,
                                                   NEUI_COMPOUND_LAYER_ASSET, 0);
        app.compound->set_anchor(sess, cs, img_layer,
                                   NEUI_ANCHOR_CENTER, NEUI_ANCHOR_CENTER);
        app.compound->set_int  (sess, cs, img_layer, "width",   60);
        app.compound->set_int  (sess, cs, img_layer, "height",  60);
        app.compound->set_int  (sess, cs, img_layer, "tint",   (int)0xC080FF80);
        app.compound->set_asset(sess, cs, img_layer, "asset",  app.customdraw_bg);
      }

      // Layer 3: PATH chevron. A right-pointing chevron drawn in a
      // 20x20 layer-local box anchored to the bottom-right of the
      // widget. The painter pushes a transform to the layer rect
      // before replaying the command list, so the coordinates below
      // are in (0..20) units. fill_color = solid white at full alpha;
      // stroke_color = darker outline for a 1-px crisp edge.
      auto path_layer = app.compound->add_layer(sess, cs,
                                                  NEUI_COMPOUND_LAYER_PATH, 1);
      app.compound->set_anchor(sess, cs, path_layer,
                                 NEUI_ANCHOR_BOTTOM_RIGHT, NEUI_ANCHOR_BOTTOM_RIGHT);
      app.compound->set_int(sess, cs, path_layer, "width",  20);
      app.compound->set_int(sess, cs, path_layer, "height", 20);
      app.compound->set_int(sess, cs, path_layer, "offset_x", -6);
      app.compound->set_int(sess, cs, path_layer, "offset_y", -6);
      app.compound->set_int(sess, cs, path_layer, "fill_color",   (int)0xFFFFFFFF);
      app.compound->set_int(sess, cs, path_layer, "stroke_color", (int)0xFF202830);
      app.compound->set_float(sess, cs, path_layer, "stroke_width", 1.0f);
      /* Chevron: outer triangle (right-pointing >) with a notch carved
         out on the back-left edge for a flat-shouldered look. (Block comment,
         not //, so the trailing backslashes in the art don't trip -Wcomment.)

           (4, 3) -----+
                \      \
                 \      \
                  \      +  (15, 10)
                  /      /
                 /      /
           (4, 17) ----+

         Inner notch (clockwise so the even-odd or non-zero fill rule
         carves the interior) starts at (7, 8) -> (10, 10) -> (7, 12). */
      neui_path_cmd_t chev[] = {
        { NEUI_PATH_CMD_MOVE_TO, {  4.0f,  3.0f, 0, 0, 0 } },
        { NEUI_PATH_CMD_LINE_TO, { 15.0f, 10.0f, 0, 0, 0 } },
        { NEUI_PATH_CMD_LINE_TO, {  4.0f, 17.0f, 0, 0, 0 } },
        { NEUI_PATH_CMD_LINE_TO, {  9.0f, 10.0f, 0, 0, 0 } },
        { NEUI_PATH_CMD_CLOSE,   {  0.0f,  0.0f, 0, 0, 0 } },
      };
      app.compound->set_path(sess, cs, path_layer, chev,
                               (uint32_t)(sizeof(chev) / sizeof(chev[0])));

      auto fw = app.widgets->create(sess, win, NEUI_W_CUSTOMDRAW,
                                      790, 145, 85, 120, nullptr);
      app.features_widget_id = fw.id;
      app.widgets->set_asset(sess, fw, app.features_compound);
    }
  }

  // (The QR-code compound-layer demo lives inside the SECTION widget above,
  // driven by its input field + "Set QR code" button.)

#if 0
  // Enable event emission on interactive controls
  app.widgets->set_emit_events(sess, input,  true);
  app.widgets->set_emit_events(sess, button, true);
  app.widgets->set_emit_events(sess, list,   true);
  app.widgets->set_emit_events(sess, combo,  true);
  app.widgets->set_emit_events(sess, check,  true);
  app.widgets->set_emit_events(sess, check3, true);
  app.widgets->set_emit_events(sess, treev,  true);
#endif

  app.widgets->show(sess, win);
  app.widgets->set_focus(sess, input);

  neui->run(sess);  // calling the main loop

  if (app.compound_widget_a != 0)
    app.widgets->destroy(sess, neui_widget_t{ app.compound_widget_a });
  if (app.compound_widget_b != 0)
    app.widgets->destroy(sess, neui_widget_t{ app.compound_widget_b });
  if (app.img_knob_widget_id != 0)
    app.widgets->destroy(sess, neui_widget_t{ app.img_knob_widget_id });
  if (app.features_widget_id != 0)
    app.widgets->destroy(sess, neui_widget_t{ app.features_widget_id });
  if (app.assets && app.features_compound.id != asset_none.id)
    app.assets->destroy(sess, app.features_compound);
  if (app.assets && app.img_knob_compound.id != asset_none.id)
    app.assets->destroy(sess, app.img_knob_compound);
  if (app.assets && app.img_knob_behavior.id != asset_none.id)
    app.assets->destroy(sess, app.img_knob_behavior);
  if (app.assets && app.img_knob_bg.id != asset_none.id)
    app.assets->destroy(sess, app.img_knob_bg);
  if (app.assets && app.img_knob_move.id != asset_none.id)
    app.assets->destroy(sess, app.img_knob_move);
  if (app.assets && app.compound_shape.id != asset_none.id)
    app.assets->destroy(sess, app.compound_shape);
  if (app.assets && app.customdraw_bg.id != asset_none.id)
    app.assets->destroy(sess, app.customdraw_bg);
  if (app.assets && app.image_via_asset.id != asset_none.id)
    app.assets->destroy(sess, app.image_via_asset);
  app.widgets->destroy(sess, customdraw);
  app.widgets->destroy(sess, value_label);
  app.widgets->destroy(sess, knob2);
  app.widgets->destroy(sess, knob2_label);
  app.widgets->destroy(sess, knob_b);
  app.widgets->destroy(sess, knob);
  app.widgets->destroy(sess, knob_label);
  app.widgets->destroy(sess, slider2);
  app.widgets->destroy(sess, slider2_label);
  app.widgets->destroy(sess, slider);
  app.widgets->destroy(sess, slider_label);
  if (app.qr_widget_id != 0)
    app.widgets->destroy(sess, neui_widget_t{ app.qr_widget_id });
  if (app.assets && app.qr_compound.id != asset_none.id)
    app.assets->destroy(sess, app.qr_compound);
  app.widgets->destroy(sess, sec_btn);
  app.widgets->destroy(sess, sec_input);
  app.widgets->destroy(sess, section);
  app.widgets->destroy(sess, align_combo);
  app.widgets->destroy(sess, img);
  app.widgets->destroy(sess, treev);
  app.widgets->destroy(sess, tree_label);
  app.widgets->destroy(sess, check3);
  app.widgets->destroy(sess, check);
  app.widgets->destroy(sess, combo);
  app.widgets->destroy(sess, combo_label);
  app.widgets->destroy(sess, list);
  app.widgets->destroy(sess, list_label);
  app.widgets->destroy(sess, button);
  app.widgets->destroy(sess, input);
  app.widgets->destroy(sess, label);
  app.widgets->destroy(sess, menubar);
  neui->destroy(sess);
  return 0;
}
