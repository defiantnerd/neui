#pragma once

/*
    contents.h

    Declares the low level event and eventhandler classes. An event is an object being sent from the seat to the client view.
    The client view will process the event and eventually pass it to an event handler which calls a lambda function.

*/

#include <string>
#include <functional>
#include <memory>

namespace neui
{
  class Itemlist;
  class IItemListOwner
  {
  public:
    virtual void update(Itemlist* sender) = 0;
  };
  class Itemlist
  {
  public:
    
  protected:
    Itemlist(IItemListOwner* owner) : _owner(owner) {}
    std::vector<std::string> _texts;
    IItemListOwner* _owner = nullptr;
  };


}