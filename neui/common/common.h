#pragma once

/*
    common.h
    templates and definitions to be used all over the project

*/

#include <type_traits>
#include <cassert>

#if WIN32
#if _HAS_CXX20
#include <bit>
#else
enum class endian { little = 0, big = 1, native = little };
#endif
#else
#include <bit>
#endif

template <typename S, typename T>
inline constexpr bool is_similar_v = std::is_same_v<std::decay_t<S>, std::decay_t<T>>;

template <typename Head, typename...> struct head { using type = Head; };
template <typename... Ts> using head_t = typename head<Ts...>::type;

namespace neui
{
  constexpr uint32_t magic(const char n[5])
  {
    if constexpr (endian::native == endian::little)
    {
      return n[3] | n[2] << 8 | n[1] << 16 | n[0] << 24;
    }
    if constexpr (endian::native == endian::big)
    {
      return n[0] | n[1] << 8 | n[2] << 16 | n[3] << 24;
    }
    return 0;
  }

  enum widgettype 
  {
    appwindow = magic("wapp"),
    pluginwindow = magic("wplg"),
    panel = magic("cpnl"),
    label = magic("clbl"),
    text = magic("cedt"),
    button = magic("cbtn"),
    radiobutton = magic("crad"),
    checkbox = magic("cchk"),
    dropbutton = magic("cdrp"),
    droplist = magic("cdrl"),
    toggle = magic("ctgl"),
    progressbar = magic("cbar"),
    slider = magic("csld"),
    bitmap = magic("cbmp"),
    canvas = magic("ccan"),
    // accordion
    none = magic("none")
  };

}


