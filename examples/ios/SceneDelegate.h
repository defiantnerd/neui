#import <UIKit/UIKit.h>

// Scene delegate. The neui UI is built in scene:willConnectTo: - AFTER the
// UIWindowScene connects - so platform_create_appwindow can bind the frame's
// UIWindow to the live scene.
@interface SceneDelegate : UIResponder <UIWindowSceneDelegate>
@end
