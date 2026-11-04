#include "neui_test.h"

#include "ios/ios_api.h"

// iOS-only Tier 1 checks. Compiled into neui_tests only on an iOS build
// (tests/CMakeLists.txt), where CI installs the suite on a simulator, launches
// it and scrapes the summary - so these run on every push.
//
// SCOPE: this bundle has a main() from test_main.cpp, not UIApplicationMain, so
// there is no UIApplication and no window. That rules out anything frame-scoped
// (status bar, home indicator, orientation, keyboard) - those are exercised by
// examples/ios. What IS reachable is every process-global query, which is most
// of the "device, power & environment" half of the interface, plus the
// session-state bookkeeping, which is pure C++ behind the UIKit calls.

using namespace neui_detail;

TEST_CASE("ios device: the idle-timer hold is refcounted per session")
{
  // The contract d/ios.h states: two components can hold the screen awake
  // without one's release cancelling the other's.
  const neui_session_t a = { 4001 };
  const neui_session_t b = { 4002 };

  CHECK_EQ(ios_idle_timer_disabled(a), 0);

  ios_set_idle_timer_disabled(a, 1);
  ios_set_idle_timer_disabled(a, 1);          // a second, independent hold
  CHECK_EQ(ios_idle_timer_disabled(a), 1);

  ios_set_idle_timer_disabled(a, 0);          // one release is not enough
  CHECK_EQ(ios_idle_timer_disabled(a), 1);

  ios_set_idle_timer_disabled(a, 0);
  CHECK_EQ(ios_idle_timer_disabled(a), 0);

  // Releasing past zero must not go negative, or the next hold would not take.
  ios_set_idle_timer_disabled(a, 0);
  ios_set_idle_timer_disabled(a, 1);
  CHECK_EQ(ios_idle_timer_disabled(a), 1);

  // Sessions are independent.
  CHECK_EQ(ios_idle_timer_disabled(b), 0);

  ios_session_shutdown(a);
  CHECK_EQ(ios_idle_timer_disabled(a), 0);    // teardown drops the hold
}

TEST_CASE("ios device: frame chrome is remembered, and forgotten on destroy")
{
  const neui_widget_t frame = { 0x00010002 };

  // Nothing set: the view-controller overrides must see no state and fall
  // through to super rather than forcing a default.
  ios_frame_forget(frame);
  CHECK(ios_frame_state_if_present(frame) == nullptr);

  // Defaults before anything is set, so a frame that only sets the status bar
  // does not accidentally restrict its orientations.
  CHECK_EQ((int)ios_supported_orientations(neui_session_t{ 0 }, frame),
           (int)NEUI_IOS_ORIENTATION_ALL);

  ios_frame_state(frame).deferring_edges = NEUI_IOS_EDGE_BOTTOM;
  ios_frame_state(frame).supported_orientations = NEUI_IOS_ORIENTATION_ALL_LANDSCAPE;
  const IosFrameState* st = ios_frame_state_if_present(frame);
  CHECK(st != nullptr);
  CHECK_EQ((int)st->deferring_edges, (int)NEUI_IOS_EDGE_BOTTOM);
  CHECK_EQ((int)ios_supported_orientations(neui_session_t{ 0 }, frame),
           (int)NEUI_IOS_ORIENTATION_ALL_LANDSCAPE);

  // Widget ids are recycled: a destroyed frame must not leave its chrome behind
  // for whoever gets the id next.
  ios_frame_forget(frame);
  CHECK(ios_frame_state_if_present(frame) == nullptr);
  CHECK_EQ((int)ios_supported_orientations(neui_session_t{ 0 }, frame),
           (int)NEUI_IOS_ORIENTATION_ALL);
}

TEST_CASE("ios device: UIKit translations map every value")
{
  CHECK_EQ((int)ios_uikit_rect_edge(NEUI_IOS_EDGE_NONE), (int)UIRectEdgeNone);
  CHECK_EQ((int)ios_uikit_rect_edge(NEUI_IOS_EDGE_ALL), (int)UIRectEdgeAll);
  CHECK_EQ((int)ios_uikit_rect_edge(NEUI_IOS_EDGE_BOTTOM), (int)UIRectEdgeBottom);

  // An empty mask must not lock the app to nothing - it falls back to "all".
  CHECK_EQ((int)ios_uikit_orientation_mask(0), (int)UIInterfaceOrientationMaskAll);
  CHECK_EQ((int)ios_uikit_orientation_mask(NEUI_IOS_ORIENTATION_PORTRAIT),
           (int)UIInterfaceOrientationMaskPortrait);
  CHECK_EQ((int)ios_uikit_orientation_mask(NEUI_IOS_ORIENTATION_ALL_LANDSCAPE),
           (int)UIInterfaceOrientationMaskLandscape);

  CHECK_EQ((int)ios_uikit_status_bar_style(NEUI_IOS_STATUS_BAR_DEFAULT),
           (int)UIStatusBarStyleDefault);
  CHECK_EQ((int)ios_uikit_status_bar_style(NEUI_IOS_STATUS_BAR_LIGHT),
           (int)UIStatusBarStyleLightContent);
}

TEST_CASE("ios device: the process-global queries answer sanely")
{
  const neui_session_t s = { 0 };

  // Idiom: the simulator is a phone or a pad, never unknown.
  const neui_ios_idiom_t idiom = ios_idiom(s);
  CHECK(idiom == NEUI_IOS_IDIOM_PHONE || idiom == NEUI_IOS_IDIOM_PAD ||
        idiom == NEUI_IOS_IDIOM_MAC);

  int major = 0, minor = -1, patch = -1;
  ios_os_version(s, &major, &minor, &patch);
  CHECK(major >= 11);          // the oldest version anything here guards for
  CHECK(minor >= 0);
  CHECK(patch >= 0);

  // Out pointers may be NULL - the header says so, so it must be true.
  ios_os_version(s, nullptr, nullptr, nullptr);

  const char* model = ios_device_model(s);
  CHECK(model != nullptr);
  CHECK(model[0] != '\0');
  // Cached: the header promises a pointer valid for the process lifetime.
  CHECK(model == ios_device_model(s));

  CHECK(ios_low_power_mode(s) == 0 || ios_low_power_mode(s) == 1);

  const neui_ios_thermal_t t = ios_thermal_state(s);
  CHECK(t >= NEUI_IOS_THERMAL_NOMINAL && t <= NEUI_IOS_THERMAL_CRITICAL);

  // Every raised bit must be one we define.
  const uint32_t known = NEUI_IOS_A11Y_REDUCE_MOTION | NEUI_IOS_A11Y_REDUCE_TRANSPARENCY |
                         NEUI_IOS_A11Y_BOLD_TEXT | NEUI_IOS_A11Y_DARKER_COLORS |
                         NEUI_IOS_A11Y_VOICE_OVER | NEUI_IOS_A11Y_SWITCH_CONTROL;
  CHECK_EQ((int)(ios_accessibility_flags(s) & ~known), 0);

  // The category is whatever the device is set to, but it must be in range.
  const neui_ios_content_size_t c = ios_content_size_category(s);
  CHECK(c >= NEUI_IOS_CONTENT_SIZE_UNKNOWN &&
        c <= NEUI_IOS_CONTENT_SIZE_ACCESSIBILITY_XXXL);
}

TEST_CASE("ios device: battery and brightness report or admit they cannot")
{
  const neui_session_t s = { 0 };

  // The simulator has no battery. Either a real 0..1 reading or the documented
  // -1 is correct; anything else is not.
  const float level = ios_battery_level(s);
  CHECK(level == -1.0f || (level >= 0.0f && level <= 1.0f));

  const neui_ios_battery_t bs = ios_battery_state(s);
  CHECK(bs >= NEUI_IOS_BATTERY_UNKNOWN && bs <= NEUI_IOS_BATTERY_FULL);

  const float b = ios_screen_brightness(s);
  CHECK(b == -1.0f || (b >= 0.0f && b <= 1.0f));
}

TEST_CASE("ios device: a haptic on hardware without one is a silent no-op")
{
  // The header says UIKit reports no outcome and neui adds no policy. The only
  // thing to assert is that asking does not crash where there is no engine.
  const neui_session_t s = { 0 };
  ios_haptic(s, NEUI_IOS_HAPTIC_SELECTION);
  ios_haptic(s, NEUI_IOS_HAPTIC_LIGHT);
  ios_haptic(s, NEUI_IOS_HAPTIC_SUCCESS);
  CHECK(true);
}

TEST_CASE("ios device: the vtable is fully populated")
{
  // The interface is never handed out inert - a host that returns it implements
  // every method. A null slot would be a caller crash, not a graceful absence.
  const neui_ios_api_t* a = &k_ios_api;
  CHECK_EQ((int)a->neui_version, (int)NEUI_VERSION);
  CHECK(a->set_idle_timer_disabled        != nullptr);
  CHECK(a->idle_timer_disabled            != nullptr);
  CHECK(a->set_deferring_system_gestures  != nullptr);
  CHECK(a->set_home_indicator_auto_hidden != nullptr);
  CHECK(a->screen_brightness              != nullptr);
  CHECK(a->set_screen_brightness          != nullptr);
  CHECK(a->set_status_bar                 != nullptr);
  CHECK(a->orientation                    != nullptr);
  CHECK(a->set_supported_orientations     != nullptr);
  CHECK(a->supported_orientations         != nullptr);
  CHECK(a->set_user_interface_style       != nullptr);
  CHECK(a->content_size_category          != nullptr);
  CHECK(a->accessibility_flags            != nullptr);
  CHECK(a->idiom                          != nullptr);
  CHECK(a->os_version                     != nullptr);
  CHECK(a->device_model                   != nullptr);
  CHECK(a->battery_level                  != nullptr);
  CHECK(a->battery_state                  != nullptr);
  CHECK(a->low_power_mode                 != nullptr);
  CHECK(a->thermal_state                  != nullptr);
  CHECK(a->haptic                         != nullptr);
  CHECK(a->keyboard_inset                 != nullptr);
}

TEST_CASE("ios device: with no keyboard up, the inset is zero")
{
  // No window in this bundle, so the lookup finds no view - which must read as
  // "nothing covered", not as a crash or a garbage inset.
  ios_keyboard_frame() = CGRectZero;
  CHECK_EQ(ios_keyboard_inset(neui_session_t{ 0 }, neui_widget_t{ 0 }), 0);
}
