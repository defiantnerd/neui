#pragma once

// Objective-C class names are global to a process: the runtime registers each
// class by its name when an image loads, and a second image defining the same
// name gets "Class NEUIView is implemented in both ... One of the two will be
// used. Which one is undefined." That is the normal case for a plugin: a host
// loads several plugins built on neui, or two versions of one, into one
// process, and every image's NEUIView then runs whichever copy the runtime
// picked - another build's code, with that build's ivar layout.
//
// NEUI_OBJC_SUFFIX (the CMake option of that name; empty by default, so the
// names stay as they are) appends a suffix to every class neui defines on
// macOS and iOS: with -DNEUI_OBJC_SUFFIX=_AcmeSynth_1_2 NEUIView is
// NEUIView_AcmeSynth_1_2 in the binary. A product picks one per release, so
// neither another product nor another release of its own shares a name with
// it. The formats of one product (CLAP, VST3, AU) link one neui build and so
// carry the same names - the same code, and macOS never unloads an image that
// defines Objective-C classes.
//
// Include this before the first use of a class name in a file (every file
// that declares, defines or names one of them). NEUIApplication is left out:
// an iOS app names its principal class as a string (UIApplicationMain), and
// it is an app's class, not a plugin's.

#if defined(NEUI_OBJC_SUFFIX)

#define NEUI_OBJC_PASTE_(name, suffix) name##suffix
#define NEUI_OBJC_PASTE(name, suffix) NEUI_OBJC_PASTE_(name, suffix)
// A macro naming itself is not expanded again in its own replacement, so
// `#define NEUIView NEUI_OBJC_NAME(NEUIView)` is NEUIView<suffix>.
#define NEUI_OBJC_NAME(name) NEUI_OBJC_PASTE(name, NEUI_OBJC_SUFFIX)

// crossplatform host (platform_macos.mm, platform_ios.mm)
#define NEUIView                    NEUI_OBJC_NAME(NEUIView)
#define NEUIViewController          NEUI_OBJC_NAME(NEUIViewController)
#define NEUIWindowDelegate          NEUI_OBJC_NAME(NEUIWindowDelegate)
#define NEUIMenuTarget              NEUI_OBJC_NAME(NEUIMenuTarget)

// native macOS host (hosts/macos)
#define NEUINativeWindowDelegate    NEUI_OBJC_NAME(NEUINativeWindowDelegate)
#define NEUINativeContentView       NEUI_OBJC_NAME(NEUINativeContentView)
#define NEUINativePaintedView       NEUI_OBJC_NAME(NEUINativePaintedView)
#define NEUISectionBodyView         NEUI_OBJC_NAME(NEUISectionBodyView)
#define NEUINativeControlTarget     NEUI_OBJC_NAME(NEUINativeControlTarget)
#define NEUINativeSlider            NEUI_OBJC_NAME(NEUINativeSlider)
#define NEUINativeListSource        NEUI_OBJC_NAME(NEUINativeListSource)
#define NEUIPopupCollector          NEUI_OBJC_NAME(NEUIPopupCollector)
#define NEUINativeMenuTarget        NEUI_OBJC_NAME(NEUINativeMenuTarget)
#define NEUINativeOutlineSource     NEUI_OBJC_NAME(NEUINativeOutlineSource)
#define NEUINativeTextDelegate      NEUI_OBJC_NAME(NEUINativeTextDelegate)

// shared macOS helpers (hosts/shared/macos)
#define NEUIDragSource              NEUI_OBJC_NAME(NEUIDragSource)
#define NEUIDataProviderDelegate    NEUI_OBJC_NAME(NEUIDataProviderDelegate)
#define NeuiToastMacOS              NEUI_OBJC_NAME(NeuiToastMacOS)
#define NeuiToastClickView          NEUI_OBJC_NAME(NeuiToastClickView)

// native iOS host (hosts/ios)
#define NEUIIOSCheckboxSwitchView   NEUI_OBJC_NAME(NEUIIOSCheckboxSwitchView)
#define NEUINativeIOSPaintedView    NEUI_OBJC_NAME(NEUINativeIOSPaintedView)
#define NEUINativeIOSContentView    NEUI_OBJC_NAME(NEUINativeIOSContentView)
#define NEUINativeIOSViewController NEUI_OBJC_NAME(NEUINativeIOSViewController)
#define NEUIFirstResponderProbe     NEUI_OBJC_NAME(NEUIFirstResponderProbe)
#define NEUINativeIOSControlTarget  NEUI_OBJC_NAME(NEUINativeIOSControlTarget)
#define NEUIIOSListSource           NEUI_OBJC_NAME(NEUIIOSListSource)
#define NEUIIOSTreeSource           NEUI_OBJC_NAME(NEUIIOSTreeSource)

#endif // NEUI_OBJC_SUFFIX
