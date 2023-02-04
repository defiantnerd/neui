#pragma once
#include "baseseat.h"

namespace neui
{
  // factory
  namespace factory
  {
    std::shared_ptr<IPlatformView> createWidget(uint32_t type);
  }
}