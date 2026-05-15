#pragma once

#ifdef __APPLE__

#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>
#import <ImageIO/ImageIO.h>

#include <cstdint>

// macOS image-loading helpers shared by both the xpl host
// (`hosts/crossplatform/platform_macos.mm`) and the native macOS host
// (`hosts/macos/`). Both go through ImageIO + CGBitmapContextCreate to
// normalise into the BGRA8-premultiplied byte order the d2d / cg backends
// expect.
//
// CONVENTION: include from `.mm` files only.

namespace neui_detail
{
  // Resolve a possibly-relative image path: try as-is (cwd-relative or
  // absolute) first; if that file doesn't exist, fall back to the app
  // bundle's Contents/Resources. The fallback is what makes Xcode launches
  // work when cwd is the build's DerivedData dir rather than the project
  // root - the image is bundled by CMake (see top-level CMakeLists.txt's
  // RESOURCE / MACOSX_PACKAGE_LOCATION wiring on the example).
  inline NSString* resolve_image_path_macos(const char* path)
  {
    if (!path || !*path) return nil;
    NSString* ns_path = [NSString stringWithUTF8String:path];
    if (!ns_path) return nil;
    NSFileManager* fm = NSFileManager.defaultManager;
    if ([fm fileExistsAtPath:ns_path]) return ns_path;

    // Bundle Resources fallback. Only kicks in for relative paths - an
    // absolute path that doesn't exist is a real error, not a missing
    // bundle resource.
    if (![ns_path isAbsolutePath]) {
      NSString* res_dir = [NSBundle mainBundle].resourcePath;
      if (res_dir) {
        NSString* candidate =
          [res_dir stringByAppendingPathComponent:ns_path.lastPathComponent];
        if ([fm fileExistsAtPath:candidate]) return candidate;
      }
    }
    return ns_path;  // let the caller's CGImageSource fail naturally
  }

  // Decode `path` into a heap-allocated BGRA8-premultiplied buffer. Caller
  // releases via `free_image_bgra8`. Returns nullptr on failure.
  inline uint8_t* load_image_bgra8_macos(const char* path,
                                          uint32_t* width_out,
                                          uint32_t* height_out)
  {
    NSString* ns_path = resolve_image_path_macos(path);
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

  inline void free_image_bgra8(uint8_t* pixels) { delete[] pixels; }

} // namespace neui_detail

#endif // __APPLE__
