// Accessibility-declaration harness (NEUI_API_A11Y), macOS / xpl host.
//
// Covers phase 6.1 of plans/accessibility.md: the client seam. Every setter is a
// store into the widget's ordinary attribute bag, so the whole surface is
// observable through NEUI_API_ATTRS - which is what this harness reads. Nothing
// test-only is added to the public API.
//
// What is worth pinning down here, because each one is a decision that could
// silently regress:
//
//   1. FEATURE DETECT - the interface exists on the crossplatform host and is
//      NULL on the native macOS host. That asymmetry is documented in the header
//      and is the single most likely way a client gets this wrong (on macOS
//      neui_get_api(NULL) returns the NATIVE host first).
//   2. CLEARING - NULL / "" / ROLE_DEFAULT / min==max / mask==0 / widget_none all
//      REMOVE the key so the framework's derived default comes back, rather than
//      storing an empty value that would override the default with nothing. Every
//      clearing path has its own check because they are separate code paths.
//   3. DEGENERATE INPUT - a min==max range would make a normalized->real mapping
//      divide by zero downstream, and an out-of-range or NaN normalized value
//      would have an AT announcing 103%. Both are handled at the door.
//   4. MASK DISCIPLINE - set_state stores only bits inside the mask, so a client
//      passing stray bits cannot plant state the model will never consult.
//   5. SESSION ISOLATION - a widget handle from another session is silently
//      dropped, per the house invariant.
//
// No window is shown, so this runs fast and without a GUI session, but it links
// the host and therefore cannot live in the Tier-1 suite. Built but not
// ctest-registered for consistency with the other harnesses; run
// ./tests/<config>/neui_a11y_smoke_macos.

#import <Foundation/Foundation.h>

#include <neui/neui.h>

#include <cmath>
#include <cstdio>
#include <cstring>

namespace {

int g_failures = 0;

void check(bool ok, const char* what)
{
  std::printf("%s  %s\n", ok ? "[ ok ]" : "[FAIL]", what);
  if (!ok) ++g_failures;
}

void check_eq_int(int32_t got, int32_t want, const char* what)
{
  bool ok = (got == want);
  std::printf("%s  %s\n", ok ? "[ ok ]" : "[FAIL]", what);
  if (!ok) { std::printf("        want %d, got %d\n", want, got); ++g_failures; }
}

void check_eq_f(float got, float want, const char* what)
{
  bool ok = std::fabs(got - want) < 0.0001f;
  std::printf("%s  %s\n", ok ? "[ ok ]" : "[FAIL]", what);
  if (!ok) { std::printf("        want %g, got %g\n", (double)want, (double)got); ++g_failures; }
}

void check_eq_str(const char* got, const char* want, const char* what)
{
  bool ok = got && want && !std::strcmp(got, want);
  std::printf("%s  %s\n", ok ? "[ ok ]" : "[FAIL]", what);
  if (!ok) {
    std::printf("        want \"%s\", got \"%s\"\n", want ? want : "(null)",
                got ? got : "(null)");
    ++g_failures;
  }
}

bool onevent(void*, neui_event_t*) { return false; }
neui_widget_client_t g_wc = { NEUI_VERSION, nullptr, onevent };
void* iface(void*, const char* n)
{ return std::strcmp(n, NEUI_API_WIDGETS) ? nullptr : (void*)&g_wc; }
neui_client_t g_client = { NEUI_VERSION, iface };

} // namespace

int main()
{
  @autoreleasepool {
    neui_init();

    // ---- 1. feature detection ---------------------------------------------
    neui_api_t* xpl = neui_get_api("neui.host.crossplatform");
    if (!xpl) { std::printf("FAIL: no crossplatform host\n"); return 1; }

    void* app = nullptr;
    neui_session_t sess = xpl->create_session(&g_client, &app);
    auto* w     = (neui_widget_api_t*)xpl->get_interface(sess, NEUI_API_WIDGETS);
    auto* attrs = (neui_attr_api_t*)  xpl->get_interface(sess, NEUI_API_ATTRS);
    auto* a11y  = (neui_a11y_api_t*)  xpl->get_interface(sess, NEUI_API_A11Y);
    check(w != nullptr && attrs != nullptr, "widgets + attrs interfaces present");
    check(a11y != nullptr, "NEUI_API_A11Y present on the crossplatform host");
    if (!w || !attrs || !a11y) { std::printf("\nA11Y FAILED (setup)\n"); return 1; }

    // The documented trap: the NATIVE macOS host does not expose this, and it is
    // what neui_get_api(NULL) hands back first on macOS.
    if (neui_api_t* native = neui_get_api("neui.host.macos")) {
      neui_session_t nsess = native->create_session(&g_client, &app);
      check(native->get_interface(nsess, NEUI_API_A11Y) == nullptr,
            "NEUI_API_A11Y is NULL on the native macOS host (feature-detect trap)");
      native->endsession(nsess);
    } else {
      std::printf("[ -- ]  native macOS host not linked; skipping its NULL check\n");
    }

    // A frame + a couple of widgets. Nothing is shown: declarations must work
    // before realization, since an AT can query a window at any point.
    neui_widget_t win = w->create(sess, widget_none, NEUI_W_APPWINDOW,
                                  60, 60, 320, 200, nullptr);
    neui_widget_t knob = w->create(sess, win, NEUI_W_CUSTOMDRAW, 20, 20, 60, 60, nullptr);
    neui_widget_t label = w->create(sess, win, NEUI_W_LABEL, 20, 100, 90, 20, nullptr);
    neui_widget_t input = w->create(sess, win, NEUI_W_INPUTBOX, 120, 100, 160, 24, nullptr);

    // ---- 2. role: store, then clear via ROLE_DEFAULT -----------------------
    a11y->set_role(sess, knob, NEUI_A11Y_ROLE_SLIDER);
    check(attrs->has(sess, knob, NEUI_ATTR_A11Y_ROLE) != 0, "set_role stores a role");
    check_eq_int(attrs->get_int(sess, knob, NEUI_ATTR_A11Y_ROLE, -1),
                 (int32_t)NEUI_A11Y_ROLE_SLIDER, "the stored role is the one declared");
    a11y->set_role(sess, knob, NEUI_A11Y_ROLE_DEFAULT);
    check(attrs->has(sess, knob, NEUI_ATTR_A11Y_ROLE) == 0,
          "ROLE_DEFAULT REMOVES the key (restores derivation, not role 0)");
    a11y->set_role(sess, knob, NEUI_A11Y_ROLE_SLIDER);   // put it back

    // ROLE_NONE is a real declaration (prune me), not a clear - it must persist.
    a11y->set_role(sess, label, NEUI_A11Y_ROLE_NONE);
    check_eq_int(attrs->get_int(sess, label, NEUI_ATTR_A11Y_ROLE, -1),
                 (int32_t)NEUI_A11Y_ROLE_NONE,
                 "ROLE_NONE persists (it means 'prune', not 'derive')");

    // ---- 3. name / description: store, then clear both ways ---------------
    a11y->set_name(sess, knob, "Cutoff");
    check_eq_str(attrs->get_string(sess, knob, NEUI_ATTR_A11Y_NAME), "Cutoff",
                 "set_name stores the name");
    a11y->set_name(sess, knob, "");
    check(attrs->has(sess, knob, NEUI_ATTR_A11Y_NAME) == 0,
          "set_name(\"\") removes the key rather than storing an empty name");
    a11y->set_name(sess, knob, "Cutoff");
    a11y->set_name(sess, knob, nullptr);
    check(attrs->has(sess, knob, NEUI_ATTR_A11Y_NAME) == 0,
          "set_name(NULL) also removes the key");
    a11y->set_name(sess, knob, "Cutoff");

    a11y->set_description(sess, knob, "Low-pass corner frequency");
    check_eq_str(attrs->get_string(sess, knob, NEUI_ATTR_A11Y_DESCRIPTION),
                 "Low-pass corner frequency", "set_description stores");

    // ---- 4. value range, including the degenerate case --------------------
    a11y->set_value_range(sess, knob, -60.0f, 6.0f, 0.5f);
    check_eq_f(attrs->get_float(sess, knob, NEUI_ATTR_A11Y_RANGE_MIN, 0.0f),
               -60.0f, "range min stored");
    check_eq_f(attrs->get_float(sess, knob, NEUI_ATTR_A11Y_RANGE_MAX, 0.0f),
               6.0f, "range max stored");
    check_eq_f(attrs->get_float(sess, knob, NEUI_ATTR_A11Y_RANGE_STEP, 0.0f),
               0.5f, "range step stored");
    // min == max: would divide by zero in any normalized->real mapping.
    a11y->set_value_range(sess, knob, 1.0f, 1.0f, 0.0f);
    check(attrs->has(sess, knob, NEUI_ATTR_A11Y_RANGE_MIN) == 0 &&
          attrs->has(sess, knob, NEUI_ATTR_A11Y_RANGE_MAX) == 0 &&
          attrs->has(sess, knob, NEUI_ATTR_A11Y_RANGE_STEP) == 0,
          "min == max clears ALL THREE range keys (no divide-by-zero downstream)");
    a11y->set_value_range(sess, knob, -60.0f, 6.0f, 0.5f);

    // ---- 5. value clamping ------------------------------------------------
    a11y->set_value(sess, knob, 0.42f);
    check_eq_f(attrs->get_float(sess, knob, NEUI_ATTR_A11Y_VALUE, -1.0f), 0.42f,
               "set_value stores a normalized value");
    a11y->set_value(sess, knob, 1.7f);
    check_eq_f(attrs->get_float(sess, knob, NEUI_ATTR_A11Y_VALUE, -1.0f), 1.0f,
               "set_value clamps above 1.0");
    a11y->set_value(sess, knob, -0.3f);
    check_eq_f(attrs->get_float(sess, knob, NEUI_ATTR_A11Y_VALUE, -1.0f), 0.0f,
               "set_value clamps below 0.0");
    a11y->set_value(sess, knob, std::nanf(""));
    check_eq_f(attrs->get_float(sess, knob, NEUI_ATTR_A11Y_VALUE, -1.0f), 0.0f,
               "set_value turns NaN into 0 rather than storing NaN");

    // ---- 6. value text ----------------------------------------------------
    a11y->set_value_text(sess, knob, "-6.0 dB");
    check_eq_str(attrs->get_string(sess, knob, NEUI_ATTR_A11Y_VALUE_TEXT),
                 "-6.0 dB", "set_value_text stores the spoken string");
    a11y->set_value_text(sess, knob, nullptr);
    check(attrs->has(sess, knob, NEUI_ATTR_A11Y_VALUE_TEXT) == 0,
          "set_value_text(NULL) clears");

    // ---- 7. state mask discipline -----------------------------------------
    // Pass a bit OUTSIDE the mask: it must not be stored, because the model only
    // consults masked bits and storing it would be silent noise.
    a11y->set_state(sess, knob,
                    NEUI_A11Y_STATE_CHECKED,
                    NEUI_A11Y_STATE_CHECKED | NEUI_A11Y_STATE_DISABLED);
    check_eq_int(attrs->get_int(sess, knob, NEUI_ATTR_A11Y_STATE_MASK, 0),
                 (int32_t)NEUI_A11Y_STATE_CHECKED, "state mask stored as given");
    check_eq_int(attrs->get_int(sess, knob, NEUI_ATTR_A11Y_STATE_VALUES, 0),
                 (int32_t)NEUI_A11Y_STATE_CHECKED,
                 "state values are masked (the out-of-mask DISABLED bit is dropped)");
    a11y->set_state(sess, knob, 0, 0);
    check(attrs->has(sess, knob, NEUI_ATTR_A11Y_STATE_MASK) == 0 &&
          attrs->has(sess, knob, NEUI_ATTR_A11Y_STATE_VALUES) == 0,
          "mask == 0 clears both state keys (restores full derivation)");

    // ---- 8. labelled_by ---------------------------------------------------
    a11y->set_labelled_by(sess, input, label);
    check_eq_int(attrs->get_int(sess, input, NEUI_ATTR_A11Y_LABELLED_BY, 0),
                 (int32_t)label.id, "set_labelled_by stores the label's id");
    a11y->set_labelled_by(sess, input, widget_none);
    check(attrs->has(sess, input, NEUI_ATTR_A11Y_LABELLED_BY) == 0,
          "widget_none clears labelled_by");
    // A self-reference can only be a caller bug; rejecting it at the door keeps
    // the stored data trustworthy instead of relying on the model's cycle guard.
    a11y->set_labelled_by(sess, input, input);
    check(attrs->has(sess, input, NEUI_ATTR_A11Y_LABELLED_BY) == 0,
          "a self-reference is rejected, not stored");

    // ---- 9. session isolation + no-op surface -----------------------------
    neui_session_t sess2 = xpl->create_session(&g_client, &app);
    neui_widget_t win2 = w->create(sess2, widget_none, NEUI_W_APPWINDOW,
                                    0, 0, 100, 100, nullptr);
    // Declaring on sess2's widget through sess1 must be dropped silently.
    a11y->set_name(sess, win2, "wrong session");
    check(attrs->has(sess2, win2, NEUI_ATTR_A11Y_NAME) == 0,
          "a cross-session widget handle is silently dropped");

    // The read-only / notification surface must be callable and honest before
    // the provider exists (6.3), not crash and not claim an AT is listening.
    check(a11y->is_active(sess) == false,
          "is_active is false with no provider attached");
    a11y->notify(sess, knob, NEUI_A11Y_CHANGE_VALUE);
    a11y->notify(sess, knob, NEUI_A11Y_CHANGE_STRUCTURE);
    a11y->announce(sess, "saved", false);
    a11y->announce(sess, nullptr, true);      // must tolerate NULL
    check(true, "notify / announce are callable no-ops before the provider lands");

    // Declarations survive on a widget whose frame was never shown - the point
    // of storing them in the attribute bag.
    check_eq_str(attrs->get_string(sess, knob, NEUI_ATTR_A11Y_NAME), "Cutoff",
                 "declarations persist on an unrealized frame's widget");

    w->destroy(sess2, win2);
    xpl->endsession(sess2);
    w->destroy(sess, win);
    xpl->endsession(sess);

    std::printf(g_failures ? "\nA11Y FAILED (%d)\n" : "\nA11Y OK\n", g_failures);
    return g_failures ? 1 : 0;
  }
}
