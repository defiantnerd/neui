#pragma once

#ifdef __APPLE__

#import <AppKit/AppKit.h>
#import <CoreServices/CoreServices.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include <string>

// MIME <-> pasteboard type, for every format that is not one of the built-ins
// the clipboard / DnD code maps by hand (text/plain, text/html, text/uri-list,
// image/png).
//
// A pasteboard type must be a UTI. A MIME string is not one: AppKit logs
// "'application/x-example-programs' is not a valid UTI string. Cannot set data
// for an invalid UTI." and the bytes never reach the pasteboard. So a custom MIME
// is written under the UTI the system makes of its MIME tag - the type the system
// declares for it (application/json -> public.json), else the dynamic type for
// the tag (dyn.ah62...). The mapping is the system's, hence deterministic: any
// process on the machine that asks the same question gets the same type, and the
// type decodes back to the MIME.
//
// Reading goes the other way for each type on the pasteboard. A type that is
// named like a MIME (contains '/') is taken as one, as before. A dynamic type
// (dyn.*) is taken when it decodes to a MIME. Any other type is taken when it
// decodes to a MIME and that MIME maps back to it - so the types this header
// writes always come back, and the system's own types without a MIME of their
// own (public.file-url, com.apple.pasteboard.*) stay out. Types the code handles
// by hand (public.utf8-plain-text, public.html, public.png) come out under the
// built-in MIME names, which is what they are stored under anyway.
//
// The one thing that cannot round-trip is a MIME the system declares under
// another spelling: audio/x-wav is com.microsoft.waveform-audio, whose preferred
// MIME is audio/vnd.wave, and text/xml is public.xml, whose is application/xml.
// Those are written under that type, which is what other applications look for,
// and read back under the system's spelling.
//
// macOS 11+ goes through UTType. Before that the same question is asked of
// CoreServices (UTTypeCreatePreferredIdentifierForTag), which is deprecated from
// macOS 12 - hence the pragma around it, which a build targeting 12 or later
// would otherwise answer with a warning. The two do not return the same dynamic
// type for the same tag, but a given machine runs one of them.

namespace neui_detail
{
  // The pasteboard type for `mime`, or nil if the system makes none of it.
  inline NSString* pb_type_for_mime_macos(const std::string& mime)
  {
    if (mime.empty()) return nil;
    NSString* m = [NSString stringWithUTF8String:mime.c_str()];
    if (!m) return nil;
    if (@available(macOS 11.0, *)) {
      UTType* t = [UTType typeWithMIMEType:m];
      return t ? t.identifier : nil;
    }
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    CFStringRef uti = UTTypeCreatePreferredIdentifierForTag(
      kUTTagClassMIMEType, (__bridge CFStringRef)m, nullptr);
    return uti ? CFBridgingRelease(uti) : nil;
#pragma clang diagnostic pop
  }

  // The MIME a pasteboard type stands for in the system's tables ("" if none).
  inline std::string pb_mime_of_type_macos(NSString* type)
  {
    if (!type) return std::string();
    NSString* m = nil;
    if (@available(macOS 11.0, *)) {
      m = [UTType typeWithIdentifier:type].preferredMIMEType;
    } else {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
      CFStringRef c = UTTypeCopyPreferredTagWithClass(
        (__bridge CFStringRef)type, kUTTagClassMIMEType);
      if (c) m = CFBridgingRelease(c);
#pragma clang diagnostic pop
    }
    const char* u = m ? [m UTF8String] : nullptr;
    return u ? std::string(u) : std::string();
  }

  // The MIME to expose for a type found on a pasteboard, or "" if it is none of
  // neui's business (see the header comment for the rule).
  inline std::string pb_mime_for_type_macos(NSString* type)
  {
    const char* c = type ? [type UTF8String] : nullptr;
    if (!c) return std::string();
    std::string name(c);
    if (name.find('/') != std::string::npos) return name;   // named like a MIME
    std::string mime = pb_mime_of_type_macos(type);
    if (mime.empty()) return std::string();
    // A dynamic type is made of its tag, so it is the MIME's by construction
    // (and the pasteboard itself spells some of them differently from UTType).
    if (name.compare(0, 4, "dyn.") == 0) return mime;
    NSString* back = pb_type_for_mime_macos(mime);
    return (back && [back isEqualToString:type]) ? mime : std::string();
  }

  // The pasteboard type a drop target registers for to be offered a drag that
  // carries `mime`: the native type of the four built-ins, else the type above.
  inline NSString* pb_dragged_type_for_mime_macos(const std::string& mime)
  {
    if (mime == "text/plain;charset=utf-8" || mime == "text/plain")
      return NSPasteboardTypeString;
    if (mime == "text/html")      return NSPasteboardTypeHTML;
    if (mime == "text/uri-list")  return NSPasteboardTypeFileURL;
    if (mime == "image/png")      return NSPasteboardTypePNG;
    return pb_type_for_mime_macos(mime);
  }
}

#endif // __APPLE__
