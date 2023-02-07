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
    friend class ItemList;
  public:
    virtual void updateItemList(Itemlist* sender) = 0;
  protected:
  };
  class Itemlist
  {
  public:
    friend class IItemListOwner;
    size_t add(const std::string_view text)
    {
      auto k = _texts.size();
      _texts.push_back(std::string(text));
      _owner->updateItemList(this);
      return k;
    }
    void remove(size_t index)
    {
      if (index < _texts.size())
      {
        _texts.erase(_texts.begin()+index);
      }
      _owner->updateItemList(this);
    }
    std::vector<std::string> _texts;
    void setOwner(IItemListOwner* owner) { _owner = owner; }
  protected:
    IItemListOwner* _owner = nullptr;
  };


}