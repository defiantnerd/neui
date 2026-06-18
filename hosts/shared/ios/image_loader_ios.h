#pragma once

#if defined(__APPLE__)
#import <TargetConditionals.h>
#if TARGET_OS_IPHONE

#import <Foundation/Foundation.h>
#import <CoreGraphics/CoreGraphics.h>
#import <ImageIO/ImageIO.h>

#include <cstdint>

// iOS image-loading helpers for the xpl host (hosts/crossplatform/platform_ios.mm).
// The CGImageSource + CGBitmapContextCreate BGRA8-premultiplied core is identical
// to image_loader_macos.h (CoreGraphics / ImageIO are the same on iOS); only the
// bundle-resource resolution differs - an iOS .app flattens resources directly
// into the bundle root, so [NSBundle mainBundle].resourcePath is the bundle
// directory itself.
//
// CONVENTION: include from `.mm` files only.

namespace neui_detail
{
  // Resolve a possibly-relative image path: try as-is (cwd-relative / absolute)
  // first; if that file doesn't exist, fall back to the app bundle's resources.
  // On iOS the bundle is the .app directory and CMake's RESOURCE wiring copies
  // bundled files to its root, so look there by last path component. -[NSBundle
  // pathForResource:ofType:] handles the lookup including any localisation dirs.
  inline NSString* resolve_image_path_ios(const char* path)
  {
    if (!path || !*path) return nil;
    NSString* ns_path = [NSString stringWithUTF8String:path];
    if (!ns_path) return nil;
    NSFileManager* fm = NSFileManager.defaultManager;
    if ([fm fileExistsAtPath:ns_path]) return ns_path;

    // Bundle fallback for relative paths only - an absolute path that doesn't
    // exist is a real error, not a missing bundle resource.
    if (![ns_path isAbsolutePath]) {
      NSBundle* bundle = [NSBundle mainBundle];
      // Split "name.ext" into base + extension for pathForResource:ofType:.
      NSString* file = ns_path.lastPathComponent;
      NSString* base = [file stringByDeletingPathExtension];
      NSString* ext  = [file pathExtension];
      NSString* found = [bundle pathForResource:base ofType:ext];
      if (found && [fm fileExistsAtPath:found]) return found;

      // Direct resourcePath join as a secondary attempt (matches macOS).
      NSString* res_dir = bundle.resourcePath;
      if (res_dir) {
        NSString* candidate =
          [res_dir stringByAppendingPathComponent:file];
        if ([fm fileExistsAtPath:candidate]) return candidate;
      }
    }
    return ns_path;  // let the caller's CGImageSource fail naturally
  }

  // Decode `path` into a heap-allocated BGRA8-premultiplied buffer. Caller
  // releases via `free_image_bgra8`. Returns nullptr on failure.
  inline uint8_t* load_image_bgra8_ios(const char* path,
                                       uint32_t* width_out,
                                       uint32_t* height_out)
  {
    NSString* ns_path = resolve_image_path_ios(path);
    if (!ns_path) return nullptr;
    NSURL* url = [NSURL fileURLWithPath:ns_path];
    if (!url) return nullptr;

    CGImageSourceRef src = CGImageSourceCreateWithURL((__bridge CFURLRef)url, NULL);
    if (!src) return nullptr;
    CGImageRef img = CGImageSourceCreateImageAtIndex(src, 0, NULL);
    CFRelease(src);
    if (!img) return nullptr;

    size_t w = CGImageGetWidth(img);
    size_t h = CGImageGetHeight(img);
    if (w == 0 || h == 0) {
      CGImageRelease(img);
      return nullptr;
    }

    size_t row_bytes = w * 4;
    size_t total     = row_bytes * h;
    uint8_t* buf     = new uint8_t[total]();

    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef ctx = CGBitmapContextCreate(
      buf, w, h, 8, row_bytes, cs,
      kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
    CGColorSpaceRelease(cs);

    if (!ctx) {
      delete[] buf;
      CGImageRelease(img);
      return nullptr;
    }
    CGContextDrawImage(ctx, CGRectMake(0, 0, (CGFloat)w, (CGFloat)h), img);
    CGContextRelease(ctx);
    CGImageRelease(img);

    if (width_out)  *width_out  = (uint32_t)w;
    if (height_out) *height_out = (uint32_t)h;
    return buf;
  }

  // Defined identically in image_loader_macos.h; only one of the two is
  // compiled per platform (TARGET_OS_IPHONE gate), so no ODR clash.
  inline void free_image_bgra8(uint8_t* pixels) { delete[] pixels; }

} // namespace neui_detail

#endif // TARGET_OS_IPHONE
#endif // __APPLE__
