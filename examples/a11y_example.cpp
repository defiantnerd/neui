// Accessibility demo (NEUI_API_A11Y).
//
// The point of this example is the shape of a PLUGIN UI: controls that are
// custom-painted, values that are normalized [0..1] internally but mean
// something in the real world, and labels that sit next to their controls rather
// than inside them. All three of those are invisible to an accessibility
// framework unless the client says something, and this file is the "something".
//
// Run it with VoiceOver on (Cmd-F5) and press VO-Right to walk the window. Then
// comment out the three declaration blocks marked DECLARE and listen again: the
// knob becomes "group", its value becomes nothing, and the text field is
// announced with no name at all. That difference is the whole feature.
//
// The three things worth copying, in the order they matter:
//
//   1. DECLARE A ROLE on every custom-painted control. A CUSTOMDRAW is a knob, a
//      meter, a toggle, an XY pad or pure decoration, and the framework refuses
//      to guess (guessing "slider" at a thing that is not one is worse than
//      saying "group"). This is the single highest-value call here.
//   2. DECLARE WHAT THE VALUE MEANS. "Zero point four two" tells a screen-reader
//      user nothing; "-14.6 dB" tells them everything. set_value_range maps the
//      normalized value onto real units; set_value_text overrides the string
//      outright when you want units or an enum name.
//   3. PAIR LABELS WITH CONTROLS. A LABEL to the left of an INPUTBOX is a layout
//      convention, not a relationship the framework can see - and inferring it
//      from proximity would be wrong often enough to be worse than nothing.
//
// Also shown: the decorative-background opt-out (ROLE_NONE prunes a node AND its
// subtree, so a screen-reader user does not walk through your chrome), and
// notify(), which a hand-painted control needs after changing its own state
// because the framework cannot see a value it does not own.
//
// NEUI_API_A11Y is a CROSSPLATFORM-HOST interface, like NEUI_API_TIMER /
// _POINTER / _EMBED. On Windows and macOS neui_get_api(NULL) hands back the
// NATIVE host first, which returns NULL for it - so this asks for the xpl host by
// name. See docs/accessibility.md for per-platform status (macOS reads this
// today; Windows is written but unverified; Linux has no provider yet).

#include <neui/neui.h>

#include <cmath>
#include <cstdio>
#include <cstring>

namespace {

// The client owns this value. That is the whole reason set_value / notify exist:
// the framework never sees it, so it cannot describe or announce it for us.
float g_cutoff_norm = 0.42f;

// -60 .. +6 dB, the range the knob really represents.
constexpr float kCutoffMinDb = -60.0f;
constexpr float kCutoffMaxDb = 6.0f;
constexpr float kCutoffStepDb = 1.0f;   // what one AT increment should move

float cutoff_db() { return kCutoffMinDb + (kCutoffMaxDb - kCutoffMinDb) * g_cutoff_norm; }

struct App {
  neui_session_t     sess{};
  neui_widget_api_t* w     = nullptr;
  neui_attr_api_t*   attrs = nullptr;
  neui_a11y_api_t*   a11y  = nullptr;

  neui_widget_t frame{};
  neui_widget_t backdrop{};        // decorative - declared ROLE_NONE
  neui_widget_t cutoff_knob{};     // CUSTOMDRAW + declared SLIDER role
  neui_widget_t cutoff_label{};
  neui_widget_t bypass{};          // CUSTOMDRAW + declared TOGGLE_BUTTON role
  neui_widget_t name_label{};
  neui_widget_t name_field{};
  neui_widget_t native_knob{};     // a built-in KNOB, for contrast
  neui_widget_t status{};
};

App g;
bool g_bypassed = false;

void refresh_status()
{
  char buf[160];
  std::snprintf(buf, sizeof buf, "cutoff %.1f dB   bypass %s",
                static_cast<double>(cutoff_db()), g_bypassed ? "on" : "off");
  g.w->set_text(g.sess, g.status, buf);
}

// Tell the accessibility layer about a value only WE know. Two calls, and both
// are needed: set_value updates what an AT would read, notify tells it to
// re-read. Built-in KNOB / SLIDER need neither - the framework owns their value
// and raises its own notifications.
void publish_cutoff()
{
  g.a11y->set_value(g.sess, g.cutoff_knob, g_cutoff_norm);
  char txt[32];
  std::snprintf(txt, sizeof txt, "%.1f dB", static_cast<double>(cutoff_db()));
  g.a11y->set_value_text(g.sess, g.cutoff_knob, txt);
  g.a11y->notify(g.sess, g.cutoff_knob, NEUI_A11Y_CHANGE_VALUE);
}

void publish_bypass()
{
  // A hand-painted toggle's checked-ness is not something the framework tracks,
  // so override just that bit and leave the rest (focus, enabled) derived. The
  // mask is why this does not freeze the others at whatever they happened to be.
  g.a11y->set_state(g.sess, g.bypass,
                    NEUI_A11Y_STATE_CHECKED,
                    g_bypassed ? NEUI_A11Y_STATE_CHECKED : 0u);
  g.a11y->notify(g.sess, g.bypass, NEUI_A11Y_CHANGE_STATE);
}

// ---- painting -------------------------------------------------------------

void paint_knob(neui_event_paint_t& p)
{
  auto* pa = p.painter_api; auto* h = p.p;
  const float cx = p.width * 0.5f, cy = p.height * 0.5f;
  const float r  = (p.width < p.height ? p.width : p.height) * 0.42f;
  pa->fill_ellipse(h, cx - r, cy - r, r * 2.0f, r * 2.0f, 0xFF2A2F3A);
  pa->draw_ellipse(h, cx - r, cy - r, r * 2.0f, r * 2.0f, 1.5f, 0xFF8892A6);
  // -135deg .. +135deg, the same sweep the built-in KNOB paints.
  const float a = (-135.0f + 270.0f * g_cutoff_norm) * 3.14159265f / 180.0f;
  pa->draw_line(h, cx, cy,
                cx + std::sin(a) * r * 0.82f,
                cy - std::cos(a) * r * 0.82f, 2.0f, 0xFF7FD1FF);
}

void paint_bypass(neui_event_paint_t& p)
{
  auto* pa = p.painter_api; auto* h = p.p;
  pa->fill_rect(h, 0, 0, p.width, p.height, g_bypassed ? 0xFF7FD1FF : 0xFF2A2F3A);
  pa->draw_rect(h, 0, 0, p.width, p.height, 1.0f, 0xFF8892A6);
  pa->draw_text(h, 0, p.height * 0.28f, p.width, p.height, "BYPASS", 11.0f,
                g_bypassed ? 0xFF102030 : 0xFFD8DEE9);
}

void paint_backdrop(neui_event_paint_t& p)
{
  auto* pa = p.painter_api; auto* h = p.p;
  pa->fill_rect(h, 0, 0, p.width, p.height, 0xFF1B1F27);
  for (float y = 8.0f; y < p.height; y += 8.0f)
    pa->fill_rect(h, 0, y, p.width, 1.0f, 0x18FFFFFF);
}

// ---- input ----------------------------------------------------------------

bool NEUI_ABI onevent(void* /*token*/, neui_event_t* ev)
{
  switch (ev->type) {
    case NEUI_EVENT_WIDGET_PAINT: {
      const uint32_t id = ev->data.paint.widget.id;
      if (id == g.cutoff_knob.id) { paint_knob(ev->data.paint);     return true; }
      if (id == g.bypass.id)      { paint_bypass(ev->data.paint);   return true; }
      if (id == g.backdrop.id)    { paint_backdrop(ev->data.paint); return true; }
      break;
    }

    // A declared role also makes an AT OFFER that role's actions, and for a
    // hand-painted control only the client can perform them. They arrive as
    // ordinary key events on the widget - SPACE for a press / toggle, arrows for
    // a step - so handling these keys is what makes the offered action real.
    // Ignore them and VoiceOver says "press" and nothing happens.
    case NEUI_EVENT_KEYDOWN: {
      const uint32_t id = ev->data.key.widget.id;
      if (id == g.cutoff_knob.id) {
        const float step = kCutoffStepDb / (kCutoffMaxDb - kCutoffMinDb);
        if (ev->data.key.keycode == NEUI_KEY_RIGHT ||
            ev->data.key.keycode == NEUI_KEY_UP) {
          g_cutoff_norm = g_cutoff_norm + step > 1.0f ? 1.0f : g_cutoff_norm + step;
        } else if (ev->data.key.keycode == NEUI_KEY_LEFT ||
                   ev->data.key.keycode == NEUI_KEY_DOWN) {
          g_cutoff_norm = g_cutoff_norm - step < 0.0f ? 0.0f : g_cutoff_norm - step;
        } else {
          break;
        }
        g.w->invalidate(g.sess, g.cutoff_knob);
        publish_cutoff();
        refresh_status();
        return true;
      }
      if (id == g.bypass.id && ev->data.key.keycode == NEUI_KEY_SPACE) {
        g_bypassed = !g_bypassed;
        g.w->invalidate(g.sess, g.bypass);
        publish_bypass();
        refresh_status();
        return true;
      }
      break;
    }

    case NEUI_EVENT_MOUSE_BUTTON_CLICK: {
      if (ev->data.mouse.widget.id == g.bypass.id) {
        g_bypassed = !g_bypassed;
        g.w->invalidate(g.sess, g.bypass);
        publish_bypass();
        refresh_status();
        return true;
      }
      break;
    }

    case NEUI_EVENT_MOUSE_WHEEL: {
      if (ev->data.wheel.widget.id == g.cutoff_knob.id) {
        const float step = kCutoffStepDb / (kCutoffMaxDb - kCutoffMinDb);
        g_cutoff_norm += (ev->data.wheel.delta > 0 ? step : -step);
        if (g_cutoff_norm < 0.0f) g_cutoff_norm = 0.0f;
        if (g_cutoff_norm > 1.0f) g_cutoff_norm = 1.0f;
        g.w->invalidate(g.sess, g.cutoff_knob);
        publish_cutoff();
        refresh_status();
        return true;
      }
      break;
    }

    case NEUI_EVENT_APP_QUIT: return true;
    default: break;
  }
  return false;
}

neui_widget_client_t g_widget_client = { NEUI_VERSION, nullptr, onevent };

void* NEUI_ABI get_iface(void* /*token*/, const char* name)
{
  if (!std::strcmp(name, NEUI_API_WIDGETS)) return &g_widget_client;
  return nullptr;
}

neui_client_t g_client = { NEUI_VERSION, get_iface };

} // namespace

int main()
{
  neui_init();
  // xpl host BY NAME: on macOS / Windows the default is the NATIVE host, which
  // returns NULL for NEUI_API_A11Y. This is the documented trap.
  neui_api_t* api = neui_get_api("neui.host.crossplatform");
  if (!api) { std::printf("no crossplatform host\n"); return 1; }

  g.sess  = api->create_session(&g_client, nullptr);
  g.w     = (neui_widget_api_t*)api->get_interface(g.sess, NEUI_API_WIDGETS);
  g.attrs = (neui_attr_api_t*)  api->get_interface(g.sess, NEUI_API_ATTRS);
  g.a11y  = (neui_a11y_api_t*)  api->get_interface(g.sess, NEUI_API_A11Y);
  // Feature-detect rather than assume: a host without a provider returns NULL,
  // and a client should still work (just without accessibility).
  if (!g.w || !g.attrs) { std::printf("missing interfaces\n"); return 1; }
  if (!g.a11y) { std::printf("NEUI_API_A11Y unavailable on this host\n"); return 1; }

  auto* w = g.w;

  // Content: 12 px margins, max child extent 396 x 232 -> a 420 x 260 client.
  g.frame = w->create(g.sess, widget_none, NEUI_W_APPWINDOW, 120, 120, 420, 260, nullptr);
  w->set_text(g.sess, g.frame, "neui - accessibility");

  // Decorative chrome behind the controls.
  g.backdrop = w->create(g.sess, g.frame, NEUI_W_CUSTOMDRAW, 12, 12, 396, 150, nullptr);
  // DECLARE 1: decorative. Prunes this node AND its subtree, so a screen-reader
  // user never walks through it. Prefer this over leaving a meaningless node in
  // the tree - navigating past chrome is a real cost.
  g.a11y->set_role(g.sess, g.backdrop, NEUI_A11Y_ROLE_NONE);

  // ---- the custom-painted knob ------------------------------------------
  g.cutoff_knob = w->create(g.sess, g.frame, NEUI_W_CUSTOMDRAW, 28, 34, 72, 72, nullptr);
  g.cutoff_label = w->create(g.sess, g.frame, NEUI_W_LABEL, 20, 112, 88, 20, nullptr);
  w->set_text(g.sess, g.cutoff_label, "Cutoff");

  // DECLARE 2: what this custom-painted thing IS, what its value MEANS, and
  // which label names it. Without the role it is a "group"; without the range
  // its value is a bare percentage; without labelled_by it has no name, because
  // the text lives in a separate LABEL widget.
  g.a11y->set_role(g.sess, g.cutoff_knob, NEUI_A11Y_ROLE_SLIDER);
  g.a11y->set_value_range(g.sess, g.cutoff_knob, kCutoffMinDb, kCutoffMaxDb,
                          kCutoffStepDb);
  g.a11y->set_labelled_by(g.sess, g.cutoff_knob, g.cutoff_label);
  g.a11y->set_description(g.sess, g.cutoff_knob,
                          "Low-pass filter cutoff. Arrow keys adjust by 1 dB.");
  publish_cutoff();      // seeds set_value + set_value_text

  // ---- the custom-painted toggle ----------------------------------------
  g.bypass = w->create(g.sess, g.frame, NEUI_W_CUSTOMDRAW, 124, 34, 84, 32, nullptr);
  // DECLARE 3: a toggle, named by its own painted text (which the framework
  // cannot read, since we drew it ourselves).
  g.a11y->set_role(g.sess, g.bypass, NEUI_A11Y_ROLE_TOGGLE_BUTTON);
  g.a11y->set_name(g.sess, g.bypass, "Bypass");
  publish_bypass();

  // ---- a LABEL + INPUTBOX pair -----------------------------------------
  g.name_label = w->create(g.sess, g.frame, NEUI_W_LABEL, 124, 78, 60, 20, nullptr);
  w->set_text(g.sess, g.name_label, "Preset");
  g.name_field = w->create(g.sess, g.frame, NEUI_W_INPUTBOX, 124, 100, 140, 24, nullptr);
  w->set_text(g.sess, g.name_field, "Init");
  // The LABEL's text becomes the field's accessible NAME, and the label itself
  // drops out of the tree so an AT does not read "Preset" twice.
  g.a11y->set_labelled_by(g.sess, g.name_field, g.name_label);

  // ---- a built-in KNOB, for contrast ------------------------------------
  // Declares NOTHING. The framework already knows it is a slider and owns its
  // value, so it is announced sensibly out of the box - only the real-world
  // range is worth adding, because normalized 0..1 means nothing to a listener.
  g.native_knob = w->create(g.sess, g.frame, NEUI_W_KNOB, 300, 34, 72, 72, nullptr);
  g.attrs->set_float(g.sess, g.native_knob, NEUI_PARAM_VALUE, 0.7f);
  g.a11y->set_name(g.sess, g.native_knob, "Resonance");
  g.a11y->set_value_range(g.sess, g.native_knob, 0.0f, 100.0f, 5.0f);

  g.status = w->create(g.sess, g.frame, NEUI_W_LABEL, 12, 172, 396, 22, nullptr);
  refresh_status();

  neui_widget_t hint = w->create(g.sess, g.frame, NEUI_W_LABEL, 12, 198, 396, 40, nullptr);
  w->set_text(g.sess, hint,
              "VoiceOver: Cmd-F5, then VO-Right to walk. The two left controls are "
              "CUSTOMDRAW - they read correctly only because of the declarations.");

  // is_active is ADVISORY - never gate correctness on it. It is false here until
  // something actually queries (macOS accessibility is lazy and has no attach
  // signal), so this only ever reports, never decides.
  std::printf("a11y active at startup: %s\n",
              g.a11y->is_active(g.sess) ? "yes" : "no");

  w->show(g.sess, g.frame);
  // Flushed because this example is routinely run with stdout redirected (and
  // then killed), where a block-buffered line would simply be lost.
  std::printf("window shown - walk it with VoiceOver (Cmd-F5)\n");
  std::fflush(stdout);
  api->run(g.sess);
  return 0;
}
