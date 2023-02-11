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
    event::Textupdate e {text, caretPos, 0, 0};
    eventcallback->processEvent(e);
    if (e.useContent)
    {
      caretPos = e.caretPos;
      text = e.text;
    }
    return e.useContent;
  }

}