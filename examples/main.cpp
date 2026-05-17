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
#ifdef _WIN32
#define ACTIVE_HOSTx "neui.host.crossplatform"
#define ACTIVE_HOST "neui.host.win32"
#elif defined(__APPLE__)
#define ACTIVE_HOSTx "neui.host.crossplatform"
#define ACTIVE_HOST "neui.host.macos"
#else
// Other platforms only have the crossplatform host today.
#define ACTIVE_HOST "neui.host.crossplatform"
#endif

// Forward declaration: registers the crossplatform host with the neui registry.
// Calling this also forces the linker to include the neui-xplhost static library.
extern "C" void neui_register_xplhost();
#ifdef __APPLE__
// Native macOS host (hosts/macos/). Forced reference for the static-lib pull-in;
// linked alongside the xpl host on macOS so users can switch ACTIVE_HOST at
// compile time. Step 1 scaffold today - no widget impls yet.
extern "C" void neui_register_macoshost();
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
// neui_session_t / neui_widget_t have const members and cannot be copy-assigned,
// so we store the raw IDs and reconstruct the handles when calling the API.
struct AppState {
  neui_api_t*        neui      = nullptr;
  neui_widget_api_t* widgets   = nullptr;
  neui_items_api_t*  items     = nullptr;
  neui_tree_api_t*   tree      = nullptr;
  neui_attr_api_t*   attrs     = nullptr;
  uint32_t           session   = 0;
  uint32_t           win_id    = 0;
  uint32_t           input_id  = 0;
  uint32_t           button_id = 0;
  uint32_t           list_id   = 0;
  uint32_t           combo_id  = 0;
  uint32_t           check_id  = 0;
  uint32_t           check3_id = 0;
  uint32_t           menubar_id = 0;
  uint32_t           treev_id  = 0;
  uint32_t           slider_id  = 0;
  uint32_t           slider2_id = 0;   // 16-step variant
  uint32_t           knob_id    = 0;
  uint32_t           knob2_id   = 0;   // 16-step variant
  uint32_t           rot_slider_id  = 0;   // controls the rotating image below
  uint32_t           rot_image_id   = 0;
  uint32_t           value_label_id = 0;
  // Modal "About" dialog (created on demand by Help > About).
  uint32_t           about_dlg_id = 0;
  uint32_t           about_ok_id  = 0;
  // SECTION demo: a non-interactive coloured backdrop with an optional
  // header label; children paint on top via normal tree traversal.
  uint32_t           section_id    = 0;
  uint32_t           section_btn_id = 0;
  // Combobox that drives the section's NEUI_ATTR_ALIGN_TEXT attribute,
  // letting the user pick which side the title chip sits on.
  uint32_t           align_combo_id = 0;
  // Combobox above the rotation slider that swaps the rotating IMAGE's
  // source file at runtime (Lemur / Lion / Panda).
  uint32_t           image_combo_id = 0;
};

// Open a modal "About" dialog owned by the main window. The dialog has a
// short label and a single OK button that destroys the dialog (which
// re-enables the owner). Idempotent - does nothing if already open.
static void open_about_dialog(AppState* app)
{
  if (!app || app->about_dlg_id != 0) return;  // already open
  neui_session_t sess  = { app->session };
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
        dbglog("section button clicked - section is non-interactive, children still work\n");
        return true;
      }
      if (app && event->data.mouse.widget.id == app->button_id) {
        neui_session_t sess  = { app->session };
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
      // OK button inside the modal "About" dialog: dismiss the dialog.
      if (app && app->about_dlg_id != 0 &&
          event->data.mouse.widget.id == app->about_ok_id) {
        neui_session_t sess = { app->session };
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
      neui_session_t sess   = { app->session };
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

    case NEUI_EVENT_WIDGET_PREUPDATE: {
      // Refresh the knob's NEUI_ATTR_VALUE_TEXT just before paint reads it.
      // This is the recommended pattern: the framework "pulls" updated
      // display text from the client right before drawing, so the overlay
      // never lags the value.
      if (!app || !app->attrs) return false;
      uint32_t wid = event->data.preupdate.widget.id;
      if (wid == app->knob_id || wid == app->knob2_id) {
        neui_session_t sess = { app->session };
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
                       : (wid == app->knob2_id)      ? "knob16"
                       : (wid == app->rot_slider_id) ? "rotation"
                                                     : "?";
      dbglog("value changed: widget=0x%08x (%s) value=%.3f\n",
             wid, which, v);
      if (app->value_label_id != 0 && app->widgets) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%s: %.2f", which, v);
        neui_session_t sess  = { app->session };
        neui_widget_t  label = { app->value_label_id };
        app->widgets->set_text(sess, label, buf);
      }
      // Rotation slider drives the image's NEUI_ATTR_ROTATION attribute.
      // The framework's transform stack picks it up on the next paint.
      if (wid == app->rot_slider_id && app->rot_image_id != 0 && app->attrs) {
        neui_session_t sess  = { app->session };
        neui_widget_t  image = { app->rot_image_id };
        const float two_pi = 6.28318530717958647692f;
        app->attrs->set_float(sess, image, NEUI_ATTR_ROTATION, v * two_pi);
      }
      return true;
    }

    case NEUI_EVENT_TREE_ITEM_SELECTED: {
      if (!app) return true;
      neui_session_t sess   = { app->session };
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
      neui_session_t sess   = { app->session };
      neui_widget_t  widget = event->data.tree.widget;
      neui_item_t    item   = event->data.tree.item;
      char buf[256] = {};
      app->tree->get_text(sess, widget, item, buf, (int)sizeof(buf));
      void* ud = app->tree->get_userdata(sess, widget, item);
      dbglog("tree item activated: \"%s\" -> %p\n", buf, ud);
      if (ud == (void*)4 && app->neui)  // Exit
        app->neui->endsession(sess);
      if (ud == (void*)20)              // Help > About → open modal dialog
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

  neui_hello();
  neui_register_xplhost();
#ifdef __APPLE__
  neui_register_macoshost();
#endif
  auto neui = neui_get_api(ACTIVE_HOST);

  // Pass &app as token so callbacks can access widget handles and the widget API
  auto sess = neui->create_session(&host_client, &app);
  app.neui    = neui;
  app.session = sess.session;
  app.widgets = (neui_widget_api_t*)neui->get_interface(sess, NEUI_API_WIDGETS);
  app.items   = (neui_items_api_t*) neui->get_interface(sess, NEUI_API_ITEMS);
  app.tree    = (neui_tree_api_t*)  neui->get_interface(sess, NEUI_API_TREE);
  app.attrs   = (neui_attr_api_t*)  neui->get_interface(sess, NEUI_API_ATTRS);

  // Window: four columns - left = input + listbox, middle = combobox + checkboxes,
  //   right = treeview, far-right = slider + knob (continuous + 16-step variants).
  // Outer window dimensions sized so the client area has roughly the same
  // ~5px margin on right and bottom as it has on left and top:
  //   rightmost widget right edge: x=865  → client width ≈ 870
  //   bottommost widget bottom    : y=580  → client height ≈ 585
  // Win11 non-client chrome (resize borders + title bar + menu bar) is
  // about 16 horizontal + 58 vertical, so ≈ 890 × 645 outer.
  auto win = app.widgets->create(sess, widget_none, NEUI_W_APPWINDOW, 100, 100, 890, 645, nullptr);
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
  auto exit_item = app.tree->add(sess, menubar, file_menu,      "Exit", (void*)4);
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
  auto about_item = app.tree->add(sess, menubar, help_menu,      "About", (void*)20);

  // --- Left column (x=5, width=195) ---

  auto label  = app.widgets->create(sess, win, NEUI_W_LABEL,     5,   5, 195,  20, nullptr);
  auto input  = app.widgets->create(sess, win, NEUI_W_INPUTBOX,  5,  30, 195,  24, nullptr);
  auto button = app.widgets->create(sess, win, NEUI_W_BUTTON,    5,  60, 100,  28, nullptr);
  app.input_id  = input.id;
  app.button_id = button.id;

  app.widgets->set_text(sess, label,  "Type something below:");
  app.widgets->set_text(sess, button, "Read input");

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

  // Combo height = COMBO_COLLAPSED_H (22) + room for the 3-item dropdown.
  // Only the top 22 px is visually rendered when collapsed; siblings
  // sitting in the lower phantom area (the slider just below) are fine.
  auto image_combo = app.widgets->create(sess, win, NEUI_W_COMBOBOX,
                                          5, 313, 195, 76, nullptr);
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
  auto combo       = app.widgets->create(sess, win, NEUI_W_COMBOBOX,215, 30, 190, 150, nullptr);
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

  app.widgets->set_text(sess, check,  "Enable feature");
  app.widgets->set_text(sess, check3, "Allow override");

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
  // Combo height includes the dropdown overlay's vertical room (the
  // collapsed bar is COMBO_COLLAPSED_H = 22 px; the rest is reserved
  // for the open dropdown).
  auto align_combo = app.widgets->create(sess, win, NEUI_W_COMBOBOX,
                                          215, 320, 200, 80, nullptr);
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
  // top-left (the section is at frame-coords 215, 350, so (10, 28) below
  // lands at frame-coords (225, 378)). Same semantics on both hosts.
  auto sec_label = app.widgets->create(sess, section, NEUI_W_LABEL,
                                        10, 28, 180, 20, nullptr);
  app.widgets->set_text(sess, sec_label, "Children paint on top:");

  auto sec_btn = app.widgets->create(sess, section, NEUI_W_BUTTON,
                                      10, 56, 180, 28, nullptr);
  app.widgets->set_text(sess, sec_btn, "Click me");
  app.section_btn_id = sec_btn.id;

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

  app.widgets->destroy(sess, value_label);
  app.widgets->destroy(sess, knob2);
  app.widgets->destroy(sess, knob2_label);
  app.widgets->destroy(sess, knob);
  app.widgets->destroy(sess, knob_label);
  app.widgets->destroy(sess, slider2);
  app.widgets->destroy(sess, slider2_label);
  app.widgets->destroy(sess, slider);
  app.widgets->destroy(sess, slider_label);
  app.widgets->destroy(sess, sec_btn);
  app.widgets->destroy(sess, sec_label);
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
