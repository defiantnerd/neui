#pragma once

#include <cstdint>

#include <neui/d/attrs.h>

#include "attrs.h"

// NEUI_ATTR_DISABLED_DIM: how the crossplatform host dims a disabled widget.
//
// The attribute is an ARGB value. Each channel byte scales the same channel of
// everything the disabled widget paints by byte / 256, where 0xFF means exactly
// 1 (unchanged) - so 0x80 is exactly 0.5. A scales the opacity, R, G and B the
// colour channels. Unset, a disabled widget gets 0x80FFFFFF: half opacity,
// colours unchanged, the 50 % dim the host has always applied.
//
// Two decisions live here, free of any host and backend, so the Tier-1 suite
// covers them (tests/test_disabled_dim.cpp):
//   - the decode: a channel byte -> the factor it multiplies by;
//   - what a widget gets: no dim while enabled, else the attribute when it holds
//     an int, else the default.
// The host applies the result to the widget's two paint passes: the A factor on
// the backend's alpha stack (push_alpha, as the 50 % always was), and only when
// R, G or B is below 0xFF the colour factors, through ColorDimBackend
// (color_dim_backend.h).

namespace neui_detail
{
  // Unset: half opacity, colours unchanged.
  inline constexpr uint32_t k_disabled_dim_default = 0x80FFFFFFu;
  // Every factor exactly 1. What an enabled widget gets, and the value a
  // component that draws its own disabled look sets.
  inline constexpr uint32_t k_disabled_dim_none = 0xFFFFFFFFu;

  // A channel byte as the factor it multiplies by, in 1/256 units: the byte
  // itself, except 0xFF, which is 256 - exactly 1.
  inline constexpr uint32_t dim_channel_units(uint32_t byte)
  {
    return (byte & 0xFFu) == 0xFFu ? 256u : (byte & 0xFFu);
  }

  // The same factor as a float: byte / 256, and exactly 1.0 for 0xFF.
  inline constexpr float dim_channel_factor(uint32_t byte)
  {
    return static_cast<float>(dim_channel_units(byte)) / 256.0f;
  }

  // The opacity factor of a dim (its A byte): what goes on the alpha stack.
  inline constexpr float dim_alpha_factor(uint32_t dim)
  {
    return dim_channel_factor(dim >> 24);
  }

  // Does the dim change the opacity / any colour channel?
  inline constexpr bool dim_scales_alpha(uint32_t dim)
  {
    return (dim >> 24) != 0xFFu;
  }
  inline constexpr bool dim_scales_color(uint32_t dim)
  {
    return (dim & 0x00FFFFFFu) != 0x00FFFFFFu;
  }

  // An 8-bit channel value scaled by a dim byte, rounded to nearest.
  inline constexpr uint32_t dim_scale_channel(uint32_t value, uint32_t dim_byte)
  {
    return ((value & 0xFFu) * dim_channel_units(dim_byte) + 128u) >> 8;
  }

  // An ARGB colour with R, G and B scaled by the dim's R, G and B. Its alpha is
  // left alone: the alpha stack scales that.
  inline constexpr uint32_t dim_color(uint32_t argb, uint32_t dim)
  {
    return (argb & 0xFF000000u)
         | (dim_scale_channel(argb >> 16, dim >> 16) << 16)
         | (dim_scale_channel(argb >> 8,  dim >> 8)  << 8)
         |  dim_scale_channel(argb,       dim);
  }

  // The dim a widget's own paint gets. Enabled: none, whatever the attribute
  // says. Disabled: NEUI_ATTR_DISABLED_DIM read as unsigned when it holds an
  // int, else the default - a missing key, a null bag and a wrong-kind slot
  // (set_float on the INT key) all mean "unset".
  inline uint32_t widget_disabled_dim(bool enabled, const AttrBag* attrs)
  {
    if (enabled) return k_disabled_dim_none;
    if (attr_is_int(attrs, NEUI_ATTR_DISABLED_DIM))
      return static_cast<uint32_t>(attrs->get_int(NEUI_ATTR_DISABLED_DIM, 0));
    return k_disabled_dim_default;
  }

} // namespace neui_detail
