// iOS example app entry point.
//
// UIApplicationMain owns the run loop (neui->run() does NOT - see the iOS
// divergence notes in platform_ios.mm). The UIApplicationSceneManifest in
// Info.plist names AppDelegate + SceneDelegate; the actual neui UI is built
// from SceneDelegate's scene:willConnectTo: AFTER the UIWindowScene connects,
// so platform_create_appwindow can find the connected scene.
//
// PRINCIPAL CLASS = @"NEUIApplication" (3rd arg). This is what makes the iPad
// hardware-keyboard system menu bar work: NEUIApplication (defined in the neui
// iOS host) overrides -buildMenuWithBuilder: at the APP level, which is always in
// the responder chain - unlike a content view's override, which UIKit only
// consults while that view is the active responder (so on a freshly-launched app
// nothing appeared). An iOS client that wants the iPad menu bar passes
// @"NEUIApplication" here (or a subclass of it, or forwards -buildMenuWithBuilder:
// / -handleMenuKeyCommand: to the neui hooks NEUIApplication calls). The
// AppDelegate stays the 4th arg. Passing nil for the principal class would fall
// back to plain UIApplication and the menu bar would not be contributed.

#import <UIKit/UIKit.h>
#import "AppDelegate.h"

int main(int argc, char* argv[])
{
  @autoreleasepool {
    return UIApplicationMain(argc, argv, @"NEUIApplication",
                             NSStringFromClass([AppDelegate class]));
  }
}
