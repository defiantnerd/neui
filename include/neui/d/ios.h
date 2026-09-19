#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "api.h"
#include "events.h"

#ifdef __cplusplus
extern "C" {
#endif

// iOS / iPadOS host specialities: the parts of UIKit with no portable
// equivalent, and therefore no home elsewhere in the API. Keeping the screen
// awake through a set, stopping a thumb near the bezel from swiping the app
// away mid-song, asking whether the device dropped into Low Power Mode, firing
// a haptic tick.
//
// Exposed ONLY by the two iOS hosts - neui.host.ios (native UIKit) and
// neui.host.crossplatform on platform_ios.mm. Every other host returns NULL
// from get_interface, which is how a client feature-detects it:
//
//   neui_ios_api_t* ios = (neui_ios_api_t*)api->get_interface(sess, NEUI_API_IOS);
//   if (ios) ios->set_idle_timer_disabled(sess, 1);   // NULL everywhere else
//
// Never handed out inert: a host that returns it implements every method, and a
// call it cannot answer says so in its RETURN VALUE (documented per method).
//
// NOT here, deliberately: safe-area insets and the Dynamic Type SCALE are
// portable and already exist in NEUI_API_METRICS (d/metrics.h) - and
// get_client_rect already excludes the top inset. content_size_category() below
// adds the named CATEGORY, which a float cannot express; it does not replace
// ui_scale. NEUI_IOS_CHECKBOX_STYLE (d/attrs.h) stays a session key: it is a
// creation-time rendering choice, not a live setting.
//
// ANDROID: entries below are marked "Android: <counterpart>" for a future
// hosts/android - annotation only, nothing Android exists yet. "Android: none"
// means an iOS peculiarity that should NOT be promoted to a portable API.
//
// Threading: main thread only, like the rest of neui. UIKit requires it.
#define NEUI_API_IOS "com.defiantnerd.neui.extension.ios/0"

// ---------------------------------------------------------------------------
// Enumerations and bit flags.
//
// Explicitly numbered because they cross an ABI boundary. Reserved for future
// values: do NOT renumber; bind a new value to the next unused integer.

// Status-bar foreground style.
// Android: WindowInsetsController APPEARANCE_LIGHT_STATUS_BARS.
typedef enum neui_ios_status_bar {
  NEUI_IOS_STATUS_BAR_DEFAULT = 0,  // follow the system (dark content on light)
  NEUI_IOS_STATUS_BAR_LIGHT   = 1,  // light content, for a dark UI
  NEUI_IOS_STATUS_BAR_DARK    = 2,  // dark content, for a light UI
} neui_ios_status_bar_t;

// Interface orientation. The values are bit positions, so one enum serves both
// orientation() - which returns exactly one of them - and
// set_supported_orientations(), which takes an OR of them.
// Android: Display.getRotation() / Activity.setRequestedOrientation.
typedef enum neui_ios_orientation {
  NEUI_IOS_ORIENTATION_UNKNOWN              = 0,
  NEUI_IOS_ORIENTATION_PORTRAIT             = 1 << 0,
  NEUI_IOS_ORIENTATION_PORTRAIT_UPSIDE_DOWN = 1 << 1,
  NEUI_IOS_ORIENTATION_LANDSCAPE_LEFT       = 1 << 2,
  NEUI_IOS_ORIENTATION_LANDSCAPE_RIGHT      = 1 << 3,
} neui_ios_orientation_t;

// Convenience masks for set_supported_orientations.
enum {
  NEUI_IOS_ORIENTATION_ALL_PORTRAIT  = NEUI_IOS_ORIENTATION_PORTRAIT |
                                       NEUI_IOS_ORIENTATION_PORTRAIT_UPSIDE_DOWN,
  NEUI_IOS_ORIENTATION_ALL_LANDSCAPE = NEUI_IOS_ORIENTATION_LANDSCAPE_LEFT |
                                       NEUI_IOS_ORIENTATION_LANDSCAPE_RIGHT,
  NEUI_IOS_ORIENTATION_ALL           = NEUI_IOS_ORIENTATION_ALL_PORTRAIT |
                                       NEUI_IOS_ORIENTATION_ALL_LANDSCAPE,
};

// Screen edges, for set_deferring_system_gestures.
// Android: View.setSystemGestureExclusionRects - same intent, rects not edges.
enum {
  NEUI_IOS_EDGE_NONE   = 0,
  NEUI_IOS_EDGE_TOP    = 1 << 0,
  NEUI_IOS_EDGE_LEFT   = 1 << 1,
  NEUI_IOS_EDGE_BOTTOM = 1 << 2,
  NEUI_IOS_EDGE_RIGHT  = 1 << 3,
  NEUI_IOS_EDGE_ALL    = 0x0F,
};

// Forced appearance, overriding the device's Light/Dark setting.
// Android: AppCompatDelegate.setDefaultNightMode.
typedef enum neui_ios_style {
  NEUI_IOS_STYLE_SYSTEM = 0,  // follow the device setting (the default)
  NEUI_IOS_STYLE_LIGHT  = 1,
  NEUI_IOS_STYLE_DARK   = 2,
} neui_ios_style_t;

// Device idiom. Android: screen-size class / smallestScreenWidthDp.
typedef enum neui_ios_idiom {
  NEUI_IOS_IDIOM_UNKNOWN = 0,
  NEUI_IOS_IDIOM_PHONE   = 1,
  NEUI_IOS_IDIOM_PAD     = 2,
  NEUI_IOS_IDIOM_TV      = 3,
  NEUI_IOS_IDIOM_MAC     = 4,  // Catalyst, or "Designed for iPad" on Apple silicon
  NEUI_IOS_IDIOM_VISION  = 5,
} neui_ios_idiom_t;

// Haptic feedback kinds. The three families UIKit offers, flattened.
// Android: HapticFeedbackConstants / Vibrator (no notification family there).
typedef enum neui_ios_haptic {
  NEUI_IOS_HAPTIC_SELECTION = 0,  // UISelectionFeedbackGenerator - value ticked
  NEUI_IOS_HAPTIC_LIGHT     = 1,  // UIImpactFeedbackGenerator, .light
  NEUI_IOS_HAPTIC_MEDIUM    = 2,  // ... .medium
  NEUI_IOS_HAPTIC_HEAVY     = 3,  // ... .heavy
  NEUI_IOS_HAPTIC_SUCCESS   = 4,  // UINotificationFeedbackGenerator, .success
  NEUI_IOS_HAPTIC_WARNING   = 5,  // ... .warning
  NEUI_IOS_HAPTIC_ERROR     = 6,  // ... .error
} neui_ios_haptic_t;

// Thermal pressure. Android: PowerManager.getThermalStatus (API 29+).
typedef enum neui_ios_thermal {
  NEUI_IOS_THERMAL_NOMINAL  = 0,
  NEUI_IOS_THERMAL_FAIR     = 1,
  NEUI_IOS_THERMAL_SERIOUS  = 2,  // shed work: stop animating, drop frame rate
  NEUI_IOS_THERMAL_CRITICAL = 3,
} neui_ios_thermal_t;

// Battery charging state. Android: BatteryManager BATTERY_STATUS_*.
typedef enum neui_ios_battery {
  NEUI_IOS_BATTERY_UNKNOWN   = 0,  // also: monitoring unavailable
  NEUI_IOS_BATTERY_UNPLUGGED = 1,
  NEUI_IOS_BATTERY_CHARGING  = 2,
  NEUI_IOS_BATTERY_FULL      = 3,
} neui_ios_battery_t;

// Dynamic Type content-size category, ordered smallest to largest so a client
// can COMPARE: `cat >= NEUI_IOS_CONTENT_SIZE_ACCESSIBILITY_M` is the usual
// "this user wants the accessibility sizes, switch to a taller layout" test.
// That decision cannot be made from a scale factor, which is why this exists
// alongside metrics->ui_scale rather than instead of it.
// Android: Configuration.fontScale - a float, with no named steps.
typedef enum neui_ios_content_size {
  NEUI_IOS_CONTENT_SIZE_UNKNOWN            = 0,
  NEUI_IOS_CONTENT_SIZE_XS                 = 1,
  NEUI_IOS_CONTENT_SIZE_S                  = 2,
  NEUI_IOS_CONTENT_SIZE_M                  = 3,
  NEUI_IOS_CONTENT_SIZE_L                  = 4,  // the system default
  NEUI_IOS_CONTENT_SIZE_XL                 = 5,
  NEUI_IOS_CONTENT_SIZE_XXL                = 6,
  NEUI_IOS_CONTENT_SIZE_XXXL               = 7,
  NEUI_IOS_CONTENT_SIZE_ACCESSIBILITY_M    = 8,
  NEUI_IOS_CONTENT_SIZE_ACCESSIBILITY_L    = 9,
  NEUI_IOS_CONTENT_SIZE_ACCESSIBILITY_XL   = 10,
  NEUI_IOS_CONTENT_SIZE_ACCESSIBILITY_XXL  = 11,
  NEUI_IOS_CONTENT_SIZE_ACCESSIBILITY_XXXL = 12,
} neui_ios_content_size_t;

// Accessibility switches, as a bitmask returned by accessibility_flags().
// One call rather than six, because a client that cares usually reads several
// and they all change through the same notification.
enum {
  NEUI_IOS_A11Y_REDUCE_MOTION       = 1 << 0,  // Android: ANIMATOR_DURATION_SCALE == 0
  NEUI_IOS_A11Y_REDUCE_TRANSPARENCY = 1 << 1,  // Android: none
  NEUI_IOS_A11Y_BOLD_TEXT           = 1 << 2,  // Android: none
  NEUI_IOS_A11Y_DARKER_COLORS       = 1 << 3,  // Android: none
  NEUI_IOS_A11Y_VOICE_OVER          = 1 << 4,  // Android: TalkBack, via AccessibilityManager
  NEUI_IOS_A11Y_SWITCH_CONTROL      = 1 << 5,  // Android: Switch Access
};

// What moved, in neui_event_ios_env_t::changed. A bitmask rather than one
// event type per observable: these change independently and rarely, and a
// client re-reads only the ones it cares about.
enum {
  NEUI_IOS_ENV_ORIENTATION   = 1 << 0,
  NEUI_IOS_ENV_LOW_POWER     = 1 << 1,
  NEUI_IOS_ENV_THERMAL       = 1 << 2,
  NEUI_IOS_ENV_BATTERY       = 1 << 3,
  NEUI_IOS_ENV_ACCESSIBILITY = 1 << 4,
  NEUI_IOS_ENV_KEYBOARD      = 1 << 5,
};

// ---------------------------------------------------------------------------

// Frame-scoped calls take an APPWINDOW / PLUGWINDOW / DIALOG widget and are
// no-ops for anything else. Session-scoped calls take the session only; the
// underlying UIKit state is process-wide, and where that matters it is said
// so per method.
typedef struct neui_ios_api {
  uint32_t neui_version;

  // ---- Stage & session ----------------------------------------------------

  // Hold the screen awake (UIApplication.idleTimerDisabled). REFCOUNTED per
  // session: two components can hold it without one's release cancelling the
  // other's, and the hold is dropped when the session is destroyed - a session
  // that goes away never leaves the device awake. Nesting is per session, not
  // per process, so two sessions each hold their own.
  // Android: FLAG_KEEP_SCREEN_ON.
  void (NEUI_ABI *set_idle_timer_disabled)(neui_session_t session, int disabled);
  // This session's current hold state (not the process-wide flag).
  int  (NEUI_ABI *idle_timer_disabled)(neui_session_t session);

  // Which screen edges swallow the first swipe, so the system gesture (swipe-up
  // to home, pull-down for notifications) needs a second one. An OR of
  // NEUI_IOS_EDGE_*, or NEUI_IOS_EDGE_NONE to stop deferring. This does not
  // DISABLE the system gesture - iOS gives no way to - it only makes it
  // deliberate, which is what a full-screen control surface needs.
  // Android: View.setSystemGestureExclusionRects.
  void (NEUI_ABI *set_deferring_system_gestures)(neui_session_t session,
                                                 neui_widget_t frame,
                                                 uint32_t edges);

  // Let the home indicator fade out when the user stops interacting
  // (prefersHomeIndicatorAutoHidden). iOS decides when; this only permits it.
  // Android: immersive mode / WindowInsetsController.hide(navigationBars()).
  void (NEUI_ABI *set_home_indicator_auto_hidden)(neui_session_t session,
                                                  neui_widget_t frame,
                                                  int hidden);

  // Screen brightness, 0.0 to 1.0. Returns -1.0 if it cannot be read.
  // SYSTEM-WIDE and it outlives the process, so the host records the value it
  // found on the first set and restores it when the session is destroyed. Pass
  // a negative value to restore it explicitly, earlier.
  // Android: WindowManager.LayoutParams.screenBrightness (per window there).
  float (NEUI_ABI *screen_brightness)(neui_session_t session);
  void  (NEUI_ABI *set_screen_brightness)(neui_session_t session, float value);

  // ---- Chrome & orientation -----------------------------------------------

  // Status-bar style and visibility for a frame. Takes effect on the next
  // -setNeedsStatusBarAppearanceUpdate, which this issues.
  void (NEUI_ABI *set_status_bar)(neui_session_t session, neui_widget_t frame,
                                  neui_ios_status_bar_t style, int hidden);

  // The frame's current interface orientation: exactly one
  // NEUI_IOS_ORIENTATION_* value, or _UNKNOWN before the frame is realized.
  neui_ios_orientation_t (NEUI_ABI *orientation)(neui_session_t session,
                                                 neui_widget_t frame);

  // Restrict which orientations the frame will rotate to (an OR of
  // NEUI_IOS_ORIENTATION_*). This can only NARROW what the app's Info.plist
  // UISupportedInterfaceOrientations already allows - iOS will not rotate to an
  // orientation the plist omits, whatever is passed here. Pass
  // NEUI_IOS_ORIENTATION_ALL to lift a restriction back to the plist's set.
  // Android: Activity.setRequestedOrientation.
  void     (NEUI_ABI *set_supported_orientations)(neui_session_t session,
                                                  neui_widget_t frame,
                                                  uint32_t mask);
  uint32_t (NEUI_ABI *supported_orientations)(neui_session_t session,
                                              neui_widget_t frame);

  // Force Light or Dark for this frame's view tree, overriding the device
  // setting (UIView.overrideUserInterfaceStyle). Repaints painted widgets
  // through the same path a system appearance change takes, so native controls
  // and painted widgets stay in agreement. NEUI_IOS_STYLE_SYSTEM restores the
  // default. Independent of NEUI_ATTR_FOLLOW_SYSTEM_THEME, which decides
  // whether the PALETTE tracks the appearance; this decides what the appearance
  // IS. With a forced style, FOLLOW_SYSTEM_THEME follows the forced one.
  void (NEUI_ABI *set_user_interface_style)(neui_session_t session,
                                            neui_widget_t frame,
                                            neui_ios_style_t style);

  // ---- Accessibility & environment ----------------------------------------

  // The live Dynamic Type category. See the enum's note on why this exists
  // next to metrics->ui_scale. Process-wide; `session` is not consulted.
  neui_ios_content_size_t (NEUI_ABI *content_size_category)(neui_session_t session);

  // The accessibility switches, as an OR of NEUI_IOS_A11Y_*. Process-wide;
  // `session` is not consulted.
  uint32_t (NEUI_ABI *accessibility_flags)(neui_session_t session);

  // ---- Device, power & feedback -------------------------------------------

  neui_ios_idiom_t (NEUI_ABI *idiom)(neui_session_t session);

  // Running OS version. Any out pointer may be NULL to skip that component.
  void (NEUI_ABI *os_version)(neui_session_t session,
                              int* major, int* minor, int* patch);

  // Hardware identifier, e.g. "iPhone16,1" - NOT a marketing name. Points at
  // host-owned storage that stays valid for the process's lifetime. Never NULL;
  // "" if it cannot be read. Android: Build.MODEL.
  const char* (NEUI_ABI *device_model)(neui_session_t session);

  // Battery charge, 0.0 to 1.0, or -1.0 when unknown (the simulator, or the
  // moment before monitoring warms up).
  //
  // COST: the first call to either battery method turns on
  // UIDevice.batteryMonitoringEnabled and leaves it on for the process.
  // Monitoring is not free, so it is not switched on for a client that never
  // asks. A client that polls these is holding it on.
  float               (NEUI_ABI *battery_level)(neui_session_t session);
  neui_ios_battery_t  (NEUI_ABI *battery_state)(neui_session_t session);

  // Low Power Mode. Android: PowerManager.isPowerSaveMode.
  int (NEUI_ABI *low_power_mode)(neui_session_t session);

  // Thermal pressure. Android: PowerManager.getThermalStatus (API 29+).
  neui_ios_thermal_t (NEUI_ABI *thermal_state)(neui_session_t session);

  // Fire a haptic. Silently does nothing on hardware without a Taptic Engine,
  // in the simulator, and while the device is in Low Power Mode - that is
  // UIKit's behaviour, not a neui policy. Returns nothing because iOS reports
  // no outcome either.
  void (NEUI_ABI *haptic)(neui_session_t session, neui_ios_haptic_t kind);

  // How much of the frame's bottom edge the software keyboard is covering, in
  // logical pixels at 96 DPI (the same units as every other geometry call), or
  // 0 when no keyboard is up. Nothing in neui moves content out of the way -
  // this is the number a client needs to do it, and NEUI_IOS_ENV_KEYBOARD is
  // the notification that it changed.
  // Android: WindowInsets.Type.ime() bottom inset.
  int (NEUI_ABI *keyboard_inset)(neui_session_t session, neui_widget_t frame);

  // Append new methods at the end (vtable-append evolution rule).
} neui_ios_api_t;

#ifdef __cplusplus
}
#endif
