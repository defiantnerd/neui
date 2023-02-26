#pragma once

#include <cstdint>
#include <functional>

namespace neui
{
 
  class IResourceProvider
  {
  public:
    virtual bool getResource(const char* resourcename, std::function<void(const uint8_t* mem, size_t len)> fn) = 0;
    virtual ~IResourceProvider() = default;
  };
}