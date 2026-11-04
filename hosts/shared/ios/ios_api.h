#pragma once

#if defined(__APPLE__)
#import <TargetConditionals.h>
#if TARGET_OS_IPHONE

#import <UIKit/UIKit.h>
#import <sys/sysctl.h>

#include <map>
#include <string>
#include <vector>

#include <neui/neui.h>      // NEUI_VERSION
#include <neui/d/ios.h>
#include <neui/d/events.h>

// Shared, host-neutral implementation of NEUI_API_IOS (include/neui/d/ios.h).
//
// UIApplication, UIDevice, UIScreen, NSProcessInfo and UIAccessibility are
// process-global and behave the same whether the frame is a native-control tree
// (hosts/ios) or one painted view (platform_ios.mm). The hosts differ in two
// things only - resolving a frame to its UIViewController, and reaching every
// live frame to deliver an event - so those are seams, the same shape
// hosts/shared/metrics.h uses.
//
// Safe to include from more than one TU: every definition is inline, so all TUs
// share one instance of the state and one address for k_ios_api. That matters -
// a client may link both iOS hosts into one binary, and a non-inline definition
// would be a duplicate symbol.

namespace neui_detail
{
  // ---------------------------------------------------------------------------
  // Per-host seams.

  // Seams are REGISTRIES, not single slots. Both iOS hosts can be linked into
  // one binary and neui_init() registers ios BEFORE xpl, so an assigned slot
  // would always end up holding xpl's - the frame lookup would resolve nothing
  // and the broadcast would walk an empty registry, both silently. Each host
  // ADDS instead: the frame lookup takes the first that resolves (a frame
  // belongs to exactly one host) and the broadcast runs every one. add()
  // ignores a pointer it already holds, so a repeated register_host() is safe.
  // Same rule in hosts/shared/metrics.h.

  template <typename Fn>
  inline void ios_seam_add(std::vector<Fn>& list, Fn fn)
  {
    if (!fn) return;
    for (Fn existing : list)
      if (existing == fn) return;
    list.push_back(fn);
  }

  // A frame widget -> the UIViewController that owns its view.
  using ios_frame_controller_fn = UIViewController* (*)(neui_session_t, neui_widget_t);

  inline std::vector<ios_frame_controller_fn>& ios_frame_controller_seams()
  {
    static std::vector<ios_frame_controller_fn> v;
    return v;
  }

  inline void ios_add_frame_controller_seam(ios_frame_controller_fn fn)
  {
    ios_seam_add(ios_frame_controller_seams(), fn);
  }

  // The fallback when no host resolved the frame: the key window's root
  // controller. Right for a single-window app, and the only thing available
  // before a host has installed anything.
  inline UIViewController* ios_frame_controller_default(neui_session_t, neui_widget_t)
  {
    if (@available(iOS 13.0, *)) {
      for (UIScene* scene in UIApplication.sharedApplication.connectedScenes) {
        if (![scene isKindOfClass:[UIWindowScene class]]) continue;
        for (UIWindow* w in ((UIWindowScene*)scene).windows)
          if (w.isKeyWindow && w.rootViewController) return w.rootViewController;
      }
    }
    return nil;
  }

  inline UIViewController* ios_controller_for(neui_session_t s, neui_widget_t frame)
  {
    for (ios_frame_controller_fn fn : ios_frame_controller_seams())
      if (UIViewController* vc = fn(s, frame)) return vc;
    return ios_frame_controller_default(s, frame);
  }

  // Deliver NEUI_EVENT_IOS_ENVIRONMENT_CHANGED to every live frame. Only a host
  // knows its own session and widget registries, so each contributes its own
  // walk and all of them run.
  using ios_broadcast_env_fn = void (*)(uint32_t changed);

  inline std::vector<ios_broadcast_env_fn>& ios_broadcast_env_seams()
  {
    static std::vector<ios_broadcast_env_fn> v;
    return v;
  }

  inline void ios_add_broadcast_env_seam(ios_broadcast_env_fn fn)
  {
    ios_seam_add(ios_broadcast_env_seams(), fn);
  }

  inline void ios_broadcast_env(uint32_t changed)
  {
    for (ios_broadcast_env_fn fn : ios_broadcast_env_seams()) fn(changed);
  }

  // ---------------------------------------------------------------------------
  // State.
  //
  // Frame chrome lives here rather than in each host's WidgetData: there are
  // two different WidgetData structs and only one set of settings, and the
  // view-controller overrides in both hosts read it back through
  // ios_frame_state_if_present().

  struct IosFrameState
  {
    neui_ios_status_bar_t status_style = NEUI_IOS_STATUS_BAR_DEFAULT;
    bool     status_hidden             = false;
    bool     home_indicator_auto_hidden = false;
    uint32_t deferring_edges           = NEUI_IOS_EDGE_NONE;
    uint32_t supported_orientations    = NEUI_IOS_ORIENTATION_ALL;
  };

  inline std::map<uint32_t, IosFrameState>& ios_frame_states()
  {
    static std::map<uint32_t, IosFrameState> m;
    return m;
  }

  inline IosFrameState& ios_frame_state(neui_widget_t frame)
  {
    return ios_frame_states()[frame.id];
  }

  // Read-only lookup for the view-controller overrides: nullptr when the client
  // never set anything, so the override can fall through to super.
  inline const IosFrameState* ios_frame_state_if_present(neui_widget_t frame)
  {
    auto& m = ios_frame_states();
    auto it = m.find(frame.id);
    return it == m.end() ? nullptr : &it->second;
  }

  // Called by each host when a frame is destroyed. Without it a recycled widget
  // id would inherit the previous frame's status-bar style.
  inline void ios_frame_forget(neui_widget_t frame)
  {
    ios_frame_states().erase(frame.id);
  }

  struct IosSessionState
  {
    int   idle_hold         = 0;      // refcount, per the header's contract
    bool  brightness_saved  = false;
    float saved_brightness  = 0.0f;
  };

  // Keyed on the raw session handle. Two hosts in one process each mint their
  // own ids and could in principle collide here; in practice a client drives
  // one host, and the cost of a collision is a shared idle-timer refcount.
  inline std::map<uint32_t, IosSessionState>& ios_session_states()
  {
    static std::map<uint32_t, IosSessionState> m;
    return m;
  }

  inline IosSessionState& ios_session_state(neui_session_t s)
  {
    return ios_session_states()[s.session];
  }

  // The keyboard's frame in SCREEN coordinates, or CGRectZero when it is down.
  // Kept globally and intersected per frame on demand, so no frame has to be
  // told about it in advance.
  inline CGRect& ios_keyboard_frame()
  {
    static CGRect r = CGRectZero;
    return r;
  }

  // ---------------------------------------------------------------------------
  // The idle timer, applied process-wide from the sum of the per-session holds.

  inline void ios_apply_idle_timer()
  {
    bool any = false;
    for (const auto& kv : ios_session_states())
      if (kv.second.idle_hold > 0) { any = true; break; }
    UIApplication.sharedApplication.idleTimerDisabled = any ? YES : NO;
  }

  // ---------------------------------------------------------------------------
  // Environment observation. Installed once, on the first get_interface hit, so
  // a client that never asks for this interface pays nothing.

  inline uint32_t ios_accessibility_flags_now()
  {
    uint32_t f = 0;
    if (UIAccessibilityIsReduceMotionEnabled())       f |= NEUI_IOS_A11Y_REDUCE_MOTION;
    if (UIAccessibilityIsReduceTransparencyEnabled()) f |= NEUI_IOS_A11Y_REDUCE_TRANSPARENCY;
    if (UIAccessibilityIsBoldTextEnabled())           f |= NEUI_IOS_A11Y_BOLD_TEXT;
    if (UIAccessibilityDarkerSystemColorsEnabled())   f |= NEUI_IOS_A11Y_DARKER_COLORS;
    if (UIAccessibilityIsVoiceOverRunning())          f |= NEUI_IOS_A11Y_VOICE_OVER;
    if (UIAccessibilityIsSwitchControlRunning())      f |= NEUI_IOS_A11Y_SWITCH_CONTROL;
    return f;
  }

  inline neui_ios_orientation_t ios_orientation_of(UIViewController* vc)
  {
    UIInterfaceOrientation o = UIInterfaceOrientationUnknown;
    if (@available(iOS 13.0, *)) {
      UIWindowScene* scene = vc.view.window.windowScene;
      if (scene) o = scene.interfaceOrientation;
    }
    switch (o) {
      case UIInterfaceOrientationPortrait:           return NEUI_IOS_ORIENTATION_PORTRAIT;
      case UIInterfaceOrientationPortraitUpsideDown: return NEUI_IOS_ORIENTATION_PORTRAIT_UPSIDE_DOWN;
      case UIInterfaceOrientationLandscapeLeft:      return NEUI_IOS_ORIENTATION_LANDSCAPE_LEFT;
      case UIInterfaceOrientationLandscapeRight:     return NEUI_IOS_ORIENTATION_LANDSCAPE_RIGHT;
      default:                                       return NEUI_IOS_ORIENTATION_UNKNOWN;
    }
  }

  // Last seen values, so an observer only broadcasts on a real change.
  inline uint32_t& ios_last_a11y()      { static uint32_t v = 0; return v; }
  inline int&      ios_last_low_power() { static int v = -1;     return v; }
  inline int&      ios_last_thermal()   { static int v = -1;     return v; }
  inline int&      ios_last_orientation() { static int v = -1;   return v; }

  // Called by each host from its view controller once a layout / rotation has
  // settled. Cheaper and more accurate than UIDevice orientation notifications,
  // which need the accelerometer running and report DEVICE, not INTERFACE,
  // orientation.
  inline void ios_note_orientation_changed(UIViewController* vc)
  {
    const int now = (int)ios_orientation_of(vc);
    if (now == ios_last_orientation()) return;
    ios_last_orientation() = now;
    ios_broadcast_env(NEUI_IOS_ENV_ORIENTATION);
  }

  inline void ios_install_observers()
  {
    static bool installed = false;
    if (installed) return;
    installed = true;

    NSNotificationCenter* nc = NSNotificationCenter.defaultCenter;
    NSOperationQueue* main = NSOperationQueue.mainQueue;

    ios_last_a11y()      = ios_accessibility_flags_now();
    ios_last_low_power() = NSProcessInfo.processInfo.isLowPowerModeEnabled ? 1 : 0;
    ios_last_thermal()   = (int)NSProcessInfo.processInfo.thermalState;

    auto a11y = ^(NSNotification*) {
      const uint32_t now = ios_accessibility_flags_now();
      if (now == ios_last_a11y()) return;
      ios_last_a11y() = now;
      ios_broadcast_env(NEUI_IOS_ENV_ACCESSIBILITY);
    };
    for (NSNotificationName n in @[ UIAccessibilityReduceMotionStatusDidChangeNotification,
                                    UIAccessibilityReduceTransparencyStatusDidChangeNotification,
                                    UIAccessibilityBoldTextStatusDidChangeNotification,
                                    UIAccessibilityDarkerSystemColorsStatusDidChangeNotification,
                                    UIAccessibilityVoiceOverStatusDidChangeNotification,
                                    UIAccessibilitySwitchControlStatusDidChangeNotification ])
      [nc addObserverForName:n object:nil queue:main usingBlock:a11y];

    [nc addObserverForName:NSProcessInfoPowerStateDidChangeNotification
                    object:nil queue:main usingBlock:^(NSNotification*) {
      const int now = NSProcessInfo.processInfo.isLowPowerModeEnabled ? 1 : 0;
      if (now == ios_last_low_power()) return;
      ios_last_low_power() = now;
      ios_broadcast_env(NEUI_IOS_ENV_LOW_POWER);
    }];

    [nc addObserverForName:NSProcessInfoThermalStateDidChangeNotification
                    object:nil queue:main usingBlock:^(NSNotification*) {
      const int now = (int)NSProcessInfo.processInfo.thermalState;
      if (now == ios_last_thermal()) return;
      ios_last_thermal() = now;
      ios_broadcast_env(NEUI_IOS_ENV_THERMAL);
    }];

    auto battery = ^(NSNotification*) {
      ios_broadcast_env(NEUI_IOS_ENV_BATTERY);
    };
    [nc addObserverForName:UIDeviceBatteryLevelDidChangeNotification
                    object:nil queue:main usingBlock:battery];
    [nc addObserverForName:UIDeviceBatteryStateDidChangeNotification
                    object:nil queue:main usingBlock:battery];

    [nc addObserverForName:UIKeyboardWillChangeFrameNotification
                    object:nil queue:main usingBlock:^(NSNotification* note) {
      NSValue* v = note.userInfo[UIKeyboardFrameEndUserInfoKey];
      ios_keyboard_frame() = v ? v.CGRectValue : CGRectZero;
      ios_broadcast_env(NEUI_IOS_ENV_KEYBOARD);
    }];
    [nc addObserverForName:UIKeyboardWillHideNotification
                    object:nil queue:main usingBlock:^(NSNotification*) {
      ios_keyboard_frame() = CGRectZero;
      ios_broadcast_env(NEUI_IOS_ENV_KEYBOARD);
    }];
  }

  // ---------------------------------------------------------------------------
  // The vtable methods.

  inline void ios_set_idle_timer_disabled(neui_session_t session, int disabled)
  {
    IosSessionState& st = ios_session_state(session);
    if (disabled) {
      ++st.idle_hold;
    } else if (st.idle_hold > 0) {
      --st.idle_hold;
    }
    ios_apply_idle_timer();
  }

  inline int ios_idle_timer_disabled(neui_session_t session)
  {
    return ios_session_state(session).idle_hold > 0 ? 1 : 0;
  }

  inline void ios_set_deferring_system_gestures(neui_session_t session,
                                                neui_widget_t frame, uint32_t edges)
  {
    ios_frame_state(frame).deferring_edges = edges;
    UIViewController* vc = ios_controller_for(session, frame);
    if (!vc) return;
    if (@available(iOS 11.0, *))
      [vc setNeedsUpdateOfScreenEdgesDeferringSystemGestures];
  }

  inline void ios_set_home_indicator_auto_hidden(neui_session_t session,
                                                 neui_widget_t frame, int hidden)
  {
    ios_frame_state(frame).home_indicator_auto_hidden = hidden != 0;
    UIViewController* vc = ios_controller_for(session, frame);
    if (!vc) return;
    if (@available(iOS 11.0, *))
      [vc setNeedsUpdateOfHomeIndicatorAutoHidden];
  }

  inline float ios_screen_brightness(neui_session_t)
  {
    UIScreen* s = UIScreen.mainScreen;
    return s ? (float)s.brightness : -1.0f;
  }

  inline void ios_set_screen_brightness(neui_session_t session, float value)
  {
    UIScreen* s = UIScreen.mainScreen;
    if (!s) return;
    IosSessionState& st = ios_session_state(session);
    if (value < 0.0f) {
      // Explicit restore.
      if (st.brightness_saved) {
        s.brightness = st.saved_brightness;
        st.brightness_saved = false;
      }
      return;
    }
    // Remember what the user had, once, so session teardown can put it back.
    if (!st.brightness_saved) {
      st.saved_brightness = (float)s.brightness;
      st.brightness_saved = true;
    }
    if (value > 1.0f) value = 1.0f;
    s.brightness = value;
  }

  inline void ios_set_status_bar(neui_session_t session, neui_widget_t frame,
                                 neui_ios_status_bar_t style, int hidden)
  {
    IosFrameState& fs = ios_frame_state(frame);
    fs.status_style  = style;
    fs.status_hidden = hidden != 0;
    UIViewController* vc = ios_controller_for(session, frame);
    if (vc) [vc setNeedsStatusBarAppearanceUpdate];
  }

  inline neui_ios_orientation_t ios_orientation(neui_session_t session,
                                                neui_widget_t frame)
  {
    return ios_orientation_of(ios_controller_for(session, frame));
  }

  inline void ios_set_supported_orientations(neui_session_t session,
                                             neui_widget_t frame, uint32_t mask)
  {
    ios_frame_state(frame).supported_orientations = mask;
    UIViewController* vc = ios_controller_for(session, frame);
    if (!vc) return;
    if (@available(iOS 16.0, *)) {
      [vc setNeedsUpdateOfSupportedInterfaceOrientations];
    } else {
      [UIViewController attemptRotationToDeviceOrientation];
    }
  }

  inline uint32_t ios_supported_orientations(neui_session_t, neui_widget_t frame)
  {
    const IosFrameState* fs = ios_frame_state_if_present(frame);
    return fs ? fs->supported_orientations : (uint32_t)NEUI_IOS_ORIENTATION_ALL;
  }

  // Contributed by each host: repaint after an appearance override, so painted
  // widgets follow the native controls instead of keeping the old palette. A
  // registry for the same reason as the two above.
  using ios_theme_refresh_fn = void (*)();

  inline std::vector<ios_theme_refresh_fn>& ios_theme_refresh_seams()
  {
    static std::vector<ios_theme_refresh_fn> v;
    return v;
  }

  inline void ios_add_theme_refresh_seam(ios_theme_refresh_fn fn)
  {
    ios_seam_add(ios_theme_refresh_seams(), fn);
  }

  inline void ios_theme_refresh()
  {
    for (ios_theme_refresh_fn fn : ios_theme_refresh_seams()) fn();
  }

  inline void ios_set_user_interface_style(neui_session_t session, neui_widget_t frame,
                                           neui_ios_style_t style)
  {
    UIViewController* vc = ios_controller_for(session, frame);
    if (!vc) return;
    if (@available(iOS 13.0, *)) {
      UIUserInterfaceStyle s = UIUserInterfaceStyleUnspecified;
      if (style == NEUI_IOS_STYLE_LIGHT) s = UIUserInterfaceStyleLight;
      else if (style == NEUI_IOS_STYLE_DARK) s = UIUserInterfaceStyleDark;
      vc.overrideUserInterfaceStyle = s;
      // The trait change reaches the views asynchronously; repaint through the
      // host's own theme path so painted widgets and native controls agree.
      ios_theme_refresh();
    }
  }

  inline neui_ios_content_size_t ios_content_size_category(neui_session_t)
  {
    UIContentSizeCategory c = UIApplication.sharedApplication.preferredContentSizeCategory;
    if (!c) return NEUI_IOS_CONTENT_SIZE_UNKNOWN;
    if ([c isEqualToString:UIContentSizeCategoryExtraSmall])  return NEUI_IOS_CONTENT_SIZE_XS;
    if ([c isEqualToString:UIContentSizeCategorySmall])       return NEUI_IOS_CONTENT_SIZE_S;
    if ([c isEqualToString:UIContentSizeCategoryMedium])      return NEUI_IOS_CONTENT_SIZE_M;
    if ([c isEqualToString:UIContentSizeCategoryLarge])       return NEUI_IOS_CONTENT_SIZE_L;
    if ([c isEqualToString:UIContentSizeCategoryExtraLarge])  return NEUI_IOS_CONTENT_SIZE_XL;
    if ([c isEqualToString:UIContentSizeCategoryExtraExtraLarge])
      return NEUI_IOS_CONTENT_SIZE_XXL;
    if ([c isEqualToString:UIContentSizeCategoryExtraExtraExtraLarge])
      return NEUI_IOS_CONTENT_SIZE_XXXL;
    if ([c isEqualToString:UIContentSizeCategoryAccessibilityMedium])
      return NEUI_IOS_CONTENT_SIZE_ACCESSIBILITY_M;
    if ([c isEqualToString:UIContentSizeCategoryAccessibilityLarge])
      return NEUI_IOS_CONTENT_SIZE_ACCESSIBILITY_L;
    if ([c isEqualToString:UIContentSizeCategoryAccessibilityExtraLarge])
      return NEUI_IOS_CONTENT_SIZE_ACCESSIBILITY_XL;
    if ([c isEqualToString:UIContentSizeCategoryAccessibilityExtraExtraLarge])
      return NEUI_IOS_CONTENT_SIZE_ACCESSIBILITY_XXL;
    if ([c isEqualToString:UIContentSizeCategoryAccessibilityExtraExtraExtraLarge])
      return NEUI_IOS_CONTENT_SIZE_ACCESSIBILITY_XXXL;
    return NEUI_IOS_CONTENT_SIZE_UNKNOWN;
  }

  inline uint32_t ios_accessibility_flags(neui_session_t)
  {
    return ios_accessibility_flags_now();
  }

  inline neui_ios_idiom_t ios_idiom(neui_session_t)
  {
    switch (UIDevice.currentDevice.userInterfaceIdiom) {
      case UIUserInterfaceIdiomPhone: return NEUI_IOS_IDIOM_PHONE;
      case UIUserInterfaceIdiomPad:   return NEUI_IOS_IDIOM_PAD;
      case UIUserInterfaceIdiomTV:    return NEUI_IOS_IDIOM_TV;
      case UIUserInterfaceIdiomMac:   return NEUI_IOS_IDIOM_MAC;
      default:                        return NEUI_IOS_IDIOM_UNKNOWN;
    }
  }

  inline void ios_os_version(neui_session_t, int* major, int* minor, int* patch)
  {
    NSOperatingSystemVersion v = NSProcessInfo.processInfo.operatingSystemVersion;
    if (major) *major = (int)v.majorVersion;
    if (minor) *minor = (int)v.minorVersion;
    if (patch) *patch = (int)v.patchVersion;
  }

  inline const char* ios_device_model(neui_session_t)
  {
    // Cached: sysctl is cheap but the header promises a pointer that stays
    // valid for the process, so the storage has to outlive the call.
    static std::string model = [] {
      size_t len = 0;
      if (sysctlbyname("hw.machine", nullptr, &len, nullptr, 0) != 0 || len == 0)
        return std::string();
      std::string s(len, '\0');
      if (sysctlbyname("hw.machine", &s[0], &len, nullptr, 0) != 0)
        return std::string();
      if (!s.empty() && s.back() == '\0') s.pop_back();
      return s;
    }();
    return model.c_str();
  }

  // Battery monitoring is off by default and costs power, so it is turned on
  // by the first query and left on - see the header's COST note.
  inline void ios_ensure_battery_monitoring()
  {
    static bool on = false;
    if (on) return;
    on = true;
    UIDevice.currentDevice.batteryMonitoringEnabled = YES;
  }

  inline float ios_battery_level(neui_session_t)
  {
    ios_ensure_battery_monitoring();
    const float v = UIDevice.currentDevice.batteryLevel;
    return v < 0.0f ? -1.0f : v;   // UIKit reports -1 when it does not know
  }

  inline neui_ios_battery_t ios_battery_state(neui_session_t)
  {
    ios_ensure_battery_monitoring();
    switch (UIDevice.currentDevice.batteryState) {
      case UIDeviceBatteryStateUnplugged: return NEUI_IOS_BATTERY_UNPLUGGED;
      case UIDeviceBatteryStateCharging:  return NEUI_IOS_BATTERY_CHARGING;
      case UIDeviceBatteryStateFull:      return NEUI_IOS_BATTERY_FULL;
      default:                            return NEUI_IOS_BATTERY_UNKNOWN;
    }
  }

  inline int ios_low_power_mode(neui_session_t)
  {
    return NSProcessInfo.processInfo.isLowPowerModeEnabled ? 1 : 0;
  }

  inline neui_ios_thermal_t ios_thermal_state(neui_session_t)
  {
    switch (NSProcessInfo.processInfo.thermalState) {
      case NSProcessInfoThermalStateFair:     return NEUI_IOS_THERMAL_FAIR;
      case NSProcessInfoThermalStateSerious:  return NEUI_IOS_THERMAL_SERIOUS;
      case NSProcessInfoThermalStateCritical: return NEUI_IOS_THERMAL_CRITICAL;
      default:                                return NEUI_IOS_THERMAL_NOMINAL;
    }
  }

  inline void ios_haptic(neui_session_t, neui_ios_haptic_t kind)
  {
    if (@available(iOS 10.0, *)) {
      switch (kind) {
        case NEUI_IOS_HAPTIC_SELECTION: {
          UISelectionFeedbackGenerator* g = [[UISelectionFeedbackGenerator alloc] init];
          [g prepare];
          [g selectionChanged];
          break;
        }
        case NEUI_IOS_HAPTIC_SUCCESS:
        case NEUI_IOS_HAPTIC_WARNING:
        case NEUI_IOS_HAPTIC_ERROR: {
          UINotificationFeedbackType t = UINotificationFeedbackTypeSuccess;
          if (kind == NEUI_IOS_HAPTIC_WARNING) t = UINotificationFeedbackTypeWarning;
          else if (kind == NEUI_IOS_HAPTIC_ERROR) t = UINotificationFeedbackTypeError;
          UINotificationFeedbackGenerator* g =
              [[UINotificationFeedbackGenerator alloc] init];
          [g prepare];
          [g notificationOccurred:t];
          break;
        }
        default: {
          UIImpactFeedbackStyle st = UIImpactFeedbackStyleMedium;
          if (kind == NEUI_IOS_HAPTIC_LIGHT) st = UIImpactFeedbackStyleLight;
          else if (kind == NEUI_IOS_HAPTIC_HEAVY) st = UIImpactFeedbackStyleHeavy;
          UIImpactFeedbackGenerator* g =
              [[UIImpactFeedbackGenerator alloc] initWithStyle:st];
          [g prepare];
          [g impactOccurred];
          break;
        }
      }
    }
  }

  inline int ios_keyboard_inset(neui_session_t session, neui_widget_t frame)
  {
    const CGRect kb = ios_keyboard_frame();
    if (CGRectIsEmpty(kb)) return 0;
    UIViewController* vc = ios_controller_for(session, frame);
    UIView* view = vc.view;
    if (!view || !view.window) return 0;
    // The notification's rect is in screen space; bring the view into the same
    // space rather than the other way round, so a frame that is not full-screen
    // (an iPad form-sheet DIALOG) gets the overlap it actually has.
    const CGRect self_in_screen = [view convertRect:view.bounds toView:nil];
    const CGRect overlap = CGRectIntersection(self_in_screen, kb);
    if (CGRectIsNull(overlap) || CGRectIsEmpty(overlap)) return 0;
    return (int)(overlap.size.height + 0.5);
  }

  // ---------------------------------------------------------------------------
  // Teardown. Each host calls this from ~Session, which is what makes the
  // idle-timer refcount and the brightness restore promises in d/ios.h true.

  inline void ios_session_shutdown(neui_session_t session)
  {
    auto& m = ios_session_states();
    auto it = m.find(session.session);
    if (it == m.end()) return;
    if (it->second.brightness_saved) {
      if (UIScreen* s = UIScreen.mainScreen) s.brightness = it->second.saved_brightness;
    }
    m.erase(it);
    ios_apply_idle_timer();
  }

  // ---------------------------------------------------------------------------
  // The shared vtable. One definition shared by both iOS hosts (inline).

  inline neui_ios_api_t k_ios_api = {
    NEUI_VERSION,
    ios_set_idle_timer_disabled,
    ios_idle_timer_disabled,
    ios_set_deferring_system_gestures,
    ios_set_home_indicator_auto_hidden,
    ios_screen_brightness,
    ios_set_screen_brightness,
    ios_set_status_bar,
    ios_orientation,
    ios_set_supported_orientations,
    ios_supported_orientations,
    ios_set_user_interface_style,
    ios_content_size_category,
    ios_accessibility_flags,
    ios_idiom,
    ios_os_version,
    ios_device_model,
    ios_battery_level,
    ios_battery_state,
    ios_low_power_mode,
    ios_thermal_state,
    ios_haptic,
    ios_keyboard_inset,
  };

  // What each host's get_interface returns for NEUI_API_IOS. Installs the
  // environment observers on first use, so a client that never fetches the
  // interface pays for none of them.
  inline neui_ios_api_t* ios_api()
  {
    ios_install_observers();
    return &k_ios_api;
  }

  // ---------------------------------------------------------------------------
  // UIKit translations for the view-controller overrides in both hosts.

  inline UIStatusBarStyle ios_uikit_status_bar_style(neui_ios_status_bar_t s)
  {
    if (@available(iOS 13.0, *)) {
      if (s == NEUI_IOS_STATUS_BAR_LIGHT) return UIStatusBarStyleLightContent;
      if (s == NEUI_IOS_STATUS_BAR_DARK)  return UIStatusBarStyleDarkContent;
    } else if (s == NEUI_IOS_STATUS_BAR_LIGHT) {
      return UIStatusBarStyleLightContent;
    }
    return UIStatusBarStyleDefault;
  }

  inline UIRectEdge ios_uikit_rect_edge(uint32_t edges)
  {
    UIRectEdge e = UIRectEdgeNone;
    if (edges & NEUI_IOS_EDGE_TOP)    e |= UIRectEdgeTop;
    if (edges & NEUI_IOS_EDGE_LEFT)   e |= UIRectEdgeLeft;
    if (edges & NEUI_IOS_EDGE_BOTTOM) e |= UIRectEdgeBottom;
    if (edges & NEUI_IOS_EDGE_RIGHT)  e |= UIRectEdgeRight;
    return e;
  }

  inline UIInterfaceOrientationMask ios_uikit_orientation_mask(uint32_t mask)
  {
    UIInterfaceOrientationMask m = (UIInterfaceOrientationMask)0;
    if (mask & NEUI_IOS_ORIENTATION_PORTRAIT)
      m |= UIInterfaceOrientationMaskPortrait;
    if (mask & NEUI_IOS_ORIENTATION_PORTRAIT_UPSIDE_DOWN)
      m |= UIInterfaceOrientationMaskPortraitUpsideDown;
    if (mask & NEUI_IOS_ORIENTATION_LANDSCAPE_LEFT)
      m |= UIInterfaceOrientationMaskLandscapeLeft;
    if (mask & NEUI_IOS_ORIENTATION_LANDSCAPE_RIGHT)
      m |= UIInterfaceOrientationMaskLandscapeRight;
    return m ? m : UIInterfaceOrientationMaskAll;
  }

} // namespace neui_detail

#endif // TARGET_OS_IPHONE
#endif // __APPLE__
