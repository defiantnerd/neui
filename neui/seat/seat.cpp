#include "seat.h"
#include "base/baseseat.h"
#include <memory>


#include "wind2d/seatimpl.h"

namespace neui
{
  std::unique_ptr<ISeat> seat;
  ISeat* ptr = nullptr;

  ISeat& Seat::Instance()
  {
    if (!seat)
    {
      seat = std::make_unique<wind2dSeat>();
    }
    return *seat;
  }
  
  bool ViewHandle::wantsEvent(const event::type eventtype)
  {
    return eventcallback->wantsEvent(eventtype);
  }

  void ViewHandle::sendEvent(Event& ev)
  {
    eventcallback->processEvent(ev);
  }

  bool ViewHandle::validateContent(std::string& text, int32_t& caretPos)
  {
    // return true if text and/or caretPos has been changed
    // TODO: enable a callback in the eventcallback pointer
    return false;
  }

}