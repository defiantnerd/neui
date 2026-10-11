#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "api.h"
#include "assets.h"
#include "painter.h"

// Shader API - optional. Reached via
//   neui_shader_api_t* sh = (neui_shader_api_t*)
//     neui_api->get_interface(session, NEUI_API_SHADER);
// and NULL where shaders are not supported (the null platform, the
// experimental LVGL host, builds configured with NEUI_NO_SHADERS). Always
// feature-detect: a client that gets NULL should fall back to plain art.
//
// A shader is a small program in the neui shading language (.nsl, a GLSL
// subset specified in docs/shader-language.md) that computes one colour per
// pixel:
//
//   uniform float value = 0.5;
//   vec4 main(vec2 p) {                       // p: logical px, y down
//     float d = length(p - neui_size * 0.5);
//     return vec4(1.0, 0.6, 0.2, smoothstep(40.0, 38.0, d) * value);
//   }
//
// create() compiles source into a SHADER asset (NEUI_ASSET_KIND_SHADER),
// destroyed like any asset with neui_asset_api_t::destroy. There are three
// ways to put it on screen:
//
//   - a compound SHADER layer (NEUI_COMPOUND_LAYER_SHADER, <neui/d/compound.h>)
//     whose "u.<name>" props set and bind uniforms to widget attributes, so a
//     knob's look follows its value;
//   - draw() from a CUSTOMDRAW paint callback;
//   - bake_bitmap / bake_filmstrip into an ordinary BITMAP asset, once. A
//     filmstrip of one uniform's sweep is drawn by index by the existing frame
//     machinery (painter draw_asset_frame, or a compound ASSET layer with its
//     "frame" prop bound to a value attr) and costs nothing per frame.
//
// A shader can also read bitmaps: a `uniform sampler2D` takes a BITMAP or
// SURFACE asset of the session (textures in bake / draw, or a SHADER layer's
// "u.<name>" set with set_asset or bound with bind_asset), and texture2D(s,
// uv) samples it (uv 0..1 across the bitmap, y down). A sampler without a
// bitmap reads as transparent.
//
// The first two re-run the shader only when its inputs change: the result is
// cached per session by content (shader, size, scale, sub-pixel position,
// uniform values, time, the bitmaps' contents), so fifty knobs at one value
// share one bitmap. A SURFACE repainted with paint_surface is new content: a
// widget that samples it repaints when the client invalidates it.
//
// On the CPU the pixels are the same on every platform and compiler, bit for
// bit: neui runs shaders with its own deterministic math. Where the renderer
// can run them on the GPU (Direct2D), the live paths do so by default, within
// a small tolerance of the CPU result; see set_gpu.

#ifdef __cplusplus
extern "C" {
#endif

#define NEUI_API_SHADER "com.defiantnerd.neui.extension.shader/0"

  // A uniform value, matched to the shader's `uniform` declarations by name.
  // A float uniform reads v[0]; a vecN reads v[0..N-1].
  typedef struct neui_shader_uniform {
    const char* name;
    float       v[4];
  } neui_shader_uniform_t;

  // A colour for a vec4 uniform from an ARGB value, as straight RGBA
  // components of byte / 255 (the language's colour rule, exact).
  static inline neui_shader_uniform_t neui_shader_color(const char* name, uint32_t argb)
  {
    neui_shader_uniform_t u;
    u.name = name;
    u.v[0] = (float)((argb >> 16) & 0xFFu) / 255.0f;
    u.v[1] = (float)((argb >> 8) & 0xFFu) / 255.0f;
    u.v[2] = (float)(argb & 0xFFu) / 255.0f;
    u.v[3] = (float)((argb >> 24) & 0xFFu) / 255.0f;
    return u;
  }

  // A bitmap for a `uniform sampler2D`, matched by name: a BITMAP or SURFACE
  // asset of the session (a filmstrip is sampled as its whole sheet).
  // asset_none or any other kind leaves the sampler unbound.
  typedef struct neui_shader_texture {
    const char*  name;
    neui_asset_t asset;
  } neui_shader_texture_t;

  // How a sampler reads outside the bitmap and between texels, as its
  // "#pragma neui sampler" line sets it (clamp and linear by default).
#define NEUI_SHADER_WRAP_CLAMP    0u  // the edge texel continues
#define NEUI_SHADER_WRAP_REPEAT   1u  // the bitmap tiles
#define NEUI_SHADER_WRAP_BORDER   2u  // transparent outside
#define NEUI_SHADER_FILTER_LINEAR  0u
#define NEUI_SHADER_FILTER_NEAREST 1u

  // A sampler2D as the shader declares it, from sampler_at.
  typedef struct neui_shader_sampler_info {
    const char* name;    // owned by the shader asset; valid until it is destroyed
    uint32_t    wrap;    // NEUI_SHADER_WRAP_*
    uint32_t    filter;  // NEUI_SHADER_FILTER_*
  } neui_shader_sampler_info_t;

  // A uniform as the shader declares it, from uniform_at.
  typedef struct neui_shader_uniform_info {
    const char* name;        // owned by the shader asset; valid until it is destroyed
    uint32_t    components;  // 1 = float, 2..4 = vec2..vec4
    float       def[4];      // declared default (0 where none); unused components 0
  } neui_shader_uniform_info_t;

  // What to bake: one image (or one filmstrip frame). Passed by pointer;
  // fields may be appended in later versions, zero-initialise it.
  typedef struct neui_shader_bake {
    float                        width, height;  // logical px; the shader's neui_size
    float                        scale;          // device px per logical px; <= 0 means 1
    float                        time;           // neui_time in seconds; wrapped to [0, 3600)
    const neui_shader_uniform_t* uniforms;       // values by name; may be NULL
    uint32_t                     uniform_count;
    const neui_shader_texture_t* textures;       // bitmaps by sampler name; may be NULL
    uint32_t                     texture_count;
  } neui_shader_bake_t;

  // What to draw: a shader over a rect of the painter's current space.
  // Passed by pointer; fields may be appended, zero-initialise it.
  typedef struct neui_shader_draw {
    neui_asset_t                 shader;
    float                        x, y, width, height;  // logical px, painter space
    float                        resolution;  // shading density, (0, 4]; <= 0 means 1
    float                        time;        // neui_time in seconds; wrapped to [0, 3600)
                                              // (animation_time() to animate)
    const neui_shader_uniform_t* uniforms;    // values by name; may be NULL
    uint32_t                     uniform_count;
    const neui_shader_texture_t* textures;    // bitmaps by sampler name; may be NULL
    uint32_t                     texture_count;
  } neui_shader_draw_t;

  typedef struct neui_shader_api {
    uint32_t neui_version;

    // -------- Compile --------------------------------------------------------

    // Compile .nsl source (len bytes, need not be NUL-terminated). `name`
    // labels diagnostics (NULL = "<source>"). Returns a SHADER asset, or
    // asset_none on failure with the first error written to err as
    // "<name>:<line>:<col>: error: <message>" (truncated to err_cap - 1
    // bytes, always NUL-terminated; err may be NULL).
    neui_asset_t (NEUI_ABI *create)(neui_session_t session,
                                    const char* src, uint32_t len,
                                    const char* name,
                                    char* err, uint32_t err_cap);

    // Compile a .nsl file. The bytes come from the client resource provider
    // (NEUI_RESOURCE_KIND_SHADER) first, then the filesystem; `path` doubles
    // as the resource name and labels diagnostics.
    neui_asset_t (NEUI_ABI *create_from_file)(neui_session_t session,
                                              const char* path,
                                              char* err, uint32_t err_cap);

    // -------- Introspection --------------------------------------------------

    // The shader's declared uniforms, in declaration order (the reserved
    // neui_* uniforms and the sampler2D uniforms are not listed). 0 / false
    // for an invalid handle.
    uint32_t (NEUI_ABI *uniform_count)(neui_session_t session, neui_asset_t shader);
    bool     (NEUI_ABI *uniform_at)(neui_session_t session, neui_asset_t shader,
                                    uint32_t index, neui_shader_uniform_info_t* out);

    // -------- Bake -----------------------------------------------------------

    // Render the shader into a new BITMAP asset of ceil(width * scale) x
    // ceil(height * scale) device px, tagged with `scale` so it draws at
    // width x height logical px. Uniforms not named in bake->uniforms keep
    // their declared defaults, samplers not named in bake->textures read as
    // transparent; names the shader does not declare are ignored. The new
    // bitmap is a copy: changing a sampled bitmap later does not change it.
    // Returns asset_none for a bad handle, a non-positive or non-finite size,
    // or a bitmap side over 16384 px.
    neui_asset_t (NEUI_ABI *bake_bitmap)(neui_session_t session, neui_asset_t shader,
                                         const neui_shader_bake_t* bake);

    // Render `frames` images into one BITMAP asset tagged as a frame strip
    // (get_frame_count == frames). Frame i sets the float uniform `sweep` to
    //   from + (to - from) * i / (frames - 1)
    // (frame 0 = from, the last frame = to), matching a "frame" binding with
    // scale frames - 1. `sweep` may also be "neui_time" to bake an animation;
    // for a seamless loop of period T use to = T * (frames - 1) / frames.
    // neui picks the cell grid (frames are drawn by index, so the layout is
    // invisible to the frame machinery) to keep both bitmap sides small.
    // Returns asset_none as bake_bitmap does, and when `sweep` is not a float
    // uniform of the shader or "neui_time", frames is 0, or the grid does not
    // fit 16384 px a side.
    neui_asset_t (NEUI_ABI *bake_filmstrip)(neui_session_t session, neui_asset_t shader,
                                            const neui_shader_bake_t* bake,
                                            const char* sweep, float from, float to,
                                            uint32_t frames);

    // -------- Draw (vtable-appended) -----------------------------------------

    // Draw the shader into a paint callback: `p` is the painter of a
    // NEUI_EVENT_WIDGET_PAINT event or a surface paint function, and is only
    // valid inside it. The shader sees a layer of width x height logical px
    // (neui_size) and is evaluated once per device pixel, so the result is
    // drawn 1:1 and stays sharp at any display scale; under a rotation or
    // skew it is drawn transformed. `resolution` below 1 shades fewer pixels
    // and scales the result up with neui's own filter, the same on every
    // backend (cheap, soft backgrounds). Clip, transform and
    // the alpha stack apply as for any painter call. Returns false and draws
    // nothing for a bad handle, an empty or oversized rect (a side over 16384
    // device px), or a painter of another session.
    bool (NEUI_ABI *draw)(neui_painter_t* p, const neui_shader_draw_t* draw);

    // -------- GPU (vtable-appended) -------------------------------------------

    // Where the renderer has a GPU path (Direct2D on Windows), the compound
    // SHADER layer and draw() run shaders on the GPU, within 2/255 of the CPU
    // result per channel on 99.9% of pixels and 8/255 on all, except where
    // the shader jumps (a step, a comparison): there a rounding difference
    // can put a pixel on the other side of the edge. Off (false)
    // makes this session run every shader on the CPU, with the same pixels
    // on every platform; "#pragma neui cpu_only" does it per shader. On by
    // default. The bakes always run on the CPU.
    void (NEUI_ABI *set_gpu)(neui_session_t session, bool enabled);

    // -------- Animation (vtable-appended) -------------------------------------

    // The session's animation clock: seconds of running time, wrapped to
    // [0, 3600) like neui_time. A compound SHADER layer whose shader reads
    // neui_time and whose "time" is neither set nor bound runs on it by
    // itself. Read in a paint callback, it also makes that widget repaint at
    // the animation rate for as long as its paints keep reading it (pass it to
    // draw() as `time`, or animate anything else with it). Every widget
    // painted in one frame reads the same value.
    float (NEUI_ABI *animation_time)(neui_session_t session);

    // Frames per second of the animation, clamped to [1, 120]; 30 by default.
    // Approximate (OS timers; Windows ticks on a ~15.6 ms grid), and never
    // more than the display shows: above its refresh rate frames are capped.
    // A busy UI thread (a DAW's) lowers the frame rate, not the speed.
    void  (NEUI_ABI *set_animation_fps)(neui_session_t session, uint32_t fps);

    // Paused, the clock stands still and nothing repaints for it; resuming
    // continues from where it stopped. The clock also stands while the OS asks
    // for reduced motion ("Show animations in Windows" off, Reduce motion on
    // macOS and iOS).
    void  (NEUI_ABI *set_animation_paused)(neui_session_t session, bool paused);

    // -------- Samplers (vtable-appended) --------------------------------------

    // The shader's sampler2D uniforms, in declaration order (at most 4). Bind
    // bitmaps to them by name: textures in bake / draw, or a SHADER layer's
    // "u.<name>". 0 / false for an invalid handle.
    uint32_t (NEUI_ABI *sampler_count)(neui_session_t session, neui_asset_t shader);
    bool     (NEUI_ABI *sampler_at)(neui_session_t session, neui_asset_t shader,
                                    uint32_t index, neui_shader_sampler_info_t* out);
  } neui_shader_api_t;

#ifdef __cplusplus
}
#endif
